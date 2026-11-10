# tick 与 NOHZ（Linux 6.6）

> 阅读目标：理清 `tick_device`/`tick_sched` 的结构与关系；走通周期模式 `tick_periodic` 的完整串联；解剖 NOHZ_IDLE 的停 tick/补偿全流程（`tick_nohz_next_event` 的下一事件计算、`ts->idle_expires`、退出时的补记账）；理解 NOHZ_FULL 的 housekeeping/context tracking/tick 依赖计数四层掩码，以及为什么 RCU/watchdog/printk 离不开 tick；评估 NOHZ 对调度器与 PELT 的影响；掌握 broadcast tick 的 C3STOP 唤醒链与 CONFIG_HZ 的取舍。

代码出自 Linux 6.6 `kernel/time/tick-{common,sched,oneshot,broadcast}.c`、`kernel/time/tick-sched.h`、`include/linux/tick.h`、`kernel/Kconfig.hz`。

---

## 🧱 一、两个结构：设备与调度状态

```c
// kernel/time/tick-sched.h (v6.6)
enum tick_device_mode { TICKDEV_MODE_PERIODIC, TICKDEV_MODE_ONESHOT };

struct tick_device {
	struct clock_event_device *evtdev;    /* 本 CPU 的事件设备 */
	enum tick_device_mode mode;           /* 周期 or 单次 */
};

enum tick_nohz_mode { NOHZ_MODE_INACTIVE, NOHZ_MODE_LOWRES, NOHZ_MODE_HIGHRES };

struct tick_sched {                        /* 每 CPU 一份 (tick_cpu_sched) */
	/* 通用标志 */
	unsigned int	inidle : 1;         /* 正处于 tick-idle 流程(idle 循环置位) */
	unsigned int	tick_stopped : 1;   /* ★ tick 已停(NOHHZ 的核心状态) */
	unsigned int	idle_active : 1;    /* idle 计时进行中(irq 打断时清) */
	unsigned int	do_timer_last : 1;  /* 睡前曾是 do_timer 值班者 */
	unsigned int	got_idle_tick : 1;  /* 睡着时被动收到过一次 tick */
	unsigned int	stalled_jiffies;    /* jiffies 停滞检测(见第五节) */
	unsigned long	last_tick_jiffies;
	struct hrtimer	sched_timer;        /* hres/nolowres 下的 tick 本体(第 5 章) */
	ktime_t		last_tick;          /* 停 tick 前的相位锚(恢复网格用) */
	ktime_t		next_tick;          /* 停 tick 期间编程的下个事件 */
	unsigned long	idle_jiffies;       /* 进 idle 时的 jiffies(补记账基准) */
	ktime_t		idle_waketime, idle_entrytime;   /* idle 时长统计 */
	seqcount_t	idle_sleeptime_seq;
	/* tick 停止 */
	enum tick_nohz_mode	nohz_mode;
	unsigned long	last_jiffies;       /* 计算 next event 时的 jiffies 快照 */
	u64		timer_expires_base; /* 计算 next event 时的 mono 基准 */
	u64		timer_expires;      /* 预期到期(=编程给硬件的值) */
	u64		next_timer;         /* 下个时间轮到期(仅调试) */
	ktime_t		idle_expires;       /* 睡眠截止(仅调试: /proc/timer_list) */
	unsigned long	idle_calls, idle_sleeps;    /* 统计 */
	ktime_t		idle_exittime, idle_sleeptime, iowait_sleeptime;
	/* NOHZ_FULL */
	atomic_t	tick_dep_mask;      /* 本 CPU 的 tick 依赖位 */
	unsigned long	check_clocks;       /* clocksource 变更通知位 */
};
```

`tick_device` 是"硬件在手上的样子"，`tick_sched` 是"策略状态机"。全局另有三件套：`tick_do_timer_cpu`（谁负责推 jiffies，初值 `TICK_DO_TIMER_BOOT`，可被置 `TICK_DO_TIMER_NONE` 表示无人值班）、`tick_next_period`（理想 tick 网格锚）、`jiffies_lock`+`jiffies_seq`。

---

