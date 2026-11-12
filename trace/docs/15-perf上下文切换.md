# perf 上下文切换：PMU 的任务边界（Linux 6.6）

> 阅读目标：吃透 perf 在调度切换时做什么——`__perf_event_task_sched_out/in`（finish_task_switch 的被调者，[scheduler/16](../../scheduler/docs/16-观测扇出模式.md) 六连扇出中最重的一支）：三级跳（ctx 查找→克隆快速路径的**上下文交换**→慢路径完整切出/切入）、pinned/flexible 两组的事件调度顺序（"cpu pinned, task pinned, cpu flexible, task flexible" 注释原话）、perf_sched_cb_usages 的 PMU 回调侧门、context_equiv 的克隆判定。全部对照 v6.6（kernel/events/core.c 13810 行、include/linux/perf_event.h:907）。前置：[06-perf 与 BPF 观测](06-perf与BPF观测.md)（PMU 与采样管线）｜[scheduler/16-观测扇出模式](../../scheduler/docs/16-观测扇出模式.md)（扇出全景）。

---

## 一、入口：sched_out/in 的三件事（core.c:3625-3637 / :3979-3989）

```c
void __perf_event_task_sched_out(struct task_struct *task, struct task_struct *next)
{
	if (__this_cpu_read(perf_sched_cb_usages))
		perf_pmu_sched_task(task, next, false);   /* ① PMU 私有侧门 */

	if (atomic_read(&nr_switch_events))
		perf_event_switch(task, next, false);     /* ② switch 事件消费者 */

	perf_event_context_sched_out(task, next);    /* ③ 主体：ctx 切出 */

	perf_cgroup_switch(next);                    /* ④ cgroup 事件的 PMU 状态 */
}
```

`__perf_event_task_sched_in`（:3979）对称三件（context_sched_in / switch 事件 / pmu_sched_task）。调用链：`prepare_task_switch → perf_event_task_sched_out`（sched 侧）与 `finish_task_switch → perf_event_task_sched_in`。

## 二、ctx 模型：每任务的 perf 账本（perf_event.h:907-940+）

```c
struct perf_event_context {            /* task->perf_event_ctxp 指向 */
	raw_spinlock_t		lock;           /* 临界区主锁 */
	struct mutex		mutex;
	struct list_head	pmu_ctx_list;   /* 各 PMU 的子上下文 */
	struct perf_event_groups	pinned_groups;   /* ★固定组：不许迁移 */
	struct perf_event_groups	flexible_groups; /* ★灵活组：可轮转/抢占 */
	struct list_head	event_list;
	int			nr_events;
	int			is_active;
	struct task_struct	*task;          /* 归属任务（克隆交换时改写）*/
	...
};
```

**两组分野是整个切换机制的心脏**：pinned 事件（如 precise ip 需要固定 CPU 的）必须"跟着 CPU 或任务钉死"；flexible 事件（普通计数）在资源不足时被轮转挤出——切换时两组的进出顺序有严格优先级。

## 三、sched_out 的克隆快速路径（core.c:3451-3525）

`perf_event_context_sched_out(task, next)` 的精华——两个任务若 perf 上下文是**克隆关系**（fork 时继承的计数组），直接**交换整个上下文**而不逐事件停启：

```c
parent = rcu_dereference(ctx->parent_ctx);
next_parent = rcu_dereference(next_ctx->parent_ctx);
if (next_parent == ctx || next_ctx == parent || next_parent == parent) {
	/* 注释原话（:3493-3498）："Looks like the two contexts are clones,
	   so we might be able to optimize the context switch. We lock both
	   contexts and check that they are clones under the lock" */
	raw_spin_lock(&ctx->lock);
	raw_spin_lock_nested(&next_ctx->lock, SINGLE_DEPTH_NESTING);
	if (context_equiv(ctx, next_ctx)) {
		perf_ctx_disable(ctx);
		/* PMIs are disabled; ctx->nr_pending is stable. 注释原话 */
		if (local_read(&ctx->nr_pending) ||
		    local_read(&next_ctx->nr_pending))
			goto inside_switch;       /* 有 pending 事件：放弃快速路径 */
		WRITE_ONCE(ctx->task, next);
		WRITE_ONCE(next_ctx->task, task);
		perf_event_swap_task_ctx_data(ctx, next_ctx);
		RCU_INIT_POINTER(task->perf_event_ctxp, next_ctx);
		RCU_INIT_POINTER(next->perf_event_ctxp, ctx);   /* ★交换指针 */
		do_switch = 0;               /* 跳过完整切出 */
		perf_event_sync_stat(ctx, next_ctx);
	}
	...
}
if (do_switch) { ... 完整路径：停事件/记录值/清 cpuctx->task_ctx ... }
```

★收益：克隆任务对（同 fork 组）之间切换**零 PMU 编程**——上下文整体易主（计数器不断流），继承组的测量连续性得以保持。

## 四、sched_in 的优先级协议（core.c:3910-3968）

