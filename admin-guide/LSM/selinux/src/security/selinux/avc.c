// SPDX-License-Identifier: GPL-2.0-only
/*
 * Implementation of the kernel access vector cache (AVC).
 *
 * Authors:  Stephen Smalley, <stephen.smalley.work@gmail.com>
 *	     James Morris <jmorris@redhat.com>
 *
 * Update:   KaiGai, Kohei <kaigai@ak.jp.nec.com>
 *	Replaced the avc_lock spinlock by RCU.
 *
 * Copyright (C) 2003 Red Hat, Inc., James Morris <jmorris@redhat.com>
 */
#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/init.h>
#include <linux/skbuff.h>
#include <linux/percpu.h>
#include <linux/list.h>
#include <net/sock.h>
#include <linux/un.h>
#include <net/af_unix.h>
#include <linux/ip.h>
#include <linux/audit.h>
#include <linux/ipv6.h>
#include <net/ipv6.h>
#include "avc.h"
#include "avc_ss.h"
#include "classmap.h"

#define CREATE_TRACE_POINTS
#include <trace/events/avc.h>

#define AVC_CACHE_SLOTS			512
#define AVC_DEF_CACHE_THRESHOLD		512
#define AVC_CACHE_RECLAIM		16

#ifdef CONFIG_SECURITY_SELINUX_AVC_STATS
#define avc_cache_stats_incr(field)	this_cpu_inc(avc_cache_stats.field)
#else
#define avc_cache_stats_incr(field)	do {} while (0)
#endif

struct avc_entry {
	u32			ssid;
	u32			tsid;
	u16			tclass;
	struct av_decision	avd;
	struct avc_xperms_node	*xp_node;
};

struct avc_node {
	struct avc_entry	ae;
	struct hlist_node	list; /* anchored in avc_cache->slots[i] */
	struct rcu_head		rhead;
};

struct avc_xperms_decision_node {
	struct extended_perms_decision xpd;
	struct list_head xpd_list; /* list of extended_perms_decision */
};

struct avc_xperms_node {
	struct extended_perms xp;
	struct list_head xpd_head; /* list head of extended_perms_decision */
};

struct avc_cache {
	struct hlist_head	slots[AVC_CACHE_SLOTS]; /* head for avc_node->list */
	spinlock_t		slots_lock[AVC_CACHE_SLOTS]; /* lock for writes */
	atomic_t		lru_hint;	/* LRU hint for reclaim scan */
	atomic_t		active_nodes;
	u32			latest_notif;	/* latest revocation notification */
};

struct avc_callback_node {
	int (*callback) (u32 event);
	u32 events;
	struct avc_callback_node *next;
};

#ifdef CONFIG_SECURITY_SELINUX_AVC_STATS
DEFINE_PER_CPU(struct avc_cache_stats, avc_cache_stats) = { 0 };
#endif

struct selinux_avc {
	unsigned int avc_cache_threshold;
	struct avc_cache avc_cache;
};

static struct selinux_avc selinux_avc;

void selinux_avc_init(void)
{
	int i;

	selinux_avc.avc_cache_threshold = AVC_DEF_CACHE_THRESHOLD;
	for (i = 0; i < AVC_CACHE_SLOTS; i++) {
		INIT_HLIST_HEAD(&selinux_avc.avc_cache.slots[i]);
		spin_lock_init(&selinux_avc.avc_cache.slots_lock[i]);
	}
	atomic_set(&selinux_avc.avc_cache.active_nodes, 0);
	atomic_set(&selinux_avc.avc_cache.lru_hint, 0);
}

unsigned int avc_get_cache_threshold(void)
{
	return selinux_avc.avc_cache_threshold;
}

void avc_set_cache_threshold(unsigned int cache_threshold)
{
	selinux_avc.avc_cache_threshold = cache_threshold;
}

static struct avc_callback_node *avc_callbacks __ro_after_init;
static struct kmem_cache *avc_node_cachep __ro_after_init;
static struct kmem_cache *avc_xperms_data_cachep __ro_after_init;
static struct kmem_cache *avc_xperms_decision_cachep __ro_after_init;
static struct kmem_cache *avc_xperms_cachep __ro_after_init;

/**
 * avc_hash - 计算AVC缓存槽位的哈希索引（关键hash函数）
 * @ssid: 源安全标识符，标识发起访问的主体
 * @tsid: 目标安全标识符，标识被访问的客体
 * @tclass: 目标安全类别，用于解释权限位的含义
 *
 * 该函数使用基于位移+异或的轻量级哈希算法，将三元组(ssid, tsid, tclass)
 * 映射到哈希表的槽位索引 [0, AVC_CACHE_SLOTS-1]。
 *
 * ═══════════════════════════════════════════════════════
 * 【为什么选择这种哈希算法？数学结构分析】
 * ═══════════════════════════════════════════════════════
 *
 * 一、数学本质：位域分离 + 线性混合
 *
 *   设哈希结果 H = ssid ^ (tsid << 2) ^ (tclass << 4)
 *
 *   从二进制位域视角分析（以低16位为例）：
 *
 *   位位置:  15..8   7..6    5..4    3..2    1..0
 *            ------  ------  ------  ------  ------
 *   ssid:    ssid[7..0]                             (原始位置)
 *   tsid<<2:          tsid[5..0]                    (左移2位)
 *   tclass<<4:                tclass[3..0]          (左移4位)
 *
 *   关键点：三个值在不同的"位带"上叠加，避免了低位之间的直接覆盖：
 *   - bits[1:0]  : 仅受 ssid 影响
 *   - bits[3:2]  : ssid XOR tsid 共同影响
 *   - bits[5:4]  : ssid XOR tsid XOR tclass 三者共同影响
 *   - bits[7:6]  : ssid XOR tsid 共同影响（tsid高位）
 *   - bits[8+]   : 主要受 ssid 影响
 *
 *   这本质上是一种"错位异或"（staggered XOR）结构，
 *   使得每个槽位索引的比特都尽可能多地受三个输入影响。
 *
 * 二、为什么用异或(XOR)而不是加法或乘法？
 *
 *   1. 【零开销】XOR 是单周期指令，无进位传播，延迟极低。
 *      在内核热路径中（avc_has_perm_noaudit 每次访问都调用），
 *      这一点至关重要。
 *
 *   2. 【位独立性】XOR 满足 GF(2) 上的线性性质：
 *      每个输出位仅由对应输入位决定，不存在进位耦合，
 *      便于分析哈希冲突的理论下界。
 *
 *   3. 【对称性】XOR 满足交换律和结合律，但这里通过位移
 *      人为打破了对称性（ssid/tsid/tclass 地位不同），
 *      避免了 hash(a,b,c) == hash(b,a,c) 这类退化情况。
 *
 * 三、为什么位移量选 2 和 4？
 *
 *   AVC_CACHE_SLOTS = 512 = 2^9，索引使用低9位。
 *
 *   在 SELinux 的实际负载中：
 *   - ssid/tsid 通常是小整数（进程、文件类型数量有限），
 *     有效比特集中在低8~16位；
 *   - tclass 值更小（类别数量少），有效比特在低8位以内。
 *
 *   位移量 2 和 4 的选取使得：
 *   - tsid 的低7位能影响槽位索引的 bit[2..8]（覆盖9位索引的中高位）
 *   - tclass 的低5位能影响槽位索引的 bit[4..8]（覆盖高位）
 *   - 三者在9位索引空间内形成最大程度的"错位覆盖"
 *
 *   若位移量过小（如1）：tclass/tsid 与 ssid 重叠严重，冲突率上升；
 *   若位移量过大（如8）：高位溢出9位窗口，高位信息丢失，退化为 ssid 哈希。
 *
 * 四、& (AVC_CACHE_SLOTS - 1) 的数学含义
 *
 *   AVC_CACHE_SLOTS = 2^9，故 AVC_CACHE_SLOTS - 1 = 0x1FF（9个1）。
 *   位与操作等价于模运算：H mod 2^9，但避免了除法指令。
 *
 *   这要求槽位数必须是2的幂——这是一个设计约束，
 *   换来的是每次哈希计算节省一次除法（约20~40个时钟周期）。
 *
 * 五、局限性（为什么不用更复杂的哈希）
 *
 *   更复杂的哈希（如 MurmurHash、FNV）能提供更好的雪崩效应，
 *   但在内核的 AVC 场景中不必要，原因：
 *   - 输入集合（SID对）在实际系统中数量有限且分布相对均匀；
 *   - AVC 有 LRU 淘汰机制（avc_reclaim_node），
 *     少量冲突不会导致严重退化；
 *   - 简单算法对 CPU 流水线友好，分支预测代价低。
 *
 * ═══════════════════════════════════════════════════════
 *
 * 返回值：
 *   返回范围在 [0, AVC_CACHE_SLOTS-1] 之间的哈希槽位索引
 */
