# PREEMPT_RT 替换矩阵：全景图（Linux 6.6）

> 阅读目标：一张完整矩阵看清 PREEMPT_RT 对锁子系统的**全部替换**——哪些锁的**结构体**被换成 rt 底座（spinlock/mutex/rwsem/ww_mutex）、哪些**保留 raw**（raw_spinlock 在中断上下文仍不可睡眠）、`local_lock` 的 RT 变身（preempt_disable→migrate_disable+spin_lock）、以及 **spinlock_rt 的语义变化**（可睡眠+RCU 读锁替代禁抢占——头注释原话）。全部对照 v6.6 的条件编译。前置：[ww-rt-mutex](ww-rt-mutex.md)｜[mutex](mutex.md)｜[rwsem](rwsem.md)。

---

## 一、全景替换矩阵

```
                    非 PREEMPT_RT              PREEMPT_RT
                    ─────────────              ──────────
spinlock_t           struct raw_spinlock        struct rt_mutex_base  ★可睡眠
raw_spinlock_t       arch_spinlock_t            arch_spinlock_t      ★不变（真原子）
mutex                struct mutex               struct rt_mutex_base
rw_semaphore         struct rw_semaphore        struct rwbase_rt
ww_mutex             struct mutex base          struct rt_mutex base
local_lock_t         （空——只有 preempt_disable） spinlock_t（per-CPU）
seqcount             seqcount_t                 seqcount_t           ★不变
seqcount_latch       seqcount_latch_t           seqcount_latch_t     ★不变
完成/RW 锁           completion/rw_lock         rt_mutex 包装
```

## 二、逐项展开

### 2.1 spinlock（include/linux/spinlock_types.h:14-72）

```c
#ifndef CONFIG_PREEMPT_RT
/* 非 RT：spinlock 就是 raw_spinlock 的包装 */
typedef struct spinlock {
	union { struct raw_spinlock rlock; ... };
} spinlock_t;

#else /* !CONFIG_PREEMPT_RT */
/* RT：spinlock 变成 rt_mutex */
#include <linux/rtmutex.h>
typedef struct spinlock {
	struct rt_mutex_base	lock;      /* ★可睡眠的 PI 锁 */
	...
} spinlock_t;
```

★**语义巨变**：非 RT 的 `spin_lock` 禁抢占 + 忙等；RT 的 `spin_lock` **可睡眠** + PI 排队。中断上下文不能再碰它——必须用 `raw_spin_lock`。

### 2.2 mutex（include/linux/mutex.h:130-172）

```c
#else /* !CONFIG_PREEMPT_RT */
/* "Preempt-RT variant based on rtmutexes." 注释原话 */
struct mutex {
	struct rt_mutex_base	rtmutex;    /* PI 能力的 mutex */
	...
};
```

RT 下 mutex 本来就可睡眠——语义最接近，只是底座换成 PI 版。

### 2.3 rwsem（include/linux/rwsem.h:125-165）

```c
#else /* !CONFIG_PREEMPT_RT */
#include <linux/rwbase_rt.h>
struct rw_semaphore {
	struct rwbase_rt	rwbase;     /* rwbase_rt 的读者计数+rt 底座 */
	...
};
```

### 2.4 ww_mutex（include/linux/ww_mutex.h:28-36/:353）

[ww-rt-mutex](ww-rt-mutex.md) 章已详述——`WW_MUTEX_BASE` 条件编译。

### 2.5 local_lock（include/linux/local_lock_internal.h:9-130）

```c
/* 非 RT 版（:9-67）：
   local_lock_t 本体为空——只有 lockdep 检查 + preempt_disable */
typedef struct { ... } local_lock_t;     /* 仅 debug 字段 */

/* RT 版（:100-130）——头注释原话：
   "On PREEMPT_RT local_lock maps to a per CPU spinlock,
    which protects the critical section while staying preemptible." */
typedef spinlock_t local_lock_t;

#define __local_lock(__lock)                   \
	do {                                   \
		migrate_disable();             /* ★禁迁移而非禁抢占 */ \
		spin_lock(this_cpu_ptr((__lock))); /* ★per-CPU 真锁 */ \
	} while (0)
```

★非 RT 的 `local_lock` 只是 `preempt_disable` 的**类型化包装**（防止"我在什么保护下"忘掉）；RT 下变成**真正的 per-CPU spinlock** + `migrate_disable`——保持 preemptible 但不迁移。

### 2.6 raw_spinlock（不变）

```c
/* include/linux/spinlock_types_raw.h */
typedef struct raw_spinlock {
	arch_spinlock_t raw_lock;    /* ★真原子（qspinlock）——RT 不动它 */
	...
} raw_spinlock_t;
```