```c
static void perf_event_context_sched_in(struct task_struct *task)
{
	cpuctx = this_cpu_ptr(&perf_cpu_context);
	ctx = rcu_dereference(task->perf_event_ctxp);
	if (cpuctx->task_ctx == ctx) { ... 快速：ctx 已在此 CPU 上 ... }

	perf_ctx_lock(cpuctx, ctx);
	/* :3936-3938 附近注释（"We must check ctx->nr_events"）——与
	   perf_install_in_context() 的串行化 */
	if (!ctx->nr_events)
		goto unlock;

	perf_ctx_disable(ctx);
	/* ★:3940-3946 注释原话：
	   "We want to keep the following priority order:
	    cpu pinned (that don't need to move), task pinned,
	    cpu flexible, task flexible.
	    However, if task's ctx is not carrying any pinned events,
	    no need to flip the cpuctx's events around." */
	if (!RB_EMPTY_ROOT(&ctx->pinned_groups.tree)) {
		perf_ctx_disable(&cpuctx->ctx);
		ctx_sched_out(&cpuctx->ctx, EVENT_FLEXIBLE);  /* 让位：
		                                                 cpu flexible
		                                                 给 task pinned */
	}
	perf_event_sched_in(cpuctx, ctx);                  /* 两组依序入 */
	...
}
```

优先级链的含义：**任务的 pinned 事件挤掉 CPU 的 flexible 事件**；任务的 flexible 与 CPU 的 flexible 竞争（轮转裁决）。`cpuctx->task_ctx` 记录"当前哪个任务的 ctx 驻留本 CPU"——sched_out 已把它清空，此处重新装上。

## 五、PMU 侧门：perf_sched_cb_usages

`__this_cpu_read(perf_sched_cb_usages)` 非零时 `perf_pmu_sched_task` 额外调用——给**需要在任务边界做私有维护的 PMU**（如 AMD IBS 的缓冲清点）挂 per-CPU 回调（`pmu->sched_task`）。perf 子系统平时不拖累普通切换，仅当注册方声明（`perf_sched_cb_inc`）才走此路——扇出模式"每支自门控"的又一例（[scheduler/16 §三](../../scheduler/docs/16-观测扇出模式.md)）。

## 六、成本画像

```
无 perf 事件：task->perf_event_ctxp == NULL → sched_out/in 各一次
  NULL 检查即返回（~1-2ns）
任务有 ctx 且克隆对：快速路径双锁 + 指针交换（无 PMU 编程）
任务有 ctx 非克隆：完整路径——ctx_sched_out/in 逐事件
  pmu->del/add（每次都是 MSR/寄存器写）+ cpuctx 的 flexible 让位
监测代价的实践结论：per-task 计数 + 高切换率 = PMU 编程风暴
  （这正是 perf stat 对短命任务开销大的底层原因）
```

## 七、观测实战

```bash
# 1) 谁在付 ctx 切换税
perf stat -e context-switches,cpu-clock -- <workload>
# 任务寿命极短时对比: perf stat -- fork bomb 一秒钟的上下文成本

# 2) switch 事件（nr_switch_events 消费者）
perf record -e sched:sched_switch -a sleep 1
#   perf 自身也消费（perf_event_switch）—— 事件溯源的套娃

# 3) 克隆关系的验证（fork 继承组）
perf record -e cycles -- perf record -e cycles ...   # 嵌套测量的连续性

# 4) 源码侧断点（实验环境）
bpftrace -e 'kprobe:perf_event_context_sched_out /comm=="X"/ { @[pid]=count(); }'
```

## 八、思考题

1. 克隆快速路径里为什么先 `perf_ctx_disable(ctx)` 再查 `nr_pending`？（头注释提示 "PMIs are disabled; ctx->nr_pending is stable"——关闭的是什么中断？为何必须先关才能读？）
2. 上下文交换（RCU_INIT_POINTER 互换）为什么不用 kmalloc 或复制——账本换主后**事件值**归谁？（提示：计数器硬件没动，读数连续——统计意义上归"这个 CPU 时间段上的任务"。）
3. 优先级链 "cpu pinned, task pinned, cpu flexible, task flexible" 的两端由什么保证？（提示：pinned 组 RB 树非空才触发让位——注释说 "no need to flip"；谁来检查 task pinned 真能上？）
4. `cpuctx->task_ctx` 的快路径（ctx 未变）什么时候成立？（提示：同任务被连续调度两次/同 ctx 的任务对在克隆交换后。）
5. perf_sched_cb_usages 的 per-CPU 计数设计意味着什么？（提示：部分 CPU 上有 IBS 用户时，只有那些 CPU 走侧门——对照 static key 的全局性。）
6. perf stat 一个 1ms 短命进程为何不准？（提示：完整路径的 PMU 编程成本 vs 事件窗口；采样计数 vs 继承组。）

---

> 回看：[06-perf 与 BPF 观测](06-perf与BPF观测.md)（PMU 家族与采样管线）｜[scheduler/16-观测扇出模式](../../scheduler/docs/16-观测扇出模式.md)（prepare_task_switch 六连扇出）｜[trace/02-tracepoint 机制](02-tracepoint机制.md)（sched_switch tracepoint——第②件事的对面）｜[cgroup 分册](../../cgroup/README.md)（perf_cgroup_switch 的 cgroup 事件）
