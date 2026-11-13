# lockdep chains：依赖链、BFS 与 wait-context（Linux 6.6）

> 阅读目标：候选池题 "lockdep cross-release" 的**如实改写**——cross-release 系列（把 lockdep 依赖图扩展到 completion/waitqueue 等"等待"的提案）**从未合入主线**（v6.6 全树 git grep 实质命中仅 1 处，且是抱怨它不存在，见 §一）；主线实有的是三件：①**依赖链缓存**（`chain_key` 逐步折叠 + `struct lock_chain` 快照表 + 命中即跳过全套检测）；②**BFS 循环检测**（`check_noncircular` 从 next 出发搜 prev 是否可达）；③**wait-context 检查**（`LD_WAIT_*` + `task_wait_context`——PREEMPT_RT 的守门员，cross-release 想抓的 bug 大类的子集解法）。容量熔断五连报文与 `/proc/lockdep*` 观测面。全部对照 v6.6 当轮核对。前置：[debug-adversarial](debug-adversarial.md)（lockdep 数据模型基础）｜[preempt-rt-matrix](preempt-rt-matrix.md)。

---

## 一、cross-release 的生死记（考古开篇）

**提案**（2016-2017，Byungchul Park）：把 `lockdep` 的依赖图从"锁 A 持有中拿锁 B"扩展到"锁 A 持有中**等待** W"（completion/swait/waitqueue/dma_fence_wait）——跨"锁"与"释放点"的全量死锁检测。

**结局**：未合入。主线 v6.6 的全部踪迹只有一处——而且是在**抱怨这个缺口**（dma-fence.c:221-228 注释原文）：

```
 * Furthermore lockdep does not handle cross-release dependencies, which means
 * any deadlocks between dma_fence_wait() and dma_fence_signal() can't be caught
 * at runtime with some quick testing. The simplest example is one thread
 * waiting on a &dma_fence while holding a lock::
 *
 *     lock(A);
 *     dma_fence_wait(B);
 *     unlock(A);
```

（反例的另一半在 [dma_fence 章](dma-fence.md)的死锁约束清单：signal(B) 的路径若需 A——环闭。）——dma-fence 作者被迫在头注释里**人工枚举**哪些路径可以调用 signal，因为 runtime 检测器帮不上忙。

**主线的子集解法**：放弃"等待导致的间接死锁"全量检测（误报率与复杂度失控），保留**wait-context**（§四）抓"睡眠锁进原子上下文"这一最痛的大类。章节题名因此从 "cross-release" 改写为链检测 + wait-context。

## 二、依赖链：lockdep 的缓存骨架

每次拿锁都要对新依赖 `<prev>→<next>` 做全套检查（§三 BFS + irq-usage）——太贵。解法：**把"当前持有的锁集合"折叠成 64 位 key**，缓存"这个集合出现过了且无环"。

```
struct held_lock 的 chain_key 字段头注释（include/linux/lockdep.h:87-91 原文）：
  "One-way hash of the dependency chain up to this point. We
   hash the hashes step by step as the dependency chain grows.
   We use it for dependency-caching and we skip detection
   passes and dependency-updates if there is a cache-hit"

折叠函数：iterate_chain_key(key, idx)（lockdep.c:444）——拿锁时
  chain_key = iterate_chain_key(chain_key, hlock_id)（:3560/:3928）
  ——一次哈希把新锁并入 key，O(1) 且免算术
```

**缓存表**：`struct lock_chain`（include/linux/lockdep.h:75-85）——按 chain_key 哈希：

```c
struct lock_chain {
	/* see BUILD_BUG_ON()s in add_chain_cache() */
	unsigned int irq_context :  2,   /* 硬中断/软中断/进程三态分区 */
		     depth       :  6,   /* 链长（最长 63 层——超过即按无缓存处理 */
		     base	    : 24;   /* 在 chain_hlocks[] 里的起点 */
	struct hlist_node entry;         /* 哈希碰撞链 */
	u64 chain_key;
};
```

`add_chain_cache`（lockdep.c:3694，由 validate_chain :3815 调用）把链存进表；`validate_chain` 每次先查 key——**命中则整段检测跳过**（含 BFS 与 usage 标记），这就是 lockdep 日常开销可控的秘密。

**容量与熔断**（写满即永久关机，`debug_locks_off`）：

