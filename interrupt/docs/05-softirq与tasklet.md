# softirq 与 tasklet

硬中断 handler 必须快进快出，但很多工作不能在硬中断里做（收包协议栈、块 IO 完成、RCU 回调……）。Linux 的"下半部（bottom half）"机制谱系：**softirq（最底层、静态注册、可并发）→ tasklet（softirq 之上的动态封装，自串行，已被标记淘汰）→ workqueue（进程上下文、可睡眠）**；threaded irq 则把下半部直接做成设备专属线程（第 6 章）。本章聚焦前两者。

一句话预览各自的性格：softirq 是"子系统专属的高频引擎"（编译期绑定、per-CPU、预算驱动）；tasklet 是"驱动可用的延迟函数"（动态注册但跑在软中断上下文）；workqueue 是"能睡觉的 tasklet"；threaded irq 是"设备专属的实时下半部线程"。选型直觉：**频率越高、越要快 → 越靠近 softirq/硬中断；工作越重、越可能睡眠 → 越靠近 workqueue/线程**。

### 🧮 softirq 机制全解

softirq 是 2.3 引入的"可延迟处理的 per-CPU 机制"。核心数据只有一个 per-CPU 位图：

```c
// include/linux/interrupt.h (v6.6)
#define local_softirq_pending_ref  irq_stat.__softirq_pending
#define local_softirq_pending()    (__this_cpu_read(local_softirq_pending_ref))
#define set_softirq_pending(x)     (__this_cpu_write(local_softirq_pending_ref, (x)))
#define or_softirq_pending(x)      (__this_cpu_or(local_softirq_pending_ref, (x)))
```

`irq_stat.__softirq_pending` 是每 CPU 一个 `__u32`，**bit N 置 1 表示第 N 号 softirq 有待处理**。软中断编号是编译期固定的（不能动态注册）：

```c
// include/linux/interrupt.h (v6.6, 原文)
enum {
	HI_SOFTIRQ=0,          /* tasklet 高优先级队列 */
	TIMER_SOFTIRQ,         /* hrtimer/timer wheel 的软化部分 */
	NET_TX_SOFTIRQ,        /* 网络发送 */
	NET_RX_SOFTIRQ,        /* 网络接收 (NAPI poll) */
	BLOCK_SOFTIRQ,         /* 块 IO 完成 (blk_done/softirq) */
	IRQ_POLL_SOFTIRQ,      /* blk-iopoll (现在多为单队列回退用) */
	TASKLET_SOFTIRQ,       /* tasklet 普通队列 */
	SCHED_SOFTIRQ,         /* 负载均衡 (run_rebalance_domains) */
	HRTIMER_SOFTIRQ,       /* hrtimer 软化过期 */
	RCU_SOFTIRQ,           /* RCU 回调 —— 注释: RCU 应永远排最后 */
	NR_SOFTIRQS
};
```

编号即优先级：`__do_softirq()` 从低位（HI）向高位（RCU）扫描执行——同轮 pending 中 HI_SOFTIRQ 的 tasklet 先于网络收包，RCU 永远垫底。**新增编号会改变 /proc/softirqs 的列序**，所以历史上编号只增不改。

注册与触发：

```c
// kernel/softirq.c (v6.6)
void open_softirq(int nr, void (*action)(struct softirq_action *))
{
	softirq_vec[nr].action = action;
}

inline void raise_softirq_irqoff(unsigned int nr)   /* 调用方须关中断 */
{
	__raise_softirq_irqoff(nr);        /* or_softirq_pending(BIT(nr)) */
	if (!in_interrupt())
		wakeup_softirqd();         /* 在进程上下文 raise: 直接叫醒 ksoftirqd */
}

void raise_softirq(unsigned int nr)                  /* 自动管中断开关 */
{
	unsigned long flags;
	local_irq_save(flags);
	raise_softirq_irqoff(nr);
	local_irq_restore(flags);
}
```

`softirq_vec[NR_SOFTIRQS]` 是全局函数表。`raise_softirq` 只置位（µs 级便宜），执行被推迟到下一个检查点（中断退出、`local_bh_enable`、`do_softirq` 主动调用、或 ksoftirqd 被唤醒）。

### ⚙️ 执行路径：__do_softirq 的预算机制