static inline u32 avc_hash(u32 ssid, u32 tsid, u16 tclass)
{
	/*
	 * 错位异或哈希（Staggered XOR Hash）：
	 *
	 *   ssid              : 贡献低位，作为基准
	 *   tsid   << 2       : 左移2位，错开ssid低2位，扩展覆盖范围
	 *   tclass << 4       : 左移4位，进一步错开，覆盖中高位
	 *   & (512 - 1)       : 取低9位，等价于 mod 512，映射到槽位空间
	 *
	 * 三个值在9位索引空间内形成错位叠加，最大化各输入对结果的影响。
	 */
	return (ssid ^ (tsid << 2) ^ (tclass << 4)) & (AVC_CACHE_SLOTS - 1);
}

/**
 * avc_init - Initialize the AVC.
 *
 * Initialize the access vector cache.
 */
void __init avc_init(void)
{
	avc_node_cachep = kmem_cache_create("avc_node", sizeof(struct avc_node),
					0, SLAB_PANIC, NULL);
	avc_xperms_cachep = kmem_cache_create("avc_xperms_node",
					sizeof(struct avc_xperms_node),
					0, SLAB_PANIC, NULL);
	avc_xperms_decision_cachep = kmem_cache_create(
					"avc_xperms_decision_node",
					sizeof(struct avc_xperms_decision_node),
					0, SLAB_PANIC, NULL);
	avc_xperms_data_cachep = kmem_cache_create("avc_xperms_data",
					sizeof(struct extended_perms_data),
					0, SLAB_PANIC, NULL);
}

int avc_get_hash_stats(char *page)
{
	int i, chain_len, max_chain_len, slots_used;
	struct avc_node *node;
	struct hlist_head *head;

	rcu_read_lock();

	slots_used = 0;
	max_chain_len = 0;
	for (i = 0; i < AVC_CACHE_SLOTS; i++) {
		head = &selinux_avc.avc_cache.slots[i];
		if (!hlist_empty(head)) {
			slots_used++;
			chain_len = 0;
			hlist_for_each_entry_rcu(node, head, list)
				chain_len++;
			if (chain_len > max_chain_len)
				max_chain_len = chain_len;
		}
	}

	rcu_read_unlock();

	return scnprintf(page, PAGE_SIZE, "entries: %d\nbuckets used: %d/%d\n"
			 "longest chain: %d\n",
			 atomic_read(&selinux_avc.avc_cache.active_nodes),
			 slots_used, AVC_CACHE_SLOTS, max_chain_len);
}

/*
 * using a linked list for extended_perms_decision lookup because the list is
 * always small. i.e. less than 5, typically 1
 */
static struct extended_perms_decision *avc_xperms_decision_lookup(u8 driver,
					struct avc_xperms_node *xp_node)
{
	struct avc_xperms_decision_node *xpd_node;

	list_for_each_entry(xpd_node, &xp_node->xpd_head, xpd_list) {
		if (xpd_node->xpd.driver == driver)
			return &xpd_node->xpd;
	}
	return NULL;
}

static inline unsigned int
avc_xperms_has_perm(struct extended_perms_decision *xpd,
					u8 perm, u8 which)
{
	unsigned int rc = 0;

	if ((which == XPERMS_ALLOWED) &&
			(xpd->used & XPERMS_ALLOWED))
		rc = security_xperm_test(xpd->allowed->p, perm);
	else if ((which == XPERMS_AUDITALLOW) &&
			(xpd->used & XPERMS_AUDITALLOW))
		rc = security_xperm_test(xpd->auditallow->p, perm);
	else if ((which == XPERMS_DONTAUDIT) &&
			(xpd->used & XPERMS_DONTAUDIT))
		rc = security_xperm_test(xpd->dontaudit->p, perm);
	return rc;
}

static void avc_xperms_allow_perm(struct avc_xperms_node *xp_node,
				u8 driver, u8 perm)
{
	struct extended_perms_decision *xpd;
	security_xperm_set(xp_node->xp.drivers.p, driver);
	xpd = avc_xperms_decision_lookup(driver, xp_node);
	if (xpd && xpd->allowed)
		security_xperm_set(xpd->allowed->p, perm);
}

static void avc_xperms_decision_free(struct avc_xperms_decision_node *xpd_node)
{
	struct extended_perms_decision *xpd;

	xpd = &xpd_node->xpd;
	if (xpd->allowed)
		kmem_cache_free(avc_xperms_data_cachep, xpd->allowed);
	if (xpd->auditallow)
		kmem_cache_free(avc_xperms_data_cachep, xpd->auditallow);
	if (xpd->dontaudit)
		kmem_cache_free(avc_xperms_data_cachep, xpd->dontaudit);
	kmem_cache_free(avc_xperms_decision_cachep, xpd_node);
}

static void avc_xperms_free(struct avc_xperms_node *xp_node)
{
	struct avc_xperms_decision_node *xpd_node, *tmp;

	if (!xp_node)
		return;

	list_for_each_entry_safe(xpd_node, tmp, &xp_node->xpd_head, xpd_list) {
		list_del(&xpd_node->xpd_list);
		avc_xperms_decision_free(xpd_node);
	}
	kmem_cache_free(avc_xperms_cachep, xp_node);
}

static void avc_copy_xperms_decision(struct extended_perms_decision *dest,
					struct extended_perms_decision *src)
{
	dest->driver = src->driver;
	dest->used = src->used;
	if (dest->used & XPERMS_ALLOWED)
		memcpy(dest->allowed->p, src->allowed->p,
				sizeof(src->allowed->p));
	if (dest->used & XPERMS_AUDITALLOW)
		memcpy(dest->auditallow->p, src->auditallow->p,
				sizeof(src->auditallow->p));
	if (dest->used & XPERMS_DONTAUDIT)
		memcpy(dest->dontaudit->p, src->dontaudit->p,
				sizeof(src->dontaudit->p));
}

/*
 * similar to avc_copy_xperms_decision, but only copy decision
 * information relevant to this perm
 */
static inline void avc_quick_copy_xperms_decision(u8 perm,
			struct extended_perms_decision *dest,
			struct extended_perms_decision *src)
{
	/*
	 * compute index of the u32 of the 256 bits (8 u32s) that contain this
	 * command permission
	 */
	u8 i = perm >> 5;

	dest->used = src->used;
	if (dest->used & XPERMS_ALLOWED)
		dest->allowed->p[i] = src->allowed->p[i];
	if (dest->used & XPERMS_AUDITALLOW)
		dest->auditallow->p[i] = src->auditallow->p[i];
	if (dest->used & XPERMS_DONTAUDIT)
		dest->dontaudit->p[i] = src->dontaudit->p[i];
}

static struct avc_xperms_decision_node
		*avc_xperms_decision_alloc(u8 which)
{
	struct avc_xperms_decision_node *xpd_node;
	struct extended_perms_decision *xpd;

	xpd_node = kmem_cache_zalloc(avc_xperms_decision_cachep,
				     GFP_NOWAIT | __GFP_NOWARN);
	if (!xpd_node)
		return NULL;

	xpd = &xpd_node->xpd;
	if (which & XPERMS_ALLOWED) {
		xpd->allowed = kmem_cache_zalloc(avc_xperms_data_cachep,
						GFP_NOWAIT | __GFP_NOWARN);
		if (!xpd->allowed)
			goto error;
	}
	if (which & XPERMS_AUDITALLOW) {
		xpd->auditallow = kmem_cache_zalloc(avc_xperms_data_cachep,
						GFP_NOWAIT | __GFP_NOWARN);
		if (!xpd->auditallow)
			goto error;
	}
	if (which & XPERMS_DONTAUDIT) {
		xpd->dontaudit = kmem_cache_zalloc(avc_xperms_data_cachep,
						GFP_NOWAIT | __GFP_NOWARN);
		if (!xpd->dontaudit)
			goto error;
	}
	return xpd_node;
error:
	avc_xperms_decision_free(xpd_node);
	return NULL;
}

