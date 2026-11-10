# posix 定时器（Linux 6.6）

> 阅读目标：掌握 posix-timers 框架的 `struct k_itimer` 与 `k_clock` 分派表；破译 `clockid_t` 的位编码（信号式定时器 / CPUCLOCK / tid / fd）；理解 CPU 定时器（`CPUCLOCK_SCHED` 与 `RLIMIT_CPU`）、timerfd（epoll 友好等待的 hrtimer 封装）、alarmtimer（RTC 唤醒与 suspend 语义）三套实现；最后用一张对比表完成三种用户态定时器接口的选型，并解剖 SIGEV_SIGNAL 丢失/合并的经典坑。

代码出自 Linux 6.6 `kernel/time/posix-timers.c`、`kernel/time/posix-cpu-timers.c`、`fs/timerfd.c`、`kernel/time/alarmtimer.c`、`include/linux/posix-timers.h`。

---

## 🏗️ 一、框架：k_itimer 与 k_clock 分派

```c
// include/linux/posix-timers.h (v6.6)
struct k_itimer {
	struct list_head	list;       /* 挂 signal->posix_timers(进程级链) */
	struct hlist_node	t_hash;     /* 全局哈希表桶链 */
	spinlock_t		it_lock;
	const struct k_clock	*kclock;  /* ★ 分派到的时钟实现 */
	clockid_t		it_clock;
	timer_t			it_id;      /* 进程内自增分配的 ID */
	int			it_active;  /* armed 状态(SIGEV_NONE 不入队, 仅此位) */
	s64			it_overrun;       /* 未投递到期计数(overrun 核心) */
	s64			it_overrun_last;  /* 上次投递时的 overrun 快照 */
	int			it_requeue_pending;  /* 信号投递时重挂的握手序号 */
	int			it_sigev_notify;     /* SIGEV_SIGNAL/THREAD_ID/NONE/THREAD */
	ktime_t			it_interval;         /* 周期; 0 = 单次 */
	struct signal_struct	*it_signal;
	union { struct pid		*it_pid;      /* 信号投递目标 */
		struct task_struct	*it_process; }; /* CPU 定时器 nanosleep 唤醒目标 */
	struct sigqueue		*sigq;      /* 预分配: 投递绝不因 OOM 失败 */
	union {
		struct { struct hrtimer	timer; } real;      /* CLOCK_REALTIME/MONO/TAI/BOOT */
		struct cpu_timer	cpu;                    /* CPU 定时器(timerqueue) */
		struct { struct alarm	alarmtimer; } alarm; /* _ALARM 时钟 */
	} it;
	struct rcu_head		rcu;        /* 延迟释放 */
};
```

管理结构（posix-timers.c）：全局 `DEFINE_HASHTABLE(posix_timers_hashtable, 9)`——512 桶，键 = `signal 指针 ^ timer_id`，**允许无锁 RCU 查找**（checkpoint/restore 要求 ID 精确重建）；ID 由 `signal->next_posix_timer_id` 自增（回绕 + 已占用重试，耗尽返回 POSIX 的 `-EAGAIN`）。

```c
// kernel/time/posix-timers.c (v6.6) 分派表
static const struct k_clock * const posix_clocks[] = {
	[CLOCK_REALTIME]     = &clock_realtime,      /* common_timer_* (hrtimer 后端) */
	[CLOCK_MONOTONIC]    = &clock_monotonic,
	[CLOCK_PROCESS_CPUTIME_ID] = &clock_process, /* clock_posix_cpu (CPU 定时器) */
	[CLOCK_THREAD_CPUTIME_ID]  = &clock_thread,
	[CLOCK_MONOTONIC_RAW]= &clock_monotonic_raw, /* 只读, 无 timer_* */
	[CLOCK_REALTIME_COARSE]    = &clock_realtime_coarse,  /* 只读 */
	[CLOCK_MONOTONIC_COARSE]   = &clock_monotonic_coarse,
	[CLOCK_BOOTTIME]     = &clock_boottime,
	[CLOCK_REALTIME_ALARM] = &alarm_clock,       /* alarmtimer 后端 */
	[CLOCK_BOOTTIME_ALARM] = &alarm_clock,
	[CLOCK_TAI]          = &clock_tai,
};
```

`k_clock` 是"时钟能力接口"：`clock_get_timespec/clock_get_ktime/clock_set/clock_adj/clock_getres/nsleep/timer_create/timer_set/timer_get/timer_del/timer_rearm/timer_forward/timer_remaining/timer_try_to_cancel/timer_wait_running/timer_arm`——**每个系统调用都只是查表转发**：