## ⏰ 二、周期模式：一切的基准线

```c
// kernel/time/tick-common.c (v6.6)
static void tick_periodic(int cpu)
{
	if (tick_do_timer_cpu == cpu) {        /* 全系统每 tick 只有一个 CPU 做这事 */
		raw_spin_lock(&jiffies_lock);
		write_seqcount_begin(&jiffies_seq);
		tick_next_period = ktime_add_ns(tick_next_period, TICK_NSEC);
		do_timer(1);                       /* jiffies_64++ + calc_global_load */
		write_seqcount_end(&jiffies_seq);
		raw_spin_unlock(&jiffies_lock);
		update_wall_time();                /* timekeeping 推进(第 3 章) */
	}
	update_process_times(user_mode(get_irq_regs()));
	  /* account_process_tick → run_local_timers(时间轮+hrtimer入口)
	     → rcu_sched_clock_irq → irq_work_tick → scheduler_tick
	     → run_posix_cpu_timers —— 周期 tick 的全部受惠方 */
	profile_tick(CPU_PROFILING);
}

void tick_handle_periodic(struct clock_event_device *dev)   /* event_handler */
{
	tick_periodic(cpu);
	/* 硬件若无 PERIODIC 特性(只支持 oneshot), 用循环补编程:
	   next += TICK_NSEC; clockevents_program_event(dev, next, false)
	   —— 已过期就现场再跑一轮 tick_periodic(需 timekeeping_valid_for_hres
	      防止在假时钟上无限空转) */
}
```

串联视图（周期模式，每 1/HZ 秒）：

```
clockevent 硬件到期
  → event_handler = tick_handle_periodic
      ├─ [值班 CPU] jiffies 网格推进 + do_timer + update_wall_time
      └─ [所有 CPU] update_process_times
           ├─ account_process_tick     CPU 时间记账
           ├─ run_local_timers         → TIMER_SOFTIRQ → 时间轮
           ├─ rcu_sched_clock_irq      RCU 静默期记账
           ├─ scheduler_tick           调度器(时间片/负载/PELT 衰减)
           └─ run_posix_cpu_timers     CPU 定时器采样
```

### 设备注册与 do_timer 值班的交接

`tick_check_new_device()`（tick-common.c）为新 clockevent 排座次：优先 per-CPU 设备（`cpumask_equal(newdev->cpumask, cpumask_of(cpu))`），其次 rating，且偏爱 oneshot 能力。首次拿到设备的 CPU 顺手接下 do_timer 值班（`tick_do_timer_cpu == TICK_DO_TIMER_BOOT` 时接管并锚定 `tick_next_period = ktime_get()`）。nohz_full 特例：boot CPU 自己若在 nohz_full 掩码里，记录 `tick_do_timer_boot_cpu`，等第一个 housekeeping 副 CPU 上线时 `tick_take_do_timer_from_boot()` 用 IPI 把值班权移交出去（源码注释：值班 CPU 承担 unbound timer/workqueue/timekeeping 等后勤，nohz_full 模式下**永不弃权**——`tick_sched_do_timer` 里的 `WARN_ON_ONCE(tick_nohz_full_running)`）。CPU 下线时 `tick_handover_do_timer()` 把值班转给 `cpumask_first(cpu_online_mask)`。

---

## 🌙 三、NOHZ_IDLE：停 tick 的完整流程

### 3.1 进入：算出"值得睡多久"