static int avc_add_xperms_decision(struct avc_node *node,
			struct extended_perms_decision *src)
{
	struct avc_xperms_decision_node *dest_xpd;

	dest_xpd = avc_xperms_decision_alloc(src->used);
	if (!dest_xpd)
		return -ENOMEM;
	avc_copy_xperms_decision(&dest_xpd->xpd, src);
	list_add(&dest_xpd->xpd_list, &node->ae.xp_node->xpd_head);
	node->ae.xp_node->xp.len++;
	return 0;
}

static struct avc_xperms_node *avc_xperms_alloc(void)
{
	struct avc_xperms_node *xp_node;

	xp_node = kmem_cache_zalloc(avc_xperms_cachep, GFP_NOWAIT | __GFP_NOWARN);
	if (!xp_node)
		return xp_node;
	INIT_LIST_HEAD(&xp_node->xpd_head);
	return xp_node;
}

static int avc_xperms_populate(struct avc_node *node,
				struct avc_xperms_node *src)
{
	struct avc_xperms_node *dest;
	struct avc_xperms_decision_node *dest_xpd;
	struct avc_xperms_decision_node *src_xpd;

	if (src->xp.len == 0)
		return 0;
	dest = avc_xperms_alloc();
	if (!dest)
		return -ENOMEM;

	memcpy(dest->xp.drivers.p, src->xp.drivers.p, sizeof(dest->xp.drivers.p));
	dest->xp.len = src->xp.len;

	/* for each source xpd allocate a destination xpd and copy */
	list_for_each_entry(src_xpd, &src->xpd_head, xpd_list) {
		dest_xpd = avc_xperms_decision_alloc(src_xpd->xpd.used);
		if (!dest_xpd)
			goto error;
		avc_copy_xperms_decision(&dest_xpd->xpd, &src_xpd->xpd);
		list_add(&dest_xpd->xpd_list, &dest->xpd_head);
	}
	node->ae.xp_node = dest;
	return 0;
error:
	avc_xperms_free(dest);
	return -ENOMEM;

}

static inline u32 avc_xperms_audit_required(u32 requested,
					struct av_decision *avd,
					struct extended_perms_decision *xpd,
					u8 perm,
					int result,
					u32 *deniedp)
{
	u32 denied, audited;

	denied = requested & ~avd->allowed;
	if (unlikely(denied)) {
		audited = denied & avd->auditdeny;
		if (audited && xpd) {
			if (avc_xperms_has_perm(xpd, perm, XPERMS_DONTAUDIT))
				audited &= ~requested;
		}
	} else if (result) {
		audited = denied = requested;
	} else {
		audited = requested & avd->auditallow;
		if (audited && xpd) {
			if (!avc_xperms_has_perm(xpd, perm, XPERMS_AUDITALLOW))
				audited &= ~requested;
		}
	}

	*deniedp = denied;
	return audited;
}

static inline int avc_xperms_audit(u32 ssid, u32 tsid, u16 tclass,
				   u32 requested, struct av_decision *avd,
				   struct extended_perms_decision *xpd,
				   u8 perm, int result,
				   struct common_audit_data *ad)
{
	u32 audited, denied;

	audited = avc_xperms_audit_required(
			requested, avd, xpd, perm, result, &denied);
	if (likely(!audited))
		return 0;
	return slow_avc_audit(ssid, tsid, tclass, requested,
			audited, denied, result, ad);
}

static void avc_node_free(struct rcu_head *rhead)
{
	struct avc_node *node = container_of(rhead, struct avc_node, rhead);
	avc_xperms_free(node->ae.xp_node);
	kmem_cache_free(avc_node_cachep, node);
	avc_cache_stats_incr(frees);
}

static void avc_node_delete(struct avc_node *node)
{
	hlist_del_rcu(&node->list);
	call_rcu(&node->rhead, avc_node_free);
	atomic_dec(&selinux_avc.avc_cache.active_nodes);
}

static void avc_node_kill(struct avc_node *node)
{
	avc_xperms_free(node->ae.xp_node);
	kmem_cache_free(avc_node_cachep, node);
	avc_cache_stats_incr(frees);
	atomic_dec(&selinux_avc.avc_cache.active_nodes);
}

static void avc_node_replace(struct avc_node *new, struct avc_node *old)
{
	hlist_replace_rcu(&old->list, &new->list);
	call_rcu(&old->rhead, avc_node_free);
	atomic_dec(&selinux_avc.avc_cache.active_nodes);
}

static inline int avc_reclaim_node(void)
{
	struct avc_node *node;
	int hvalue, try, ecx;
	unsigned long flags;
	struct hlist_head *head;
	spinlock_t *lock;

	for (try = 0, ecx = 0; try < AVC_CACHE_SLOTS; try++) {
		hvalue = atomic_inc_return(&selinux_avc.avc_cache.lru_hint) &
			(AVC_CACHE_SLOTS - 1);
		head = &selinux_avc.avc_cache.slots[hvalue];
		lock = &selinux_avc.avc_cache.slots_lock[hvalue];

		if (!spin_trylock_irqsave(lock, flags))
			continue;

		rcu_read_lock();
		hlist_for_each_entry(node, head, list) {
			avc_node_delete(node);
			avc_cache_stats_incr(reclaims);
			ecx++;
			if (ecx >= AVC_CACHE_RECLAIM) {
				rcu_read_unlock();
				spin_unlock_irqrestore(lock, flags);
				goto out;
			}
		}
		rcu_read_unlock();
		spin_unlock_irqrestore(lock, flags);
	}
out:
	return ecx;
}

static struct avc_node *avc_alloc_node(void)
{
	struct avc_node *node;

	node = kmem_cache_zalloc(avc_node_cachep, GFP_NOWAIT | __GFP_NOWARN);
	if (!node)
		goto out;

	INIT_HLIST_NODE(&node->list);
	avc_cache_stats_incr(allocations);

	if (atomic_inc_return(&selinux_avc.avc_cache.active_nodes) >
	    selinux_avc.avc_cache_threshold)
		avc_reclaim_node();

out:
	return node;
}

static void avc_node_populate(struct avc_node *node, u32 ssid, u32 tsid, u16 tclass, struct av_decision *avd)
{
	node->ae.ssid = ssid;
	node->ae.tsid = tsid;
	node->ae.tclass = tclass;
	memcpy(&node->ae.avd, avd, sizeof(node->ae.avd));
}

/**
 * avc_search_node - 在AVC缓存中搜索特定的节点，（关键函数）
 * @ssid: 源安全标识符
 * @tsid: 目标安全标识符
 * @tclass: 目标安全类别
 *
 * 根据给定的三元组在AVC缓存中进行精确查找。
 * 首先通过哈希函数定位到对应的哈希桶，然后遍历桶内的链表逐一比对。
 *
 * 注意：此函数使用RCU机制保护链表遍历，调用前必须持有rcu_read_lock。
 *
 * 返回值：
 * 成功时返回指向匹配的avc_node的指针，失败时返回NULL。
 */
static inline struct avc_node *avc_search_node(u32 ssid, u32 tsid, u16 tclass)
{
	struct avc_node *node, *ret = NULL; /* 遍历指针和返回值 */
	u32 hvalue;                         /* 哈希值，用于定位缓存槽位 */
	struct hlist_head *head;            /* 指向哈希桶链表头的指针 */

	/* 计算哈希值，将三元组映射到[0, AVC_CACHE_SLOTS-1]的索引 */
	hvalue = avc_hash(ssid, tsid, tclass);

	/* 获取对应索引的哈希桶头 */
	head = &selinux_avc.avc_cache.slots[hvalue];

	/* 使用RCU保护的宏遍历该桶内的节点链表 */
	hlist_for_each_entry_rcu(node, head, list) {
		/* 匹配条件：源SID、目标SID和目标类别必须完全一致 */
		if (ssid == node->ae.ssid &&
		    tclass == node->ae.tclass &&
		    tsid == node->ae.tsid) {
			ret = node; /* 命中：保存节点指针 */
			break;      /* 退出循环 */
		}
	}

	return ret;
}

