# rtmutex PI：优先级继承的完整机制（Linux 6.6）

> 阅读目标：吃透 rtmutex 的**优先级继承（PI）引擎**——本分册三章（[rwbase-rt](rwbase-rt.md)/[ww-rt-mutex](ww-rt-mutex.md)/[preempt-rt-matrix](preempt-rt-matrix.md)）共同踩着的底座：waiter 的**双树身份**（锁的 waiters 树 + 持有者的 pi_waiters 树）与 `rt_waiter_node` 的 prio+deadline **DL 感知排序**；★`rt_mutex_steal` 的横向抢占策略（"RT tasks are excluded from same priority (lateral) steals to prevent the introduction of an unbounded latency" 注释原文）；★**链走 13 步协议**（rtmutex.c:590-655 头注释的 [R]/[P]/[L] 保护域图例——"Drop the locks"与 again/retry 双循环）；死锁检测（`orig_lock == lock || owner == top_task` 的 -EDEADLK + ww 特例 + P3 陷阱）与 `max_lock_depth=1024` 深限；优先级落地 `rt_mutex_setprio` 的类迁移。全部对照 v6.6 当轮核对。前置：[mutex](mutex.md)｜[rwbase-rt](rwbase-rt.md)。

---

## 一、问题：无界延迟与 PI 的解法

```
优先级反转（无 PI）：
  L 持锁；H 抢占 L 等 L 的锁；M（中间优先级）持续抢占 L
  → L 不前进 → H 被非最高优先级的 M 无界延迟

PI 的解法：把"等待"捐给持有者——
  H 阻塞在 L 上时，L 临时抬升到 max(L.prio, H.prio)
  → M 压不住 L → L 速跑速放 → H 的延迟界 = L 的临界区长
```

PI 的传递性：L 若又阻塞在 K 上，捐赠沿链上传——**链走（chain walk）**就是维护这条捐赠链的算法。

## 二、数据结构：一个 waiter 的双树身份

### 2.1 锁与任务两侧（include/linux/rtmutex.h:23-26 / include/linux/sched.h:1141-1145）

```c
struct rt_mutex_base {
	raw_spinlock_t		wait_lock;
	struct rb_root_cached	waiters;    /* 锁的 waiter 树（prio+deadline 序） */
	struct task_struct	*owner;      /* ★低位带 RT_MUTEX_HAS_WAITERS 标志 */
};

/* task_struct 侧： */
	struct rb_root_cached	pi_waiters;      /* 谁在等我（捐赠树） */
	struct rt_mutex_waiter	*pi_blocked_on;  /* 我在等谁（单值——一次只阻在一把锁） */
	struct rt_mutex_waiter	*pi_top_waiter;  /* 捐赠树的树头缓存 */
```

★owner 指针**低位当标志位**（rtmutex.c:98/:122/:192——`owner | RT_MUTEX_HAS_WAITERS` 的位装配）：有等待者标志强迫后来者走慢路径串行化（`try_to_take_rt_mutex` 的注释原话："This forces all other tasks which try to modify @lock into the slow path"）。

### 2.2 waiter 节点：栈分配纪律 + DL 感知排序（rtmutex_common.h:32-59）

```c
struct rt_waiter_node {           /* 排序键：prio + deadline 双轴 */
	struct rb_node	entry;
	int		prio;      /* 有效优先级 */
	u64		deadline;  /* "Deadline of the waiter if applicable" */
};
struct rt_mutex_waiter {
	struct rt_waiter_node	tree;      /* 挂 lock->waiters */
	struct rt_waiter_node	pi_tree;   /* 挂 owner->pi_waiters */
	struct task_struct	*task;
	struct rt_mutex_base	*lock;
	unsigned int		wake_state;
	struct ww_acquire_ctx	*ww_ctx;   /* ww 消费端的挂钩 */
};
```

头注释明确：*"which is allocated on the kernel stack of the blocked task"*——**waiter 活在等待者的内核栈上**（不分配不释放；pi_blocked_on 指向栈上节点，链走持 task 引用保活）。

## 三、排序与抢占：DL 时代的两难裁决

### 3.1 rt_waiter_node_less（rtmutex.c:371-389）

```c
	if (left->prio < right->prio)
		return 1;
	/*
	 * If both waiters have dl_prio(), we check the deadlines of the
	 * associated tasks. If left waiter has a dl_prio(), and we didn't
	 * return 1 above, then right waiter has a dl_prio() too.
	 */
	if (dl_prio(left->prio))
		return dl_time_before(left->deadline, right->deadline);
	return 0;
```

★树序不是全序——**同优先级的 DL 任务按 deadline 再排**（更早截止期者胜出），普通任务同优先级则由入树时的 FIFO 位置决（rbtree 稳定插入）。

### 3.2 ★rt_mutex_steal（rtmutex.c:407-424）——"横向抢占"的窄窗