```
idle 循环 (do_idle, kernel/sched/idle.c)
  → tick_nohz_idle_enter()            /* inidle=1, idle_entrytime 记账开始 */
  → menu governor 预测睡眠长度
      → tick_nohz_get_sleep_length(&delta_next)      kernel/time/tick-sched.c
          *delta_next = dev->next_event - now;       /* 不停 tick 时的睡眠长度 */
          if (can_stop_idle_tick(cpu, ts)):          /* 见 3.3 前置检查 */
              next_event = tick_nohz_next_event(ts, cpu)
                ┌──────────────────────────────────────────────────┐
                │ ① 一致快照 jiffies + last_jiffies_update (seq)     │
                │ ② rcu_needs_cpu()||arch_needs_cpu()||             │
                │    irq_work_needs_cpu()||TIMER_SOFTIRQ pending?    │
                │    → 下个 tick 就得醒(basemono+TICK_NSEC)          │
                │ ③ 否则 next_tick = get_next_timer_interrupt()     │
                │      (时间轮下一到期; hres 下不含 hrtimer,          │
                │       !hres 下 cmp_next_hrtimer_event 一并取 min)  │
                │ ④ 值班 CPU 的睡眠上限 = timekeeping_max_deferment  │
                │    (clocksource max_idle_ns, 防时间读数溢出/回绕)    │
                │ ⑤ ts->timer_expires = min(上限, next_tick)         │
                └──────────────────────────────────────────────────┘
              next_event = min(next_event,
                          hrtimer_next_event_without(&ts->sched_timer))
                  /* hres: 排除 tick 自己后的最早 hrtimer —— menu 要的是
                     "用户真正在乎的下一个事件" */
  → tick_nohz_idle_stop_tick()
      → tick_nohz_stop_tick(ts, cpu)
          ├─ 本 CPU 若是 do_timer 值班者: 弃权!
          │    tick_do_timer_cpu = TICK_DO_TIMER_NONE; do_timer_last=1
          │    /* 下一个自然醒的 CPU 会在 tick_sched_do_timer 里接管 */
          ├─ !tick_stopped 首次停表: calc_load_nohz_start()(负载记账暂停)
          │    quiet_vmstat()(静默 vmstat 差量刷新) ts->tick_stopped=1
          ├─ ts->next_tick = tick; ts->idle_expires = expires  /* 调试可见 */
          └─ 编程硬件到 tick:
               hres  → hrtimer_start(&ts->sched_timer, tick, ABS_PINNED_HARD)
               lowres→ tick_program_event(tick, 1)
               expires==KTIME_MAX → 彻底关掉设备(ONESHOT_STOPPED)
      → 进 deep idle (mwait/wfi)
```

### 3.2 退出：补账三件事

```
中断/唤醒 → idle 循环退出
  → tick_nohz_idle_exit()
      ├─ tick_nohz_stop_idle(): idle_sleeptime/iowait_sleeptime 结算
      │    (get_cpu_idle_time_us 语义, seqcount 保护)
      └─ tick_stopped ?
          → tick_nohz_idle_update_tick(ts, now)
              ├─ tick_nohz_restart_sched_tick(ts, now):
              │   ① tick_do_update_jiffies64(now)   ★ 补 jiffies:
              │      delta = now - tick_next_period; ticks = delta/TICK_NSEC+1
              │      jiffies_64 += ticks —— 跳过的 tick 数一次性入账
              │      (附带 update_wall_time: timekeeper 对数累加补时)
              │   ② timer_clear_idle()  时间轮 base->is_idle=false(免 IPI)
              │   ③ calc_load_nohz_stop()  负载记账恢复
              │   ④ tick_stopped=0; tick_nohz_restart():
              │      hrtimer_cancel(sched_timer) → expires=last_tick(旧相位)
              │      hrtimer_forward(now, TICK_NSEC) → 重启 —— tick 相位
              │      回到停表前的网格, jiffies 网格与墙钟网格重新对齐
              └─ tick_nohz_account_idle_time(ts, now):
                  ticks = jiffies - idle_jiffies;
                  account_idle_ticks(ticks)   /* 睡掉的 tick 全记为 idle,
                                                update_process_times 每 tick
                                                只记 1 tick 的语义被补齐 */
```

补记账的完整性清单：jiffies（数值）、wall time（timekeeping 对数累加）、CPU idle 时间（`account_idle_ticks`）、loadavg（`calc_load_nohz_*` 分片）、时间轮 `base->clk`（`__run_timers` 跳桶结算）——**每一条独立机制都要单独补**，这是 NOHZ 复杂度的真正来源。

### 3.3 前置检查：can_stop_idle_tick