/**
 * avc_lookup - 在AVC缓存中查找一个缓存条目。
 * @ssid: 源安全标识符（source security identifier），标识发起访问的主体
 * @tsid: 目标安全标识符（target security identifier），标识被访问的客体
 * @tclass: 目标安全类别（target security class），用于解释权限位的含义
 *
 * 在AVC缓存中查找与（@ssid, @tsid, @tclass）三元组匹配的有效缓存节点。
 *
 * 处理流程：
 * 1. 递增查找计数统计（lookups）
 * 2. 调用avc_search_node()在对应的哈希槽中线性搜索匹配节点
 * 3. 若找到匹配节点，直接返回该节点指针（缓存命中）
 * 4. 若未找到，递增未命中计数统计（misses），返回NULL（缓存未命中）
 *
 * 返回值：
 * - 成功：返回指向匹配的avc_node的指针
 * - 失败：返回NULL，表示缓存中不存在对应条目，需要向安全服务器请求新决策
 *
 * 注意：调用者必须在rcu_read_lock()保护下调用此函数，
 * 以确保返回的节点在使用期间不会被释放。
 */
static struct avc_node *avc_lookup(u32 ssid, u32 tsid, u16 tclass)
{
	struct avc_node *node;

	/* 递增AVC缓存查找次数统计计数器（仅在启用AVC统计时有效） */
	avc_cache_stats_incr(lookups);

	/*
	 * 在AVC缓存的哈希表中搜索匹配的节点：
	 * 通过avc_hash(ssid, tsid, tclass)计算哈希槽索引，
	 * 然后遍历该槽的链表，逐一比较ssid、tsid、tclass三元组
	 */
	node = avc_search_node(ssid, tsid, tclass);

	if (node)
		/* 缓存命中：直接返回找到的节点，调用者可直接使用其中的avd决策 */
		return node;

	/* 缓存未命中：递增未命中统计计数器，提示需要向安全服务器请求新决策 */
	avc_cache_stats_incr(misses);
	return NULL;
}

static int avc_latest_notif_update(u32 seqno, int is_insert)
{
	int ret = 0;
	static DEFINE_SPINLOCK(notif_lock);
	unsigned long flag;

	spin_lock_irqsave(&notif_lock, flag);
	if (is_insert) {
		if (seqno < selinux_avc.avc_cache.latest_notif) {
			pr_warn("SELinux: avc:  seqno %d < latest_notif %d\n",
			       seqno, selinux_avc.avc_cache.latest_notif);
			ret = -EAGAIN;
		}
	} else {
		if (seqno > selinux_avc.avc_cache.latest_notif)
			selinux_avc.avc_cache.latest_notif = seqno;
	}
	spin_unlock_irqrestore(&notif_lock, flag);

	return ret;
}

/**
 * avc_insert - Insert an AVC entry.
 * @ssid: source security identifier
 * @tsid: target security identifier
 * @tclass: target security class
 * @avd: resulting av decision
 * @xp_node: resulting extended permissions
 *
 * Insert an AVC entry for the SID pair
 * (@ssid, @tsid) and class @tclass.
 * The access vectors and the sequence number are
 * normally provided by the security server in
 * response to a security_compute_av() call.  If the
 * sequence number @avd->seqno is not less than the latest
 * revocation notification, then the function copies
 * the access vectors into a cache entry.
 */
static void avc_insert(u32 ssid, u32 tsid, u16 tclass,
		       struct av_decision *avd, struct avc_xperms_node *xp_node)
{
	struct avc_node *pos, *node = NULL;
	u32 hvalue;
	unsigned long flag;
	spinlock_t *lock;
	struct hlist_head *head;

	if (avc_latest_notif_update(avd->seqno, 1))
		return;

	node = avc_alloc_node();
	if (!node)
		return;

	avc_node_populate(node, ssid, tsid, tclass, avd);
	if (avc_xperms_populate(node, xp_node)) {
		avc_node_kill(node);
		return;
	}

	hvalue = avc_hash(ssid, tsid, tclass);
	head = &selinux_avc.avc_cache.slots[hvalue];
	lock = &selinux_avc.avc_cache.slots_lock[hvalue];
	spin_lock_irqsave(lock, flag);
	hlist_for_each_entry(pos, head, list) {
		if (pos->ae.ssid == ssid &&
			pos->ae.tsid == tsid &&
			pos->ae.tclass == tclass) {
			avc_node_replace(node, pos);
			goto found;
		}
	}
	hlist_add_head_rcu(&node->list, head);
found:
	spin_unlock_irqrestore(lock, flag);
}

/**
 * avc_audit_pre_callback - SELinux specific information
 * will be called by generic audit code
 * @ab: the audit buffer
 * @a: audit_data
 */
static void avc_audit_pre_callback(struct audit_buffer *ab, void *a)
{
	struct common_audit_data *ad = a;
	struct selinux_audit_data *sad = ad->selinux_audit_data;
	u32 av = sad->audited, perm;
	const char *const *perms;
	u32 i;

	audit_log_format(ab, "avc:  %s ", sad->denied ? "denied" : "granted");

	if (av == 0) {
		audit_log_format(ab, " null");
		return;
	}

	perms = secclass_map[sad->tclass-1].perms;

	audit_log_format(ab, " {");
	i = 0;
	perm = 1;
	while (i < (sizeof(av) * 8)) {
		if ((perm & av) && perms[i]) {
			audit_log_format(ab, " %s", perms[i]);
			av &= ~perm;
		}
		i++;
		perm <<= 1;
	}

	if (av)
		audit_log_format(ab, " 0x%x", av);

	audit_log_format(ab, " } for ");
}

/**
 * avc_audit_post_callback - SELinux specific information
 * will be called by generic audit code
 * @ab: the audit buffer
 * @a: audit_data
 */
static void avc_audit_post_callback(struct audit_buffer *ab, void *a)
{
	struct common_audit_data *ad = a;
	struct selinux_audit_data *sad = ad->selinux_audit_data;
	char *scontext = NULL;
	char *tcontext = NULL;
	const char *tclass = NULL;
	u32 scontext_len;
	u32 tcontext_len;
	int rc;

	rc = security_sid_to_context(sad->ssid, &scontext,
				     &scontext_len);
	if (rc)
		audit_log_format(ab, " ssid=%d", sad->ssid);
	else
		audit_log_format(ab, " scontext=%s", scontext);

	rc = security_sid_to_context(sad->tsid, &tcontext,
				     &tcontext_len);
	if (rc)
		audit_log_format(ab, " tsid=%d", sad->tsid);
	else
		audit_log_format(ab, " tcontext=%s", tcontext);

	tclass = secclass_map[sad->tclass-1].name;
	audit_log_format(ab, " tclass=%s", tclass);

	if (sad->denied)
		audit_log_format(ab, " permissive=%u", sad->result ? 0 : 1);

	trace_selinux_audited(sad, scontext, tcontext, tclass);
	kfree(tcontext);
	kfree(scontext);

	/* in case of invalid context report also the actual context string */
	rc = security_sid_to_context_inval(sad->ssid, &scontext,
					   &scontext_len);
	if (!rc && scontext) {
		if (scontext_len && scontext[scontext_len - 1] == '\0')
			scontext_len--;
		audit_log_format(ab, " srawcon=");
		audit_log_n_untrustedstring(ab, scontext, scontext_len);
		kfree(scontext);
	}

	rc = security_sid_to_context_inval(sad->tsid, &scontext,
					   &scontext_len);
	if (!rc && scontext) {
		if (scontext_len && scontext[scontext_len - 1] == '\0')
			scontext_len--;
		audit_log_format(ab, " trawcon=");
		audit_log_n_untrustedstring(ab, scontext, scontext_len);
		kfree(scontext);
	}
}

