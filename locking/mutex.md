# mutex（Linux 6.6）

> 阅读目标：吃透 mutex 的三段式拿锁（快路径原子交换 → 乐观自旋 → 睡眠队列）与 owner 字段的复用编码；理解 osq（optimistic spin queue）如何让自旋者排队、handoff 机制如何防止锁被新来者偷走、ww_mutex 的 wound-wait 协议；掌握 mutex 与 PREEMPT_RT 的关系（rt_mutex）。

---

## 一、数据结构：4 个字段的极简主义

```c
struct mutex {                        /* include/linux/mutex.h, 6.6 */
	atomic_long_t		owner;        /* ★当前持有者 task_struct 指针 + 低 3 位标志
	                                            task_struct 指针按 L1_CACHE_BYTES 对齐，
	                                            低 3 位白送！ */
	raw_spinlock_t		wait_lock;    /* 保护 wait_list 的自旋锁（锁中锁）*/
#ifdef CONFIG_MUTEX_SPIN_ON_OWNER
	struct optimistic_spin_queue osq; /* 自旋者的 MCS 队列（见下）*/
#endif
	struct list_head	wait_list;    /* mutex_waiter 链表（FIFO）*/
	...
};
```

owner 低 3 位（`kernel/locking/mutex.c`）：

```
bit0  MUTEX_FLAG_WAITERS (0x01)  等待队列非空：unlock 必须走慢路径唤醒
bit1  MUTEX_FLAG_HANDOFF (0x02)  队首等得太久：unlock 必须把锁**递给**队首，
                                 不许新来者抢
bit2  MUTEX_FLAG_PICKUP  (0x04)  handoff 已发生，等队首来取
```

> 设计美学：传统 mutex 要 owner + waiters 标志两套状态，6.6 压成一个 `atomic_long_t`——快路径只需**一次** cmpxchg，且 unlock 能以一次原子读判断"要不要进慢路径"。

---

## 二、拿锁三段式

### 2.1 快路径（`__mutex_trylock`）

```c
static __always_inline bool __mutex_trylock(struct mutex *lock)
{
	unsigned long owner = atomic_long_read(&lock->owner);

	return !owner ||                                /* 空闲或只有 flag 位 */
		(atomic_long_try_cmpxchg_acquire(&lock->owner, &owner,
		                                 owner | MUTEX_FLAG_LOCKED) &&
		 __mutex_owner(lock));                      /* 写入 current 指针 */
}
```

无争用：一条 `lock cmpxchg`，完事。**PREEMPT_RT 的 mutex 在此分岔**（见第五节）。

### 2.2 乐观自旋（`mutex_optimistic_spin`，CONFIG_MUTEX_SPIN_ON_OWNER）

拿不到不立刻睡：持有者若**正在别的 CPU 上跑**，等它一会儿大概率马上放（临界区通常 µs 级）——睡眠/唤醒往返 ~几 µs 起步，自旋划算：

```
mutex_optimistic_spin(lock, ww_ctx, waiter)
 ├─ osq_lock(&lock->osq)                 /* ★自旋者也要排队！防止 N 个
 │                                          自旋者同时弹射 owner cacheline */
 ├─ for (;;) {
 │     owner = atomic_long_read(&lock->owner)
 │     if (owner 无 flag && owner 任务不在 CPU 上跑 → 放弃自旋去睡）
 │        （need_resched / !owner_on_cpu 同样放弃——别烧 CPU 等睡觉的人）
 │     if (__mutex_trylock_or_handoff(...)) 成功
 │  }
 └─ osq_unlock
```

两个克制条款（防止乐观变悲观）：

* `mutex_spin_on_owner` 里检查持有者 `on_cpu`——持有者被换下 CPU（在睡/在等别的锁）→ 自旋无意义，立刻转入睡眠路径。
* 自旋循环检查 `need_resched`——本 CPU 有更要紧的事，走人。

**osq（optimistic_spin_queue，kernel/locking/osq_lock.c）**：专为自旋者准备的迷你 MCS 锁（一个 atomic_t tail），让"自旋的人"也排成一队，只有队首对着 owner 的 cacheline 读——把自旋的缓存弹跳从 O(N) 压到 O(1)。mutex/rwsem/qspinlock 的 PV 路径共享这一思想。

### 2.3 慢路径（`__mutex_lock_common`，6.6 完整流程）