`nohz_mode == INACTIVE`（无 oneshot 设备）→ 不停；`need_resched()` → 不停；**软中断待处理不停**——`report_idle_softirq()` 打出著名的 `NOHZ tick-stop error: local softirq work is pending, handler #%02x!!!`（若 ksoftirqd 被卡住还硬停 tick，软中断就永远没人跑了，只限速告警 10 次）；nohz_full 在场时本 CPU 若是值班者 → 必须保留 tick 保时间推进。

### 3.4 全生命周期时序图（合并视角）

```
CPU0 (值班, 睡前)        clockevent/dev          CPU1 (值班候选)         时间轮base
─────────────────────────────────────────────────────────────────────────────────
idle: can_stop_idle_tick ✓
tick_nohz_next_event:
  get_next_timer_interrupt ───────────────────────────────────────→ is_idle=true
  (下一到期 = 3.2s 后)                                             next_expiry=X
弃权 do_timer→NONE
tick_stopped=1
program_event(3.2s) ────→ 硬件编程
  ┊ (深睡 2.1s)
  ┊                        [远程 CPU 挂入更早定时器 0.7s]
  ┊   trigger_dyntick_cpu: is_idle → wake_up_nohz_cpu ── IPI ──→ CPU0
IRQ(IPI) → tick_nohz_irq_enter
  tick_nohz_update_jiffies(now)
  tick_do_update_jiffies64: 发现无人值班 → 接管 do_timer, 补 jiffies
idle 循环重估: 更早事件 0.7s → 重编程
  ┊ (再睡 0.7s)
到期 → tick_sched_handle:
  tick_stopped? touch_softlockup, idle_jiffies++, next_tick=0
  update_process_times (跑一轮完整 tick 负载)
  sched_timer 不再续期 (仍在 idle) → 重估下一事件
  ┊
唤醒(非定时器中断): idle_exit
  tick_nohz_idle_exit:
    tick_do_update_jiffies64(now)   ← 补跳过的 jiffies
    timer_clear_idle() ────────────────────────────────────────→ is_idle=false
    tick_nohz_restart: sched_timer 对齐旧相位重启
    account_idle_ticks(jiffies-idle_jiffies)   ← idle 时间补账
```

三处"交接"值得重读：IPI 救场（is_idle 的意义）、值班权漂移（NONE→醒来者）、相位恢复（`last_tick` + `hrtimer_forward` 保证重启的 tick 网格与 `tick_next_period` 网格对齐——否则 jiffies 网格与 wall 网格错位，每醒一次误差累加一次）。

---

## ☀️ 四、NOHZ_FULL：任务运行时也停 tick

`nohz_full=` 启动参数圈定一组 CPU（`tick_nohz_full_mask`），其余为 housekeeping（kernel/sched/isolation.c 的 `housekeeping` 结构按 `HK_TYPE_*` 记各类后勤归属）。**前提三件**：SMP、context tracking 用户态钩子（`ct_cpu_track_user()`——`context_tracking` 在用户态↔内核态边界记账，否则停 tick 后无法区分"CPU 在用户态静默"与"卡死"）、`VIRT_CPU_ACCOUNTING_GEN`（用 clocksource 精确记账 CPU 时间，替代 tick 采样）。

### tick 依赖计数：四层掩码

```c
// include/linux/tick.h (v6.6)
enum tick_dep_bits {
	TICK_DEP_BIT_POSIX_TIMER  = 0,   /* 进程有 CPU 定时器在等采样 */
	TICK_DEP_BIT_PERF_EVENTS  = 1,   /* perf 限流/采样依赖 tick */
	TICK_DEP_BIT_SCHED        = 2,   /* 调度器要求(如负载均衡/老任务) */
	TICK_DEP_BIT_CLOCK_UNSTABLE = 3, /* clocksource 不稳, 需要 watchdog */
	TICK_DEP_BIT_RCU          = 4,   /* RCU 宽限期推进需要本 CPU 报 QS */
	TICK_DEP_BIT_RCU_EXP      = 5,   /* expedited RCU */
};
/* 四个层级: 全局 tick_dep_mask / per-CPU ts->tick_dep_mask /
   per-task tsk->tick_dep_mask / per-signal sig->tick_dep_mask
   can_stop_full_tick() 依次检查四层, 任一置位 → 保留 tick */
```