/*
 * slow_avc_audit - AVC审计的慢路径处理函数（栈空间占用较大）
 * @ssid: 源安全标识符（Source Security ID），标识发起访问的主体
 * @tsid: 目标安全标识符（Target Security ID），标识被访问的客体
 * @tclass: 目标安全类别，用于解释权限位的含义
 * @requested: 请求的权限位掩码
 * @audited: 需要审计的权限位掩码（根据策略的auditallow/auditdeny确定）
 * @denied: 被拒绝的权限位掩码
 * @result: 权限检查结果，0表示成功（允许），非0表示失败（拒绝）
 * @a: 通用审计数据结构指针，可为NULL
 *
 * ═════════════════════════════════════════════════════════════════
 * 【函数设计原理】
 * ═════════════════════════════════════════════════════════════════
 *
 * 一、为什么叫"slow"路径？
 *
 *   1. 栈空间消耗大：
 *      - struct common_audit_data 结构较大
 *      - struct selinux_audit_data 包含多个字段
 *      - 将此函数标记为 noinline 避免膨胀调用者的栈帧
 *
 *   2. 非热路径：
 *      - 大多数权限检查在缓存命中时快速完成
 *      - 审计只在特定条件下触发（策略配置了审计规则）
 *      - 审计涉及字符串格式化、内核日志等慢操作
 *
 * 二、调用时机
 *
 *   此函数在以下场景被调用：
 *   - avc_audit() 确定需要审计后
 *   - avc_xperms_audit() 处理扩展权限审计时
 *
 *   调用链示例：
 *   avc_has_perm()
 *     └── avc_audit()
 *           └── slow_avc_audit()  // 仅当 audited != 0 时
 *
 * 三、RCU安全性
 *
 *   函数注释明确指出可以在 rcu_read_lock() 保护下调用：
 *   - 函数内部不阻塞（不睡眠）
 *   - 所有内存分配已在调用者中完成
 *   - 审计缓冲区操作是原子安全的
 *
 * 四、审计数据的来源与用途
 *
 *   - ssid/tsid: 用于查找并打印安全上下文字符串（如 "unconfined_u:..."）
 *   - tclass: 决定如何解释权限位（如 "file" 类别的 "read" 权限）
 *   - requested: 记录原始请求，用于审计日志的完整性
 *   - audited: 实际需要记录的权限（可能少于 requested）
 *   - denied: 区分"拒绝并审计"与"允许但审计"两种场景
 *   - result: 影响 permissive 模式的日志格式
 *
 * 返回值：
 *   - 0: 审计成功完成
 *   - -EINVAL: tclass 参数无效（0或超出范围）
 */
noinline int slow_avc_audit(u32 ssid, u32 tsid, u16 tclass,
			    u32 requested, u32 audited, u32 denied, int result,
			    struct common_audit_data *a)
{
	struct common_audit_data stack_data;  /* 栈上备用的审计数据结构 */
	struct selinux_audit_data sad;        /* SELinux特定的审计数据 */

	/*
	 * 安全类别有效性检查：
	 * - tclass 不能为 0（无效类别）
	 * - tclass 必须小于 secclass_map 数组的大小
	 *
	 * secclass_map 定义在 classmap.h 中，包含了所有合法的安全类别。
	 * 使用 ARRAY_SIZE 宏而非硬编码数值，确保代码与数组定义同步。
	 *
	 * WARN_ON 会在调试时打印警告并返回真，生产环境仅返回值判断。
	 */
	if (WARN_ON(!tclass || tclass >= ARRAY_SIZE(secclass_map)))
		return -EINVAL;

	/*
	 * 处理调用者未提供审计数据结构的情况：
	 *
	 * 某些调用路径可能没有预先准备 common_audit_data 结构，
	 * 此时使用栈上的 stack_data 作为后备。
	 *
	 * 设置 type = LSM_AUDIT_DATA_NONE 表示没有额外的对象信息
	 * （如文件路径、inode 号等），仅记录 SELinux 审计数据。
	 */
	if (!a) {
		a = &stack_data;
		a->type = LSM_AUDIT_DATA_NONE;
	}

	/*
	 * 填充 SELinux 审计数据结构：
	 *
	 * 这些字段将被 avc_audit_pre_callback 和 avc_audit_post_callback
	 * 回调函数使用，格式化成人类可读的审计日志。
	 *
	 * 例如，最终日志格式类似：
	 *   avc:  denied  { read } for  scontext=unconfined_u:...
	 *           tcontext=system_u:... tclass=file permissive=0
	 */
	sad.tclass = tclass;      /* 目标安全类别，用于权限名查找 */
	sad.requested = requested; /* 原始请求权限，用于调试参考 */
	sad.ssid = ssid;          /* 源SID，用于查找源安全上下文 */
	sad.tsid = tsid;          /* 目标SID，用于查找目标安全上下文 */
	sad.audited = audited;    /* 实际审计的权限，驱动 pre_callback 输出 */
	sad.denied = denied;      /* 被拒绝的权限，影响日志中的 "denied/granted" */
	sad.result = result;      /* 检查结果，影响 "permissive=" 字段 */

	/*
	 * 将 SELinux 审计数据挂载到通用审计结构：
	 *
	 * common_lsm_audit 是 LSM（Linux Security Modules）框架的
	 * 通用审计入口，它会调用我们提供的回调函数来填充
	 * SELinux 特定的审计信息。
	 */
	a->selinux_audit_data = &sad;

	/*
	 * 执行通用 LSM 审计流程：
	 *
	 * @avc_audit_pre_callback: 前置回调，格式化权限信息
	 *   - 输出 "avc: denied/granted { 权限名 } for "
	 *   - 将权限位转换为可读的权限名称字符串
	 *
	 * @avc_audit_post_callback: 后置回调，格式化上下文信息
	 *   - 输出 "scontext=... tcontext=... tclass=..."
	 *   - 调用 security_sid_to_context 获取安全上下文字符串
	 *   - 在 permissive 模式下输出 "permissive=1"
	 *
	 * 此函数内部会处理审计缓冲区的分配、格式化和提交，
	 * 整个过程是非阻塞的（GFP_ATOMIC 分配）。
	 */
	common_lsm_audit(a, avc_audit_pre_callback, avc_audit_post_callback);

	return 0;  /* 审计记录成功提交，返回成功状态 */
}

/**
 * avc_add_callback - Register a callback for security events.
 * @callback: callback function
 * @events: security events
 *
 * Register a callback function for events in the set @events.
 * Returns %0 on success or -%ENOMEM if insufficient memory
 * exists to add the callback.
 */
int __init avc_add_callback(int (*callback)(u32 event), u32 events)
{
	struct avc_callback_node *c;
	int rc = 0;

	c = kmalloc(sizeof(*c), GFP_KERNEL);
	if (!c) {
		rc = -ENOMEM;
		goto out;
	}

	c->callback = callback;
	c->events = events;
	c->next = avc_callbacks;
	avc_callbacks = c;
out:
	return rc;
}

/**
 * avc_update_node - Update an AVC entry
 * @event : Updating event
 * @perms : Permission mask bits
 * @driver: xperm driver information
 * @xperm: xperm permissions
 * @ssid: AVC entry source sid
 * @tsid: AVC entry target sid
 * @tclass : AVC entry target object class
 * @seqno : sequence number when decision was made
 * @xpd: extended_perms_decision to be added to the node
 * @flags: the AVC_* flags, e.g. AVC_EXTENDED_PERMS, or 0.
 *
 * if a valid AVC entry doesn't exist,this function returns -ENOENT.
 * if kmalloc() called internal returns NULL, this function returns -ENOMEM.
 * otherwise, this function updates the AVC entry. The original AVC-entry object
 * will release later by RCU.
 */
static int avc_update_node(u32 event, u32 perms, u8 driver, u8 xperm, u32 ssid,
			   u32 tsid, u16 tclass, u32 seqno,
			   struct extended_perms_decision *xpd,
			   u32 flags)
{
	u32 hvalue;
	int rc = 0;
	unsigned long flag;
	struct avc_node *pos, *node, *orig = NULL;
	struct hlist_head *head;
	spinlock_t *lock;

	node = avc_alloc_node();
	if (!node) {
		rc = -ENOMEM;
		goto out;
	}

	/* Lock the target slot */
	hvalue = avc_hash(ssid, tsid, tclass);

	head = &selinux_avc.avc_cache.slots[hvalue];
	lock = &selinux_avc.avc_cache.slots_lock[hvalue];

	spin_lock_irqsave(lock, flag);

	hlist_for_each_entry(pos, head, list) {
		if (ssid == pos->ae.ssid &&
		    tsid == pos->ae.tsid &&
		    tclass == pos->ae.tclass &&
		    seqno == pos->ae.avd.seqno){
			orig = pos;
			break;
		}
	}

	if (!orig) {
		rc = -ENOENT;
		avc_node_kill(node);
		goto out_unlock;
	}

	/*
	 * Copy and replace original node.
	 */

	avc_node_populate(node, ssid, tsid, tclass, &orig->ae.avd);

	if (orig->ae.xp_node) {
		rc = avc_xperms_populate(node, orig->ae.xp_node);
		if (rc) {
			avc_node_kill(node);
			goto out_unlock;
		}
	}

	switch (event) {
	case AVC_CALLBACK_GRANT:
		node->ae.avd.allowed |= perms;
		if (node->ae.xp_node && (flags & AVC_EXTENDED_PERMS))
			avc_xperms_allow_perm(node->ae.xp_node, driver, xperm);
		break;
	case AVC_CALLBACK_TRY_REVOKE:
	case AVC_CALLBACK_REVOKE:
		node->ae.avd.allowed &= ~perms;
		break;
	case AVC_CALLBACK_AUDITALLOW_ENABLE:
		node->ae.avd.auditallow |= perms;
		break;
	case AVC_CALLBACK_AUDITALLOW_DISABLE:
		node->ae.avd.auditallow &= ~perms;
		break;
	case AVC_CALLBACK_AUDITDENY_ENABLE:
		node->ae.avd.auditdeny |= perms;
		break;
	case AVC_CALLBACK_AUDITDENY_DISABLE:
		node->ae.avd.auditdeny &= ~perms;
		break;
	case AVC_CALLBACK_ADD_XPERMS:
		rc = avc_add_xperms_decision(node, xpd);
		if (rc) {
			avc_node_kill(node);
			goto out_unlock;
		}
		break;
	}
	avc_node_replace(node, orig);
out_unlock:
	spin_unlock_irqrestore(lock, flag);
out:
	return rc;
}