第 3 章已给出入口：`irq_exit_rcu() → __irq_exit_rcu()` 在 `!in_interrupt() && local_softirq_pending()` 时 `invoke_softirq()` → 就地 `__do_softirq()`（或唤醒 ksoftirqd）。执行主体：

```c
// kernel/softirq.c (v6.6, 原文节选)
#define MAX_SOFTIRQ_TIME  msecs_to_jiffies(2)   /* 时间预算: 2ms */
#define MAX_SOFTIRQ_RESTART 10                  /* 轮次预算: 10 轮 */

asmlinkage __visible void __softirq_entry __do_softirq(void)
{
	unsigned long end = jiffies + MAX_SOFTIRQ_TIME;
	int max_restart = MAX_SOFTIRQ_RESTART;
	__u32 pending;
	...
	pending = local_softirq_pending();
	softirq_handle_begin();          /* preempt_count += SOFTIRQ_OFFSET 等 */
	in_hardirq = lockdep_softirq_start();

restart:
	set_softirq_pending(0);          /* ① 先清位图, 再开中断 */
	local_irq_enable();              /*    软中断执行期间允许硬中断进来 */

	h = softirq_vec;
	while ((softirq_bit = ffs(pending))) {   /* ② 从低位(HI)到高位(RCU) */
		h += softirq_bit - 1;
		vec_nr = h - softirq_vec;
		prev_count = preempt_count();

		kstat_incr_softirqs_this_cpu(vec_nr);   /* /proc/softirqs 计数 */
		trace_softirq_entry(vec_nr);
		h->action(h);                           /* ★ 执行 (硬中断开着!) */
		trace_softirq_exit(vec_nr);

		if (unlikely(prev_count != preempt_count())) {  /* ③ handler 漏配对 */
			pr_err("huh, entered softirq %u ... exited with %08x?\n", ...);
			preempt_count_set(prev_count);
		}
		h++;
		pending >>= softirq_bit;
	}

	local_irq_disable();

	pending = local_softirq_pending();        /* ④ 执行期间又有人 raise? */
	if (pending) {
		if (time_before(jiffies, end) && !need_resched()
		    && --max_restart)
			goto restart;                 /* ⑤ 预算内: 再来一轮 */
		wakeup_softirqd();                    /* ⑥ 超预算: 移交 ksoftirqd */
	}
	...
	softirq_handle_end();
}
```

五个设计细节：

1. **先清后开**：`set_softirq_pending(0)` 在 `local_irq_enable()` 之前——若反过来，清位前的新 raise 会被清掉（丢软中断）。执行中新 raise 的会落进重新读取的 pending，进下一轮。
2. **开中断执行**：softirq handler 运行时硬中断是开的（收包不能让网卡中断等 2ms）。代价是 handler 可能被硬中断嵌套打断——所以 handler 仍不可睡眠（上下文仍是 atomic）。
3. **双重预算（2ms + 10 轮）**：时间预算防长尾（如 NET_RX 大流量），轮次预算防 jiffies 停走时的时间预算失效（`stop_machine`/tick 停摆场景，注释明言这两条边界来自实验）。`need_resched()` 检查保证被抢占任务不让位过久。
4. **移交 ksoftirqd 的语义**：第 ⑥ 步不是"唤醒备用线程"，而是**把后续执行权完全交给 ksoftirqd**——就地执行退出后，剩余 pending 由 ksoftirqd 在**进程上下文**慢慢消化。两种执行体的分工：低负载时中断退出就地跑（零调度延迟），高负载时全部进 ksoftirqd（受调度器公平约束）。
5. **preempt_count 配对检查**：软中断 handler 不许改 preempt_count（禁 `spin_lock_bh` 之类漏配对），③ 是兜底自愈 + 报错。

### 🕐 软中断的全部执行检查点

软中断被 raise 后，**最近的检查点**决定它何时真正执行。四个检查点构成一张完整的"软中断调度网络"：

```
检查点                          触发条件                          执行者
1. irq_exit_rcu()           中断返回且 !in_interrupt()         被打断的任务(就地) 或 ksoftirqd
2. local_bh_enable()        bh 禁用计数归零且有 pending          开 bh 的任务(就地 do_softirq)
3. ksoftirqd 被唤醒          raise 发生在进程上下文/超预算移交     ksoftirqd/%u 线程
4. do_softirq() 显式调用     极少数内核入口(如 smp_call_function) 当前上下文
```