| 常量 | 值 | 熔断报文（lockdep.c） |
| :--- | :--- | :--- |
| `MAX_LOCKDEP_CHAINS` | 1<<15 = 32768（lockdep_internals.h:98/:120） | "BUG: MAX_LOCKDEP_CHAINS too low!"（:3715） |
| `MAX_LOCKDEP_CHAIN_HLOCKS` | 32768×5（:122） | "BUG: MAX_LOCKDEP_CHAIN_HLOCKS too low!"（:3733） |
| `MAX_LOCKDEP_KEYS` | 1<<13 = 8192（lockdep.h:87-88） | "BUG: MAX_LOCKDEP_KEYS too low!"（:1320） |
| `MAX_LOCKDEP_ENTRIES` | 16384（lockdep_internals.h:96） | "BUG: MAX_LOCKDEP_ENTRIES too low!"（:1396） |
| `MAX_STACK_TRACE_ENTRIES` | 262144（:99） | "BUG: MAX_STACK_TRACE_ENTRIES too low!"（:577） |

熔断时打印 "turning off the locking correctness validator."（:522/:894/:975）——**不可再开**（boot 参数 `debug_locks=0` 是显式关，容量熔断后没有任何再开口）。

## 三、BFS 循环检测：check_prev_add 的核心一步

缓存未命中时，`check_prev_add` 要证明新依赖 `<prev>→<next>` 不成环（lockdep.c:3126-3135 注释原文）：

```
 * Prove that the new <prev> -> <next> dependency would not
 * create a circular dependency in the graph. (We do this by
 * a breadth-first search into the graph starting at <next>,
 * and check whether we can reach <prev>.)
 *
 * The search is limited by the size of the circular queue (i.e.,
 * MAX_CIRCULAR_QUEUE_SIZE) which keeps track of a breadth of nodes
 * in the graph whose neighbours are to be checked.
```

```
check_noncircular(next, prev)（:2161）
  → __bfs(source=next, target=prev)（:1717）
     沿 class 的 after 邻接表广度优先走
  返回码（:1559-1568 注释）：
     BFS_EINVALIDNODE / BFS_EQUEUEFULL   错（前者即 class 已被 zap）
     BFS_RMATCH / BFS_RNOMATCH           果（RMATCH=找到 prev=环！）
```

**为什么 BFS 而非 DFS**：环检测要的是"可达性"而非"路径本身"——BFS 层层推进可被 MAX_CIRCULAR_QUEUE_SIZE 硬性限深（`max_bfs_queue_depth` 统计，:1460），行为可预算。环坐实则走 `print_circular_bug` 报 "circular locking dependency detected"。

**check_prev_add 的两个前置彩蛋**（:3100-3128）：
- class 键为空时 `WARN_ONCE(... "Detected use-after-free of lock class %px/%s")`——注释自述权衡："It is better to trigger a use-after free and to have the class name most of the time instead of not having the class name available."
- **`class->cmp_fn`**（:3123-3128）：同 class 的两个实例若 `cmp_fn(prev->instance, next->instance) < 0` 直接放行——**per 实例序**的钩子（ww-mutex 的加锁序由它进 lockdep 而非靠 class 全序）。

## 四、wait-context：PREEMPT_RT 的守门员

### 4.1 两轴类型（lockdep_types.h:16-37）

```c
enum lockdep_wait_type {            /* 这把锁"等多深"——持有它可能睡眠么 */
	LD_WAIT_INV = 0,   /* not checked, catch all */
	LD_WAIT_FREE,      /* wait free, rcu etc.. */
	LD_WAIT_SPIN,      /* spin loops, raw_spinlock_t etc.. */
#ifdef CONFIG_PROVE_RAW_LOCK_NESTING
	LD_WAIT_CONFIG,    /* preemptible in PREEMPT_RT, spinlock_t etc.. */
#else
	LD_WAIT_CONFIG = LD_WAIT_SPIN,   /* ★非检测构建里折叠——两轴并一轴 */
#endif
	LD_WAIT_SLEEP,     /* sleeping locks, mutex_t etc.. */
};
enum lockdep_type {                 /* 这把锁"是什么"——正交的第二轴 */
	LD_LOCK_NORMAL = 0, LD_LOCK_PERCPU, LD_LOCK_WAIT_OVERRIDE /* annotation */,
};
```

`CONFIG_PROVE_RAW_LOCK_NESTING`（lib/Kconfig.debug:1340-1347）：bool、`depends on PROVE_LOCKING`、**default n**——"Enable the raw_spinlock - spinlock nesting checks which ensure that the lock nesting rules for PREEMPT_RT enabled kernels are not violated"。非 RT 构建（spinlock_t 本是真自旋锁）打开它是为了**提前替 RT 审计**。

### 4.2 上下文侧：task_wait_context（lockdep.c:4698-4720）