/**
 * avc_flush - Flush the cache
 */
static void avc_flush(void)
{
	struct hlist_head *head;
	struct avc_node *node;
	spinlock_t *lock;
	unsigned long flag;
	int i;

	for (i = 0; i < AVC_CACHE_SLOTS; i++) {
		head = &selinux_avc.avc_cache.slots[i];
		lock = &selinux_avc.avc_cache.slots_lock[i];

		spin_lock_irqsave(lock, flag);
		/*
		 * With preemptable RCU, the outer spinlock does not
		 * prevent RCU grace periods from ending.
		 */
		rcu_read_lock();
		hlist_for_each_entry(node, head, list)
			avc_node_delete(node);
		rcu_read_unlock();
		spin_unlock_irqrestore(lock, flag);
	}
}

/**
 * avc_ss_reset - Flush the cache and revalidate migrated permissions.
 * @seqno: policy sequence number
 */
int avc_ss_reset(u32 seqno)
{
	struct avc_callback_node *c;
	int rc = 0, tmprc;

	avc_flush();

	for (c = avc_callbacks; c; c = c->next) {
		if (c->events & AVC_CALLBACK_RESET) {
			tmprc = c->callback(AVC_CALLBACK_RESET);
			/* save the first error encountered for the return
			   value and continue processing the callbacks */
			if (!rc)
				rc = tmprc;
		}
	}

	avc_latest_notif_update(seqno, 0);
	return rc;
}

/**
 * avc_compute_av - 根据安全策略计算访问向量并插入AVC缓存（慢路径核心函数）
 * @ssid: 源安全标识符（subject security identifier），标识发起访问的主体
 * @tsid: 目标安全标识符（target security identifier），标识被访问的客体
 * @tclass: 目标安全类别（target security class），用于解释权限位的含义
 * @avd: 输出参数，用于返回从安全服务器获取的访问向量决策（access vector decision）
 * @xp_node: 扩展权限节点（extended permissions node），用于存储ioctl等细粒度权限决策
 *
 * ═════════════════════════════════════════════════════════════════
 * 【函数定位与设计哲学】
 * ═════════════════════════════════════════════════════════════════
 *
 * 这是 avc_has_perm_noaudit() 的"缓存未命中"慢路径核心辅助函数。
 *
 * 命名含义：
 *   "compute" = 向安全服务器请求计算（非简单查找）
 *   "av"      = access vector（访问向量），即权限决策结果
 *
 * 为什么标记为 noinline？
 *   1. 【栈空间优化】：xp_node 结构体在栈上分配，占用较大空间。
 *      若内联到调用者，会使 avc_has_perm_noaudit 的栈帧在所有路径
 *      （包括缓存命中的热路径）上都膨胀，浪费内核栈资源。
 *   2. 【指令缓存优化】：将冷路径代码物理隔离，减少热路径的
 *      指令缓存污染，提升缓存命中率和分支预测准确率。
 *   3. 【编译器提示】：明确告知编译器此为 unlikely 路径，
 *      允许对热路径进行更激进的优化。
 *
 * ═════════════════════════════════════════════════════════════════
 * 【执行流程与数据流】
 * ═════════════════════════════════════════════════════════════════
 *
 *   ┌─────────────────┐     ┌──────────────────────┐     ┌──────────────┐
 *   │ 初始化xp_node    │ ──→ │ 调用安全服务器          │ ──→ │ 插入AVC缓存   │
 *   │ (链表头初始化)    │     │ (security_compute_av)│     │ (avc_insert) │
 *   └─────────────────┘     └──────────────────────┘     └──────────────┘
 *           │                        │                        │
 *           ▼                        ▼                        ▼
 *      xp_node->xpd_head        avd->allowed              哈希表定位
 *      初始化为空链表           avd->auditallow               处理冲突
 *                                xp->drivers               RCU安全插入
 *
 * ═════════════════════════════════════════════════════════════════
 * 【关键操作详解】
 * ═════════════════════════════════════════════════════════════════
 *
 * 1. INIT_LIST_HEAD(&xp_node->xpd_head)
 *    - 初始化扩展权限决策链表头
 *    - security_compute_av 可能向此链表添加扩展权限决策节点
 *    - 必须在调用安全服务器前完成初始化，避免未定义行为
 *
 * 2. security_compute_av(ssid, tsid, tclass, avd, &xp_node->xp)
 *    - 向 SELinux 安全服务器发起策略查询请求
 *    - 安全服务器根据当前加载的策略，计算 (ssid, tsid, tclass) 的：
 *      * 允许权限集合（avd->allowed）
 *      * 审计允许集合（avd->auditallow）
 *      * 审计拒绝集合（avd->auditdeny）
 *      * 标志位（avd->flags，如 AVD_FLAGS_PERMISSIVE）
 *      * 序列号（avd->seqno，用于策略更新时的失效检测）
 *      * 扩展权限信息（xp_node->xp，用于 ioctl 等场景）
 *    - 此操作可能涉及策略规则的线性扫描，是性能敏感点
 *
 * 3. avc_insert(ssid, tsid, tclass, avd, xp_node)
 *    - 将新计算的决策插入AVC哈希缓存
 *    - 使用 avc_hash() 定位槽位，处理哈希冲突（链表头插法）
 *    - 采用 RCU 机制保证并发读安全
 *    - 若该三元组已存在，则替换旧节点（avc_node_replace）
 *
 * ═════════════════════════════════════════════════════════════════
 * 【调用上下文与锁状态】
 * ═════════════════════════════════════════════════════════════════
 *
 * 调用时锁状态：
 *   - 调用者（avc_perm_nonode 或 avc_has_extended_perms）已释放
 *     rcu_read_lock()，因此本函数可以安全地睡眠（实际不睡眠，
 *     但 GFP_NOWAIT 分配可能失败）
 *
 * 内存分配上下文：
 *   - avc_insert 内部使用 GFP_NOWAIT | __GFP_NOWARN 分配新节点
 *   - 若内存不足，avc_insert 静默失败（返回void），调用者需处理
 *
 * ═════════════════════════════════════════════════════════════════
 *
 * 返回值：无（void）
 *   - 计算结果通过 avd 和 xp_node 输出参数返回
 *   - 缓存插入失败为静默处理，不影响函数返回
 */
static noinline void avc_compute_av(u32 ssid, u32 tsid, u16 tclass,
				    struct av_decision *avd,
				    struct avc_xperms_node *xp_node)
{
	/*
	 * 初始化扩展权限决策链表头：
	 *
	 * security_compute_av() 可能会根据策略配置，向 xp_node->xp
	 * 添加扩展权限信息（如 ioctl 命令白名单）。这些扩展权限以
	 * avc_xperms_decision_node 链表形式挂载在 xpd_head 下。
	 *
	 * 必须在调用安全服务器前完成初始化，否则若策略包含扩展权限，
	 * 会导致链表操作在未初始化的指针上进行，引发内核崩溃。
	 */
	INIT_LIST_HEAD(&xp_node->xpd_head);

