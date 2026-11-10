# seqlock（Linux 6.6）

> 阅读目标：掌握 seqlock 的"乐观读"模型与适用边界；吃透 6.6 的 seqcount 家族（`seqcount_t`/`seqcount_spinlock_t` 等关联锁变体）与 `_Generic` 泛型机制；理解 latch 双缓冲变体、PREEMPT_RT 下的正确性改造，以及 jiffies/timekeeping 中的实战用法。

---

## 一、模型：读侧零开销的乐观锁

读写问题的第三条路（前两条：rwsem 排队、RCU 延迟回收）：

```
写者：进入临界区前 seq++（变奇数）；退出时 seq++（变偶数）
读者：seq1 = 读 seq；……读数据……；seq2 = 读 seq
      若 seq1 == seq2 且为偶数 → 读到一致快照，成功
      若 seq1 != seq2 或 seq1 为奇数 → 写者介入过，重试
```

```
seq: 0 → 1（写开始）→ 数据改写 → 2（写结束）→ 3 → … → 2n
            └── 读者若在 [1,2) 区间读过 → 检测到奇数/序列变化 → 放弃重读
```

本质是**把同步成本转嫁给失败者（读者重试）与写者（两步序列变更）**，读侧只需两次普通读——连 cache line 独占都不需要（共享读），扩展性天花板级。代价：

1. **读者可能活锁**（starvation）：写者持续写 → 读者永远失败。因此 seqlock 只适合**写频率低**的场合。
2. **读侧数据必须能原子快照**：多字段的"撕裂读"能被检测，但读取期间数据不一致是常态——**读出的值必须整体校验后才能用**。
3. **写者必须串行**：多个写者并发进入会破坏序列语义 → 需要另配写者互斥锁（这就是"关联锁"家族的由来）。

典型用户：`jiffies`（`jiffies_seq`? 实际 jiffies_64 用自己的顺序计数）、`get_jiffies_64`、`timekeeper` 时钟数据（`tk_core.seq`，struct timekeeper 的多字段快照）、路由 `fn_hash`（历史）、`dentry` 的 `d_seq`（lockless lookup）、`/proc` 统计快照、时钟源 `clocksource` watchdog。

---

## 二、API 家族（include/linux/seqlock.h）

### 2.1 基础五件套

```c
/* 初始化 */
DEFINE_SEQLOCK(sl)                 /* seqlock_t = seqcount + spinlock 一体 */
seqlock_init(&sl)

/* 写侧（内部先拿 sl->lock 再动 seqcount）*/
write_seqlock(&sl)   → spin_lock + write_seqcount_begin
write_sequnlock(&sl) → write_seqcount_end + spin_unlock
write_seqcount_begin/sc)/invalidate...

/* 读侧（两种姿势）*/
seq = read_seqbegin(&sl);          /* 或 read_seqcount_begin */
do {
    seq = read_seqbegin(&sl);
    ... 读共享数据 ...
} while (read_seqretry(&sl, seq)); /* 检测到写者 → 重来 */

/* 不加锁的纯 seqcount（写者自行互斥时）*/
write_seqcount_begin(&sc) / write_seqcount_end(&sc)
read_seqcount_begin(&sc) / read_seqcount_retry(&sc, start)
```

### 2.2 6.6 的家族化改造：seqcount_LOCKNAME_t（5.13 引入）

裸 `seqcount_t` 的写侧**不关抢占**（见第三节陷阱），历史上靠使用者自觉。5.13 起按"写者用什么互斥"派生专用类型：

