# ksm 与 zswap：以 CPU 换内存的两把刀（Linux 6.6）

> 阅读目标：掌握**同页合并**（KSM 的双树协议、checksum 加速、COW 解合并）与**压缩交换**（zswap 的三层数据结构、same-filled 直存、32 路 zpool 分流、限额回写）。两者都属"用 CPU 换内存"，但分别站在匿名页的**去重端**与**换出端**。前置：[vma 与 mmap 生命周期](../vma/vma与mmap生命周期.md)、[cow](../cow/cow.md)、`swap.rst`。

---

## 一、KSM：内容寻址的匿名页去重（mm/ksm.c）

**入场券**：进程显式 `madvise(MADV_MERGEABLE)`（或 PR_SET_MEMORY_MERGE，进程级开关 6.4+）——KSM 从不自动扫描普通匿名页。

### 1. 数据结构（已核对 6.6）

```c
struct mm_slot {                  /* mm/mm_slot.h：进入 ksm_scan 队列的 mm */
	struct hlist_node hash;  struct list_head mm_node;  struct mm_struct *mm;
};
struct ksm_rmap_item {            /* 每个被扫描过的用户页一条 */
	struct ksm_rmap_item *rmap_list;   /* 挂在 mm 的扫描链上 */
	union { struct anon_vma *anon_vma; /* stable 后换锚点 */
		int nid; };                 /* unstable 期记录 NUMA 节点 */
	struct mm_struct *mm;
	unsigned long address;        /* 低 bit 复用做状态标志 */
	unsigned int oldchecksum;     /* unstable 期的内容指纹 */
	union { struct rb_node node;  /* 挂 unstable 树 */
		struct { struct ksm_stable_node *head;  /* stable 后挂 */
			 struct hlist_node hlist; } };     /* dup 链 */
};
struct ksm_stable_node {          /* 合并组：指向 KSM 大页（可迁移） */
	union { struct rb_node node;          /* stable 树节点 */
		struct { struct list_head *head;  /* 迁移挂起链 */
			 struct hlist_node hlist_dup; ... } };
	...  unsigned long kpfn;   int rmap_hlist_len; ...
};
```

三件套：**mm_slot 队列**（哪些 mm 要扫）→ **rmap_item 链**（每 mm 扫过哪些页）→ **两棵树**（unstable：本周期候选，按 checksum 排序；stable：已合并组，永久）。

### 2. 扫描协议（ksm_scan，单例游标）

```
ksm_thread 周期：pages_to_scan=100 页 / sleep_millisecs=20ms（sysfs 可调）
对每个 mm_slot 的 rmap 链逐页：
  ① write-lock 页 + calc_checksum（内容指纹，快速筛掉绝大多数页）
  ② unstable_tree_search_insert：按 checksum 查 unstable 树
      命中候选 → try_to_merge_two_pages（字节比对+改映射到同一 KSM 页，
                  同时 break_cow 两端旧映射）→ 成功则 stable 树插入
      未命中   → rmap_item 挂入 unstable 树等下周期的"另一半"
  ③ 每轮 full scan 结束 unstable 树整棵重建（内容会变——树不能长期有效）
零页特例：use_zero_pages=1 时全零页直接映射到全局共享零页
  （ksm_zero_pages 计数；write fault 走 unshare 例外路径）
```

**COW 语义**：合并后的页写时缺页 → `break_cow(rmap_item)` → 复制私有副本并从 stable 链表摘除该 rmap（`unmerge_ksm_pages`）。`get_ksm_page()` 以 lock+refcount 协议取 stable_node 指向的 page（树可能并发删除）。

### 3. 观测与调参

```
/sys/kernel/mm/ksm/
  run(0/1/2)  pages_to_scan  sleep_millisecs  merge_across_nodes
  pages_shared  pages_sharing  full_scans      use_zero_pages
去重收益 = pages_sharing / (pages_shared + pages_sharing)
  （典型容器/虚机场景 10-30%；QEMU 同镜像多开是最优场景）
```

**6.6 边界（如实记录）**：smart scan（跳过冷候选省 CPU）是 6.7 才合入的功能，6.6 仍为全量扫描模型。

## 二、zswap：换出路径上的 RAM 压缩缓存（mm/zswap.c）

**定位**：swap-out 的页先压缩存进**内存里的 zpool**，真正的磁盘 swap 只收 zswap 回写的"溢出"；swap-in 先查 zswap，命中即免磁盘 IO。

### 1. 三层数据结构（已核对 6.6）