检查点 2 值得展开：`local_bh_disable()/local_bh_enable()` 夹住的临界区里 raise 的软中断，会在 **enable 那一刻**就地执行（若允许）——而不是等下一次硬中断退出。这保证了"bh 临界区内产生的软中断"不额外积压延迟，也解释了为什么网络栈收包路径里 `local_bh_disable` 区间的长度直接影响软中断时效。

检查点 1 的细分逻辑（把第 3 章 `invoke_softirq()` 的分支图放进 softirq 的全局视角）：

```
irq_exit_rcu → invoke_softirq
  非 RT:
    force_irqthreads 关闭 → __do_softirq() 就地(2ms/10轮预算)
    force_irqthreads 开启(threadirqs) → wakeup_softirqd()   ← RT 模拟/调试用
  PREEMPT_RT:
    始终 wakeup_softirqd()   ← 可调度性优先
```

`threadirqs` 参数在非 RT 内核上的意义正是"预演 RT 行为"：排查"软中断就地执行干扰实时任务"类问题时，先开 `threadirqs` 看延迟是否改善，即可定位问题是否出在软中断上下文。

### 👷 ksoftirqd：per-CPU 软中断守护线程

```c
// kernel/softirq.c (v6.6)
static struct smp_hotplug_thread softirq_threads = {
	.store			= &ksoftirqd,
	.thread_should_run	= ksoftirqd_should_run,
	.thread_fn		= run_ksoftirqd,
	.thread_comm		= "ksoftirqd/%u",
};
```

ksoftirqd 是 smpboot 框架创建的 per-CPU 内核线程，**普通调度类（SCHED_OTHER），nice 0**——这一点是其设计的灵魂：

- **避免实时任务被饿死**：若软中断总在硬中断尾部以"高优先级伪上下文"就地执行，`SCHED_FIFO` 任务可能被 NET_RX 打断得无法运行。交给 SCHED_OTHER 的 ksoftirqd 后，软中断与普通任务公平排队，实时任务随时可抢占它。
- **避免 softirq 抢占用户态造成抖动**：大量软中断若一直就地执行，用户任务会被"看不见的内核工作"反复插队；ksoftirqd 把这部分成本变成**可见、可调度、可记账**的普通任务。
- **空转守夜**：`run_ksoftirqd()` 循环 `local_bh_disable(); if (pending) __do_softirq(); ...`——在 ksoftirqd 里跑 `__do_softirq` 时预算同样生效（超限则让出 CPU 等下轮调度）。
- **`__this_cpu_read(ksoftirqd) == current` 分支**：`__do_softirq` 里 `rcu_softirq_qs()` 仅在"当前执行者就是 ksoftirqd"时报告 RCU 静默（quiescent state）——因为 ksoftirqd 是正经任务上下文。

PREEMPT_RT 下 ksoftirqd 承担**全部**软中断执行（第 3 章 `invoke_softirq` 的 RT 分支只 wake 不就地跑），且 RT 把 tasklet/部分 softirq 的实现改走别的路径（见下文）。

### 🔒 local_bh_disable/enable 与上下文判定

`local_bh_disable()` 的语义是"**本 CPU 上软中断不许就地执行**"（保护 per-CPU softirq 数据不被 softirq handler 并发访问），它通过 preempt_count 计数实现：

```c
// include/linux/preempt.h (v6.6)
#define SOFTIRQ_OFFSET          (1UL << SOFTIRQ_SHIFT)       /* 执行 softirq 时加 1 份 */
#define SOFTIRQ_DISABLE_OFFSET  (2 * SOFTIRQ_OFFSET)         /* bh disable 加 2 份! */

#define softirq_count()      (preempt_count() & SOFTIRQ_MASK)   /* !RT */
#define in_serving_softirq() (softirq_count() & SOFTIRQ_OFFSET) /* 奇数倍: 正在跑 */
#define in_softirq()         (softirq_count())   /* 非 0: bh 禁用或正在跑 */
#define in_interrupt()       (irq_count())       /* NMI|hardirq|softirq 任一 */
#define in_atomic()          (preempt_count() != 0)  /* 含 preempt_disable 部分 */
```