	/*
	 * 向安全服务器请求访问向量决策：
	 *
	 * 这是整个 SELinux 权限检查的核心：安全服务器根据当前策略，
	 * 计算主体（ssid）对客体（tsid）在特定类别（tclass）下的
	 * 完整权限决策。
	 *
	 * 输入：ssid, tsid, tclass（三元组唯一标识访问场景）
	 * 输出：avd（标准权限决策）, xp_node->xp（扩展权限信息）
	 *
	 * 注意：此函数可能遍历大量策略规则，是 SELinux 的性能瓶颈之一。
	 * 因此 AVC 缓存的存在至关重要，避免重复计算。
	 */
	security_compute_av(ssid, tsid, tclass, avd, &xp_node->xp);

	/*
	 * 将新计算的决策插入AVC缓存：
	 *
	 * 使用 avc_hash(ssid, tsid, tclass) 定位哈希槽位，
	 * 在对应槽的链表头部插入新节点（RCU-safe）。
	 *
	 * 插入后，后续相同的 (ssid, tsid, tclass) 查询将直接
	 * 在 avc_lookup() 中命中缓存，走热路径快速返回。
	 *
	 * 若该三元组已存在缓存条目（竞争条件），则替换旧节点，
	 * 旧节点通过 call_rcu() 延迟释放。
	 */
	avc_insert(ssid, tsid, tclass, avd, xp_node);
}

static noinline int avc_denied(u32 ssid, u32 tsid,
			       u16 tclass, u32 requested,
			       u8 driver, u8 xperm, unsigned int flags,
			       struct av_decision *avd)
{
	if (flags & AVC_STRICT)
		return -EACCES;

	if (enforcing_enabled() &&
	    !(avd->flags & AVD_FLAGS_PERMISSIVE))
		return -EACCES;

	avc_update_node(AVC_CALLBACK_GRANT, requested, driver,
			xperm, ssid, tsid, tclass, avd->seqno, NULL, flags);
	return 0;
}

/*
 * The avc extended permissions logic adds an additional 256 bits of
 * permissions to an avc node when extended permissions for that node are
 * specified in the avtab. If the additional 256 permissions is not adequate,
 * as-is the case with ioctls, then multiple may be chained together and the
 * driver field is used to specify which set contains the permission.
 */
int avc_has_extended_perms(u32 ssid, u32 tsid, u16 tclass, u32 requested,
			   u8 driver, u8 xperm, struct common_audit_data *ad)
{
	struct avc_node *node;
	struct av_decision avd;
	u32 denied;
	struct extended_perms_decision local_xpd;
	struct extended_perms_decision *xpd = NULL;
	struct extended_perms_data allowed;
	struct extended_perms_data auditallow;
	struct extended_perms_data dontaudit;
	struct avc_xperms_node local_xp_node;
	struct avc_xperms_node *xp_node;
	int rc = 0, rc2;

	xp_node = &local_xp_node;
	if (WARN_ON(!requested))
		return -EACCES;

	rcu_read_lock();

	node = avc_lookup(ssid, tsid, tclass);
	if (unlikely(!node)) {
		avc_compute_av(ssid, tsid, tclass, &avd, xp_node);
	} else {
		memcpy(&avd, &node->ae.avd, sizeof(avd));
		xp_node = node->ae.xp_node;
	}
	/* if extended permissions are not defined, only consider av_decision */
	if (!xp_node || !xp_node->xp.len)
		goto decision;

	local_xpd.allowed = &allowed;
	local_xpd.auditallow = &auditallow;
	local_xpd.dontaudit = &dontaudit;

	xpd = avc_xperms_decision_lookup(driver, xp_node);
	if (unlikely(!xpd)) {
		/*
		 * Compute the extended_perms_decision only if the driver
		 * is flagged
		 */
		if (!security_xperm_test(xp_node->xp.drivers.p, driver)) {
			avd.allowed &= ~requested;
			goto decision;
		}
		rcu_read_unlock();
		security_compute_xperms_decision(ssid, tsid, tclass,
						 driver, &local_xpd);
		rcu_read_lock();
		avc_update_node(AVC_CALLBACK_ADD_XPERMS, requested,
				driver, xperm, ssid, tsid, tclass, avd.seqno,
				&local_xpd, 0);
	} else {
		avc_quick_copy_xperms_decision(xperm, &local_xpd, xpd);
	}
	xpd = &local_xpd;

	if (!avc_xperms_has_perm(xpd, xperm, XPERMS_ALLOWED))
		avd.allowed &= ~requested;

decision:
	denied = requested & ~(avd.allowed);
	if (unlikely(denied))
		rc = avc_denied(ssid, tsid, tclass, requested,
				driver, xperm, AVC_EXTENDED_PERMS, &avd);

	rcu_read_unlock();

	rc2 = avc_xperms_audit(ssid, tsid, tclass, requested,
			&avd, xpd, xperm, rc, ad);
	if (rc2)
		return rc2;
	return rc;
}

/**
 * avc_perm_nonode - 在AVC缓存未命中时计算并插入新的访问控制决策（慢路径）
 * @ssid: 源安全标识符（subject security identifier），标识发起访问的主体
 * @tsid: 目标安全标识符（target/object security identifier），标识被访问的客体
 * @tclass: 目标安全类别（target security class），用于解释权限位的含义
 * @requested: 请求的权限位掩码，基于@tclass进行解释
 * @flags: AVC控制标志，如AVC_STRICT（严格模式）或0
 * @avd: 输出参数，用于返回从安全服务器获取的访问向量决策
 *
 * ═══════════════════════════════════════════════════════════════
 * 【函数设计背景与职责】
 * ═══════════════════════════════════════════════════════════════
 *
 * 此函数是 avc_has_perm_noaudit() 的"缓存未命中"慢路径分支。
 *
 * 调用时机：
 *   当 avc_lookup() 在哈希表中未找到匹配的(ssid, tsid, tclass)三元组时，
 *   意味着该访问决策尚未被缓存，需要向安全服务器请求新的决策。
 *
 * 为什么不内联（noinline）？
 *   1. 【栈空间隔离】：函数内部声明了 struct avc_xperms_node xp_node，
 *      该结构体占用较大栈空间。若内联到 avc_has_perm_noaudit()，
 *      会使调用者的栈帧在所有路径上（包括缓存命中的快路径）都增大，
 *      而缓存命中是绝大多数情况，浪费宝贵的内核栈空间。
 *   2. 【代码优化提示】：标记 noinline 告知编译器此为冷路径（unlikely path），
 *      有助于编译器对调用者（热路径）进行更激进的优化，例如更好的
 *      指令缓存布局和分支预测优化。
 *   3. 【与 unlikely() 配合】：调用者使用 unlikely(!node) 标记此路径为
 *      非预期路径，noinline 进一步将其物理隔离出热路径代码段。
 *
 * ═══════════════════════════════════════════════════════════════
 * 【执行流程】
 * ═══════════════════════════════════════════════════════════════
 *
 *   1. 调用 avc_compute_av() 向安全服务器请求访问向量决策：
 *      - 初始化扩展权限节点 xp_node
 *      - 调用 security_compute_av() 计算访问决策
 *      - 调用 avc_insert() 将新决策插入AVC哈希缓存，供后续查找
 *      - 决策结果写入 avd 输出参数
 *
 *   2. 计算被拒绝的权限：
 *      denied = requested & ~(avd->allowed)
 *      即：请求的权限中，不在"已允许"集合内的部分
 *
 *   3. 若存在被拒绝的权限（unlikely，正常系统中访问通常被允许）：
 *      调用 avc_denied() 处理拒绝逻辑：
 *      - driver=0, xperm=0 表示非扩展权限（ioctl）场景
 *      - 根据 enforcing 模式和 flags 决定行为：
 *        * enforcing=1 且无 permissive 标志 → 返回 -EACCES
 *        * enforcing=0 或 permissive 模式   → 更新节点并返回 0
 *
 *   4. 若所有权限均被授予，返回 0（成功）
 *
 * ═══════════════════════════════════════════════════════════════
 *
 * 返回值：
 *   - 0       : 所有 @requested 权限均已授予，访问允许
 *   - -EACCES : 至少有一个权限被拒绝，且处于 enforcing 模式
 */
static noinline int avc_perm_nonode(u32 ssid, u32 tsid, u16 tclass,
				    u32 requested, unsigned int flags,
				    struct av_decision *avd)
{
	u32 denied;                     /* 被拒绝的权限位：requested中未被允许的部分 */
	struct avc_xperms_node xp_node; /* 扩展权限节点，存储ioctl等扩展权限决策 */

