# rcu_sync：多写者的宽限期合并器（Linux 6.6）

> 阅读目标：吃透 percpu-rwsem 内嵌的 **rcu_sync**（kernel/rcu/sync.c 206 行，Oleg Nesterov 2015）——多写者共享 grace period 的状态机：五态模型（`GP_IDLE→GP_ENTER→GP_PASSED→GP_EXIT→GP_REPLAY`）、`gp_count` 的**引用计数式宽限期共享**（"closely spaced calls to rcu_sync_enter() can optimize away the grace-period wait" 注释原话）、`GP_REPLAY` 的**追赶延迟**（紧邻的 exit 不再重新排队 call_rcu）、以及它与 percpu_ref 每次 `call_rcu` 的对照——**单个宽限期被"批发"给所有并发写者**。全部对照 v6.6（include/linux/rcu_sync.h 54 行、kernel/rcu/sync.c 206 行、kernel/locking/percpu-rwsem.c）。前置：[percpu-refcount](percpu-refcount.md)（percpu_ref 每次 kill 的 call_rcu 对照）。

---

## 一、问题：每个写者各排一个 call_rcu 的浪费

```
percpu-rwsem 的写者路径（percpu_down_write）：
  ① rcu_sync_enter（等 GP——旧读者全部退出）
  ② block 位 + 等读者清零

三个写者 W1/W2/W3 依次进入：
  percpu_ref 方案（无共享）：每个 kill 各排一个 call_rcu → 3 个 GP
  rcu_sync 方案（共享）：W1 排 GP，W2/W3 在 W1 的 GP 上"搭车" → 1 个 GP

头注释原话（rs.h:6-7）：
"Structure to mediate between updaters and fastpath-using readers."
——中介者：让多个 updater 共享一个 GP 进度
```

## 二、五态状态机（sync.c:14）

```c
enum { GP_IDLE = 0, GP_ENTER, GP_PASSED, GP_EXIT, GP_REPLAY };
```

```
GP_IDLE ──enter──► GP_ENTER ──[GP 到来]──► GP_PASSED ──exit──► GP_EXIT
   ▲                                                                        │
   └──────────────[GP 到来且 gp_count==0]─────────────────────────────────┘
                                       │
                                       └─[exit 在 GP_EXIT 期间]──► GP_REPLAY
                                            ──[enter]──► 回 GP_PASSED
                                            ──[GP 到来且 gp_count==0]──► GP_IDLE
```

| 状态 | 语义 | 读者 fastpath |
| :--- | :--- | :--- |
| `GP_IDLE` | 无写者在场 | **开**（`rcu_sync_is_idle` 返回 true） |
| `GP_ENTER` | 首写者已发起 `synchronize_rcu`，GP 未到 | 关 |
| `GP_PASSED` | GP 已过——旧读者全部退出 | 关（至少还有一个写者） |
| `GP_EXIT` | 最后写者已退，新 GP 在等 | 关（等 GP——新读者可能未见到 block 清除） |
| `GP_REPLAY` | exit 在 GP_EXIT 期间又来 → 重排 call_rcu | 关 |

★**GP_EXIT 是必需的中间态**：exit 后不能立即恢复 fastpath——因为 exit 发生瞬间可能还有读者**刚走到** `rcu_sync_is_idle` 检查处但还没走到读路径——必须再等一个 GP 确保这些"边界读者"也退场。

## 三、数据与 API（rs.h:15-19）

```c
struct rcu_sync {
	int		gp_state;    /* 上述五态 */
	int		gp_count;    /* ★共享引用计数——写者数 */
	wait_queue_head_t	gp_wait;  /* 等待 GP_PASSED 的写者队列 */
	struct rcu_head		cb_head;     /* call_rcu_hurry 的载体 */
};
#define rss_lock	gp_wait.lock   /* ★锁即等待队列的锁——免额外锁 */
```

| API | 效果 |
| :--- | :--- |
| `rcu_sync_init` | 清零 + init_waitqueue_head |
| `rcu_sync_enter_start` | 预置为 GP_PASSED（"Force readers onto slow path for **multiple** updates"） |
| `rcu_sync_enter` | 写者进场（→is_idle 变 false）；如为首写者则排 GP |
| `rcu_sync_exit` | 写者退场（gp_count--）；如为最后写者则排退出 GP |
| `rcu_sync_dtor` | 等待 GP 回到 IDLE 后释放 |
| `rcu_sync_is_idle` | 读者 fastpath 判定（`!READ_ONCE(gp_state)`） |

## 四、enter 的关键路径（sync.c:105-141）

```c
void rcu_sync_enter(struct rcu_sync *rsp)
{
	spin_lock_irq(&rsp->rss_lock);
	gp_state = rsp->gp_state;
	if (gp_state == GP_IDLE) {
		WRITE_ONCE(rsp->gp_state, GP_ENTER);
		WARN_ON_ONCE(rsp->gp_count);
		/* 注释原话："Note that we could simply do rcu_sync_call(rsp)
		   here...However, synchronize_rcu() can be faster if
		   rcu_expedited or rcu_blocking_is_gp() is true." */
	}
	rsp->gp_count++;                 /* ★共享计数++ */
	spin_unlock_irq(&rsp->rss_lock);

	if (gp_state == GP_IDLE) {
		/* 首写者的快路径：同步 GP + 立即调 func 推进到 PASSED */
		synchronize_rcu();
		rcu_sync_func(&rsp->cb_head);
		return;
	}

	/* 后续写者：搭车等 GP_PASSED */
	wait_event(rsp->gp_wait, READ_ONCE(rsp->gp_state) >= GP_PASSED);
}
```