```
__mutex_lock_common(lock, state, subclass, nest_lock, ip, ww_ctx, use_ww)
 ├─ might_sleep()（非原子上下文断言）+ lockdep 记账
 ├─ 快路径 __mutex_trylock 失败 → mutex_optimistic_spin 失败
 ├─ raw_spin_lock(&lock->wait_lock)          /* 进入等待队列编排 */
 ├─ 再 trylock（可能刚才那次争用在 wait_lock 期间已散）
 ├─ __mutex_add_waiter(lock, &waiter, &list)  /* FIFO 挂队尾 */
 │     若是队首：设置 MUTEX_FLAG_WAITERS（owner |= 0x01）
 ├─ set_current_state(TASK_UNINTERRUPTIBLE / state)
 ├─ for (;;) {
 │     __mutex_trylock 拿到（含 handoff 队首优先）→ acquired
 │     signal_pending_state → 摘队退出 -EINTR（TASK_KILLABLE 场景）
 │     ww_ctx 检查 wound → -EDEADLK（见 ww 一节）
 │     raw_spin_unlock(wait_lock);
 │     schedule_preempt_disabled();            /* ★真正睡去 */
 │     first = __mutex_waiter_is_first(...);   /* 醒来：我是不是队首 */
 │     __mutex_trylock_or_handoff(lock, first) /* 队首可接收 handoff */
 │     if (first) 再试一轮乐观自旋（刚醒，cache 正热）
 │     raw_spin_lock(wait_lock); loop
 │  }
 └─ acquired: 摘 waiter、清 flags、放 wait_lock
```

唤醒由 unlock 慢路径发起（`__mutex_unlock_slowpath` → `__mutex_wake_up_next_owner`：取队首 waiter，`wake_q_add`（无锁唤醒队列，最后统一 `wake_up_q`），避免在持 wait_lock 时直接调度）。

### 2.4 handoff：反偷锁保险

没有 handoff 的 FIFO 是伪 FIFO：队首刚被唤醒还没跑，第三个新来的用快路径 `cmpxchg` 直接把锁抢走——队首再睡再醒再被偷，**极端争用下队首可能饿死**（实测曾在 futex 压测中复现）。6.6 的机制：

```
慢路径循环里，队首 waiter 等待超过阈值/被偷多次（__mutex_trylock_or_handoff）：
  → owner 置 MUTEX_FLAG_HANDOFF
unlock 慢路径看到 HANDOFF：
  → 不能简单清 owner；把 owner 改写为"队首 waiter 的任务指针 | PICKUP"，
    并唤醒之（锁已预先"递到手上"）
队首醒来发现 owner==自己 → 直接收货，新来者的 cmpxchg 全部失败
```

公平性与吞吐的平衡点：handoff 只在"确实被偷烦了"才触发，平时仍保留快路径的吞吐。

---

## 三、unlock 两段式

```
mutex_unlock
 ├─ 快路径 __mutex_unlock_fast:
 │     owner 指针无 WAITERS 标志 → cmpxchg(owner, current, 0) 直接放（1 条原子指令）
 └─ 慢路径 __mutex_unlock_slowpath:
       raw_spin_lock(wait_lock)
       有 HANDOFF → 交接给队首（上面流程）
       否则 owner &= ~MUTEX_FLAG_LOCKED? 实为清 owner
       队首 waiter → wake_q_add + 最后 wake_up_q
       若队列空 → 清 WAITERS 标志
```

细节：唤醒发生在**放锁之后**（先让 owner 空，再唤醒），醒来者用 trylock 竞争——与 handoff 语义配合后，醒来者里只有被指定者能必胜。

---

## 四、ww_mutex：wound-wait（写-写死锁的正解）

GPU/DRM 子系统的经典需求：一次 ioctl 要按**调用方自定顺序**拿几十把锁（buffer 依赖图），全局锁序不存在。`struct ww_mutex`（`include/linux/ww_mutex.h` + `kernel/locking/ww_mutex.h`）提供**带上下文的锁**：

```
ww_acquire_init(ctx, class)        /* 拿号（全局递增 stamp）*/
for each lock:
    ww_mutex_lock(l, ctx)          /* 正常拿 */
    若冲突（对方 ctx.stamp 更老/更新）：
        Wait-Die：我的 stamp 新 → 我退出释放已持有的，重试
        Wound-Wait：我 stamp 老 → 我"打伤"对方（设对方 wounded），
                    对方在其下一个 ww_mutex_lock 点 -EDEADLK 退出
ww_mutex_unlock / ww_acquire_done
```

6.6 默认 **wound-wait**（`ww_ctx->is_wait_die == false`；wait-die 留给历史兼容）。wound-wait 的优势：老事务（号小）永不退让，新事务主动退——高并发下整体重试率更低。`__ww_mutex_add_waiter` 把 waiter 按 stamp 插队 + `__ww_mutex_check_kill` 在锁点执行"处决"。DRM 的 `dma_resv`（显存对象保留锁）是最大用户。

