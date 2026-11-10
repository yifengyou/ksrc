# 工作队列（workqueue）分册导读

> 基于 Linux 6.6 源码（`kernel/workqueue.c`，6816 行；`kernel/workqueue_internal.h`；`include/linux/workqueue.h`）。workqueue 是内核**最常用**的异步执行机制：把 `work_struct` 排队，由 kworker 内核线程在**进程上下文**执行——能睡眠、能拿 mutex、能做任意阻塞操作。本分册聚焦 CMWQ（Concurrency Managed Workqueue）的机制内核与工程实践。

## 为什么值得深读

workqueue 的特殊地位在于它是**多个子系统的交汇点**：中断下半部的进程上下文出口、调度器并发管理的最深度用户（三个调度钩子）、suspend 冻结协议的参与者、内存回收前向推进的保障对象（rescuer）、以及内核里最精巧的"一个原子字编码世界"（`work->data`）。读懂它，同步、调度、PM、NUMA 的知识会全部被串起来。

## 学习路线

```
00 一页速查                     全部浓缩：三件套/flags/数值/观测（含
                                 ★6.6 无 workqueue debugfs 勘误）
01 CMWQ 设计与 worker pool      机制地基：四件套结构、worker 主循环、
   ★ 必读                       并发管理算法、worker 生死、与调度器共生
        │
02 work 生命周期与 flush/cancel  提交全链（PENDING/last_pool 粘性/限流分叉）、
   ★ 必读                       cancel 偷取协议、6.6 双轨 flush（color 仍在
        │                       + wq_barrier）、观测 sysfs/tracepoint
        │
03 unbound 池与 affinity scope   长任务归宿：attrs 池索引、6.6 pod 模型、
   选读（做性能/NUMA 必读）       affn_strict、cpumask 治理与热更新
        │
04 使用规范与死锁分析            WQ_* 全表、system_* 选型、五类死锁、
   ★ 必读（驱动作者）           检测工具箱、十条军规、观测实战
```

建议顺序 01→02→04，03 按需。每章结构：概述 → 机制详解（带真实源码）→ ASCII 图 → 深挖难点 → 观测调试 → 思考题（附提示）。

## 与其他分册的联系

workqueue 不是孤岛——它上承中断的"下半部"需求，下接调度器的执行资源，横跨同步、电源、定时器三个子系统。一张概念地图：

```
                    ┌─ softirq/tasklet（原子上下文，不能睡眠）── interrupt 分册
硬件中断 ──下半部──┤
                    └─ work_struct（进程上下文，可睡眠）─────── 本分册
                              │ queue_work
                              ▼
                    kworker（CFS 任务，nice 0 / -20）────── scheduler 分册
                              │ schedule()/tick 的钩子：
                              │ wq_worker_running/sleeping/tick
                              ▼
                    worker_pool（并发管理 nr_running）
                              │ delayed_work 的 delay 段
                              ▼
                    timer（IRQSAFE，时间轮/hrtimer）───────── timers 分册
```

| 分册 | 交汇点 | 从哪头看 |
| :--- | :--- | :--- |
| [中断分册](../interrupt/README.md) | [softirq 与 tasklet](../interrupt/docs/05-softirq与tasklet.md)：tasklet 同样是"延迟执行"，但运行在**原子上下文**（不能睡眠）；workqueue 是它的进程上下文对偶。经典迁移路径：tasklet → `WQ_UNBOUND` work（内核正在清退 tasklet） | 中断侧：上半部只留最小工作，其余 queue_work |
| [调度器分册](../scheduler/README.md) | kworker 是 CFS 任务：per-CPU worker 的 `kthread_set_per_cpu` 身份、`wq_worker_running/sleeping/tick` 三个钩子挂在 `kernel/sched/core.c` 的 schedule/scheduler_tick 路径、TASK_IDLE 睡眠不进 loadavg、WQ_HIGHPRI = nice -20 的池。参见调度器分册 [04-调度时机与上下文切换](../scheduler/docs/04-调度时机与上下文切换.md) | 调度侧：workqueue 是"sched 钩子的最大客户" |
| [timers 分册](../timers/README.md) | `delayed_work` 的 delay 由普通 timer（**IRQSAFE**）承担：`delayed_work_timer_fn` 到期后走 `__queue_work`。timer 的时间轮细节见 [时间轮与低分辨率定时器](../timers/docs/04-时间轮与低分辨率定时器.md) | timer 侧：wq 是 timer 回调"转进程上下文"的常用跳板 |
| [locking 分册](../locking/README.md) | lockdep 对 wq 的双层伪锁（wq_completion/work_completion）；flush/cancel 与持锁的死锁矩阵 | 同步侧：work 函数 = 一个隐式临界区 |
| power 分册 | WQ_FREEZABLE 与 suspend 冻结协议（max_active 归零法）；pm_wq 是 workqueue 最大的用户之一 | PM 侧：runtime suspend/resume 回调全跑在 pm_wq |

## core-api/workqueue.rst 镜像导读

