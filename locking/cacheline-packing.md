# cacheline 打包：热数据的排布工程（Linux 6.6）

> 阅读目标：吃透内核在**缓存行粒度**上做布局工程的完整打法——基础设施（cache.h 宏族：`____cacheline_aligned` 三兄弟/`__read_mostly`/`__ro_after_init`/`CACHELINE_PADDING` 的零长成员实现/`INTERNODE_CACHE_SHIFT` 跨节点档）；五个案例（**pcpu_hot** 的 `u8 pad[64]` + `static_assert(sizeof==64)`——x86 最热的 per-CPU 快照压进恰好一行；**lockref** 的 4+4 字节打包进 `aligned_u64` 供 cmpxchg64——锁与计数器的原子联合体；**zone 四段式** 三处 `CACHELINE_PADDING`；**struct file** 的★双原则注释原文；sched_clock 的 seq+data[0] 同行）；一个反案例（super_block 的 "There is no need to put them into separate cachelines"——难得的**不**分隔论证）。全部对照 v6.6 当轮核对。前置：[latch-vs-rcu](latch-vs-rcu.md)（sched_clock 案例）｜[concession](concession.md)（争用思维）。

---

## 一、问题：同一行缓存的两个方向

```
真共享（要）：同一次访问需要的字段挤同一行——一次 load 全拿到
伪共享（防）：不同 CPU 写不同字段却落在同一行——缓存行乒乓

★file 注释（fs.h:986-991）一句话讲透双原则：
 "f_{lock,count,pos_lock} members can be highly contended and share
  the same cacheline. f_{lock,mode} are very frequently used together
  and so share the same cacheline as well. The read-mostly
  f_{path,inode,op} are kept on a separate cacheline."

——争用组内部"合"（一起用的挤一行）、争用组之间"分"（写热 vs 读多分行）
```

## 二、基础设施（include/linux/cache.h）

| 宏 | 定义 | 用途 |
| :--- | :--- | :--- |
| `L1_CACHE_ALIGN(x)` | `__ALIGN_KERNEL(x, L1_CACHE_BYTES)` | 把尺寸向上对齐到行 |
| `SMP_CACHE_BYTES` | `= L1_CACHE_BYTES` | 通用行大小（x86：`L1_CACHE_SHIFT=CONFIG_X86_L1_CACHE_SHIFT`，64 字节，asm/cache.h:8-9） |
| `____cacheline_aligned` | `__aligned__(SMP_CACHE_BYTES)` | 成员/结构体对齐 |
| `__cacheline_aligned` | 对齐 **+ 专用段** `__section(".data..cacheline_aligned")` | 全局变量——对齐且聚段 |
| `____cacheline_internodealigned_in_smp` | `__aligned__(1 << INTERNODE_CACHE_SHIFT)` | **跨 NUMA 节点**争用的更大间隔 |
| `__read_mostly` | x86 侧 `__section(".data..read_mostly")`（asm/cache.h:11） | 读多写少数据聚段 |
| `__ro_after_init` | `__section(".data..ro_after_init")` | init 后只读（+CR0.WP 保护） |
| `CACHELINE_PADDING(name)` | `struct cacheline_padding name`，而 `struct cacheline_padding { char x[0]; } ____cacheline_internodealigned_in_smp;`（cache.h:91-97） | ★零长成员 + 对齐 = 纯填充（SMP 限定，UP 下为空） |

★`CACHELINE_PADDING` 的巧思：**零长数组本身不占字节**，但带 `internodealigned` 属性——编译器把下一个成员推到行边界。结构体不膨胀、没有"幽灵字节"，只是对齐。

## 三、案例五件（外加一个反例）

### 1. pcpu_hot：x86 最热的 per-CPU 快照（arch/x86/include/asm/current.h:14-37）

