# rwsem（读写信号量，Linux 6.6）

> 阅读目标：拆解 `struct rw_semaphore` 的 count 位编码与 owner 复用；理解写者乐观自旋（含"读者持有时长预测"）、读者批量唤醒（woken list）、写者 handoff 防饿；对比 rwsem 与 rwlock/RCU/seqlock 的选型逻辑。

---

## 一、数据结构与位编码（kernel/locking/rwsem.c，6.6）

```c
struct rw_semaphore {               /* include/linux/rwsem.h */
	atomic_long_t	count;          /* ★状态机主体 */
	atomic_long_t	owner;          /* 写者指针 / 读者标记（乐观自旋用）*/
#ifdef CONFIG_RWSEM_SPIN_ON_OWNER
	struct optimistic_spin_queue osq;   /* 写者自旋排队（同 mutex）*/
#endif
	raw_spinlock_t	wait_lock;
	struct list_head	wait_list;      /* rwsem_waiter 链表（读/写混挂）*/
	...
};
```

`count` 的 64 位布局（注释原文顺序）：

```
bit 0     RWSEM_WRITER_LOCKED   写者持有中
bit 1     RWSEM_FLAG_WAITERS    有等待者（unlock 必须看队列）
bit 2     RWSEM_FLAG_HANDOFF    队首等待者被承诺"下一棒"（写者版防偷锁）
bit 3..7   （保留）
bit 8..62  reader count         持读锁的读者数（RWSEM_READER_BIAS = 1<<8 步进）
bit 63     RWSEM_FLAG_READFAIL  守卫位（读者计数溢出哨兵，理论不可达）
```

两条快路径据此成型：

```
down_read:   atomic_long_fetch_add_acquire(RWSEM_READER_BIAS, &count)
             若 (新值 & READ_FAILED_MASK) == 0 → 拿到（读者只加计数，无队列操作！）
down_write:  atomic_long_cmpxchg_acquire(&count, 0, WRITER_LOCKED)   空→写者
```

**读者快路径只有一次原子加**——这是 rwsem 在读多场景碾压 rwlock（要原子 RMW 竞争同一字）的结构原因。代价是"读者多了以后每次解锁也要一次原子操作减计数"，以及写者可能饿。

`owner` 的两种身份：写者持有 → 指向写者 task_struct（自旋方跟踪 `on_cpu`）；读者持有 → 写第一个读者时写入 `READER_OWNED | RWSEM_NONSPINNABLE?` 的合成值（读者无法逐个追踪，写者自旋面对"一群读者"性价比低，`NONSPINNABLE` 位在发现读者群后置位，抑制后续写者的乐观自旋）。

---

## 二、写者路径：乐观自旋 + 睡眠

### 2.1 乐观自旋（rwsem_optimistic_spin，6.6）

与 mutex 同骨架（osq 排队 → 盯 owner → need_resched/on_cpu 放弃），但增加**读者群的时间预算**：

```c
/* 自旋面对读者群时，"读者散场"没有 owner on_cpu 可盯 —— 改为限时自旋 */
static inline u64 rwsem_rspin_threshold(struct rw_semaphore *sem)
{
	int readers = min(count >> RWSEM_READER_SHIFT, 30);
	/* 经验公式：Spinning threshold = (10 + nr_readers/2) µs，封顶 25µs */
	return sched_clock() + (20 + readers) * NSEC_PER_USEC / 2;
}
```

直觉：30 个读者各自临界区若 ~µs 级，全部退场约需 15µs——预算内自旋比睡觉划算；超预算立刻入队睡（别赌）。同时有个 wakeup hint：`RWSEM_WAKE_READ_OWNED`（写者醒来发现锁被读者接手时的一种唤醒类型，见下）。

### 2.2 慢路径（rwsem_down_write_slowpath 概览）

```
├─ __rwsem_down_write_failed_common：
│    ① list_add_tail 挂 waiter（writer 类型）
│    ② count |= RWSEM_FLAG_WAITERS
│    ③ 循环 {
│         rwsem_try_write_lock(sem, waiter)：   /* 队首才有资格 */
│             count 空闲（无 reader/无 writer）→ cmpxchg 拿写锁
│             且 handoff 检查（等待太久 → 置 HANDOFF，见下）
│         失败 → set_state(TASK_UNINTERRUPTIBLE) → schedule()
│         醒来若是队首 → 再试一轮乐观自旋（cache 热）
│       }
│    ④ 拿到：从队列摘除
└─ 解锁 up_write：先 count 清 WRITER_LOCKED，再看 WAITERS → rwsem_mark_wake
```

### 2.3 写者 handoff（防读者群"无限续杯"）

读者快路径是 `fetch_add`——只要计数一直有人续，写者永远等不到 0。6.6 机制（与 mutex handoff 同思想）：

```
等待写者在慢路径中反复失败 → count 置 RWSEM_FLAG_HANDOFF
此后读者快路径的 fetch_add 结果 & READ_FAILED_MASK 非零 → 读者被迫走慢路径入队
（HANDOFF 位挡住所有"插队"）
unlock 路径看到 HANDOFF → 把锁直接交给队首（写者收货，或按队列类型唤醒读者批）
```

即：**等待者有能力"关闸"**，把后来者全部导入公平队列。

---

