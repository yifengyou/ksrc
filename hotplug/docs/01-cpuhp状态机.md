# cpuhp 状态机：CPU 热插拔的统一秩序（Linux 6.6）

> 阅读目标：掌握 4.x 后统一化的 cpuhp 回调框架——状态机模型（`enum cpuhp_state` 从 `CPUHP_OFFLINE(0)` 到 `CPUHP_ONLINE`）、`struct cpuhp_step` 的单实例/多实例回调、上下线的执行者分工（控制 CPU 走 PREPARE 段、被插 CPU 自己的 `cpuhp/%u` 线程走 AP 段）、失败回滚、以及 `takedown_cpu` 的 stop_machine 终局。前置：[init 分册 smpboot](../../init/docs/02-start_kernel详解.md)、[workqueue 分册](../../workqueue/README.md)（CPUHP_AP_WORKQUEUE_ONLINE 的消费方）。

---

## 一、状态机模型（include/linux/cpuhotplug.h + kernel/cpu.c，已核对 6.6）

```c
struct cpuhp_step {
	const char *name;
	union { int (*single)(unsigned int cpu);            /* 单实例 */
		int (*multi)(unsigned int cpu, struct hlist_node *node); } startup;
	union { int (*single)(unsigned int cpu);
		int (*multi)(unsigned int cpu, struct hlist_node *node); } teardown;
	struct hlist_head list;      /* multi_instance 的注册链 */
	bool cant_stop;              /* 该状态禁止 stop（关键步骤） */
	bool multi_instance;         /* 允许同名多回调（如 perf/kvm 各挂各的） */
};
static struct cpuhp_step cpuhp_hp_states[CPUHP_ONLINE + 1];  /* 状态=下标 */
```

**状态枚举三段式**（核对自 cpuhotplug.h）：

```
① PREPARE 段（控制 CPU 上执行，CPUHP_OFFLINE=0 起）
   CPUHP_CREATE_THREADS → … → CPUHP_BP_PREPARE_DYN(+20 动态预留)
   → CPUHP_BRINGUP_CPU  ★分水岭：__cpu_up(cpu, idle) 真正启动 AP
② AP 段（被插 CPU 上执行）
   CPUHP_AP_ONLINE → CPUHP_TEARDOWN_CPU → CPUHP_AP_ONLINE_IDLE
   ★AP 报到点（start_kernel → cpuhp_online_idle）
   → CPUHP_AP_SMPBOOT_THREADS → CPUHP_AP_WORKQUEUE_ONLINE
   → CPUHP_AP_ACTIVE → CPUHP_ONLINE
   （CPUHP_AP_ONLINE_DYN…DYN_END 动态预留段）
```

每个 per-cpu `cpuhp_cpu_state st` 携带游标：`st->state`（当前态）与 `st->target`（目标态），`bringup = st->state < target`——回调按状态序逐个执行。

## 二、执行者分工：控制 CPU 与 `cpuhp/%u` 线程

```
cpu_up → _cpu_up（持 cpu_hotplug_lock 写锁）：
  PREPARE 段由控制 CPU 直接 cpuhp_up_callbacks 逐状态调用
  → CPUHP_BRINGUP_CPU: bringup_cpu → __cpu_up → AP 从 secondary 入口
    启动 → start_kernel 尾声 cpuhp_online_idle（st=AP_ONLINE_IDLE，
    唤醒自己的热插拔线程）
  AP 段由目标 CPU 自己的 smpboot 线程推进：
    cpuhp_threads = { .thread_fn = cpuhp_thread_fun,
                      .thread_comm = "cpuhp/%u", ... }（已核对）
    cpuhp_should_run 判断 st->state != st->target → 逐状态
    cpuhp_invoke_callback_range(bringup, …, st, target)
失败回滚：cpuhp_up_callbacks 失败 → WARN_ON(反向 invoke range)——
    半路残局必须倒着收拾；nofail 变体只用于下线路径
```

`cpuhp_invoke_callback` 按 `multi_instance` 走 `st->single` 或遍历 `hlist` 调 `multi`（tracepoint `cpuhp/multi_enter` 区分）。

## 三、下线终局：takedown_cpu（已核对 6.6 全函数）

```
CPUHP_TEARDOWN_CPU：
  kthread_park(st->thread)            ← 先把自己的 cpuhp 线程拴住
  irq_lock_sparse()                   ← irq_desc 分配全局锁
  stop_machine_cpuslocked(take_cpu_down, NULL, cpumask_of(cpu))
    ← 经典 stop_machine：全系统停机，把目标 CPU 从调度器摘除、
      迁移走它的任务/中断（对照 interrupt 分册 irq_migrate）
  wait_for_ap_thread(st, false)       ← 等 AP 侧线程走到终态
  BUG_ON(st->state != CPUHP_AP_IDLE_DEAD)
  hotplug_cpu__broadcast_tick_pull(cpu) ← tick 广播接管（timers 分册）
  __cpu_die / arch_cpuhp_cleanup_dead_cpu
6.6 新机制：cpuhp_ap_sync_alive() + arch_cpuhp_sync_state_poll()
  —— AP 进入 idle_dead 前与控制 CPU 的同步等待（替代老忙等）
```

## 四、锁序与 API（写驱动的人必须知道的）

```
cpu_hotplug_lock（percpu_rw_sem）：全局热插拔串行；普通读者用
  cpus_read_lock()（热插拔期间拒绝并发改 per-cpu 数据）
注册 API：cpuhp_setup_state(state, name, startup, teardown, multi)
  挑状态：提前的用 CPUHP_BP_PREPARE_DYN；AP 期用 CPUHP_AP_ONLINE_DYN
  cant_stop 步骤（如 stop_machine 自身）不可中断
观测：
  /sys/devices/system/cpu/hotplug/states   # 全状态表（名字+可否）
  /sys/devices/system/cpu/cpuN/online      # 写 0/1 触发上下线
  tracefs: cpuhp/enter, cpuhp/multi_enter, cpuhp/exit
  cpu_hotplug_disable()                    # 冻结热插拔（CPUHP 防御）
```

## 五、思考题

1. PREPARE 段为什么必须在**控制 CPU** 上跑，而 AP 段必须由**被插 CPU 自己**跑？（提示：PREPARE 建的东西是 AP 启动的输入；AP 段的回调要操作 per-cpu 数据。）
2. `cpuhp/%u` 线程被 park 之后再 stop_machine——顺序为什么不能反？（推演 stop_machine 后线程还在目标 CPU 上运行的下场。）
3. 回滚路径为什么 `WARN_ON(nofail 反向 invoke)`？（对照：下线半路失败时哪些 teardown 不能容忍再次失败。）
4. `cant_stop` 的存在理由？（找出 stop_machine 自身所在的状态步。）
5. 动态状态段（`*_DYN + 20`）满了怎么办？（对照 cpuhp_remove_state 的复用语义。）

---

> 下一章：[内存热插拔 memhp](02-内存热插拔memhp.md) —— 另一半：pgdat/zone 的动态生灭。