**SOFTIRQ_DISABLE_OFFSET = 2×SOFTIRQ_OFFSET 的巧妙之处**：`in_softirq()`（覆盖"禁 bh"和"正在跑"两种状态）与 `in_serving_softirq()`（只指"正在跑"）靠奇偶区分——`local_bh_disable` 加偶数份，`__do_softirq` 进出加 1 份。这让 softirq handler 内部能安全地 `local_bh_disable()`（例如再次进入临界区）而状态判定不混淆。

`local_bh_enable()` 的收尾有实际动作：计数归零时若本 CPU 有 pending softirq 且不在中断里，就地 `do_softirq()`（延迟的软中断在**临界区出口**被立刻消化，而不是傻等中断退出）。

PREEMPT_RT 的实现完全不同（`softirq_ctrl` + per-task `softirq_disable_cnt` + `local_lock`）：bh 禁用区**可被抢占**（这是 RT 的要求），真正的串行化由每 CPU 的 `local_lock` 在 softirq 实际执行时持有——被抢占的 bh 区不会阻塞 ksoftirqd 在别的时间点跑，因为 RT 上软中断本来就在 ksoftirqd 上下文执行。

RT 语义的完整对照表：

| 维度 | 非 RT | PREEMPT_RT |
| :--- | :--- | :--- |
| bh 禁用的载体 | preempt_count += 2×SOFTIRQ_OFFSET（不可抢占） | per-task 计数 + per-CPU `softirq_ctrl.cnt`（**可抢占**） |
| 串行化手段 | preempt_count 本身 | `local_lock(&softirq_ctrl.lock)`（执行期独占本 CPU） |
| 就地执行软中断 | 中断退出/bh enable 处 | **从不**（一律 ksoftirqd） |
| `in_softirq()` 判定 | `preempt_count() & SOFTIRQ_MASK` | `current->softirq_disable_cnt & SOFTIRQ_MASK`（per-task） |
| 软中断 handler 里被硬中断打断后 | 硬中断返回可继续 | 同左，但 handler 随时可被高优任务抢占 |
| 典型延迟形态 | 软中断长尾挤占用户态（µs~ms 抖动） | 软中断排队等调度（平均延迟升、抖动降） |

这张表解释了 RT 的本质取舍：**把"看不见的抖动"换成"可管理的排队"**。对吞吐型负载（大流量 NET_RX）RT 模式的平均延迟略升；对延迟敏感负载（要求 p99 有界），软中断线程化是不可缺的一环——这与第 6 章硬中断线程化是同一哲学的两半。

**softirq 的使用规则**（谁可以写 softirq handler）：
1. 编译期 `open_softirq()` 静态注册（子系统级：net、block、timer、rcu...），**驱动不能注册自己的 softirq**；
2. handler 在**raise 它的同一 CPU** 上执行（per-CPU 语义，数据结构按 per-CPU 设计，天然免大部分锁）；
3. 原子上下文：不可睡眠、不可 `GFP_KERNEL`、不可 `mutex_lock`；同一 softirq 在多核可并行（不同 CPU 各跑各的），所以跨 CPU 共享数据仍需自旋锁；
4. handler 里可以再 raise 自己/别人（NAPI 正是靠这个循环收包）。

以 NET_RX 的 NAPI 为例看这套规则的威力：网卡 hard handler 只做 `__napi_schedule()`（把 napi 结构挂上 per-CPU poll_list 并 raise NET_RX_SOFTIRQ）；`net_rx_action()`（NET_RX handler）随后在软中断里**轮询**收包——每设备每轮一个 budget（默认 64 包），没收完就再 raise 自己，让别的设备先吃 CPU。中断频率从"每包一中断"降到"每 64 包一软中断轮"，这就是 softirq + budget 分片在最高频路径上的标准用法（也是"handler 必须自己分片"规则的来源——`net_rx_action` 不能一次收完所有包，否则 2ms 预算形同虚设）。

### 📎 tasklet 完整剖析

tasklet 是 softirq 的"动态注册包装"：构建在 HI_SOFTIRQ/TASKLET_SOFTIRQ 之上，把"静态函数表"变成"每 CPU 两条链表"。6.6 的定义：

```c
// include/linux/interrupt.h (v6.6, 原文)
struct tasklet_struct
{
	struct tasklet_struct *next;
	unsigned long state;
	atomic_t count;          /* >0 = 禁用计数 */
	bool use_callback;
	union {
		void (*func)(unsigned long data);            /* 旧式 */
		void (*callback)(struct tasklet_struct *t);  /* 新式 tasklet_setup */
	};
	unsigned long data;
};

enum
{
	TASKLET_STATE_SCHED,     /* 已在队列中/已置起 */
	TASKLET_STATE_RUN        /* 正在执行 (SMP 串行化) */
};
```