```c
typedef struct seqcount {
	unsigned		seq;
	bool			preemptible;
#ifdef CONFIG_LOCKDEP
	struct lockdep_map	lock;
#endif
} seqcount_t;

/* 由宏批量生成（include/linux/seqlock.h 的 SEQCNT_LOCKNAME 宏）： */
typedef struct seqcount_spinlock_t { seqcount_t seqcount; spinlock_t *lock; } seqcount_spinlock_t;
typedef struct seqcount_rwlock_t   { seqcount_t seqcount; rwlock_t   *lock; } seqcount_rwlock_t;
typedef struct seqcount_mutex_t    { seqcount_t seqcount; struct mutex *lock; } seqcount_mutex_t;
typedef struct seqcount_ww_mutex_t { … struct ww_mutex *lock; } …;
/* 以及"写侧就是本 CPU 独占"的： */
seqcount_percpu_t   （per-CPU 数据，写侧持有本 CPU 即互斥）
seqcount_latch_t    （双缓冲，见第四节）

/* __seqprop() 用 C11 _Generic 按类型选择属性实现：
   ptr / sequence / preemptible / assert —— 同一个 read_seqcount_begin
   对不同变体自动选择"是否需要关抢占"的路径 */
#define __seqprop(s, prop) _Generic(*(s), …)
```

选型速记：**写者持什么锁写，就用什么关联的 seqcount 变体**——变体保证 begin/end 时自动 preempt_disable（若关联锁本身不能阻止与写者同 CPU 的抢占）。

---

## 三、正确性深潜：三个经典陷阱

### 3.1 抢占/中断与"读者在写者 CPU 上被换入"

`write_seqcount_begin` 只做 `seq++`，**不关抢占**。若读者与写者在同核交错（写者 begin 后被抢占，读者在 seq 为奇数时读到一半——能检测，OK；真正致命的是反例）：

```
写者 W：preempt_disable 场景缺失 → seq=1 后被抢占
低优先级读者 R 在同 CPU 读到 seq=1（奇数）→ 重试 OK
   ——奇数检测兜住了这个场景。
真正的杀手是 LOCKDEP 视角的"写者被读者打断导致写者饥饿"与
seqcount 顺序语义被破坏的情形：PREEMPT_RT 下写者持锁睡眠 →
读者检测逻辑仍正确，但 livelock 概率升高 → RT 要求 preemptible=false 的
变体退化为关抢占版本（seqprop_preemptible 选择路径）。
```

### 3.2 内存序：为什么 begin/end 是全屏障

```c
write_seqcount_begin:  s->seq++; smp_wmb();    /* 先发布序号，再改数据 */
write_seqcount_end:    smp_wmb(); s->seq++;    /* 先改完数据，再发布序号 */
read_seqcount_begin:   循环 smp_rmb() 直到偶数  /* 读序号 → rmb → 读数据 */
```

保证：读者若看到新序号，必然看到新数据；看到旧数据则必然对应旧序号。弱序架构（arm64）上这些屏障一个都不能省——这也是 `read_seqcount_begin` 相比"裸读"的真实开销来源（两次 rmb + 两次序号读）。

### 3.3 `write_seqcount_invalidate`：写者"从未来过"

某些场景（RCU 保护的替换）写者不想让读者重试，只想让旧快照**确定性地失败**：

```c
write_seqcount_invalidate(s): s->seq += 2 且保证奇偶语义失效 → 读者必 retry
```

用于 dentry 的 `__d_lookup` 路径（锁无关路径查找，见 `../filesystems/` 分册 dentry 部分）。

---

## 四、latch 变体：双缓冲的终极形态

`seqcount_latch_t`（`raw_write_seqcount_latch`）服务"读多写极少、数据体积大、读者讨厌重试"的场景：

```
struct { seqcount_latch_t seq; 数据结构 body[2]; } x;

读者：
    seq = read_seqcount_latch(&x.seq);      /* 0 或 1 */
    读 x.body[seq & 1];                      /* 选一个缓冲读 */
    if (read_seqcount_latch_retry(...)) retry;

写者（持写锁，两阶段提交）：
    raw_write_seqcount_latch(&x.seq);        /* bit0 翻转：读者改读另一份 */
    把新数据写进"不再被读"的那份 body[]
    raw_write_seqcount_latch(&x.seq);        /* 再翻转回来/序号+2 */
```