```
timer_create(clkid)  → clockid_to_kclock → kc->timer_create(new_timer)
timer_settime(id)    → lock_timer(哈希+RCU 查 id) → kc->timer_set(...)  → common_timer_set
timer_gettime(id)    → kc->timer_get  → common_timer_get
timer_delete(id)     → kc->timer_del  → common_timer_del + 出哈希 + RCU 释放
clock_nanosleep(clk) → kc->nsleep     → hrtimer_nanosleep / alarm_timer_nsleep
```

### clockid_t 的位编码

```c
// include/linux/posix-timers.h (v6.6) —— 负数 clockid 的解剖
/* 高 29 位: pid 或 fd; bit2: 线程(1)/进程(0); bit1..0: 类型 */
#define CPUCLOCK_PID(clock)     ((pid_t) ~((clock) >> 3))
#define CPUCLOCK_PERTHREAD_MASK 4
#define CPUCLOCK_WHICH(clock)   ((clock) & 3)
#define CPUCLOCK_PROF 0    /* 用户+内核时间 */
#define CPUCLOCK_VIRT 1    /* 仅用户时间 */
#define CPUCLOCK_SCHED 2   /* sum_exec_runtime: 调度器精确执行时间 */
#define CLOCKFD 3          /* fd 编码的动态时钟(PTP 设备) */
```

`clockid_to_kclock()`：负 id 且低 3 位 == `CLOCKFD` → `clock_posix_dynamic`（PTP 设备时钟，`fd_to_clockid()` 编码 fd）；其余负 id → `clock_posix_cpu`。正 id 查表（`array_index_nospec` 防推测攻击）。`make_process_cpuclock(pid, CPUCLOCK_SCHED)` 即 `((~pid)<<3)|2`——glibc 的 `CLOCK_PROCESS_CPUTIME_ID`（值 -2）等宏都落在这套编码里。

---

## ⚙️ 二、信号式定时器：从 timer_settime 到 overrun

`common_timer_set()`（hrtimer 后端共用）的关键序列：

```c
int common_timer_set(struct k_itimer *timr, int flags, ...)
{
	if (old_setting) common_timer_get(timr, old_setting);
	timr->it_interval = 0;                     /* 先断周期: 防回调里 rearm 竞态 */
	if (kc->timer_try_to_cancel(timr) < 0)
		return TIMER_RETRY;                    /* 回调在跑: 上层 timer_wait_running
		                                          解锁等待后整体重试 */
	timr->it_active = 0;
	timr->it_requeue_pending = (timr->it_requeue_pending + 2) & ~REQUEUE_PENDING;
	timr->it_overrun_last = 0;
	if (it_value == 0) return 0;               /* 单纯 disarm */
	timr->it_interval = it_interval;           /* 周期 */
	kc->timer_arm(timr, expires, ABSTIME?, sigev_none);  /* common_hrtimer_arm:
	                            hrtimer_init(POSIX 魔法: 相对 REALTIME→MONOTONIC)
	                            + hrtimer_start_expires; SIGEV_NONE 不入队! */
	timr->it_active = !sigev_none;
}
```

到期回调 `posix_timer_fn()`（hrtimer 硬/软中断上下文）：

```c
timr->it_active = 0;
if (timr->it_interval != 0)
	si_private = ++timr->it_requeue_pending;   /* 握手号: 投递方凭它决定是否重挂 */
if (posix_timer_event(timr, si_private)) {     /* 返回"投递失败"(SIG_IGN 等) */
	if (timr->it_interval != 0) {
		/* 抑制风暴: interval < 1 tick 时把 now 造假 +1 jiffie,
		   hrtimer_forward 因此只推进到下个 jiffy —— 保 overrun 记账,
		   代价是 timer_gettime 轻微失真(源码 FIXME 注释) */
		timr->it_overrun += hrtimer_forward(timer, now, timr->it_interval);
		return HRTIMER_RESTART;                /* 自己续期 */
	}
}
return HRTIMER_NORESTART;    /* 正常: 重挂交给信号投递方 */
```

**重挂-投递协议**（周期定时器的精确性所在），全链一图：

