# idempotent 模块加载：并发去重的两层（Linux 6.6）

> 阅读目标：解剖模块加载的**两层并发去重**——finit_module 侧的 `idempotent` 层（**v6.5 引入**，commit 9b9879fc0327，2023-05；★勘误注记：本分册 ch05 曾误记 "6.6 新层"，merge-base 实证为 v6.5）与 request_module 侧的 `kmod_dup` 层（`CONFIG_MODULE_DEBUG_AUTOLOAD_DUPS` 调试门控）。吃透 cookie 用 `file_inode` 的妙处、"查 + 挂"一体注册、per-waiter completion 与 complete_all 两种唤醒拓扑、60 秒延迟删除的启发式（"let duplicates suffer" 注释原话）、以及"重复请求耗费两倍模块虚拟内存"的动机（Kconfig 原文）。全部对照 v6.6（kernel/module/main.c、dups.c、Kconfig、internal.h）。前置：[05-kernel_read_file](05-kernel_read_file加载基础设施.md)（finit_module 链）｜[06-usermodehelper](06-usermodehelper机制.md)（kmod 侧）。

---

## 一、问题：并发 insmod 的 vmalloc 双倍税

```
时序（无去重）：
  CPU0: request_module("foo") ─┐
  CPU1: request_module("foo") ─┴─► 两个 modprobe 并发跑
        └─ 各自 finit_module() → kernel_read_file 的 vmalloc
           → 签名验证 → ...直到某一方 insert 失败才放弃

Kconfig（MODULE_DEBUG_AUTOLOAD_DUPS，Kconfig:62 起）原文：
  "there is a small time window in which multiple duplicate requests
   can end up in userspace and multiple modprobe calls race calling
   finit_module() around the same time for duplicate modules. The
   finit_module() system call can consume in the worst case more than
   twice the respective module size in virtual memory for each
   duplicate module request."
——自动加载风暴（热插拔风暴/NFS 根的 udev 冷启动）下这是真实的
  vmalloc 压力；dups.c:108-116 注释称之 "finit_module() thrashing"
```

两层解法各有分工（★生产地位不同——这是本主题最容易被误读的点）：

| 层 | 位置 | 键 | 编译条件 |
| :--- | :--- | :--- | :--- |
| **idempotent** | `finit_module`（main.c） | `file_inode(f)` | **无条件**（生产特性，v6.5 起） |
| **kmod_dup** | `request_module`（dups.c） | 模块名字符串 | **CONFIG_MODULE_DEBUG_AUTOLOAD_DUPS**（Kconfig 自述 "never have it enabled on real systems"） |

## 二、idempotent 层：finit_module 的 inode 锚点（main.c:3061-3169）

### 2.1 数据结构与注册（"查 + 挂"一体）

```c
struct idempotent {                 /* :3061-3066 ★栈上对象！*/
	const void *cookie;             /* = file_inode(f) */
	struct hlist_node entry;
	struct completion complete;     /* 每个 waiter 一份 */
	int ret;
};
#define IDEM_HASH_BITS 8            /* :3068  256 桶哈希 */
static struct hlist_head idem_hash[1 << IDEM_HASH_BITS];
static DEFINE_SPINLOCK(idem_lock);

static bool idempotent(struct idempotent *u, const void *cookie)
{
	hash = hash_ptr(cookie, IDEM_HASH_BITS);
	...查同 cookie...
	hlist_add_head(&u->entry, idem_hash + hash);   /* ★无论新旧：
	                                                  waiter 也入链！*/
	return !first;                   /* true = 已有人在加载 */
}
```

**cookie 是 `file_inode(f)`**（:3159）而非 fd 或路径：同一个 .ko 文件的两个 fd（不同 `dup`/重开）去重成功；不同文件（即使内容相同）不去重——inode 是"同一份磁盘数据"的内核身份。

### 2.2 两条路径（idempotent_init_module :3151-3169）