读者永远读**完整的旧或新**（无中间态、几乎无重试），代价是双倍内存。经典用户：timekeeping 的 NMI 安全时钟（`timekeeper_data` 双份，nmi 读时钟不能重试——必须一次成功！）、`module` 的每个 CPU 数据、`printk` 的某些 ringbuffer 状态。图示（时间自左向右）：

```
seq bit0:      0 ──────── 1 ───────── 0
reader 选择:   copy0      copy1        copy0
writer 动作:   │ 翻转→写copy1 │ 翻转→写copy0
               （写的是读者"刚离开/尚未进入"的那份）
```

> 💡 面试高频：为什么 NMI 时钟读取必须 latch 而不能普通 seqcount 重试？NMI 里**不能重试**——重试条件可能在 NMI 处理期间永远不满足（打断的正是写者），死循环在 NMI 上下文 = 系统挂死。

---

## 五、与相邻机制对比

| | seqlock | rwsem | RCU |
| :--- | :--- | :--- | :--- |
| 读开销 | 2 读 + 2 rmb（可失败重试） | 原子加（可能排队） | 几乎为零（读屏障即可） |
| 写开销 | 2 次原子写 + 写者互斥 | 独占原子 | 发布 + 等 grace period |
| 读者饥饿 | 可能（写频繁时） | 写者饥饿需公平性方案 | 读者永不饿 |
| 读取一致性 | 最终整体校验 | 阻塞式强一致 | 版本一致（指针类） |
| 典型对象 | 小型 POD 结构/统计 | 大临界区/可睡眠 | 指针、链表、hash |

选型口诀：**读侧可重试 → seqlock；读侧要精确但可阻塞 → rwsem；读侧要极速且数据可版本化替换 → RCU。** 三者常嵌套使用（RCU 保护指针 + seqlock 保护结构体内容，如 timekeeper）。

---

## 六、观测与调试

* `CONFIG_DEBUG_LOCK_ALLOC`：lockdep 对 `seqcount_*_t` 写侧的嵌套/顺序同样有校验（`seqcount_init` 挂 map）。
* `seqcount_t` 的奇数卡死（写者 begin 后崩溃）会让所有读者永久重试——`CONFIG_DEBUG_SEQCOUNT`? 不存在，靠 lockdep + ftrace `write_seqcount_begin` 的 function tracer 找 begin/end 失衡者。
* benchmark 时注意：读侧延迟分布有重试长尾（`bpftrace` 对 `read_seqcount_retry` 的 kprobe 计数即可量化重试率）。

---

## 七、思考题

1. `write_seqcount_begin` 的 `seq++` 必须在数据写**前**可见，`end` 的 `seq++` 必须在数据写**后**可见。若实现里去掉 begin 的 `smp_wmb`，在 x86（TSO）上会错吗？在 arm64 上构造一个具体错误序列。
2. `jiffies_64` 在 32 位平台用 `seqcount`（`read_jiffies_64`? 见 kernel/time/jiffies.c）的原因是什么？64 位平台还需要吗？（对照 `get_jiffies_64` 的编译期分派。）
3. 为什么 seqlock 的读者**不能**同时是写者（同一线程读-改-写）？哪种机制允许这种模式？
4. per-CPU 的 seqcount_percpu_t 为什么不需要关抢占？写者持本 CPU 运行就是互斥的前提是什么（提示：preempt_disable 由谁保证）？
5. 设计：为"系统 uptime 计数器（每 CPU 写自己的，全局读者聚合所有 CPU）"在 per-cpu counter 与 seqcount 之间做选型，并写读侧伪代码。

---

> 相关：[spinlocks.md](spinlocks.md)（关联锁的最常见选择）、[mutex.md](mutex.md)、[rwsem.md](rwsem.md)、RCU 分册（[../RCU/](../RCU/README.md)）。