置位即踢人：`tick_nohz_dep_set_cpu/task/signal()` 用 `atomic_fetch_or` 的返回值判断"从 0 变 1"，再通过 **irq_work 自 IPI**（`nohz_full_kick_work`，NMI 安全）把目标 CPU 从用户态拽出来重评估——这套"踢"是 NOHZ_FULL 正确性的传送带（任务切换时 `tick_nohz_task_switch()` 也会复核新任务的依赖）。

### 为什么 RCU/watchdog/printk 需要 tick

* **RCU**：宽限期靠各 CPU 报告静默状态（quiescent state），用户态切换与上下文切换都是天然 QS，但**完全单线程跑死循环的用户任务**既不切也不进内核——`rcu_needs_cpu()`/`TICK_DEP_BIT_RCU` 让这种 CPU 保留 tick 兜底报 QS（除非 nocb offload 把回调全搬去 housekeeping）。
* **watchdog**（softlockup/hard lockup）：softlockup 检测靠"每 2*hz 秒没摸过 `touch_softlockup_watchdog` 就叫"——而摸的动作在 tick 里（`tick_sched_handle` 对 stopped 状态补 `touch_softlockup_watchdog_sched`）。停 tick 的 CPU 靠这条免误报；真卡死则由 NMI 侧 hrtimer/PMU 检测。
* **printk**：控制台输出大量依赖 deferred printing/timer 与 irq_work 链条，tick 停了这些链可能断——nohz_full CPU 上通常把 printk 责任交给 housekeeping。

---

## 📉 五、NOHZ 对调度器与 PELT 的影响（联系 scheduler 分册）

* **rq clock 的追赶**：调度器不用 jiffies 而用 `rq->clock`/`rq->clock_task`（`update_rq_clock()` 推进，`clock_task` 扣除中断时间）。tick 停止期间这些钟不动，唤醒/入队时一次跳到当前——PELT（util/load avg）在 `update_rq_clock` 后的衰减循环里**一次性补齐指数衰减**（数学上等价，因为 PELT 是几何级数），但 `sched_clock` 的抖动可能让跳变不平滑。

```
tick 运行时:  rq->clock_task 每 tick 步进 4ms(HZ=250), PELT 每 1ms(y=32/32) 衰减一拍
tick 停止后:  clock_task 冻结 —— sleep 中任务 util 不再衰减(合理: 没在跑就不该衰)
             唤醒瞬间: update_rq_clock 一跳 400ms
             ___update_load_avg: delta=400ms → 按 1024us 粒度循环补 390 拍衰减
             (大跳变时的补偿循环性能可对照 scheduler 分册的 load_avg 代码)
```

* **tick 停止状态下的调度**：`scheduler_tick` 不再跑 → 时间片检查/负载更新/hrtick 都停。抢占完全依赖事件驱动（wakeup preempt、hrtick、DL 定时器）。**时间轮/POSIX 定时器到期会通过 IPI 或既定事件唤醒**（第 4 章），调度器自己的 needs（如 idle load balancing）则靠 `nohz_balance_enter_idle()`（停 tick 时登记）由 ILB CPU 的 tick 代劳。
* **`got_idle_tick` 的妙用**：睡着时若被动收到一次真 tick（如 broadcast 接力误中），`tick_sched_handle` 置 `got_idle_tick`；idle 循环据此知道"tick 曾回来过"，menu governor 用它修正预测（睡得比预期短 → 下次预测更保守）。
* **jiffies 停滞自愈**：值班 CPU 若长时间被 `stop_machine`/VMEXIT 卡住，各 CPU 的 `tick_sched_do_timer` 里 `stalled_jiffies` 计数到 `MAX_STALLED_JIFFIES(5)` 就**强行**`tick_do_update_jiffies64`——防止 jiffies 冻结拖垮依赖它的所有机制（软定时器、统计）。

---

## 📡 六、broadcast tick：deep idle 的接力棒