**state 两位 + count 一个计数**构成全部控制状态：

- `TASKLET_STATE_SCHED`：`tasklet_schedule()` 用 `test_and_set_bit` 原子置位——**重复 schedule 是合并的**（已在队列就不再排队），执行前由 `tasklet_clear_sched()` 清除。这同时是 `tasklet_kill()` 的等待点。
- `TASKLET_STATE_RUN`：执行期间置位（`tasklet_trylock`），用于**跨 CPU 串行化**——同一 tasklet 可能刚在 CPU0 跑完、又被调度到 CPU1，RUN 位保证同一时刻全局只有一个实例在跑。这就是"tasklet 对自身严格串行"的实现。
- `count`：`tasklet_disable()` 递增（**已被 schedule 但被禁用的 tasklet 会被重新放回队列**），`tasklet_enable()` 递减。

执行引擎（挂在两个 softirq 上）：

```c
// kernel/softirq.c (v6.6, 原文节选)
static void tasklet_action_common(struct softirq_action *a,
				  struct tasklet_head *tl_head,
				  unsigned int softirq_nr)
{
	local_irq_disable();
	list = tl_head->head;              /* ① 整链摘下, 立刻放行新 schedule */
	tl_head->head = NULL;
	tl_head->tail = &tl_head->head;
	local_irq_enable();

	while (list) {
		struct tasklet_struct *t = list;
		list = list->next;

		if (tasklet_trylock(t)) {          /* 抢 RUN 位(跨CPU串行) */
			if (!atomic_read(&t->count)) { /* 未被禁用 */
				if (tasklet_clear_sched(t)) {
					... t->callback(t) / t->func(t->data);  /* ★ */
				}
				tasklet_unlock(t);
				continue;
			}
			tasklet_unlock(t);
		}
		/* ② 抢锁失败(别核在跑)或被禁用: 重新入队并再 raise 该 softirq */
		local_irq_disable();
		t->next = NULL;
		*tl_head->tail = t;
		tl_head->tail = &t->next;
		__raise_softirq_irqoff(softirq_nr);
		local_irq_enable();
	}
}

static __latent_entropy void tasklet_action(struct softirq_action *a)
{ tasklet_action_common(a, this_cpu_ptr(&tasklet_vec), TASKLET_SOFTIRQ); }

static __latent_entropy void tasklet_hi_action(struct softirq_action *a)
{ tasklet_action_common(a, this_cpu_ptr(&tasklet_hi_vec), HI_SOFTIRQ); }
```

API 族与典型用法：

```c
tasklet_setup(&t, callback);       /* 新式: callback(struct tasklet_struct *) */
tasklet_schedule(&t);              /* 挂 TASKLET_SOFTIRQ 队列 */
tasklet_hi_schedule(&t);           /* 挂 HI_SOFTIRQ 队列 (更急, 如声卡) */
tasklet_disable(&t);  tasklet_enable(&t);   /* 引用计数式禁用/恢复 */
tasklet_kill(&t);                  /* 等 SCHED 位清零并抢先置位防复活:
                                       while (test_and_set_bit(SCHED))
                                           wait_var_event(...) — 只能在进程上下文调 */
tasklet_unlock_spin_wait(&t);      /* 自旋等 RUN 位清零(被注记: 别在新代码用) */
```

PREEMPT_RT 上的 tasklet：RT 树把 tasklet 的大部分执行挪到 per-CPU 的 `ksoftirqd` 之外的专用线程化路径（RT 上 `tasklet_action_common` 不变，但 softirq 上下文本身已线程化），`tasklet_unlock_spin_wait` 在 RT 下改为 bh disable/enable 防活锁——源码注释直言这些等待接口"error prone"。

**tasklet 为什么被标记淘汰**（Documentation 里 "Do not use in new code"）：

0. 前置事实：6.6 中 tasklet 引擎原样保留（`tasklet_action`/`tasklet_hi_action` 挂在 TASKLET_SOFTIRQ/HI_SOFTIRQ 上），淘汰是**政策性**的（新代码禁止引入），而非机制性移除——存量驱动的迁移仍在进行；