```c
	if (rt_waiter_node_less(&waiter->tree, &top_waiter->tree))
		return true;                     /* 严格更高优先级：抢 */
#ifdef RT_MUTEX_BUILD_SPINLOCKS
	/*
	 * Note that RT tasks are excluded from same priority (lateral)
	 * steals to prevent the introduction of an unbounded latency.
	 */
	if (rt_prio(waiter->tree.prio) || dl_prio(waiter->tree.prio))
		return false;                    /* RT/DL 任务：同优先级不抢 */
	return rt_waiter_node_equal(&waiter->tree, &top_waiter->tree); /* 普通：可同抢 */
#else
	return false;                        /* mutex 构建：横向抢全关 */
#endif
```

★语义分层：**严格高优**永远可抢（恢复正义）；**同优先级的横向抢**只在 spinlock 构建（RT 内核的 spinlock 变体）开启、且 RT/DL 任务被排除——横向抢虽省一次唤醒切换，但会把"后到者插队"的抖动引入 RT/DL 的延迟界（原文点名的 "unbounded latency"）。

## 四、链走：13 步协议（rtmutex.c:590-655 头注释原文裁剪）

```
 Chain walk basics and protection scope

 [R] refcount on task     [Pn] task->pi_lock held     [L] rtmutex->wait_lock held

 Normal locking order:  rtmutex->wait_lock → task->pi_lock

 again:
   loop_sanity_check();
 retry:
 [1]  lock(task->pi_lock);                    acquire [P1]
 [2]  waiter = task->pi_blocked_on;
 [3]  check_exit_conditions_1();
 [4]  lock = waiter->lock;
 [5]  if (!try_lock(lock->wait_lock)) {       ★丢 P1 再重试——反转锁序死锁的解法
        unlock(task->pi_lock);  goto retry; }
 [6]  check_exit_conditions_2();              [P1]+[L]
 [7]  requeue_lock_waiter(lock, waiter);      把 waiter 挪进锁的新位置
 [8]  unlock(task->pi_lock);  put_task_struct(task);
 [9]  check_exit_conditions_3();              [L]
 [10] task = owner(lock);                     上行一步：持有者变下一个被检任务
      get_task_struct(task);  lock(task->pi_lock);
 [11] requeue_pi_waiter(tsk, waiters(lock));  捐赠树同步
 [12] check_exit_conditions_4();
 [13] unlock(task->pi_lock);  unlock(lock->wait_lock);
      goto again;
```

四组退出条件的语义：**[3]** 走到头（task 不再阻塞）；**[6]** 检测出环（§五）；**[9]** 链在中间断（owner 已放锁）；**[12]** 到达链尾或"非 top waiter 且非全链模式"提前收工（*"If the current waiter is not the top waiter on the lock, then we can stop the chain walk here if we are not in full deadlock detection mode"*）。两档模式由 `chwalk` 控制：`RT_MUTEX_MIN_CHAINWALK`（普通加锁，只维护捐赠）vs `RT_MUTEX_FULL_CHAINWALK`（futex requeue PI/ww 等需要检测环的场景，rtmutex.c:574）。

## 五、死锁检测与深限

### 5.1 环的判定（rtmutex.c:830-838）

```
 * Deadlock detection. If the lock is the same as the original
 * lock which caused us to walk the lock chain or if the
 * current lock is owned by the task which initiated the chain
 * walk, we detected a deadlock.
	if (lock == orig_lock || rt_mutex_owner(lock) == top_task)
		ret = -EDEADLK;
```

### 5.2 两个特例（注释原文）

- **ww 特例**（:838-844）："let the ww_mutex wound/die logic pick which of the contending threads gets -EDEADLK"+"NOTE: assumes the cycle only contains a single ww_class"——环里有 ww 时不在此报错，交回 wound/wait 仲裁（[ww-mutex 章](ww-mutex.md)）。
- **P3 陷阱**（:755-760）："P3 should not return -EDEADLK because it gets trapped in the cycle created by P1 and P2 (which will resolve -- and runs into max_lock_depth above). Therefore disable detect_deadlock such that the below termination condition can trigger once all relevant tasks are boosted."——**环外的无辜任务**走了全链检测会误报环；对它关闭检测让 boost 先行，环内成员由深限兜底。

### 5.3 深限（rtmutex.c:681-695）

```
	if (++depth > max_lock_depth) {
		printk(KERN_WARNING "Maximum lock depth %d reached "
		       "task: %s (%d)\n", ...);
```

`max_lock_depth = 1024`（rtmutex_api.c:14）且**sysctl 可调**（rtmutex.h:21 "for sysctl" + sysctl.c:1953-1954 注册 `/proc/sys/kernel/max_lock_depth`）——深限不是弃疗而是**保险丝**：正常 PI 链不过几层，1024 层意味着环或失控模块。