内核自带文档 [Documentation/core-api/workqueue.rst](https://docs.kernel.org/core-api/workqueue.rst)（Tejun Heo，2010 起维护）是官方正典，本分册的机制描述与它对齐，并在三处显著超越/更新它：

| workqueue.rst 章节 | 本分册对应 | 差异 |
| :--- | :--- | :--- |
| Why CMWQ / Design | 01 章 §1 | 补齐 6.6 现状（WORKER_RUNNING 已删、idle cull 重构、自动 CPU-intensive） |
| Guidelines（作者指南） | 04 章 §1~§3、§6 | rst 是"规矩"清单，这里给"为什么"+ 死锁推演 + 军规 |
| Affinity Scopes（6.6 新章节） | 03 章 §3 | 补默认值变更（cache）、sysfs 真实路径、repatriate 观测 |
| Debugging | 各章观测节 | 补 wq_monitor.py/wq_dump.py（drgn 工具，2023 新增）、tracepoint 实战 |

rst 未覆盖而本分册展开的：flush 颜色级联的逐行拆解（02 章 §4）、cancel 的 `-ENOENT` 汇聚协议、destroy_workqueue 的完整状态机（04 章 §5.4）。

## 源码索引（6.6）

| 文件 | 内容 |
| :--- | :--- |
| `kernel/workqueue.c`（6816 行） | 唯一核心：全部四件套定义、提交/执行/flush/cancel、池管理、attrs、hotplug、冻结、watchdog、sysfs、init 三阶段 |
| `kernel/workqueue_internal.h`（83 行） | `struct worker`、`current_wq_worker()`、四个调度钩子声明 |
| `include/linux/workqueue.h`（729 行） | `work_struct`/`delayed_work`/`rcu_work`/`workqueue_attrs`、WQ_* 标志、API 声明与内联（queue_work 家族的 DocBox 值得精读） |
| `include/trace/events/workqueue.h` | 4 个 tracepoint |
| `kernel/sched/core.c` | 钩子调用点：scheduler_tick(5672)/sched_submit_task(6735)/sched_update_worker(6758) |
| `tools/workqueue/wq_monitor.py`、`wq_dump.py` | drgn 观测工具 |

关键函数速查（行号为 6.6 实际行号）：`__queue_work` 1702、`process_one_work` 2536、`worker_thread` 2729、`rescuer_thread` 2824、`__flush_workqueue` 3131、`cancel_work_sync`→`__cancel_work_timer` 3446、`apply_workqueue_attrs` 4472、`destroy_workqueue` 4786、`workqueue_init_early` 6524。

## 常见误区清单（本分册帮你拔掉的刺）

| 误区 | 正解 | 出处 |
| :--- | :--- | :--- |
| "flush_workqueue 已经改用 barrier，颜色没了" | 6.6 双轨并存：整队 flush 走颜色，单 work 走 barrier；颜色 6.9 才删 | 02 章 §4 |
| "workqueue 的 sysfs 在 /sys/kernel/workqueue" | 实为 `/sys/bus/workqueue/devices/`（3.9 起）+ `/sys/devices/virtual/workqueue/cpumask` | 02 章 §5.1 |
| "destroy_workqueue 失败返回 -EBUSY" | 6.6 是 void；残留时 WARN + 放弃销毁（wq 泄漏但系统存活） | 04 章 §5.4 |
| "WORKER_RUNNING 是 worker 的状态标志" | 6.6 已删；并发状态由 nr_running + worker->sleeping 表达 | 01 章 §2.4 |
| "unbound 的默认 affinity scope 是 NUMA" | 6.6 起默认 **cache**（LLC） | 03 章 §3.1 |
| "cancel_work 可以从原子上下文调，所以它什么都能干" | 非 sync 版不保证"不在跑"；要等执行结束必须 sync 版（进程上下文） | 02 章 §3.2 |
| "queue_work 返回 false 说明 work 执行完了" | false 只说明"已 pending/执行中"；执行可能才刚开始 | 02 章 §1.3 |
| "WQ_HIGHPRI 是 RT 优先级" | 只是 CFS nice -20；RT 任务可无限推迟它 | 04 章 §一 |
| "BH workqueue 是 workqueue 的一种标志" | 6.6 没有 BH wq（6.10+ 才有）；别把新特性当旧知识 | 04 章 §例 5 |

## 自验清单（动手才算学会）

1. `echo w > /proc/sysrq-trigger; dmesg | tail -100`：找到任意两个 wq 的 pwq 行，说出 active=x/y 的两个数字各是什么（nr_active/max_active）。
2. 写一个模块：自建 wq（UNBOUND|MEM_RECLAIM），queue 一个 `msleep(100)` 的 work ×100，用第 2 章的 bpftrace 一行流验证 P99≈100ms。
3. 对同一模块改 `affinity_scope` 为 numa/cache 各跑一遍（写 sysfs），用 `wq_dump.py` 对比池数变化。
4. 用 `perf record -e workqueue:workqueue_queue_work -a -- sleep 10` 统计谁在投递、投到哪个 CPU。
5. 复现例 3 死锁（模块里故意持锁 cancel），观察 lockdep 报告的伪锁依赖链，再对照 04 章 §5.1 解读。

---

> 📌 主线后续：6.9 起颜色机制被移除（flush 全面转 barrier）、6.10 起 BH workqueue 落地——本分册描述的是 6.6 的**当前事实**，已在正文各处标注"将来时"差异点，避免与新版资料互相污染。