1. **上下文错位**：tasklet 跑在软中断上下文（不可睡眠），但驱动需要的往往是"可睡眠的延迟工作"或"确定性的实时响应"——前者 workqueue 更合适，后者 threaded irq 更合适；tasklet 卡在中间两头不讨好。
2. **延迟不确定性**：排在 softirq 队列里，受 2ms/10 轮预算、ksoftirqd 调度、HI/普通两队列的相对顺序影响，最坏延迟无上界承诺；threaded irq（SCHED_FIFO 50）与专用 workqueue 可控得多。
3. **串行化的代价**：对自身串行使多核扩展性差——N 个实例只能一次跑一个；workqueue 可并行、per-CPU。
4. **API 面模糊**：state/count/kill 语义难正确使用，`tasklet_kill` 不能在中断上下文调、与 re-schedule 竞态的经典坑驱动作者反复踩。
5. 内核演进方向明确：高速路径留给 NAPI/专用 softirq，其余全部线程化（threaded irq、threaded workqueue）。新代码应选 workqueue（`INIT_WORK`）或 `request_threaded_irq`。

### 🆚 四种下半部机制深度对比

| 维度 | softirq | tasklet | workqueue | threaded irq |
| :--- | :--- | :--- | :--- | :--- |
| 执行上下文 | 软中断（atomic） | 软中断（atomic） | **进程（可睡眠）** | 内核线程 irq/NN（可睡眠） |
| 可否睡眠 | ❌ | ❌ | ✅（GFP_KERNEL/mutex） | ✅ |
| 注册方式 | 编译期 `open_softirq`（仅子系统） | 运行期动态 | 动态（work_struct） | `request_threaded_irq` |
| 并发度 | 同类跨 CPU 并行；同 CPU 与硬中断交错 | **对自身全局串行** | 可并行（多 worker） | 每 IRQ 一个线程 |
| 调度优先级 | 就地执行（伪高优先级）或 ksoftirqd(SCHED_OTHER) | 同 softirq | worker 池 SCHED_NORMAL | **SCHED_FIFO 50**（6.6 起） |
| 触发到执行延迟 | 最低（中断退出即跑） | 低（多一跳队列） | 慢（需唤醒 worker、可被抢占） | 中（需唤醒 irq 线程） |
| 典型用户 | NET_RX/TX、TIMER、RCU、BLOCK | 少量老驱动（声卡等） | 绝大多数驱动的延迟工作 | 现代驱动主力模式 |
| 一句话定位 | 子系统级高频引擎 | 遗留兼容，勿用 | "可以睡觉的 tasklet" | "设备专属下半部线程" |

### 📊 观测与调优

**/proc/softirqs**（每行一个 softirq，每列一个 CPU）：

```
                CPU0       CPU1       CPU2       CPU3
HI:             0          0          0          1        ← tasklet_hi 队列
TIMER:      823156     801223     798451     845612      ← hrtimer/timer
NET_TX:       4521       3987       4210       4102
NET_RX:    2231456    1987234    2100456    2054321      ← NAPI; 单列暴增=RSS 失衡
BLOCK:       12345      12987      12001      12555
IRQ_POLL:        0          0          0          0
TASKLET:      3456       3122       3355       3201
SCHED:      62345      61234      63521      61200      ← 负载均衡触发
HRTIMER:    51234      50123      52011      50432
RCU:        41234      42345      41555      42100      ← 每周期批处理
```

读法：
- **NET_RX 单 CPU 独高**：RSS/中断亲和没配好，全压一个核（配对 `ethtool -L`、`/proc/irq/N/smp_affinity`，见第 6 章）；
- **RCU 持续高**：回调风暴（大量文件/进程销毁、`call_rcu` 滥用）；
- **HI/TASKLET 活跃**：附近还有 tasklet 用户，迁移改造候选；
- 计数来自 `kstat_incr_softirqs_this_cpu()`，与 `/proc/stat` 的 `softirq` 总量（全 CPU 汇总）同源。另注意两点：计数只增不减（快照间做差才是速率）；`HI` 几乎恒为 0 是正常的（现代内核里 hi tasklet 用户所剩无几），它的存在更多是历史兼容。

