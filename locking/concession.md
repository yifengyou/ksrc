# 让步模式：被等待者先走的排序纪律（Linux 6.6）

> 阅读目标：提炼散落在并发原语中的**排序让步**（ordering concession）家族——同一原则的三种形态：**新来者让老等待者**（mutex/rwsem 的 handoff 位与"stop spinning if no longer first"）、**读者让写者**（seqlock 的乐观重试——"lockless readers (read-only retry loops), and no writer starvation" 头注释原话）、**首战者让同伴**（kmod_dup 的 "let duplicates suffer" 一跳）。理解**谁让谁**的选择逻辑（先到先得 vs 吞吐优先 vs 一致性窗口）与防饿死的超时触发机制（`RWSEM_WAIT_TIMEOUT`）。全部对照 v6.6（kernel/locking/mutex.c、kernel/locking/rwsem.c、include/linux/seqlock.h、kernel/module/dups.c）。前置：[mutex](mutex.md)｜[seqlock](seqlock.md)。

---

## 一、问题：唤醒时刻的排序漏洞

```
锁释放瞬间的竞争三方：
  W = 队首等待者（已睡在 wait_list，刚被唤醒，尚未上 CPU）
  S = 自旋者（还在 optimistic spin 循环里没睡过）
  N = 新来者（刚刚 down_write 调用）
若 unlock 只是清 owner：S/N 的 cmpxchg 比 W 的被唤醒快得多
  → W 反复被"偷锁"（steal）→ 饿死
让步家族的核心决定：**谁必须让谁**
```

## 二、形态一：新来者让老等待者——mutex 的 handoff（kernel/locking/mutex.c）

```c
/* :68-72 —— owner 字段的低三位状态（注释原话）*/
/*
 * Bit0 indicates a non-empty waiter list; unlock must issue a wakeup.
 * Bit1 indicates unlock needs to hand the lock to the top-waiter
 * Bit2 indicates handoff has been done and we're waiting for pickup.
 */
#define MUTEX_FLAG_WAITERS	0x01
#define MUTEX_FLAG_HANDOFF	0x02
#define MUTEX_FLAG_PICKUP	0x04
```

两级让步纪律：

```
① 自旋者自查（mutex_optimistic_spin 的两个退出条件）：
   /* :327-331 注释原话："If we aren't on the wait list yet, cancel
      the spin if there are waiters. We want to avoid stealing the
      lock from a waiter with an earlier stamp" */
   if (!waiter && (owner & MUTEX_FLAG_WAITERS))
       return false;                     /* 有睡眠者：不睡者让已睡者 */
   /* :337-339："Similarly, stop spinning if we are no longer the
      first waiter." */
   if (waiter && !__mutex_waiter_is_first(lock, waiter))
       return false;                     /* 非队首：后到的让先到的 */

② handoff 位（队首等待者的反偷锁盾牌）：
   等待超时后设置 MUTEX_FLAG_HANDOFF → __mutex_unlock_slowpath
   走 __mutex_handoff（:231）：直接把 owner 写成队首任务
   ——★自旋者/新来者的 cmpxchg 见到 HANDOFF 位必须失败（:921-922 早退；:946 同查）
   ——等待者"先走"，绕过了被唤醒的延迟窗口
```

## 三、形态二：读者让写者——seqlock 的乐观重试（include/linux/seqlock.h）

```c
/* :5-6 头注释原话："seqcount_t / seqlock_t - a reader-writer
   consistency mechanism with lockless readers (read-only retry
   loops), and no writer starvation." */
```

让步的方向与 §二 相反：**写者从不等读者**（写者 seq++ 直入，无阻塞），**读者以重试让路**——检测到序列变化/奇数即放弃本次读取重来。选择逻辑：

| 对比 | rwsem（读者不让） | seqlock（读者让） |
| :--- | :--- | :--- |
| 读侧成本 | 原子加/减（跨核弹跳） | 两次普通读（零原子） |
| 写侧成本 | 等读者清零（可能久等） | seq++（无等待） |
| 适用 | 读临界区长 | 读临界区短且快、写少 |

★"读者让"的隐性契约：读者的重试循环**必须短**（否则写风暴下读者饿死）；数据结构必须可原子快照（指针/标量族）。jiffies 与 timekeeping 正是此形态的原生场景。

## 四、形态三：首战者让同伴——kmod_dup 的 queue_work 一跳（kernel/module/dups.c）

[module/07](../module/docs/07-idempotent模块加载.md) 已核对的注释（:234-243 原话）：