```
hrtimer 到期                     信号投递(dequeue_signal)              用户态
─────────────────────────────────────────────────────────────────────────
posix_timer_fn():
  it_active=0
  requeue_pending=N(自增) ──┐
  send_sigqueue(sigq)       │   sigqueue 挂入 pending 队
  return NORESTART          │      │ (期间若再到期: si_overrun++/it_overrun++)
                            │      ▼
                            │   handler 取到信号(si_sys_private=N)
                            │   posixtimer_rearm():
                            │     requeue_pending 匹配 N?
                            │       timer_rearm: forward+restart
                            │       it_overrun_last=it_overrun
                            │       it_overrun=-1; si_overrun=clamp(INT_MAX)
                            │       ++requeue_pending  ──→ 下一轮握手号
                            └────────────────────────── 与 timer_settime 的
                                                          +2&~1 互相排斥
```

信号被 `dequeue_signal()` 取走时，若 `si_sys_private == it_requeue_pending`，投递路径调 `posixtimer_rearm()`：`kc->timer_rearm`（`hrtimer_forward` 推进 + restart，返回补进的 overrun）→ `it_overrun_last = it_overrun; it_overrun = -1` → `si_overrun = clamp(INT_MAX)`。**定时器在信号挂起期间停止续期，只数 overrun**——这是"投递合并"的正确形态：每信号周期至多一个信号排队，不炸队列；丢失的到期次数精确计数在 `si_overrun`/`timer_getoverrun()`（后者返回 `it_overrun_last`，即最近一次投递时的合并数）。

---

## 🧮 三、CPU 定时器：以执行时间为轴

`clock_posix_cpu` 的三个时钟：`CPUCLOCK_PROF`（utime+stime）、`CPUCLOCK_VIRT`（utime）、`CPUCLOCK_SCHED`（`sum_exec_runtime`，纳秒精确、schedstat 级）。数据结构（posix-timers.h）：

```c
struct posix_cputimer_base {          /* 每时钟一个, 挂在 task/signal 上 */
	u64			nextevt;    /* 最早到期缓存(快速判断"有无到期") */
	struct timerqueue_head	tqhead;     /* 该时钟全部定时器的红黑树 */
};
struct posix_cputimers {
	struct posix_cputimer_base	bases[CPUCLOCK_MAX];
	unsigned int			timers_active;
	unsigned int			expiry_active;  /* 防多任务并发处理过期 */
};
```

到期检查由每个 tick 的 `update_process_times() → run_posix_cpu_timers()` 驱动：采样当前任务的三个时间值，与 `nextevt` 缓存比对，到期则摘树、给目标线程发 `SIGXCPU`/`SIGKILL`（PROF/VIRT）或唤醒（SCHED 型 nanosleep）。6.6 默认 `CONFIG_POSIX_CPU_TIMERS_TASK_WORK=y`：到期处理排入**目标任务自身的 task_work**（在它返回用户态时执行）——避免在任意任务的 tick 上下文里锁别人家的 sighand。`CPUCLOCK_SCHED` 型 nanosleep（`clock_nanosleep(CLOCK_THREAD_CPUTIME_ID)`）的唤醒目标即 `it_process`。

NOHZ_FULL 的联动（第 6 章）：进程装有 CPU 定时器时 `tick_nohz_dep_set_signal(tsk, TICK_DEP_BIT_POSIX_TIMER)` 置位信号级依赖——nohz_full CPU 上的该任务会因此保留 tick，否则执行时间采样断流。

**RLIMIT_CPU** 的搭车实现：`update_rlimit_cpu()` → `set_process_cpu_timer(task, CPUCLOCK_PROF, ...)` 把软/硬限制翻译成一对 CPU 定时器；soft 到期发 SIGXCPU 并自动续挂 1 秒（每秒提醒），hard 到期 SIGKILL。rt watchdog 挂钩 `posix_cputimers_rt_watchdog()`（RT 任务的 SCHED 上限）。

---

## 📎 四、timerfd：epoll 友好的第三条路

```c
// fs/timerfd.c (v6.6)
struct timerfd_ctx {
	union { struct hrtimer tmr; struct alarm alarm; } t;  /* 普通 or ALARM 时钟 */
	ktime_t tintv;                     /* 周期 */
	ktime_t moffs;                     /* mono↔real 基准(CANCEL_ON_SET 判定) */
	wait_queue_head_t wqh;
	u64 ticks;                         /* ★ 累计未读的到期次数 */
	int clockid;
	unsigned expired, settime_flags;
	struct rcu_head rcu;
	struct list_head clist;            /* CANCEL_ON_SET 全局链 */
	spinlock_t cancel_lock;
	bool might_cancel;
};
```