```
struct zswap_tree {               /* 每个 swap 设备一棵（rbtree！勘误：
	struct rb_root rbroot;        /*   6.6 非 xarray，按 swp offset 排序 */
	spinlock_t lock;  };
static struct zswap_tree *zswap_trees[MAX_SWAPFILES];

struct zswap_entry {              /* 每个 swap 槽位一条 */
	struct rb_node rbnode;  swp_entry_t swpentry;  int refcount;
	unsigned int length;    struct zswap_pool *pool;
	union { unsigned long handle;    /* zpool 里的压缩对象句柄 */
		unsigned long value; };     /* same-filled：直接存重复值不压缩！ */
	struct obj_cgroup *objcg;      /* memcg 归属 */
	struct list_head lru; };

struct zswap_pool {
	struct zpool *zpools[ZSWAP_NR_ZPOOLS];   /* ★32 路池分流（6.6 新）：
	      hash_ptr(entry, ilog2(32)) 选池——把全局锁竞争摊成 32 份 */
	struct crypto_acomp_ctx __percpu *acomp_ctx;  /* 异步压缩（percpu tfm）*/
	struct kref kref;  struct list_head list;
	struct work_struct shrink_work;              /* 限额回写工单 */
	char tfm_name[...];  struct list_head lru; spinlock_t lru_lock;
};
```

### 2. 三条路径

```
zswap_store(folio)  换入端：
  ① zswap_is_page_same_filled：逐 long memcmp——全重复则 entry.value
     直存（0 压缩开销；zswap_same_filled_pages 计数）
  ② crypto_acomp 异步压缩（percpu acomp_ctx；lzo/lz4/zstd 由
     compressor 参数定）→ zpool_malloc 进选中的第 i 池
  ③ rb 插入 zswap_tree + 挂 pool LRU
zswap_load          换出端：查 rb → zpool map → decompress 到新 folio；
  exclusive_loads=1（默认配置项）时命中即摘条目——"读一次回内存，
  槽位让给后来者"
zswap_writeback_entry  溢出端：全局池大小逼近
  totalram_pages × max_pool_percent（默认 20%）→ pool_limit_hit++
  → shrink_worker 按池 LRU 从尾端解压回写真实 swap 设备 → 释放 zpool
```

**6.6 边界**：内存压力直接驱动的全局 shrinker（把 zswap 变成积极回收执行者）在 6.6 尚未合入；6.6 的回写只由**池限额**触发——`shrink_worker` 与 entry LRU 是其后身的基础设施。

### 3. 参数与观测

```
/sys/module/zswap/parameters/
  enabled(=CONFIG_ZSWAP_DEFAULT_ON)  compressor  zpool
  max_pool_percent=20  same_filled_pages_enabled  exclusive_loads
/sys/kernel/debug/zswap/  pool_total_size  stored_pages  pool_limit_hit
  （各池分散统计）/sys/kernel/debug/zswap/pools/*/
内存收益粗估：stored_pages×4K vs pool_total_size —— 压缩比 2~3×常见；
代价：换入/换出路径 CPU（acomp），以及与直接 swap 相比多一次中转
```

## 三、两者对比（为什么放一章）

| | KSM | zswap |
| :--- | :--- | :--- |
| 换什么 | CPU（扫描+比对） | CPU（压缩/解压） |
| 得什么 | 消除**重复内容** | 缩小**唯一内容**的驻留体积 |
| 生效条件 | MADV_MERGEABLE 显式开启 | swap 活跃时自动介入 |
| 最优场景 | 同 OS 镜像多虚机/容器 | 内存吃紧的桌面/小内存实例 |
| 风险 | 边信道（时序侧信道 → 可关） | 回写风暴（pool_limit_hit 激增） |

## 四、思考题

1. KSM 的 unstable 树为什么每轮 full scan 整体重建，stable 树却长期存在？（对照：checksum 会失效 vs KSM 页不可变。）
2. `try_to_merge_two_pages` 合并后，两个 mm 的 PTE 都指到同一只读 KSM 页——写出错路径的完整链（fault → break_cow → 新匿名页）。（对照 [cow](../cow/cow.md)。）
3. same-filled 检测为什么用"逐 long memcmp"而不是 hash？（提示：它要的是"全等"证据而非指纹；顺带解释 value 复用 handle 字段的 union。）
4. `ZSWAP_NR_ZPOOLS=32` 把什么竞争拆成了 32 份？（对照 zpool 内部锁与 rb 树锁的层级。）
5. zswap 命中换入时 exclusive_loads=1 会摘除条目——这改变了什么统计性质？（提示：swap 设备上该槽位的"物理存在"与 zswap 记录的一致性。）

---

> 回看：[vma 与 mmap 生命周期](../vma/vma与mmap生命周期.md)｜[page cache 与 writeback](../pagecache/pagecache与writeback.md)｜mm 分册 [SUMMARY](../SUMMARY.md)