```c
	/*
	 * If we complete() here we may allow duplicate threads
	 * to continue before the first one that submitted the
	 * request. We're in no rush also, given that each and
	 * every bounce back to userspace is slow we avoid that
	 * with a slight delay here. So queue up the completion
	 * and let duplicates suffer, just wait a tad bit longer.
	 * There is no rush. But we also don't want to hold the
	 * caller up forever or introduce any boot delays.
	 */
	queue_work(system_wq, &kmod_req->complete_work);
```

★让步的对象反转：**最先完成的一方**（本应最快返回）主动绕一跳 workqueue——保证"第一个提交者"先于重复者完成收尾（模块 state 翻转等），重复者醒来时看到的世界已一致。**被等待的人先走**：让步者是"已经赢了的那个"，防止后来者越过它消费到中间态。

## 五、家族总表

| 案例 | 让步者 | 先行者 | 触发机制 | 目的 |
| :--- | :--- | :--- | :--- | :--- |
| mutex 自旋退出 | 自旋者/新来者 | 睡眠的等待者 | 见 WAITERS 位即退 | 防"偷锁" |
| mutex handoff | 所有非队首 | 队首等待者 | 等待超时设 HANDOFF | 队首反偷锁盾牌 |
| rwsem handoff | 读者/写者非队首 | 队首（读或写） | `RWSEM_WAIT_TIMEOUT = DIV_ROUND_UP(HZ, 250)`（rwsem.c:359；:1037 入队时上表，:465-470 的 "When we've been waiting 'too' long...request a HANDOFF to force the issue" 注释原话）——★让步是**超时触发的**：常态仍允许偷锁（吞吐优先），4ms 等待后纪律生效（公平兜底） |
| seqlock 重试 | 读者 | 写者 | 无条件（结构性） | 读侧零开销 |
| kmod_dup 一跳 | 重复者 | 首提交者 | 无条件 | 状态一致性窗口 |
| （对照）rwsem 写者偷锁 :303 | —— | 退场中的读者后的写者 | —— | 反例：允许偷（吞吐优先的另一面） |

**选择逻辑总结**：吞吐优先时让步仅在**超时后**生效（rwsem 的 4ms 设计是"先纵后擒"）；一致性敏感时让步**无条件**（kmod_dup）；读侧零开销目标下让步**结构性**（seqlock 的读者让）。三者回答同一个问题："公平与吞吐的兑换率定在哪儿？"

## 六、观测实战

```bash
# 1) mutex/rwsem 的让步事件（CONFIG_DEBUG_MUTEXES / lockdep）
cat /proc/lockdep_stats | grep -E 'handoff|wait'   # 粗粒度
perf record -e lock:mutex_* -a sleep 5             # 若开 tracepoint

# 2) seqlock 读者的重试率（时间敏感代码可自记）
#    jiffies 的读者重试几乎为 0；高频写的数据结构才是重灾区

# 3) rwsem 超时让步的现场
#    rwsem_wait :465 的 time_after(jiffies, waiter->timeout)
#    ——长等 >4ms 的等待者将设 handoff；可以 bpftrace 钩 __rwsem_* 观测

# 4) kmod_dup 的重复等待
echo 1 > /sys/module/module/parameters/enable_dups_trace  # DEBUG 门控
```

## 七、思考题

1. mutex 的两级让步（自旋者见 WAITERS 即退 + 队首 handoff）为什么需要**两道**？只有 handoff 一道会怎样？（提示：handoff 是超时触发的——超时前窗口里谁防偷？）
2. rwsem 把超时定为 `HZ/250`（4ms）——这个数值的量纲权衡是什么？（提示：比一次调度延迟略长；比用户可感知的卡顿短一个量级。）
3. seqlock 的"读者让"如果读临界区里睡了一觉会发生什么？（提示：重试循环变成死循环的可能——`preempt_disable` 版为什么必须禁抢占？）
4. kmod_dup 的让步者是"最快完成者"——它损失了什么？（提示：一个 workqueue 调度延迟。这与它保护的"一致性窗口"的宽度比起来如何？）
5. `MUTEX_FLAG_PICKUP`（handoff 已完成、等待被接走）为什么要单独一个位？（提示：handoff 设置到 owner 改写之间存在窗口——这段窗口里谁来拒绝新的偷锁者？）
6. 把"先纵后擒"（超时前允许偷、超时后 handoff）与纯 FIFO（排队即不可偷）对比：哪个在 99% 低竞争下的总吞吐高？哪个尾延迟好？（提示：偷锁省一次唤醒；FIFO 的 p99 可预测。）

---

> 回看：[mutex](mutex.md)（owner 位域与 optimistic spin 的全部机制）｜[seqlock](seqlock.md)（乐观读的完整家族）｜[module/07-idempotent](../module/docs/07-idempotent模块加载.md)（kmod_dup 的双层去重）｜[locking/README](README.md)（锁族谱）