---

## 五、mutex 与 PREEMPT_RT / rt_mutex

主线 mutex **没有优先级继承**（PI）：低优先级持有者 + 高优先级等待者 = 优先级反转无解（调度器只认 `prio`，不看你等谁）。两个出口：

1. **futex PI**：用户态锁走 `rt_mutex` 的 PI 链（`futex_lock_pi`，见本目录 `pi-futex.rst`/`rt-mutex-design.rst`）。
2. **PREEMPT_RT**：`spinlock_t`/`mutex_t` 全家 rt_mutex 化：

```c
/* include/linux/mutex.h, CONFIG_PREEMPT_RT: */
struct mutex {
	struct rt_mutex_base	rtmutex;    /* 睡眠锁 + PI 继承链 */
	...
};
```

RT 上 mutex 快路径从 cmpxchg 变成 rt_mutex 的 owner cmpxchg（语义兼容：owner 空闲判定），慢路径走 `rt_mutex_slowlock`：waiter 树（rbtree 按优先级排序，非 FIFO！）+ `task_blocks_on_rt_mutex` 沿持有链**传播提权**（优先级天花板/继承协议，见 rt-mutex-design.rst）。

> 主线也在吸收 RT：`rt_mutex_base` 结构已进入主线（6.6 里 rtmutex 与 futex REQUEUE_PI 共用），`mutex.c` 的 `__mutex_trylock_or_handoff` 与 RT 的 handoff 语义对齐。**主线 mutex 追求吞吐，RT 追求有界延迟**——同一份 API 的两副实现。

---

## 六、使用规范速查

```
可以做：                              不可以做：
mutex_lock 后睡眠/缺页/慢速 IO        持锁超过 ~ms（考虑用更粗的锁设计或 rwsem）
TASK_KILLABLE 版 mutex_lock_killable  在中断/软中断/持有 spinlock 时 mutex_lock
（D 可杀，futex 路径常用）            同一任务重复 lock（非递归！）
mutex_trylock / mutex_lock_interruptible
atomic 上下文判别：might_sleep 内建
```

* **mutex 不是 semaphore**：count 语义的 `struct semaphore` 仍在（`down/up`），用于"资源池/完成通知"类场景；互斥用 mutex（开销更低、语义更强：必须由加锁者解锁）。
* **`completion` 与 mutex** 的分工：等"事件发生"用 complete（一次性、不计数）；互斥进临界区用 mutex（本目录 `../scheduler/completion.rst` 有镜像文档）。
* 调试：`CONFIG_DEBUG_MUTEXES`（magic 校验/上下文检查）、lockdep、`CONFIG_MUTEX_SPIN_ON_OWNER` 关闭对比。

---

## 七、观测

```
/proc/lock_stat（CONFIG_LOCK_STAT）:
   每把锁的 con-bounces / waittime-total / contentions
   （mutex 的 osq 自旋让 bounce 统计天然好看——弹跳已被 osq 吸收）
perf lock record / perf lock report      按锁类聚合争用
ftrace: lock:contention_begin/end tracepoint（6.6 新），
        funcgraph 看 __mutex_lock_slowpath 时长
bpftrace kprobe:__mutex_lock_slowpath 聚合 caller 栈 → 找"谁锁得最久"
```

---

## 八、思考题

1. owner 的三个 flag 位为什么**不可能**与真实 task_struct 指针混淆？（对齐保证。）若某架构 `task_struct` 只按 8 字节对齐，低 3 位够用吗？低几位永远安全？
2. 乐观自旋在**虚拟机**里可能全盘皆输（持有者 vCPU 不在物理 CPU 上）。6.6 有哪些机制缓解？与 qspinlock 的 PV 改造对比。
3. handoff 触发条件为什么设计成"队首再次醒来仍空手"而不是"等了 X µs"？定时器方案的成本是什么？（提示：hrtimer + per-lock 状态。）
4. wound-wait 与 wait-die 在"事务到达率 λ、持锁时长 t"下谁的重试率高？给出直觉论证。为什么 wound-wait 更适合 DRM？
5. PREEMPT_RT 下 `mutex_lock` 的 waiter 用 rbtree 而非 list。队列有序性与 PI 传播之间是什么关系？（提示：rt_mutex 的 PI 链需要对"最高优先级等待者"的 O(log n) 访问。）

---

> 相关：[spinlocks.md](spinlocks.md)、[seqlock.md](seqlock.md)、[rwsem.md](rwsem.md)；本目录官方镜像：`mutex-design.rst`、`rt-mutex-design.rst`、`ww-mutex-design.rst`、`pi-futex.rst`、`futex-requeue-pi.rst`、`locktorture.rst`。