## 六、优先级落地：rt_mutex_setprio（kernel/sched/core.c:7063+）

```
prio = __rt_effective_prio(pi_task, p->normal_prio)   /* 从 top waiter 取有效优先级
                                                        （含 RT 继承的 bandwidth 约束）*/
早退两连：无变化即返（p->pi_top_task == pi_task && prio == p->prio && !dl_prio(prio)
                     ——★DL 任务即使数值同也要走全流程（时限变了的语义））
oldprio/p->prio 更新 → prev_class = p->sched_class
→ __setscheduler_prio + check_class_changed（可能触发公平类 ↔ RT/DL 类的迁移
  ——被 boost 的普通任务换调度类，put_task_struct/push 洀滥发预定）
```

★这是 PI 与调度器的接口面：**继承不只是改数字，还换调度类**——普通任务被 RT waiter 捐赠后按 RT 跑（若 bandwidth 允许），被 DL waiter 捐赠则携带其 deadline 进入 DL 类。

## 七、消费家族（本分册的 PI 应用全景）

| 消费者 | 用法 | 详见 |
| :--- | :--- | :--- |
| `mutex`（RT 构建） | rt_mutex_base 直接做锁体 | [preempt-rt-matrix](preempt-rt-matrix.md) |
| `rwbase_rt` | 读者代理锁写者（代理继承） | [rwbase-rt](rwbase-rt.md) |
| `ww_mutex`（RT 构建） | stamp 仲裁 + PI 兜底 | [ww-rt-mutex](ww-rt-mutex.md) |
| spinlock/rwlock（RT 构建） | RT_MUTEX_BUILD_SPINLOCKS 的横向抢构建 | 本章 §3.2 |
| futex PI/requeue PI | FULL_CHAINWALK 的主用户（环检测） | 本章 §四/§五 |

## 八、观测实战

```bash
# 1) 深限现状与调整
cat /proc/sys/kernel/max_lock_depth     # 1024（环/失控的保险丝）

# 2) 优先级被捐赠的现场
grep -E '^(Name|Pid|prio|voluntary|nonvoluntary)' /proc/<pid>/status
#   boost 中的任务：prio 突降（数值小=优先级高）且 nonvoluntary 切换激增

# 3) PI 相关告警
dmesg | grep -i 'lock depth\|EDEADLK\|rtmutex'

# 4) lockdep 视角（PI 链的锁序验证）
cat /proc/lockdep | grep -A2 rtmutex    # 每把 rtmutex 的 class 与依赖
#   futex PI 死锁（用户态持有-futex 等待-内核持有链）由链走的 -EDEADLK 报出

# 5) 压测：cyclictest 高负载下的 max latency（PI 生效性的系统级验证）
```

## 九、思考题

1. waiter 栈分配——链走拿着别人栈上的 `pi_blocked_on` 指针走图，凭什么保证不被踩？（提示：等待中的任务不会返回它的栈；task 引用计数 [R] + 每步先锁 pi_lock 再读——头注释的保护域表就是这份证明的目录。）
2. `pi_blocked_on` 是单值指针——任务同时只能等一把锁。链上的"锁"从哪来？（提示：从被等锁的 owner——`[10] task = owner(lock)`；链=锁与任务的交替序列，任务侧只存"当前等的那一个"。）
3. 横向抢为什么对 RT/DL 关、对普通任务开？（提示：普通任务同优先级晚到早到无所谓（公平类无硬保证）；RT/DL 的"同优先级 FIFO 次序"是调度契约的一部分——横向抢=后到者插队=老 top waiter 的额外延迟无界。）
4. `[5]` 步丢锁重试为什么不是活锁？（提示：每次 retry 重新评估退出条件；图的拓扑在每步间单调演进（waiter 只会前进/消失），有限步内必成或必退。）
5. P3 陷阱的处置（关检测保 boost）为什么是安全的？（提示：环内的 P1/P2 自己会撞深限（warning+断链）；P3 不误报地完成捐赠——**误报的代价（错杀无辜）大于漏报环的代价（深限兜底）**。）
6. rt_mutex_setprio 对 DL 任务的"数值相同也全跑"早退豁免，防的是什么 bug？（提示：DL 的等价性含 deadline 维度——继承者换了（deadline 变化）即使 prio 数字相同也须更新可运行性/带宽账本；数值比对看不到时限。）

---

> 回看：[rwbase-rt](rwbase-rt.md)（读者代理继承的取舍）｜[ww-rt-mutex](ww-rt-mutex.md)（stamp 仲裁与 PI 的叠加）｜[preempt-rt-matrix](preempt-rt-matrix.md)（谁在用这套引擎）｜[mutex](mutex.md)（睡眠锁基座）｜[concession](concession.md)（让步机制谱系）｜[locking/README](README.md)（锁族谱）