**tracepoint**：`irq:softirq_entry` / `irq:softirq_exit`（vec_nr 参数）、`irq:softirq_raise`（谁 raise 的也能看到）；`irq:tasklet_entry/exit`（6.6 存在）。示例：

```bash
cd /sys/kernel/tracing
echo 1 > events/irq/softirq_entry/enable
echo 1 > events/irq/softirq_exit/enable
cat trace_pipe     # 观察每轮 softirq 的 vec_nr 分布与耗时( entry/exit 差 )

# 一次 entry/exit 配对即一轮"全部 pending softirq 跑完"——注意粒度是
# 整轮而不是单个 vec (单 vec 粒度请用 BPF: tracepoint:irq:softirq_entry)

# 看谁在 raise (定位"谁制造了 NET_RX 洪峰"):
echo 1 > events/irq/softirq_raise/enable
```

**调优要点**：
- 中断退出预算（2ms/10 轮）是编译期常量——不可 sysctl。真正的调优手段是**减少软中断压力**（NAPI 合并、RSS 分队列、RPS/XPS 把 NET_RX 导流到别的核）而不是改预算；
- PREEMPT_RT：软中断全部 ksoftirqd 化后，延迟敏感任务的软中断干扰大幅下降，但吞吐型负载（大流量收包）的延迟上升——RT 上用 `threadirqs` 组合思考（第 6 章）；
- `local_bh_disable()` 临界区过长会推迟软中断到出口才跑（`local_bh_enable` 的就地 do_softirq 是唯一释放点），网络栈内核态路径里常见这类长临界区的延迟放大讨论；
- softirq handler 里的 `cond_resched()` 不存在（atomic 上下文无效）——handler 必须自己分片（NAPI 的 budget 每轮 64 包正是此意）；
- 实验方法学：对比实验要固定变量——同负载下分别测"就地执行主导"（默认）与"ksoftirqd 主导"（`threadirqs`）的延迟分布，差异即软中断上下文对你工作负载的真实影响。

---

### 💭 思考题

1. `__do_softirq()` 为什么必须在 `local_irq_enable()` **之前**执行 `set_softirq_pending(0)`？如果顺序交换，构造一个丢 softirq 的具体时序。
   <details><summary>提示</summary>交换后窗口内硬中断进来 raise 了 X 并返回，随后清位把 X 抹掉；而 X 的处理者以为"已置位，中断退出会跑"。</details>
2. ksoftirqd 故意用 SCHED_OTHER 而不是 SCHED_FIFO。若改成 FIFO 99，对一台跑 `SCHED_FIFO 30` 实时任务的机器会发生什么？
   <details><summary>提示</summary>软中断风暴（NET_RX 洪泛）时 ksoftirqd(FIFO99) 会无限抢占 FIFO30 任务——把"下半部"变成比任何业务都高的实时特权，正好违背其设计初衷。</details>
3. 同一个 tasklet 在 CPU0 执行期间被 CPU1 `tasklet_schedule()`。描述 `TASKLET_STATE_SCHED`/`RUN` 两位的完整变化序列，以及 CPU1 上的行为。
   <details><summary>提示</summary>CPU0: trylock 置 RUN → clear_sched（清 SCHED）→ 执行 → unlock 清 RUN。CPU1: schedule 时 SCHED 位已被清 → 成功入队 raise softirq；执行到它时若 RUN 仍置位（CPU0 还在跑）→ 重新入队下一轮。</details>
4. 为什么 `in_softirq()` 把"bh 被禁"也算在内，而 `in_serving_softirq()` 只算真正在跑？各服务于什么断言？
   <details><summary>提示</summary>per-CPU softirq 数据的临界区既包括 softirq handler 本身，也包括显式 local_bh_disable 的使用者；前者只用于执行者自我识别（如 rcu_softirq_qs 的条件之一）。</details>
5. 6.6 中 irq 线程固定 SCHED_FIFO 50，ksoftirqd 却固定 SCHED_OTHER。前者为什么需要"高于一切普通任务"，后者为什么必须"不高于"？
   <details><summary>提示</summary>irq 线程代表单设备的事件传递（背压有限且必须及时），ksoftirqd 聚合了全机无限供给的软中断工作（不设限地高优先 = 饿死一切）。</details>

---

*上一章：[irq_domain 与中断号映射](04-irq_domain与中断号映射.md)　|　下一章：[线程化中断与中断亲和性](06-线程化中断与中断亲和性.md)*