机制全在"ticks 计数 + 唤醒"这个极简设计里：

* 到期回调 `timerfd_tmrproc → timerfd_triggered`：`expired=1; ticks++; wake_up_locked_poll(EPOLLIN)`——**回调只记账不续期**。周期重挂在 `read()` 里：`ticks += hrtimer_forward_now(tmr, tintv) - 1; hrtimer_restart()`（源码注释：故意不在回调里 rearm，防用户态用超短周期定时器在硬中断里 DoS 内核）。
* `read()` 返回累计 ticks（u64），读完清零——**N 次到期合并成一次读取**，天然适配 epoll 循环。`do_timerfd_gettime` 同样在读取时顺带补 forward。
* `TFD_TIMER_CANCEL_ON_SET`（仅 `CLOCK_REALTIME[ _ALARM]`+ABSTIME）：`timerfd_setup_cancel` 挂入全局 `cancel_list`，`timerfd_clock_was_set()`（由 `clock_was_set` 通知）发现 mono↔real 偏移变了就 `ticks++` 唤醒并置 `moffs=KTIME_MAX`——下次 read 返回 `-ECANCELED`。POSIX 闰秒/校时敏感程序的官方逃生门。
* `TFD_IOC_SET_TICKS` ioctl（CRIU 恢复用）；`/proc/<pid>/fdinfo/<fd>` 直接可见 `clockid/ticks/it_value/it_interval`。
* `CLOCK_REALTIME_ALARM`/`CLOCK_BOOTTIME_ALARM` 变体需要 `CAP_WAKE_ALARM`，内部换用 alarmtimer 后端。

---

## ⏰ 五、alarmtimer：suspend 里也要响

hrtimer 在 CPU 挂起时集体失能。`alarmtimer.c`（源自 Android 的 RTC alarm 接口）把"墙钟语义 + 挂起可唤醒"补齐：

```c
static struct alarm_base {                     /* 两个全局 base(非 per-CPU) */
	spinlock_t		lock;
	struct timerqueue_head	timerqueue;     /* 全系统 ALARM 定时器红黑树 */
	ktime_t			(*get_ktime)(void);
	void			(*get_timespec)(struct timespec64 *);
	clockid_t		base_clockid;    /* CLOCK_REALTIME / CLOCK_BOOTTIME */
} alarm_bases[ALARM_NUMTYPE];                  /* + FREEZER 变体供冻结补偿 */
```

运行时它就是"hrtimer + 全局树"：`alarm_start` 同时把到期挂进 `alarm_bases` 的 timerqueue 和 per-CPU hrtimer——**醒着时精度与 hrtimer 完全一致**。

挂起时的接力（`alarmtimer_suspend`，dev_pm_ops）：

```
扫描两个 base 取最早到期 min
  ├─ 无 RTC 唤醒设备(rtcdev) → 返回(睡眠期闹钟作废)
  ├─ min < 2s → pm_wakeup_event + -EBUSY: 拒绝深睡, 2 秒内自然到期
  └─ rtc_read_time + min → rtc_timer_start()
       ★ 把闹钟编进 RTC 硬件: 睡眠中由 RTC 中断(接在唤醒源上)拉起整个系统
resume → alarmtimer_resume: rtc_timer_cancel
        (timekeeping_resume 注入睡眠时长 → BOOTTIME 型闹钟的到期判定正确)
```

选 RTC 的条件（`alarmtimer_rtc_add_device`）：`RTC_FEATURE_ALARM` + `device_may_wakeup`。`alarmtimer_do_nsleep` 支持冻结补偿（`alarmtimer_freezerset`：进程被 freeze 打断的闹钟在 suspend 前登记给 alarmtimer_suspend 一并考虑）。用户面：`clock_nanosleep(CLOCK_BOOTTIME_ALARM, TIMER_ABSTIME, ...)`（`alarm_timer_nsleep`，需 `CAP_WAKE_ALARM`）与 timerfd 的 `_ALARM` 变体，以及 Android 的 `alarmtimer` 用户态。

---

## 📊 六、接口对比与选型