```c
static inline short task_wait_context(struct task_struct *curr)
{
	if (lockdep_hardirq_context()) {
		if (curr->hardirq_threaded || curr->irq_config)
			return LD_WAIT_CONFIG;   /* force_irqthreads 会把我们线程化 */
		return LD_WAIT_SPIN;          /* 硬中断里只能自旋等待 */
	} else if (curr->softirq_context) {
		return LD_WAIT_CONFIG;       /* "Softirqs are always threaded." */
	}
	return LD_WAIT_MAX;              /* 进程态：不设限 */
}
```

### 4.3 判定：check_wait_context（:4770-4820）

```
① 取 next 的 wait_type_inner/outer（outer 缺省=inner）
② 沿 held_locks 回溯到当前 irq_context 的起点（:4786-4791）
③ 逐持锁把 curr_inner 收紧到 min(curr_inner, prev_inner)
   ——头注释："we must look for the strictest environment in the
      lock stack and compare that to the lock we're trying to acquire."
   LD_LOCK_WAIT_OVERRIDE 注解可豁免（:4807-4815 注释："typically only
   valid/needed for code that only exists when CONFIG_PREEMPT_RT=n"）
④ next_outer > curr_inner → print_lock_invalid_wait_context
   报文："[ BUG: Invalid wait context ]"（:4735）
```

★语义：**"在只能自旋等的环境里（如硬中断）不许拿要睡觉才能拿到的锁"**——这正是 cross-release 想抓的 bug 大类中最高频的一支（RT 迁移期 90% 的 "sleeping lock in atomic"），主线用这一张小表就吃下了，无需全量等待图。

## 五、观测实战

```bash
# 1) 链表与统计（lockdep_proc.c:717-723）
cat /proc/lockdep            # 全部锁 class（S_IRUSR——root only）
cat /proc/lockdep_chains     # 链缓存条目（chain_key 快照）
cat /proc/lockdep_stats      # 命中/未命中/环检测/熔断计数
cat /proc/lock_stat          # 每锁竞争统计（CONFIG_LOCK_STAT——含等待时长）

# 2) 熔断排查
dmesg | grep -E 'too low|turning off the locking correctness'
#   链爆 → 调 LOCKDEP_CHAINS_BITS；keys 爆 → 清理动态注册的 class

# 3) wait-context 违例
dmesg | grep 'Invalid wait context'
#   RT 预检（非 RT 构建开 PROVE_RAW_LOCK_NESTING 提前替 RT 审计）

# 4) cross-release 缺口的自查（无人代劳——见 §一）
#   dma_fence 侧按 [dma_fence 章] 的路径清单人工审计
```

## 六、思考题

1. chain_key 是"逐步折叠的单向哈希"——为什么不需要可逆？（提示：缓存只需等值比较；链的**成员**另外存在 `chain_hlocks[base..]` 里，key 只当指纹。）
2. `MAX_LOCKDEP_CHAIN_HLOCKS = MAX_LOCKDEP_CHAINS*5`——为什么链存储要留 5 倍余量？（提示：一条链最长存 depth 个 hlock id（:1046 的循环），而 depth 位域 6 bit 上限 63；容量要盖住"平均链长×链数"，5 是历史工作负载的平均数。）
3. BFS 用 MAX_CIRCULAR_QUEUE_SIZE 限深——图比队列大时会不会漏报环？（提示：队列满即 BFS_EQUEUEFULL → `bfs_error` → check_prev_add 返回 0 放行——**宁漏报不误停机**；漏掉的环等缓存里更长链的复查再抓。）
4. `LD_WAIT_CONFIG = LD_WAIT_SPIN` 的折叠为什么发生在 `#else` 而不是 `#ifdef`？（提示：非 RT 构建 spinlock_t 就是 raw_spinlock_t——两者语义相同才折叠；RT 构建里两者不同（preempt-rt-matrix 章），必须分立。）
5. check_wait_context 为什么比较 `next_outer` 而不是 `next_inner`？（提示：outer 是"持有它时要容忍别人等多深"——正是对**当前环境**的诉求；inner 是它内部实现要睡多深，那是下一层 check 的事。）
6. cross-release 的全量方案为什么被拒？wait-context 与它各自覆盖的 bug 集合关系？（提示：wait-context ⊂ cross-release 目标集；后者还要跨"等待点"建模间接死锁（A 等 B、B 持 C、C 等 A），图的规模与误报都失控——工程取舍放弃全集保高频子集。）

---

> 回看：[debug-adversarial](debug-adversarial.md)（lockdep 数据模型与对抗面）｜[preempt-rt-matrix](preempt-rt-matrix.md)（wait-type 的存在理由——RT 替换矩阵）｜[dma-fence](dma-fence.md)（§一 cross-release 抱怨的出处与人工路径清单）｜[ww-mutex](ww-mutex.md)（cmp_fn 的 per 实例序用户）｜[locking/README](README.md)（锁族谱）