	/*
	 * 向安全服务器请求访问向量决策并插入AVC缓存：
	 *
	 * avc_compute_av() 内部执行三个步骤：
	 *   1. INIT_LIST_HEAD(&xp_node->xpd_head)：初始化扩展权限决策链表头
	 *   2. security_compute_av()：调用安全服务器根据当前策略计算
	 *      (ssid, tsid, tclass) 的访问向量决策，结果存入 avd 和 xp_node
	 *   3. avc_insert()：将新计算的决策插入AVC哈希缓存，
	 *      后续相同三元组的查找可直接命中缓存（快路径）
	 *
	 * 注意：此函数在调用前 rcu_read_unlock() 已被调用者释放，
	 * 因此此处无需担心RCU读锁的持有问题。
	 */
	avc_compute_av(ssid, tsid, tclass, avd, &xp_node);

	/*
	 * 计算被拒绝的权限位：
	 *
	 * requested  : 调用者希望获得的权限集合（位掩码）
	 * avd->allowed: 安全策略明确允许的权限集合（位掩码）
	 * denied     : 请求但未被允许的权限 = requested AND NOT allowed
	 *
	 * 例如：requested=0b1101, allowed=0b0101 → denied=0b1000
	 *       表示第4位对应的权限被拒绝
	 */
	denied = requested & ~(avd->allowed);

	if (unlikely(denied))
		/*
		 * 存在被拒绝的权限（非正常路径，多数访问应被允许）：
		 *
		 * avc_denied() 根据系统状态决定最终行为：
		 * - AVC_STRICT 标志：无论 permissive 状态，直接返回 -EACCES
		 * - enforcing 模式 且 无 AVD_FLAGS_PERMISSIVE：返回 -EACCES
		 * - permissive 模式（全局或针对该域）：
		 *     调用 avc_update_node() 将被拒绝的权限临时标记为
		 *     granted（用于permissive模式下的授权），然后返回 0
		 *
		 * driver=0, xperm=0：表示这是标准权限检查，非扩展权限
		 * （扩展权限用于 ioctl 命令级别的细粒度控制）
		 */
		return avc_denied(ssid, tsid, tclass, requested, 0, 0,
				  flags, avd);

	/* 所有请求的权限均已被安全策略授予，访问允许，返回成功 */
	return 0;
}

/**
 * avc_has_perm_noaudit - 检查权限但不执行审计操作。
 * @ssid: 源安全标识符（source security identifier），标识发起访问的主体
 * @tsid: 目标安全标识符（target security identifier），标识被访问的客体
 * @tclass: 目标安全类别（target security class），用于解释权限位的含义
 * @requested: 请求的权限位掩码，基于@tclass进行解释
 * @flags: 控制标志，可为AVC_STRICT（严格模式，不允许permissive）或0
 * @avd: 输出参数，用于返回访问向量决策（access vector decisions）的副本
 *
 * 检查AVC缓存，判断是否为SID对（@ssid, @tsid）授予了@requested权限。
 * 权限的具体含义基于@tclass进行解释。
 *
 * 处理流程：
 * 1. 首先在AVC缓存中查找已有的决策（avc_lookup）
 * 2. 若缓存命中，直接使用缓存中的决策结果
 * 3. 若缓存未命中（unlikely），调用安全服务器计算新的访问向量决策，
 *    并将结果添加到缓存中以供后续查找
 * 4. 将决策结果的副本存入@avd
 * 5. 若存在被拒绝的权限，调用avc_denied处理拒绝逻辑
 *
 * 返回值：
 * - 返回%0：所有@requested权限均被授予
 * - 返回-%EACCES：至少有一个权限被拒绝
 * - 返回其他-errno：发生其他错误
 *
 * 注意：此函数通常由avc_has_perm()调用，但也可直接调用，
 * 以便将权限检查与审计分离。例如，在权限检查期间需要持有锁，
 * 但审计时应释放锁的场景中，可直接调用此函数。
 */
inline int avc_has_perm_noaudit(u32 ssid, u32 tsid,
				u16 tclass, u32 requested,
				unsigned int flags,
				struct av_decision *avd)
{
	u32 denied;          /* 被拒绝的权限位：requested中未被allowed的部分 */
	struct avc_node *node; /* AVC缓存节点指针，存储权限决策信息 */

	/* 合法性检查：requested权限不能为0，否则触发警告并返回拒绝 */
	if (WARN_ON(!requested))
		return -EACCES;

	/* 进入RCU读临界区，保护对AVC缓存的并发读访问 */
	rcu_read_lock();

	/* 在AVC缓存中查找匹配的节点（ssid + tsid + tclass三元组） */
	node = avc_lookup(ssid, tsid, tclass);
	if (unlikely(!node)) {
		/*
		 * 缓存未命中（慢路径）：
		 * 先释放RCU读锁（因为avc_perm_nonode需要更大的栈空间），
		 * 然后调用avc_perm_nonode向安全服务器请求新的访问向量决策，
		 * 并将结果插入AVC缓存，同时返回权限检查结果
		 */
		rcu_read_unlock();
		return avc_perm_nonode(ssid, tsid, tclass, requested,
				       flags, avd);
	}

	/*
	 * 缓存命中（快路径）：
	 * 计算被拒绝的权限：requested中未被node->ae.avd.allowed允许的位
	 * 将缓存节点中的访问向量决策复制到输出参数avd中
	 */
	denied = requested & ~node->ae.avd.allowed;
	memcpy(avd, &node->ae.avd, sizeof(*avd));

	/* 退出RCU读临界区 */
	rcu_read_unlock();

	if (unlikely(denied))
		/*
		 * 存在被拒绝的权限：
		 * 调用avc_denied处理拒绝逻辑，根据enforcing模式和flags决定
		 * 是返回-EACCES还是通过（permissive模式下可能返回0）
		 * 参数中driver=0, xperm=0表示非扩展权限场景
		 */
		return avc_denied(ssid, tsid, tclass, requested, 0, 0,
				  flags, avd);

	/* 所有请求的权限均已授予，返回成功 */
	return 0;
}

/**
 * avc_has_perm - 检查权限并执行适当的审计
 * @ssid: 源安全标识符（subject SID）
 * @tsid: 目标安全标识符（target SID）
 * @tclass: 目标安全类别
 * @requested: 请求的权限，基于@tclass进行解释
 * @auditdata: 辅助审计数据
 *
 * 检查AVC以确定是否为SID对（@ssid, @tsid）授予了@requested权限，
 * 基于@tclass解释权限，并在缓存未命中时调用安全服务器获取
 * 新决策并添加到缓存。根据策略审计权限的授予或拒绝。
 * 如果所有@requested权限都被授予，返回%0；
 * 如果任何权限被拒绝，返回-%EACCES；
 * 其他错误返回相应的-errno。
 */
int avc_has_perm(u32 ssid, u32 tsid, u16 tclass,
		 u32 requested, struct common_audit_data *auditdata)
{
	struct av_decision avd;  /* 访问向量决策结构体，存储权限决策结果 */
	int rc, rc2;             /* rc: 权限检查结果, rc2: 审计结果 */

	/* 第一步：无审计权限检查
	 * 调用avc_has_perm_noaudit执行实际的权限检查：
	 * - 首先检查AVC缓存中是否存在现有决策
	 * - 缓存未命中时，调用安全服务器计算访问向量
	 * - 如果所有请求的权限都被授予，返回0
	 * - 如果有任何权限被拒绝，返回-EACCES
	 * - 将访问向量决策详情填充到avd中
	 * 参数0表示无特殊标志（非AVC_STRICT严格模式）
	 */
	rc = avc_has_perm_noaudit(ssid, tsid, tclass, requested, 0,
				  &avd);

	/* 第二步：基于权限检查结果执行审计
	 * 调用avc_audit处理审计日志记录：
	 * - 根据auditdeny标志记录被拒绝的权限
	 * - 根据auditallow标志记录被允许的权限
	 * - 如果审计成功或不需要审计，返回0
	 * - 如果审计缓冲区分配失败，返回-ENOMEM或其他错误
	 */
	rc2 = avc_audit(ssid, tsid, tclass, requested, &avd, rc,
			auditdata);

	/* 如果审计过程出错，优先返回审计错误；
	 * 否则返回权限检查的结果（成功或拒绝） */
	if (rc2)
		return rc2;
	return rc;
}

u32 avc_policy_seqno(void)
{
	return selinux_avc.avc_cache.latest_notif;
}