```
finit_module(fd)
  └─ idempotent_init_module(f.file, ...)
       ├─ idempotent(&idem, file_inode(f)) 命中？
       │    ├─ 是（waiter）：wait_for_completion(&idem.complete)  /* :3160
       │    │               ★v6.6 为不可中断等待（"make waiting
       │    │               interruptible" 改进 commit 2124d84db293
       │    │               尚未落入 6.6）*/
       │    │               return idem.ret;      /* 与首加载者共享结果 */
       │    └─ 否（first）：idempotent_complete(&idem,
       │                        init_module_from_file(f, uargs, flags))
       │                        /* → kernel_read_file READING_MODULE
       │                           → 十四段流水线（[01 章]） */
```

### 2.3 唤醒拓扑：per-waiter completion（idempotent_complete :3103-3149）

```c
/* 注释原话（:3095-3100）："We were the first one with 'cookie' on
   the list...We now need to walk the list, remove everybody - which
   includes ourselves - fill in the return value, and then complete." */
spin_lock(&idem_lock);
hlist_for_each_entry_safe(pos, next, head, entry) {
	if (pos->cookie != cookie) continue;
	hlist_del(&pos->entry);
	pos->ret = ret;
	complete(&pos->complete);       /* ★逐项 complete——每个 waiter
	                                   自带一份 completion */
}
spin_unlock(&idem_lock);
```

**栈上对象 + 全局链**的生命周期：waiter 的 `struct idempotent` 在栈上分配却挂进全局哈希——它的栈帧被 `wait_for_completion` 钉住直到被 complete，天然防悬挂；首加载者的 idem 同样在链上（complete 自己一次，无害）。与 kmod_dup 的**单对象 complete_all**（§三）是两种唤醒拓扑的对照。

## 三、kmod_dup 层：request_module 的名字锚点（dups.c，248 行，DEBUG 门控）

### 3.1 结构与 60 秒延迟删除

```c
struct kmod_dup_req {               /* :48-55 堆分配，单对象 */
	struct list_head list;
	char name[MODULE_NAME_LEN];     /* 锚点：模块名 */
	struct completion first_req_done;
	struct work_struct complete_work;
	struct delayed_work delete_work;  /* ★60s 后自删 */
	int dup_ret;
};
```

删除的启发式注释（:77-89 原话节选）：*"The typical situation is a module successfully loaded...There is still a small chance right after we delete this entry new request_module() calls may happen...These heuristics are to protect finit_module() abuse for auto-loading"*——条目留 60 秒（`queue_delayed_work(system_wq, ..., 60*HZ)` :117）吸收**迟到风暴**，过期自删防无限累积（"just a boot optimization"）。

### 3.2 nowait 语义：不能等的不能当锚点（exists_wait :120-220）

```
查找（by name）未命中时：
  wait=false（request_module_nowait 的源头）→ 放弃登记并返回 false
  （:142-158 注释原话："we cannot wait for it and share its return
   value with other users which may have used request_module()"）
  wait=true → 登记（list_add_rcu :165），自己去做 modprobe
命中时：
  ★nowait 撞上已有锚点（:194-198）→ *dup_ret = 0 直接成功返回
  wait 型（:209-219）→ wait_for_completion_state(...KILLABLE) 等结果
```

### 3.3 announce：为什么要绕一跳 work（:222-248）

```c
void kmod_dup_request_announce(char *module_name, int ret)
{
	...kmod_req->dup_ret = ret;
	/* 注释原话（:234-243）："If we complete() here we may allow
	   duplicate threads to continue before the first one that
	   submitted the request...queue up the completion and let
	   duplicates suffer, just wait a tad bit longer. There is no
	   rush." */
	queue_work(system_wq, &kmod_req->complete_work);
}
```

★"let duplicates suffer"——**故意**让重复者比首加载者晚一个 workqueue 调度：保证首加载者先完成收尾（例如模块 state 翻转），重复者醒来时看到的世界已一致。complete_work 里 `complete_all`（每 waiter 共享这一个 completion）+ 顺手排 60s 删除。

## 四、两层对照

| 维度 | idempotent（finit_module） | kmod_dup（request_module） |
| :--- | :--- | :--- |
| 编译 | 无条件 | `MODULE_DEBUG_AUTOLOAD_DUPS`（internal.h:237-249 的 ifdef+stub） |
| 锚点 | `file_inode`（同一份文件） | 模块名（同一个名字） |
| 对象 | **栈上** per-caller | **堆上** 单对象 |
| 唤醒 | 逐项 `complete()`（per-waiter completion） | `complete_all`（共享 completion） |
| 等待 | 不可中断（v6.6） | KILLABLE |
| 覆盖 | insmod/finit_module 并发 | modprobe/自动加载并发 |
| 条目清理 | 完成即删（complete 路径） | 60s 延迟自删（吸收迟到者） |
| 观测 | 无专用开关（行为透明） | `enable_dups_trace`（dups.c:39-40）+ TRACE 变体用 WARN_ON 打全栈 |