`CLOCK_EVT_FEAT_C3STOP`：x86 Local APIC timer、多数 arm64 per-CPU 定时器在 deep idle 会随最后一级电源停振——**本地 clockevent 不可靠**。接力机制（CONFIG_GENERIC_CLOCKEVENTS_BROADCAST + tick-broadcast.c）：

```
CPU 准备进 deep idle
  → tick_broadcast_enter()(tick_broadcast_oneshot_control(ENTER))
      ├─ 本地设备 C3STOP? 不是 → 返回 0, 正常睡
      ├─ 把本 CPU 加入 broadcast 掩码(在全局 broadcast 设备上登记
      │   自己的 next_event —— 通常是 HPET/ARM MMIO timer, 不受 C3 停)
      └─ shutdown 本地设备
[全局设备到期] → broadcast 中断(通常路由到某个醒着的 CPU)
  → tick_do_broadcast → 给掩码内各 CPU 发 IPI(arch 钩子)
  → 目标 CPU 收 IPI → 本地事件处理(等效本地 tick/oneshot 到期)
CPU 醒来 → tick_broadcast_exit() 撤登记、复活本地设备
```

配套细节：`tick_device_uses_broadcast()`（tick-common.c）在设备注册时决定"谁只能靠 broadcast"；oneshot 时代 broadcast 也按到期排序编程最早者；无可用全局设备时 arm64 可用 **hrtimer 模拟 broadcast**（tick-broadcast-hrtimer.c，用一个普通 per-CPU hrtimer 假扮全局设备）。`/proc/timer_list` 的 "Tick Device" 段会同时打印 per-CPU 与 broadcast 设备——C3STOP 排障的第一现场。

```
deep idle 中的 broadcast 接力(oneshot 时代)

  CPU0(深睡,APIC已停)      broadcast 设备(HPET/MMIO,不断电)     CPU1(浅睡/运行)
  ─────────────────────────────────────────────────────────────────────────
  tick_broadcast_enter:
    登记自己 next_event ──→ 更新全局最早到期, 编程 HPET
    shutdown 本地 APIC
                            ⋮ (事件到期)
                            broadcast IRQ(可能落在 CPU1)
                            tick_handle_oneshot_broadcast:
                              对掩码内各 CPU 比较到期
  ←──────── arch 起搏 IPI ────┤
  IPI → 本地事件处理:
    (等效本地 oneshot 到期:
     hrtimer_interrupt / tick_nohz_handler)
  tick_broadcast_exit:
    复活本地 APIC, 重编程本地 next_event
    从掩码撤下
```

判别一个系统是否真的在用 broadcast：`/proc/timer_list` 里 broadcast 设备行有 `event_handler: tick_handle_oneshot_broadcast`；睡眠中依赖 broadcast 的 CPU 靠 IPI 类中断被拉起（具体计入 /proc/interrupts 的哪一列随 arch 计数口径而异，可结合 ftrace 的 `irq_vectors:*` 事件确认）。x86 笔记本（APIC C3STOP + intel_idle 深睡态）与 arm64 大核（arch timer 的 C3STOP per 实现而定）是两大常客。

---

## 🎚️ 七、CONFIG_HZ 的选择与 jiffies 语义

三种 tick 处理模式（kernel/time/Kconfig 的 `Timer tick handling` choice）与频率（kernel/Kconfig.hz 的 `Timer frequency` choice）是**两个正交的选择**：

| | HZ_PERIODIC | NOHZ_IDLE（默认生态） | NOHZ_FULL |
| :--- | :--- | :--- | :--- |
| tick 何时停 | 从不停 | 仅 idle 停 | idle + 单任务用户态运行 |
| jiffies 推进 | 每 tick，值班 CPU | 补账式（醒来算总账） | housekeeping CPU 恒定推 |
| 前置条件 | 无 | oneshot clockevent | SMP + context tracking + 精确 CPU 记账 |
| 收益 | 可预测性（实时审计/简单） | 空闲功耗 | 用户态延迟确定性（DPDK/音频类） |
| 代价 | 全负载中断开销 | 补账复杂度 | 部署约束（isolcpus 级运维） |

`kernel/Kconfig.hz`（官方帮助文本要点）：