| | POSIX 信号式<br>`timer_create/settime` | timerfd<br>`timerfd_create/settime` | alarm<br>`*_ALARM` 时钟 |
| :--- | :--- | :--- | :--- |
| 通知通道 | 信号（sigqueue 带 sigval） | fd 可读/EPOLLIN | 同左或信号（借 timerfd/POSIX） |
| epoll/事件循环 | 不友好（信号打断 EINTR） | ★ 原生 | 原生（timerfd 变体） |
| 周期合并 | `si_overrun`/`timer_getoverrun` | read 返回累计 ticks | 同 timerfd |
| 时钟域 | REALTIME/MONO/BOOT/TAI/CPU | 同左 + ALARM | REALTIME/BOOTTIME（墙钟语义） |
| suspend 行为 | 挂起即失效 | 失效（非 ALARM 变体） | ★ RTC 唤醒整个系统 |
| 权限 | 无 | ALARM 变体需 `CAP_WAKE_ALARM` | 同左 |
| 计时轴 | 也可挂 CPU 时间轴 | 不支持 CPU 轴 | — |
| 典型用户 | 老代码、多线程定向投递(SIGEV_THREAD_ID) | 一切新代码的事件循环 | 闹钟类/Android |

再加两件"遗产"作对照：

**setitimer 家族**（kernel/time/itimer.c，1970 年代 Unix 接口）：`ITIMER_REAL`（墙钟，内核内用 hrtimer 实现——`it_real_fn()` 到期给进程发 SIGALRM）、`ITIMER_VIRTUAL`（用户态时间，SIGVTALRM）、`ITIMER_PROF`（用户+内核，SIGPROF，profiler 专用）。每进程各至多一枚、微秒粒度、无 overrun 计数（丢就丢了）——语义上是 `timer_create` 的贫化版，只该在维护老代码时见到。`alarm(2)` 是 ITIMER_REAL 的更老马甲。

**select/poll 的超时与 `nanosleep`**：一次性睡眠、无周期、无通知通道（醒来即结果），底层就是第 5 章的 `hrtimer_nanosleep`/`schedule_hrtimeout`。

选型口诀：**新代码默认 timerfd + MONOTONIC；要唤醒系统用 BOOTTIME_ALARM；要给别的线程定时要 SIGEV_THREAD_ID 信号式；测代码自身耗时用 CPU 定时器；全局超时管理（libevent 等）全部走 timerfd**。

---

## 🕳️ 七、经典坑：SIGEV_SIGNAL 的丢失、合并与 overrun

**场景**：`timer_create(CLOCK_MONOTONIC, {SIGEV_SIGNAL, SIGRTMIN})`，周期 1ms，接收线程被抢占/在缺页/被调度走 50ms。逐层拆解：

1. **投递合并是设计而非 bug**：第 1 次到期把预分配 sigqueue 挂进目标 pending 队；第 2..50 次到期时 `posix_timer_event → send_sigqueue` 发现 `!list_empty(&q->list)`——`kernel/signal.c` 的处理是 **`q->info.si_overrun++` 后直接返回**（`TRACE_SIGNAL_ALREADY_PENDING`）。信号只此一枚，第 51 次起定时器停止续期（第二节的重挂协议），`it_overrun` 持续累加。
2. **读数时机**：投递时 `posixtimer_rearm` 把 `it_overrun` 搬进 `it_overrun_last` 并清零计数；`timer_getoverrun()` 返回的正是**上一次已投递信号**的合并数（`sys_timer_getoverrun` 注释原话："meaningless outside of the signal delivery path"）。想拿"当前未投递的欠账"没有直接接口——只能从 handler 里 getoverrun 差分近似。
3. **真丢信号的两个出口**：
   * 标准信号（SIGALRM 这类非 RT）：同号信号天然合并，sigqueue 版也一样只留一个；
   * `SIG_IGN`：`send_sigqueue` 返回 1 → `posix_timer_fn` 走自续期路径（+1 tick 节流）；此时**永远没有投递、永远没有 rearm 协议**，信号 handler 装回去之前周期靠回调硬扛。
4. **缺页/调度延迟的三级放大**：handler 用户态延迟 → overrun 增大；`it_requeue_pending` 握手失败（投递的 si_sys_private 过期）→ rearm 不执行 → 周期彻底停转，直到下次 settime——6.6 的 `+2 & ~1` 技巧专治这个竞态。排查工具：`strace -e trace=timer_settime,timer_getoverrun` + handler 内打点。
5. **正确姿势**：能用 timerfd 就别用信号——read 的累计 ticks 是无歧义的合并账本；必须用信号时用 RT 信号（带 sigval 区分多定时器），并意识到"period < 调度延迟"时你测的是调度器不是定时器。

---

## 🔭 八、观测调试