**串联**：`request_module`（kmod.c:168 查 kmod_dup）→ modprobe → `finit_module`（main.c:3159 查 idempotent）——两层接力：名字层拦住"同一名字的 modprobe 风暴"，inode 层拦住"modprobe 已经放行但并发 insmod 同一文件"的漏网。

## 五、老 init_module 为何豁免

`SYSCALL_DEFINE3(init_module)`（blob 从用户内存拷贝，[05 章]的对照入口）**不走 idempotent**（已核对 v6.6）：blob 没有 inode 可当 cookie，且两次调用传的缓冲可能内容各异——"同一文件"语义不存在。现代加载器（kmod/tools 全家）一律走 finit_module。

## 六、观测实战

```bash
# 1) 模块加载失败统计（CONFIG_MODULE_STATS，debugfs）
cat /sys/kernel/debug/modules/stats          # 汇总
cat /sys/kernel/debug/modules/failed_kreads  # 读文件失败计数
                                              # （init_module_from_file
                                              #   main.c:3131 mod_stat_inc）

# 2) 重复自动加载的调试（DEBUG 门控 + trace）
echo 1 > /sys/module/module/parameters/enable_dups_trace
#   CONFIG_MODULE_DEBUG_AUTOLOAD_DUPS_TRACE=y 时重复即 WARN_ON 全栈
dmesg | grep -A 20 'request_module.*duplicate\|New request_module'

# 3) 复现两层差异
#   同一 .ko 并发 insmod 两次 → idempotent 层静默去重（第二个
#     与第一个返回值相同，且不再触碰 kernel_read_file/LSM 门）
#   并发 request_module 同名（如冷启动 udev 风暴）→ 需 DEBUG
#     门控的 kmod_dup 才可见（生产内核里两个 modprobe 都会跑）

# 4) tracepoint
perf record -e module:module_request -a sleep 5   # 每次请求的现场
```

## 七、思考题

1. cookie 用 `file_inode` 而非 fd——两个进程各自 `open()` 同一 .ko 后并发 `finit_module` 会去重吗？硬链接到同一 inode 的"不同路径"呢？（提示：inode 的定义；跨 mount 的 bind 同文件？）
2. idempotent 的 waiter 也入链（而非只记计数）——第三个并发者到来时它等谁？若用"计数+单一 completion"会丢失什么？（提示：per-waiter ret 的传递语义。）
3. kmod_dup 的 nowait 首请求不做锚点（:153-158）——为什么"不能等的请求不能被等待"是同一个理由的两面？（提示：nowait 调用者不等待返回；若后来者以它为锚，等到的返回值是什么？）
4. "let duplicates suffer" 的 queue_work 一跳防的是什么竞态？如果直接 complete_all，重复者可能看到什么不一致？（提示：首加载者 announce 之后的剩余收尾；模块 state 与 /sys/module 的可见性。）
5. 60 秒延迟删除是"吸收迟到风暴"与"内存驻留"的折衷——什么工作负载会把 60 秒选错？（提示：NFS 根 + udev 冷启动的时长分布；为何不做成 sysctl？）
6. idempotent 的哈希只有 256 桶且 cookie 是指针——哈希碰撞时正确性靠什么保证？这为什么比"名字哈希"更适合 inode 键？（提示：hlist 逐项比较 cookie；指针唯一性 vs 名字可伪造。）

---

> 回看：[05-kernel_read_file](05-kernel_read_file加载基础设施.md)（finit_module 的读入链）｜[06-usermodehelper](06-usermodehelper机制.md)（request_module 的 modprobe 链）｜[01-模块布局与加载器](01-模块布局与加载器.md)（十四段流水线）｜[trace/13-内核代码生成惯用法](../../trace/docs/13-内核代码生成惯用法.md)（"一份清单多种产物"的另一面：一份操作多种结果）