## 三、读者路径与批量唤醒

```
down_read 慢路径：
  挂 waiter（reader 类型，记录 wakeup 标记）
  循环 { try 走 rwsem_mark_wake 的候选；睡；醒 }
```

`rwsem_mark_wake()`（rwsem.c 的唤醒中枢）三种模式（`enum rwsem_wake_type`）：

```
RWSEM_WAKE_ANY          通用：按队列头部的类型决定唤醒写者或读者批
RWSEM_WAKE_READERS      前一个持有者是读者/写者退场：唤醒一批读者
RWSEM_WAKE_READ_OWNED   读者自己醒来后带起的读者批（已经是读者持有态）
```

**读者批（woken batching）**：唤醒读者时不止一个——

```
1) 队列头部是 waiter(reader)，其后可能连着一串 reader（写者被夹在中间才算断）。
   一次把这一串全部移入 woken list（临时链表），批量 wake_up_q。
2) 每个 woken reader 在自己上下文里做 fetch_add +READER_BIAS 完成真正的
   持锁（醒来后不算持锁，原子加才算——保证 count 与真实持有者同步）。
3) 读者退出在 up_read 中 fetch_add(-BIAS)，减到 0 且 WAITERS → 唤醒逻辑。
```

批量唤醒把"10 个读者排队 → 10 次唤醒-尝试往返"压缩为"1 次编排 + 10 次本地 fetch_add"，这是读多写少高并发下 rwsem 吞吐的关键。

---

## 四、PREEMPT_RT 的 rwsem

`struct rw_semaphore { struct rwbase_rt rwbase; }` —— RT 上读写信号量改为 rt_mutex 家族的睡眠实现：

* **读者递归**：RT 版 `rwbase_rt` 记录 reader 深度（同任务可重入 down_read——主线版 count 无法追踪"我已持读锁"，重入是 UB）。
- **无乐观自旋**（RT 不允许忙等浪费实时预算）。
- 写者饥饿由 rt 锁的优先级/顺序保证。
- 主线 6.6 的 rwsem **不支持递归读**（同一任务两次 down_read 在写者插入时自锁死）——RT 版反而支持。这是两者可观测的行为差异之一。

---

## 五、选型对比与使用规范

| 需求 | 推荐 | 理由 |
| :--- | :--- | :--- |
| 读多写少、读临界区**可重试**（小 POD） | [seqlock](seqlock.md) | 读侧零原子操作 |
| 读多写少、读临界区**长/可睡眠** | **rwsem** | 读快路径一次原子加；睡眠兼容 |
| 读极多、写可延迟回收（指针/链表） | [RCU](../RCU/README.md) | 读侧接近零开销 |
| 读写都频繁、临界区极短、原子上下文 | rwlock（`arch_rwlock_t`，6.6 仍存在） | 自旋语义；但注意缓存弹跳通常劣于 rwsem+调度的组合 |
| 单写者 + N 读者状态发布 | [seqcount_latch](seqlock.md) | 无重试读 |

规范要点：

* `down_read_killable()/down_read_interruptible()` 优先于裸 `down_read()`（可中断避免 D 状态堆积）。
* 持读写信号量**禁止递归读**（主线版）；需要请重构或用 percpu_rwsem（`percpu-rw-semaphore.rst` 镜像在本目录：读侧走 per-CPU 计数，写侧反转为慢模式）。
* `up_read/up_write` 必须与 down 严格配对（无 owner 校验，配错只会在计数失配时炸出 WARN）。
- 观测：`/proc/lock_stat`、`perf lock`、`lock:contention_begin` tracepoint、`DEBUG_RWSEMS` 的 `sem->magic` 校验。

---

## 六、思考题

1. 读者快路径 `fetch_add` 成功的判定要同时排除 WRITER/WAITERS/HANDOFF/READFAIL 四类位。为什么 WAITERS 也要挡新读者？（提示：等待写者已 handoff 时，让新读者插队等于把闸门焊死。）
2. `rwsem_rspin_threshold` 假设"读者临界区 ~µs 级"。如果读者持锁 100ms（比如一个慢目录操作），写者的自旋预算会怎样浪费？哪个机制最终止损？（NONSPINNABLE + owner_on_cpu 放弃。）
3. 写者解锁时 `rwsem_mark_wake(RWSEM_WAKE_READERS)` 批量唤醒读者链。若读者链之后跟着写者、写者后又是读者，第二批读者为什么不顺便一起唤醒？（先到先得与写者不被永久饿死之间的取舍。）
4. 主线 rwsem 的 `owner` 对读者群只打 `READER_OWNED` 标记。为什么不能像写者那样记 `task_struct*`？（数量未知、每次原子写代价、无 on_cpu 语义。）
5. 设计：`mm->mmap_lock`（6.6 仍叫 mmap_sem，实为 rwsem）是全内核最有名的争用点之一（内存热路径 down_read）。给出三个层面（应用/内核参数/内核改动如 maple tree/RangeLock 方案）的缓解思路。

---

> 相关：[spinlocks.md](spinlocks.md)、[seqlock.md](seqlock.md)、[mutex.md](mutex.md)；官方镜像：`percpu-rw-semaphore.rst`、`locktypes.rst`（各锁类型语境约束总表）、`preempt-locking.rst`、`lockstat.rst`。