```c
struct pcpu_hot {
	union {
		struct {
			struct task_struct	*current_task;
			int			preempt_count;
			int			cpu_number;
#ifdef CONFIG_CALL_DEPTH_TRACKING
			u64			call_depth;   /* trace/22 章的调用深度计数器就住这里 */
#endif
			unsigned long		top_of_stack;
			void		*hardirq_stack_ptr;
			u16			softirq_pending;
			bool			hardirq_stack_inuse;
		};
		u8	pad[64];        /* 手工填满 */
	};
};
static_assert(sizeof(struct pcpu_hot) == 64);   /* ★编译期锁定"恰好一行" */
DECLARE_PER_CPU_ALIGNED(struct pcpu_hot, pcpu_hot);
```

`current` 即 `this_cpu_read_stable(pcpu_hot.current_task)`——**当前任务指针、抢占计数、栈顶**等极热字段一次性放进 64 字节，一次缓存行加载全拿到，且静态断言防回归。

### 2. lockref：锁与引用计数器的原子联合体（include/linux/lockref.h）

```c
#define USE_CMPXCHG_LOCKREF \
	(IS_ENABLED(CONFIG_ARCH_USE_CMPXCHG_LOCKREF) && \
	 IS_ENABLED(CONFIG_SMP) && SPINLOCK_SIZE <= 4)

struct lockref {
	union {
#if USE_CMPXCHG_LOCKREF
		aligned_u64 lock_count;   /* ★4 字节 spinlock + 4 字节 count
	#endif	                            打包进一个对齐的 8 字节 */
		struct {
			spinlock_t lock;
			int count;
		};
	};
};
```

头注释原文（lockref.h:7-15）：*"These are different from just plain atomic refcounts in that they are atomic **with respect to the spinlock that goes with them**...using a cmpxchg operation that **covers both the lock and the count word**"*——`lockref_get` 的快路径就是一次 `cmpxchg64` 同时比对锁字与计数字。消费者：dentry 的 `d_lockref`（dcache.h 的 struct dentry 内嵌）——目录项的锁与引用计数天然同热区。`USE_CMPXCHG_LOCKREF` 三条件（ARCH_USE_CMPXCHG_LOCKREF && SMP && SPINLOCK_SIZE<=4）失效时退回真拿锁的慢实现——**打包是能力，不是义务**。

### 3. zone 四段式：写热分区三处隔离（include/linux/mmzone.h）

```
struct zone {
	/* Read-mostly fields */            ← 水位/nodata 等只读区
	CACHELINE_PADDING(_pad1_);          ← mmzone.h:925
	/* Write-intensive fields used from the page allocator */
	  free_area[] / flags / lock        ← 分配器路径写热区
	CACHELINE_PADDING(_pad2_);          ← :942
	/* Write-intensive fields used by compaction and vmstats. */
	  per_zone_reclaim_stat 族          ← 回收/整理路径写热区
	CACHELINE_PADDING(_pad3_);          ← :979
	/* Zone statistics */
	  vm_stat[] / vm_numa_event[]       ← 统计区（高频原子加）
} ____cacheline_internodealigned_in_smp; ← ★结构体本身也按跨节点对齐
```

分区注释原文就是分界线：**"Write-intensive fields used from the page allocator"** / **"Write-intensive fields used by compaction and vmstats"**——按"谁在写"切区，两类写手互不乒乓。

### 4. struct file：双原则的教科书（include/linux/fs.h:986-991）

见 §一 的注释原文。落地细节：`f_lock`/`f_count`/`f_pos_lock` 同行（都争用，合）；`f_path`/`f_inode`/`f_op` 另一行（读多，分）——**字段重排本身就是锁粒度工程**。

### 5. sched_clock 的 clock_data：seq+data[0] 同行

`kernel/time/sched_clock.c:34-36` 头注释："The ordering of this structure has been chosen to optimize cache performance. In particular 'seq' and 'read_data[0]' (combined) should fit into a single 64-byte cache line."——读者的两次 load（seq + 数据）一次行加载完成。详见 [latch-vs-rcu 章](latch-vs-rcu.md) §四。

### 6. 反案例：super_block 的"不分隔"论证（fs.h:1297-1302）

```
 * The list_lru structure is essentially just a pointer to a table
 * of per-node lru lists, each of which has its own spinlock.
 * There is no need to put them into separate cachelines.
 struct list_lru s_dentry_lru;
 struct list_lru s_inode_lru;
```