| 选项 | 定位 | 取舍 |
| :--- | :--- | :--- |
| `HZ_100` | 服务器/SMP/NUMA | 中断最少、缓存/总线最友好；交互延迟上限 10ms |
| `HZ_250`（默认） | 折中 | 服务器性能 + 可接受交互 |
| `HZ_300` | 多媒体 | 250 的改良，整除 PAL/NTSC 帧率 |
| `HZ_1000` | 桌面 | 响应最快；`NR_CPUS × HZ` 的中断开销在大机器上伤吞吐 |

要点：

* 帮助文本开宗明义：SMP 下每秒中断数 = **NR_CPUS × HZ**——这是选低 HZ 的根本理由。
* `TICK_NSEC = (NSEC_PER_SEC + HZ/2)/HZ`：四舍五入，HZ=300 时 tick=3,333,333ns 带来可忽略的长期偏差（由 timekeeper 的网格化处理吸收）。
* **NOHZ 使 HZ 的意义分裂**：`HZ` 仍决定 jiffies 粒度、时间轮第 0 层粒度与（hres 关闭时的）hrtimer 精度；但 hres+NOHZ_IDLE 下硬件事件完全按需，**HZ 主要变成"tick 模拟的周期"与统计语义**，而不再是中断频率。
* jiffies 的三重身份：时间轮的时钟、`USER_HZ=100` 的用户可见单位换算（sysinfo/proc）、大量内核逻辑的"粗但便宜"比较基准（`time_after`）。NOHZ_IDLE 期间 jiffies 由醒着的值班 CPU 或醒来补账维护，**不等于中断计数**。

---

## 🔭 八、观测调试

```bash
# NOHZ_FULL 配置与 housekeeping
cat /sys/devices/system/cpu/nohz_full/cpulist    # 空 = 未启用
dmesg | grep "NO_HZ: Full dynticks CPUs"          # 启动时圈定结果

# 本地 tick 中断计数(对照 jiffies 是否同步增长)
grep "LOC" /proc/interrupts; cat /proc/uptime

# tick 停了多久: /proc/timer_list 的 tick_sched 统计
sudo grep -E "cpu:|idle_jiffies|idle_calls|idle_sleeps|idle_expires|next_tick" \
     /proc/timer_list | head -40

# 每 CPU 空闲时长(NOHZ 记账的官方出口)
cat /proc/stat | head -3      # idle/iowait 列, 每CPU行

# tracepoints: tick 启停与依赖原因
cd /sys/kernel/tracing
echo 1 > events/tick/enable          # tick_stop: success/dependency 字段
cat trace_pipe                        # dependency=RCU 就知道谁不放 tick 走

# 停 tick 状态下的活体观察(周期 1s 采样)
perf stat -a -e irq_vectors:local_timer_entry sleep 5
# NOHZ 生效时计数远低于 HZ*5*ncpu

# 停 tick 有效性验证实验(空载机器上)
#   60s 空闲后, NOHZ 生效时 LOC 增量应 << HZ*60*ncpu, 且
#   /proc/timer_list 的 idle_calls-idle_sleeps 之差揭示"没敢停"的次数
sleep 60 && grep LOC /proc/interrupts && \
  sudo grep -E "idle_calls|idle_sleeps" /proc/timer_list | head -4

# 内核自测
tools/testing/selftests/timers/nsleep-lat     # 睡眠延迟分布
tools/testing/selftests/timers/alarmtimer-suspend
```

排障速查：**"NOHZ tick-stop error: local softirq work is pending"** → 某驱动在中断里 raise 软中断但 ksoftirqd 饿死，抓 `ps -e | grep softirqd` 与 `/proc/softirqs`；**jiffies 停走** → `stalled_jiffies` 兜底场景（stop_machine 卡死/虚拟机全停），查 dmesg 与 hypervisor；**nohz_full CPU 上 perf/定时器行为怪** → 先看 `tick_dep_mask`（tracepoint tick_stop 的 dependency 字段）。

---

## 💭 九、思考题