★**首写者的优化**：用 `synchronize_rcu()` + 直接调 `rcu_sync_func` 而非 `call_rcu`——注释解释两个理由（expedited/early-boot）。

## 五、exit 与 GP_REPLAY 的追赶（sync.c:155-173）

```c
void rcu_sync_exit(struct rcu_sync *rsp)
{
	spin_lock_irq(&rsp->rss_lock);
	if (!--rsp->gp_count) {          /* 我是最后一个写者 */
		if (rsp->gp_state == GP_PASSED) {
			WRITE_ONCE(rsp->gp_state, GP_EXIT);
			rcu_sync_call(rsp);   /* call_rcu_hurry */
		} else if (rsp->gp_state == GP_EXIT) {
			WRITE_ONCE(rsp->gp_state, GP_REPLAY);
			/* ★已有 exit GP 在排——标记 REPLAY 让 func 不进 IDLE */
		}
	}
	spin_unlock_irq(&rsp->rss_lock);
}
```

**GP_REPLAY 的场景**：
```
W1: enter → GP 到来 → PASSED → exit → GP_EXIT（call_rcu 排队中）
W2:                              enter →（gp_count 又 >0）
W2:                              exit → 发现 gp_state == GP_EXIT
                                    → 设 GP_REPLAY
     [GP 到来] rcu_sync_func：
       if (gp_count > 0) → PASSED     /* 又有写者 → 不恢复 fastpath */
       elif (gp_state == GP_REPLAY) → GP_EXIT + 重新 call_rcu
       else → GP_IDLE                /* 真正恢复 fastpath */
```

★**追赶语义**：REPLAY 意味着"上次 exit 的 GP 白等了"（因为期间又有写者进出）——func 检测到后**主动重排**一个新 GP，而非让 fastpath 恢复又被关闭（浪费两次 GP）。头注释原话（rs.c:55-57）："closely spaced rcu_sync_enter() and rcu_sync_exit() pairs need not wait for a grace period."

## 六、与 percpu_ref 的 call_rcu 对照

| 维度 | percpu_ref（kill 的 call_rcu） | rcu_sync |
| :--- | :--- | :--- |
| GP 数（N 个写者并发） | N（每个 kill 各排一个） | **1**（共享 GP） |
| 回调机制 | call_rcu 一次 | call_rcu_hurry（可被 expedited 加速）+ state machine |
| 恢复方式 | resurrect（加 BIAS→清 percpu→清 ATOMIC） | exit + GP 到来自动 IDLE |
| 适用 | 引用计数（可从 kill 复活） | 读/写 fastpath 切换（一次性 enter/exit 对） |
| 消费者 | blk-mq q_usage_counter | percpu-rwsem rss |

## 七、percpu-rwsem 的完整消费链

```
percpu_down_write（kernel/locking/percpu-rwsem.c:225-245）：
  rcu_sync_enter(&sem->rss)       ← 本节主题
  __percpu_down_write_trylock     ← block 位 + 等读者清零
  ...写者临界区...
percpu_up_write（:280-）：
  rcu_sync_exit(&sem->rss)        ← 释放 GP 共享
```

## 八、观测实战

```bash
# 1) rcu_sync 本身无 sysfs/debugfs 直接观测——间接：
grep -r 'rcu_sync' kernel/locking/percpu-rwsem.c  # 消费端

# 2) percpu-rwsem 的使用者
grep -r 'percpu_init_rwsem' fs/ kernel/ | head -5
#   典型：fs/super.c 的 s_writers.rw_sem（文件系统冻结）

# 3) rcu_expedited 对首写者的影响
echo 1 > /sys/kernel/rcu_expedited     # synchronize_rcu 加速

# 4) GP 的观测（rcu 的通用接口）
cat /sys/kernel/debug/rcu/rcudata      # 每 CPU 的 GP 进度
```

## 九、思考题

1. `GP_EXIT` 为什么不直接回 `GP_IDLE`——中间那个 GP 保护的是谁？（提示：exit 瞬间可能有读者刚过了 is_idle 检查但还没走到读路径——它们需要**再一个 GP** 确保退出。）
2. `GP_REPLAY` 的存在意义——没有它会怎样？（提示：每次 exit→enter→exit 循环各排一个 GP → N 次循环 = N 个白等的 GP。REPLAY 让 func "追赶"一次。）
3. `rcu_sync_enter` 的首写者用 `synchronize_rcu` 而非 `call_rcu`——注释说"can be faster if rcu_expedited"——为什么 call_rcu 不能被 expedited？（提示：call_rcu 是异步的——expedited 影响的是等待路径。）
4. `gp_count` 引用计数与 percpu_ref 的 `data->count` 的本质区别？（提示：percpu_ref 的计数是**被保护对象**的引用者数；rcu_sync 的 gp_count 是**写者数**——保护的是 fastpath 状态而非对象生命周期。）
5. `rss_lock = gp_wait.lock` 的别名——为什么可以合并？（提示：gp_wait 只在持锁状态下操作（wake_up_locked）——锁的持有域完全覆盖 wait 队列的修改域。）
6. `rcu_sync_enter_start` 预置 GP_PASSED 而非 GP_ENTER——语义差在哪？（提示：PASSED 意味着"已过 GP"——后续 enter 无需等待即通过（`>= GP_PASSED`）——**永久关闭 fastpath** 的场景。）

---

> 回看：[percpu-refcount](percpu-refcount.md)（percpu_ref 的每次 call_rcu 对照）｜[concession](concession.md)（percpu-rwsem 的 block 位让步机制）｜[locking/README](README.md)（锁族谱）