★难得的**反**padding 论证：两个 list_lru 的实际锁在 per-node 表深处（结构体本体只是个指针）——分隔 s_dentry_lru/s_inode_lru 不会减少任何真争用。**先看争用点在哪，再决定动不动布局**。

## 四、为什么是 64，以及跨节点档

- x86：`L1_CACHE_SHIFT = CONFIG_X86_L1_CACHE_SHIFT`（asm/cache.h:8-9）→ `L1_CACHE_BYTES = 1<<6 = 64`
- 跨 NUMA：`INTERNODE_CACHE_SHIFT = CONFIG_X86_INTERNODE_CACHE_SHIFT`（:13）——跨节点缓存一致性粒度更大，`____cacheline_internodealigned_in_smp` 按它对齐
- 用户实例：`struct rcu_node` 的 `fqslock`/`exp_lock`（kernel/rcu/tree.h:129/:131）——rcu_node 每节点一枚，force-quiescent 与 expedited 两把锁必须跨节点分线

## 五、观测实战

```bash
# 1) 布局核对——pahole（dwarves 包）逐行展示结构体成员落在哪一行
pahole -C pcpu_hot vmlinux     # 应见一行装下全部成员（static_assert 已保险）
pahole -C lockref vmlinux      # union 双视图
pahole -C zone vmlinux         # 观察 __padding 字段把四段分开

# 2) 伪共享侦探——perf c2c（True/False Sharing 两大类命中率）
perf c2c record -a -- sleep 10
perf c2c report                # HITM 矩阵：谁在跨核/跨节点打同一行

# 3) 常规计数
perf stat -e cache-misses,LLC-load-misses ./workload

# 4) 编译期保险
#   pcpu_hot 的 static_assert(sizeof == 64)——改字段先撞断言
```

## 六、思考题

1. `CACHELINE_PADDING` 用零长数组 + 对齐属性而非 `char pad[64]`——省下的 64 字节去哪了？（提示：对齐属性本身把后续成员推到行边界——`x[0]` 只是属性载体；如果真放 64 字节数组，行对齐时会多浪费一段。）
2. pcpu_hot 用 `u8 pad[64]` 手工填满并 static_assert——与 CACHELINE_PADDING 的"零浪费"路线为何不同？（提示：pcpu_hot 的目标不是"推开别人"而是**恰好占满一行**——per-CPU 段内多个变量共享行，占满才能独占；断言防的是字段悄悄长大溢出行。）
3. lockref 的 cmpxchg64 快路径与"真拿锁"慢路径语义等价性靠什么保证？（提示：头注释 "the operation is done semantically as if the spinlock had been taken"——cmpxchg 失败即视为锁被人持有，退慢路径；等价性在**语义层**而非实现层。）
4. zone 的 vm_stat 为何被隔在 _pad3_ 之后而不是与 free_area 同区？（提示：vm_stat 的写者是"每次页面事件"的原子加——频率远超分配器本身；与 free_area 同区会把分配器路径也拖进统计乒乓。）
5. super_block 反案例的判断依据（"essentially just a pointer to a table"）如何推广成一个 checklist？（提示：分隔的对象应是**争用点本体**而非持有句柄的结构——先追到真正被写的地址，再决定排布。）
6. `__read_mostly`/`__ro_after_init` 与 cacheline 对齐是两个正交维度——聚段解决什么而（对）齐解决什么？（提示：聚段把同类访问模式的数据并置（TLB/预取友好+避免与写热数据同段页）；对齐解决边界跨越与乒乓——前者优化"段"，后者优化"行"。）

---

> 回看：[latch-vs-rcu](latch-vs-rcu.md)（sched_clock 案例＋读者零开销选型）｜[concession](concession.md)（争用让步思维——排布是争用工程的静态半）｜[kcsan-encoding](kcsan-encoding.md)（另一个"把状态压进一个字"的极致案例）｜[trace/22-CALL_DEPTH_ACCOUNT](../trace/docs/22-CALL_DEPTH_ACCOUNT.md)（住在 pcpu_hot 里的计数器）｜[locking/README](README.md)（锁族谱）