**raw 是逃生口**：中断/NMI 上下文、硬件操作、极短临界区——这些**必须**用 `raw_spin_lock`（RT 下仍然禁抢占 + 忙等）。

## 三、spinlock_rt 的语义说明（kernel/locking/spinlock_rt.c）

头注释原话（:1-18 节选）：

```
/*
 * PREEMPT_RT substitution for spin/rw_locks
 *
 * spin/rw_lock() on RT kernels map to a rt_mutex based
 * implementation. ...
 *
 * The RT substitutions explicitly disable migration and
 * take rcu_read_lock() across the lock held section.
 */
```

★**RCU 读锁替代禁抢占**：非 RT 的 spin_lock 隐含禁抢占→RCU 读侧临界区天然受保护；RT 的 spin_lock 可被抢占——必须**显式**加 `rcu_read_lock()` 才能维持 RCU 语义。同理 `migrate_disable` 替代 preempt_disable 的"不会走开"保证。

## 四、不可替换的（保留 raw）

| 场景 | 必须 raw | 原因 |
| :--- | :--- | :--- |
| 硬中断 handler | raw_spin_lock | 中断上下文不可睡眠 |
| NMI handler | raw_spin_lock | NMI 绝对不可睡 |
| `write_seqcount_begin` 内部 | raw_spin_lock | seqcount 写者的关联锁 |
| 极短确定型临界区 | raw_spin_lock | PI 开销不值 |
| 架构特定（如 IDT） | raw_spin_lock | 硬件语义 |

## 五、用户视角的变化

```c
/* 同一段代码在两种构建下的行为差异 */

spin_lock(&lock);           /* 非 RT：禁抢占+忙等（µs）
                               RT：可睡眠+PI（ms 级上限） */
...
spin_unlock(&lock);         /* 非 RT：恢复抢占
                               RT：唤醒高优先级等待者 */

/* 用 raw 则行为不变： */
raw_spin_lock(&rlock);      /* 两种构建：都是禁抢占+忙等 */
...
raw_spin_unlock(&rlock);    /* 两种构建：恢复抢占 */
```

## 六、观测实战

```bash
# 1) 当前构建是否 RT
grep PREEMPT_RT /boot/config-$(uname -r)

# 2) RT 下的 spinlock 行为验证
#   spin_lock 持有期间被抢占（非 RT 下不可能）
cat /sys/kernel/debug/sched/preempt

# 3) raw_spinlock 的使用密度（区分中断安全代码）
grep -r 'raw_spin_lock' kernel/ | wc -l
grep -r 'spin_lock\b' kernel/ | wc -l
#   raw 的数量 = "必须不可睡眠"的临界区数量

# 4) RT 下 local_lock 的真身
#   include/linux/local_lock_internal.h 的 #else 分支
```

## 七、思考题

1. RT 的 spinlock 可睡眠——但 `spin_lock_irqsave` 呢？（提示：RT 下 `irqsave` 版变 `spin_lock` + `migrate_disable`（flags 清零）——"disable irq" 的语义化为"disable migration"。）
2. `local_lock` 在非 RT 下是空壳（只有 preempt_disable）——为什么还要这个类型？（提示：**类型化文档**——"我在什么保护下"成为编译器可检查的接口；RT 时自动升级为真锁。）
3. RT 的 spinlock "take rcu_read_lock() across the lock held section"——如果不加会怎样？（提示：RCU GP 可能误判（认为读者已退出而实际在 spinlock 里被调度走）→过早释放被引用对象。）
4. raw_spinlock 在 RT 下仍禁抢占——为什么不用 rt？（提示：raw 的使用者包括 NMI/硬件——不可承受 PI 的不确定延迟；且 PI 本身需要 raw 锁（自举问题）。）
5. ww_mutex 的 WW_MUTEX_BASE 切换意味着 **DRM 驱动在 RT 上自动获得 PI**——这对 GPU 的实时性意味着什么？（提示：GPU 命令提交的 -EDEADLK 回退等待可被更高优先级任务 boosting。）
6. `spin_lock_irqsave` 的 flags 在 RT 下被清零——`spin_unlock_irqrestore(flags)` 拿到 0 会怎样？（提示：RT 的 irqsave 版在 unlock 时不真正恢复 flags——因为"disable irq"没发生过。）

---

> 回看：[ww-rt-mutex](ww-rt-mutex.md)（ww 的 RT 变体）｜[mutex](mutex.md)（非 RT 基座）｜[rwsem](rwsem.md)（非 RT rwsem）｜[concession](concession.md)（让步家族的 RT 差异）｜[locking/README](README.md)（锁族谱的 RT 全景）