1. `tick_nohz_stop_tick()` 里值班 CPU 要先把自己"贬为" `TICK_DO_TIMER_NONE` 才敢停表。如果不贬，全系统睡了 5 秒会发生什么？贬了之后 5 秒内 jiffies 谁推？
   <details><summary>提示</summary>不贬：唯一会推 jiffies 的 CPU 停了 tick，jiffies 冻结 5 秒（全系统超时集体漂移）。贬了：无人推——直到任一 CPU 因任何中断醒来，`tick_nohz_update_jiffies`/`tick_sched_do_timer` 发现 `TICK_DO_TIMER_NONE` 就接管并按 `tick_next_period` 网格补账。设计上赌的是"5 秒内总有人醒"。
2. menu governor 拿到的睡眠长度为何要调 `hrtimer_next_event_without(&ts->sched_timer)` 排除 tick 自己？不排除会怎样？
   <details><summary>提示</summary>sched_timer 是 tick 的化身，其到期=TICK_NSEC 的整数倍网格。不排除的话 menu 永远看到"最多再睡一 tick"，永远只敢选浅的 idle state，NOHZ 的省电意义归零。</details>
3. 停 tick 期间一个**非延迟**时间轮定时器在远程 CPU 被挂入（更早到期）：请从 `trigger_dyntick_cpu` 开始把睡着的 CPU 救活的调用链写出来。如果这是 DEFERRABLE 定时器，链条在哪一步断掉？
   <details><summary>提示</summary>`enqueue_timer → trigger_dyntick_cpu: base->is_idle → wake_up_nohz_cpu(远程 IPI) → 对方 irq_exit → tick_nohz_irq_exit/idle 循环重新评估 → get_next_timer_interrupt 重算 → tick_program_event 提前唤醒点`。DEFERRABLE 分支直接 return（除非对方是 nohz_full CPU）——链条断在第一步，睡到下一个非延迟事件为止。</details>
4. NOHZ_FULL CPU 上一个 SCHED_FIFO 任务跑死循环，perf 启动限流事件后 tick 为什么会回来？从 `TICK_DEP_BIT_PERF_EVENTS` 与 `tick_nohz_dep_set_cpu` 的"踢"机制推演。
   <details><summary>提示</summary>perf 限流（throttle）需要 tick 到期解禁；perf 侧调 tick_nohz_dep_set_cpu(cpu, TICK_DEP_BIT_PERF_EVENTS)，掩码 0→1 时 irq_work 自 IPI 把该 CPU 从用户态打断，`tick_nohz_irq_exit`/`__tick_nohz_full_update_tick` 里 can_stop_full_tick 见依赖位 → 重启 tick。</details>
5. PELT 在 tick 停止期间"醒来一次性补衰减"为什么数学上无损？哪个调度器字段在 NOHZ 下反而需要特殊处理（提示：想想谁在 tick 里更新）？
   <details><summary>提示</summary>PELT 是几何级数 y^n 的和，补衰减只是把 n 次乘 y 合成一次乘 y^n——等价。需要特殊处理的是依赖周期 tick 采样的：全局负载（calc_load 有 nohz 分片版本）、cfs bandwidth 超时（用 hrtimer 而非 tick，第 5 章）、以及 idle balance 的触发（ILB 接管）。</details>
6. 同一台 arm64 手机：关 NOHZ 与开 NOHZ_IDLE 对比，`grep LOC /proc/interrupts`（或本地定时器行）在 60 秒空闲后差多少倍？解释这个比值为什么**不是** HZ×60 的完全消除。
   <details><summary>提示</summary>比值≈空闲占比×HZ×60/(实际醒来次数)。醒不干净的原因：残余的定时器（统计/RCU/网络 keepalive）按各自到期唤醒、broadcast 接力、以及"下一个事件不足一 tick 时根本不停表"（delta <= TICK_NSEC 时 retain tick）。</details>

---

*上一章：[hrtimer 高精度定时器](05-hrtimer高精度定时器.md)　|　下一章：[posix 定时器](07-posix定时器.md) —— `k_itimer` 分派表、CPU 定时器、timerfd 与 alarmtimer 三套用户态接口。*