```bash
# 进程的 POSIX 定时器全览(含 alarm/CPU 定时器, 显示到期/区间/通知方式)
cat /proc/<pid>/timers

# timerfd 的状态(fdinfo): clockid/ticks/settime flags/it_value/it_interval
ls -l /proc/<pid>/fd | grep timerfd
cat /proc/<pid>/fdinfo/<fd>

# 信号路径追踪
strace -f -e trace=timer_create,timer_settime,timer_getoverrun,sigaction ./app
perf probe -a 'posixtimer_rearm'        # 或用 kprobe 观察重挂率
bpftrace -e 'kprobe:posix_timer_event { @[comm] = count(); }'

# alarmtimer 链路
dmesg | grep -i "alarmtimer\|rtc"       # rtcdev 绑定
sudo cat /proc/dynpm... 2>/dev/null; sudo grep -i wakeup /proc/timer_list

# 内核自测(完整覆盖三接口)
tools/testing/selftests/timers/posix_timers
tools/testing/selftests/timers/set-timer-lat
tools/testing/selftests/timers/alarmtimer-suspend
```

---

## 💭 九、思考题

1. `timer_create` 为什么给每个定时器**预分配** `sigqueue`？把它改成到期时动态分配，会在哪个环节破坏 POSIX 语义？
   <details><summary>提示</summary>投递发生在硬/软中断上下文（hrtimer 回调），那里不能睡眠分配；即便延后到线程上下文，"到期→投递"路径的失败会丢事件。预分配保证 send_sigqueue 只做挂链——内核信号语义:定时器信号不因资源不足而丢。</details>
2. `it_requeue_pending` 的握手（回调 `++`，投递方比对后再 `++`）防的是什么竞态？如果只在回调侧自增、投递方不比对，画一组交错序列说明事故。
   <details><summary>提示</summary>两个 CPU：A 在跑到期回调（置 si_sys_private=N），B 在投递旧信号（准备按 N 重挂）。若不比对，B 可能重挂一个**已被 timer_settime 重设**的定时器——用旧周期复活。握手号不匹配则放弃 rearm，交还给 settime 路径。</details>
3. CPU 定时器为什么挂在 `tick`（`run_posix_cpu_timers`）上而不是 hrtimer 上？`CPUCLOCK_SCHED` 的到期精度由什么决定？
   <details><summary>提示</summary>它的"时间"是任务执行时间——只在任务跑/记账时前进，硬件计数器测不到。精度=记账粒度：6.6 中 utime/stime 为 tick 采样（粗），sum_exec_runtime 在每次上下文切换/记账点精确到纳秒（SCHED 型远比 PROF/VIRT 精确）。</details>
4. timerfd 的周期重挂为什么放在 `read()` 而不是回调里？这个设计对"period=100µs 且 reader 卡住"的场景提供了什么保证？
   <details><summary>提示</summary>回调在硬中断（HARD hrtimer）：rearm+forward 循环若不受 reader 节流，100µs 定时器会让 CPU 在中断里空转烧干（DoS，源码注释原话）。放在 read 里：没人读 → ticks 只累加、定时器不再续期 —— 系统天然背压。</details>
5. `CLOCK_BOOTTIME_ALARM` 的闹钟定在"开机后 10 秒"，系统在第 3 秒 suspend 了 5 秒。RTC 唤醒发生在 resume 后第几秒（以醒来的墙钟计）？`CLOCK_REALTIME_ALARM` 变体呢？
   <details><summary>提示</summary>BOOTTIME 语义含睡眠：闹钟时刻=开机+10s 真实流逝，suspend 吃掉 5s → 醒来后 2s 响。REALTIME 型闹钟挂在墙钟绝对时刻：resume 后 RTC 剩余 delta 重新换算，若墙钟没被校过同样剩 2s；但 timekeeping 注入睡眠时 REALTIME 已前进——实际以 alarmtimer_suspend 编程给 RTC 的绝对墙钟时刻为准。</details>
6. 为什么 `timer_getoverrun()` 的返回值在信号 handler **之外**几乎无意义？设计一个能稳定观测 overrun 增长的最小实验。
   <details><summary>提示</summary>返回的是 it_overrun_last=上一次投递时的快照；没有投递事件它就永远是旧值。实验：1ms 周期定时器 + 接收线程人为 sleep(50ms)，在 handler 里连续调 timer_getoverrun，观察每次约 50 的读数。</details>

---

*上一章：[tick 与 NOHZ](06-tick与NOHZ.md)　|　回到 [分册导读](../README.md)。*
