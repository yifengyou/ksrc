# hrtimer 高精度定时器（Linux 6.6）

> 阅读目标：逐字段吃透 `struct hrtimer`/`hrtimer_clock_base`/`hrtimer_cpu_base`；掌握 `hrtimer_start` 家族、`hrtimer_forward` 的 overrun 语义、`hrtimer_cancel` 与 `hrtimer_active()` 的 seqcount 协议；走通 `hrtimer_interrupt()` 的 reprogram/retry/hang-detection 流程；理解高/低精度模式的运行时切换（`hrtimer_switch_to_hres`）、softirq 到期的 SOFT 定时器与 PREEMPT_RT 的上下文规则；最后用调度器三大真实案例（hrtick、CFS bandwidth、DL 补给定时器）收束。

代码出自 Linux 6.6 `kernel/time/hrtimer.c`、`include/linux/hrtimer.h`、`kernel/sched/{core,fair,deadline}.c`。

---

## 🧱 一、三个结构体：timer → clock_base → cpu_base

```c
// include/linux/hrtimer.h (v6.6)
struct hrtimer {
	struct timerqueue_node	node;        /* rb 节点: node.expires = 硬到期时刻 */
	ktime_t			_softexpires;     /* 最早到期(用户请求值); 两值差 = slack */
	enum hrtimer_restart	(*function)(struct hrtimer *);
	struct hrtimer_clock_base	*base;     /* 归属的 clock_base */
	u8	state;        /* 只有两种: 0x00 INACTIVE / 0x01 ENQUEUED */
	u8	is_rel;       /* 以相对时间 armed(CONFIG_TIME_LOW_RES 补偿用) */
	u8	is_soft;      /* 回调在 softirq 上下文执行 */
	u8	is_hard;      /* PREEMPT_RT 上仍坚持硬中断执行 */
};

struct hrtimer_clock_base {
	struct hrtimer_cpu_base	*cpu_base;
	unsigned int		index;              /* 0..7, 迁移时按 index 找同钟 base */
	clockid_t		clockid;
	seqcount_raw_spinlock_t	seq;           /* ★ hrtimer_active() 的读取协议 */
	struct hrtimer		*running;          /* 正在执行的 timer(=state 之外的"第三态") */
	struct timerqueue_head	active;            /* 红黑树(timerqueue) */
	ktime_t			(*get_time)(void); /* 该时钟的读时间函数 */
	ktime_t			offset;             /* 该时钟相对 MONOTONIC 的偏移 */
};

struct hrtimer_cpu_base {
	raw_spinlock_t		lock;                /* 一把锁保护全部 8 个 base */
	unsigned int		cpu;
	unsigned int		active_bases;        /* 位图: 哪些 base 树非空(扫描剪枝) */
	unsigned int		clock_was_set_seq;   /* 校时事件序号(偏移新鲜度) */
	unsigned int		hres_active	: 1,   /* 高精度模式是否生效 */
			in_hrtirq	: 1,           /* 正在 hrtimer_interrupt 里 */
			hang_detected	: 1,
			softirq_activated : 1;      /* HRTIMER_SOFTIRQ 已 raise */
#ifdef CONFIG_HIGH_RES_TIMERS
	unsigned int		nr_events;           /* 中断计数(/proc/timer_list 可见) */
	unsigned short		nr_retries;          /* reprogram 失败重试计数 */
	unsigned short		nr_hangs;
	unsigned int		max_hang_time;
#endif
	ktime_t			expires_next;        /* 硬+软合计的最早到期(编程进硬件的值) */
	struct hrtimer		*next_timer;         /* 最早的 timer(优化指针, 勿解引用) */
	ktime_t			softirq_expires_next;
	struct hrtimer		*softirq_next_timer;
	struct hrtimer_clock_base	clock_base[HRTIMER_MAX_CLOCK_BASES];
} ____cacheline_aligned;
```

### base 布局：8 个时钟，硬软镜像

```c
enum hrtimer_base_type {
	HRTIMER_BASE_MONOTONIC, HRTIMER_BASE_REALTIME,
	HRTIMER_BASE_BOOTTIME,  HRTIMER_BASE_TAI,
	HRTIMER_BASE_MONOTONIC_SOFT, HRTIMER_BASE_REALTIME_SOFT,
	HRTIMER_BASE_BOOTTIME_SOFT,  HRTIMER_BASE_TAI_SOFT,
	HRTIMER_MAX_CLOCK_BASES,     /* = 8 */
};
#define MASK_SHIFT   (HRTIMER_BASE_MONOTONIC_SOFT)      /* = 4 */
#define HRTIMER_ACTIVE_HARD ((1U << MASK_SHIFT) - 1)    /* 低 4 位 */
#define HRTIMER_ACTIVE_SOFT (HRTIMER_ACTIVE_HARD << MASK_SHIFT) /* 高 4 位 */
```

每个 CPU 有 **8 棵红黑树**：4 个硬中断到期 + 4 个 SOFT 软中断到期。`active_bases` 位图保证 `for_each_active_base` 只扫非空树。clockid→base 经 `hrtimer_clock_to_base_table[]` 换译，且有两条 POSIX 魔法：

```c
// __hrtimer_init(): POSIX 规定相对 CLOCK_REALTIME 定时不受校时影响
if (clock_id == CLOCK_REALTIME && mode & HRTIMER_MODE_REL)
	clock_id = CLOCK_MONOTONIC;      /* 直接改判 MONOTONIC base */
```

以及 PREEMPT_RT 的强制软执行：`!(mode & HRTIMER_MODE_HARD)` 的一律 `softtimer = true`（回调可能拿普通 spinlock，硬中断里会睡死）。

### state 的三态设计（源码注释直译）

`state` 只有 INACTIVE/ENQUEUED 两值，**"回调正在执行"不放 state**——否则回调结束后写 state 会触碰已释放的 timer（回调里 free 自身是合法且常见的）。运行态由 `base->running == timer` 表达。于是 SMP 上合法出现"**既在执行又已重新入队**"：回调给 POSIX 定时器排队信号，drop 锁的间隙另一 CPU 送信并 rearm。`next_timer` 只是 `__remove_hrtimer` 的比对优化，注释明令**不得解引用**（跨 CPU 删除时不可靠）。

---

## 🚀 二、arming：hrtimer_start 家族

```
hrtimer_start(timer, tim, mode)            /* inline: range=0 */
  → hrtimer_start_range_ns(timer, tim, 0, mode)
      ├─ WARN_ON(!(mode & SOFT) ^ !timer->is_soft)   /* mode 与 init 时的软硬必须一致 */
      ├─ lock_hrtimer_base(timer)                    /* 迁移竞态: READ_ONCE(timer->base)
      │                                                != migration_base 才拿锁再验证 */
      └─ __hrtimer_start_range_ns(timer, tim, delta_ns, mode, base)
           ├─ force_local = (base 在本 CPU) && (next_timer == timer)
           │    /* 是本 CPU 最早定时器又要重挂: 免"摘时重编程+挂时再编程"两次,
           │       摘时跳过 reprogram(keep_local), 挂完 force_reprogram 一次 */
           ├─ remove_hrtimer(timer, base, restart=true, keep_local=force_local)
           ├─ mode & REL ? tim = ktime_add_safe(tim, base->get_time()) : tim
           ├─ hrtimer_set_expires_range_ns(timer, tim, delta_ns)
           │    /* _softexpires = tim; node.expires = tim + delta
           │       slack 的方向是"只晚不早" */
           ├─ !force_local → switch_hrtimer_base(timer, base, PINNED?)
           │     /* get_target_base: timers_migration_enabled && !pinned
           │        → get_nohz_timer_target()(同时间轮那套)
           │        迁移前 hrtimer_check_target(): 若 expires 早于目标 CPU 的
           │        expires_next(远程无法重编程硬件!) → 放弃迁移回本 CPU */
           ├─ first = enqueue_hrtimer(timer, new_base, mode)
           │     /* WRITE_ONCE(state, ENQUEUED); timerqueue_add;
           │        返回是否成为树最左(最早) */
           └─ first && !force_local
                → hrtimer_reprogram(timer, true)
                     /* 比 expires_next 早 → 更新 next_timer/expires_next
                        → tick_program_event(expires_next) ★ 编程硬件 */
```

`hrtimer_forward(timer, now, interval)`（周期任务的核心原语）：把 `node.expires` 整数倍推进到 `now` 之后，**返回错过的周期数（overrun）**，本身**不重新入队**——标准用法是回调里 `forward + return HRTIMER_RESTART`，或在 `timer_restart` 时配合。内部有精度地板：`interval < hrtimer_resolution` 时按 resolution 取整（防 0 周期自旋）。

### 取消与 `hrtimer_active()` 的三段协议

```c
int hrtimer_cancel(struct hrtimer *timer)
{
	do {
		ret = hrtimer_try_to_cancel(timer);   /* -1 = 回调正在跑 */
		if (ret < 0)
			hrtimer_cancel_wait_running(timer);  /* RT: 拿 softirq_expiry_lock
			                                        非RT: cpu_relax 自旋 */
	} while (ret < 0);
}

bool hrtimer_active(const struct hrtimer *timer)    /* 无锁, 任意上下文可调 */
{
	do {
		base = READ_ONCE(timer->base);
		seq = raw_read_seqcount_begin(&base->seq);
		if (timer->state != HRTIMER_STATE_INACTIVE || base->running == timer)
			return true;
	} while (read_seqcount_retry(&base->seq, seq) || base != READ_ONCE(timer->base));
	return false;
}
```

写侧 `__run_hrtimer()` 在关键点打 `raw_write_seqcount_barrier(&base->seq)`，把执行切成三段：**queued → callback → post(inactive 或 requeued)**。读者必须从同一"段"看到 state 与 running 的组合，否则可能把"已执行完"误读成"从未激活"（false negative 注释里细述了跨多次执行的涂抹场景）。这个协议让 `hrtimer_cancel` 的循环无需长时间持锁。

---

## 💥 三、hrtimer_interrupt：硬中断主流程

```c
// kernel/time/hrtimer.c (v6.6, 骨架)
void hrtimer_interrupt(struct clock_event_device *dev)
{
	BUG_ON(!cpu_base->hres_active);
	cpu_base->nr_events++;
	dev->next_event = KTIME_MAX;                    /* 硬件已 one-shot 开火 */

	raw_spin_lock_irqsave(&cpu_base->lock, flags);
	entry_time = now = hrtimer_update_base(cpu_base); /* 读钟 + 刷新 3 个 offset
                                                        (ktime_get_update_offsets_now:
                                                         clock_was_set_seq 变了才更新) */
retry:
	cpu_base->in_hrtirq = 1;                        /* 挡住本 CPU 的 hrtimer_reprogram */
	cpu_base->expires_next = KTIME_MAX;             /* ★ 挡住迁移代码往里塞:
	                                                   "我们马上要清队列了" */

	if (!ktime_before(now, cpu_base->softirq_expires_next)) {
		cpu_base->softirq_expires_next = KTIME_MAX;
		cpu_base->softirq_activated = 1;
		raise_softirq_irqoff(HRTIMER_SOFTIRQ);      /* SOFT 定时器转交软中断 */
	}

	__hrtimer_run_queues(cpu_base, now, flags, HRTIMER_ACTIVE_HARD); /* 只跑硬树 */
	  /* 对每个 active base: while (树最左的 _softexpires <= basenow) __run_hrtimer()
	     —— 比较用 _softexpires(slack 下界), 注释解释了为何不用区间树 */

	expires_next = hrtimer_update_next_event(cpu_base);  /* 硬+软取最早 */
	cpu_base->expires_next = expires_next;
	cpu_base->in_hrtirq = 0;
	raw_spin_unlock_irqrestore(&cpu_base->lock, flags);

	if (!tick_program_event(expires_next, 0)) {     /* 编程下一次硬件到期 */
		cpu_base->hang_detected = 0;
		return;                                     /* 正常出口 */
	}
	/* tick_program_event 失败 = 编程时已经过了 expires_next!
	 * 原因(注释): tracing 拖慢 / 回调太长 / VM 被调度走 */
	raw_spin_lock_irqsave(&cpu_base->lock, flags);
	now = hrtimer_update_base(cpu_base);
	cpu_base->nr_retries++;
	if (++retries < 3)
		goto retry;                                  /* 最多 3 次重试 */

	/* 4 次都不行 = 挂死(hang) */
	cpu_base->nr_hangs++;
	cpu_base->hang_detected = 1;                    /* 之后禁 reprogram, 让系统喘息 */
	delta = ktime_sub(now, entry_time);
	if (delta > 100 * NSEC_PER_MSEC)
		expires_next = ktime_add_ns(now, 100 * NSEC_PER_MSEC);  /* 至少歇 100ms */
	tick_program_event(expires_next, 1);            /* force */
	pr_warn_once("hrtimer: interrupt took %llu ns\n", ...);
}
```

`__run_hrtimer()` 单个执行（锁协议核心）：

```c
base->running = timer;                       /* 先立旗 */
raw_write_seqcount_barrier(&base->seq);      /* 段界 1: queued→callback */
__remove_hrtimer(timer, base, INACTIVE, 0);  /* 出树 */
fn = timer->function;
raw_spin_unlock_irqrestore(&cpu_base->lock, flags);   /* ★ 回调不持锁不关中断 */
restart = fn(timer);
raw_spin_lock_irq(&cpu_base->lock);
if (restart != HRTIMER_NORESTART && !(timer->state & ENQUEUED))
	enqueue_hrtimer(timer, base, ABS);        /* 回调期间可能被别人 rearm 过 */
raw_write_seqcount_barrier(&base->seq);      /* 段界 2: callback→post */
WARN_ON_ONCE(base->running != timer);
base->running = NULL;
```

与时间轮回调的关键差异：**这里是硬中断上下文**（hres 模式），回调只能做不可睡眠的活——这正是 SOFT 模式存在的理由。

---

## 🔁 四、高/低精度模式与切换

hrtimer 框架生在 tick 上，长成硬中断事件源，中间有一次运行时切换：

```
每 tick: run_local_timers() → hrtimer_run_queues()
  ├─ hres_active ? 直接 return (硬中断路径已接管)
  ├─ tick_check_oneshot_change(allow_nohz=!hrtimer_is_hres_enabled())
  │    /* 条件: clocksource 有 VALID_FOR_HRES + clockevent 支持 oneshot
  │       (由 watchdog/tick 层通过 check_clocks 位触发评估) */
  │    ├─ 返回 1 且 highres 启用 → hrtimer_switch_to_hres()
  │    │     ├─ tick_init_highres(): tick_switch_to_oneshot(hrtimer_interrupt)
  │    │     │      ★ 把 clockevent 的 event_handler 换成 hrtimer_interrupt
  │    │     ├─ base->hres_active = 1
  │    │     ├─ hrtimer_resolution = HIGH_RES_NSEC   /* 1ns */
  │    │     ├─ tick_setup_sched_timer()             /* 挂 tick_sched_timer 模拟 tick */
  │    │     └─ retrigger_next_event()               /* 立刻按树重编程硬件 */
  │    └─ 返回 0 + allow_nohz → tick_nohz_switch_to_nohz()
  │          (低精度 NOHZ: 用 tick_nohz_handler 做 oneshot tick)
  └─ 低精度路径: __hrtimer_run_queues(HARD)   /* 每 jiffy 醒一次, 批量收割 */
```

高精度模式下 tick 并未消失，而是**由 hrtimer 模拟**：

```c
// kernel/time/tick-sched.c (v6.6)
static enum hrtimer_restart tick_sched_timer(struct hrtimer *timer)
{
	tick_sched_do_timer(ts, now);      /* jiffies 推进 + do_timer 职责交接 */
	tick_sched_handle(ts, regs);       /* update_process_times + profile_tick
	                                      —— 即周期 tick 的全部负载 */
	if (unlikely(ts->tick_stopped))
		return HRTIMER_NORESTART;  /* NOHZ: 本次是"睡眠终点"的一次性 tick */
	hrtimer_forward(timer, now, TICK_NSEC);
	return HRTIMER_RESTART;           /* 周期模式: 以 1/HZ 间隔自我续期 */
}
```

所以 **hres=on 时系统仍每 1/HZ 秒进一次 `tick_sched_timer`**（它自己就是 MONOTONIC base 里的一个 hrtimer，`HRTIMER_MODE_ABS_PINNED_HARD`）——hrtimer 与 tick 的关系从"tick 驱动 hrtimer"倒转为"hrtimer 模拟 tick"。`highres=0` 启动参数或 `tick_nohz_handler`（低精度 nohz）是降级逃生门。

---

## 🧵 五、SOFT 定时器与 PREEMPT_RT

* `HRTIMER_MODE_*_SOFT`（或 init 时指定）→ 挂入对应 `*_SOFT` base。到期时 `hrtimer_interrupt` **不执行回调**，只 `raise_softirq_irqoff(HRTIMER_SOFTIRQ)`；`hrtimer_run_softirq()` 在软中断上下文跑 `_ACTIVE_SOFT` 树，跑完 `hrtimer_update_softirq_timer()` 重算 `softirq_expires_next` 并视需要重编程硬件——**SOFT 定时器的到期时刻仍然由硬件精确保证，只是回调被推迟到软中断**（语义：到期不迟到，处理可延迟）。
* PREEMPT_RT：除显式 `MODE_HARD` 外全部转 SOFT（`__hrtimer_init`）。`hrtimer_sleeper` 特例（`__hrtimer_init_sleeper`）：**实时任务自动升 HARD**——注释解释这是为了给特权 RT 应用保住硬中断唤醒的低延迟，防普通用户用海量同刻 sleeper 制造唤醒风暴。
* 删除等待的 RT 修正：`hrtimer_cancel_wait_running()` 不再自旋（自旋会与被抢占的 ksoftirqd 形成优先级反转/活锁），改为 `atomic_inc(timer_waiters)` + `spin_lock_bh(softirq_expiry_lock)`——softirq 侧每跑完一个 SOFT 回调 `hrtimer_sync_wait_running()` 主动放锁让等待者过关。

---

## 🏭 六、调度器三大真实案例（联系 scheduler 分册）

**案例 1：hrtick——用 hrtimer 制造"准点抢占"**（kernel/sched/core.c）

```c
/* High-resolution timer tick.
 * Runs from hardirq context with interrupts disabled. */
static enum hrtimer_restart hrtick(struct hrtimer *timer)
{
	struct rq *rq = container_of(timer, struct rq, hrtick_timer);
	struct rq_flags rf;

	WARN_ON_ONCE(cpu_of(rq) != smp_processor_id());
	rq_lock(rq, &rf);
	update_rq_clock(rq);
	rq->curr->sched_class->task_tick(rq, rq->curr, 1);  /* queued=1: 触发
	                                entity_tick 的抢占检查 → resched_curr */
	rq_unlock(rq, &rf);
	return HRTIMER_NORESTART;
}
...
hrtimer_init(&rq->hrtick_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);
...
void hrtick_start(struct rq *rq, u64 delay)   /* fair.c: hrtick_start_fair() 按
              实体 deadline 差值调用, delta=0 时干脆立刻 resched */
	hrtimer_start(&rq->hrtick_timer, ns_to_ktime(delay),
		      HRTIMER_MODE_REL_PINNED_HARD);
```

`CONFIG_SCHED_HRTICK=y` 时，`hrtick_start_fair()`（fair.c）给当前实体的剩余 slice/deadline 挂一枚 hrtick——把"等 tick 检查"升级为"到点硬件中断"，是控制论式延迟保证的开关（SMP 上另有 `__hrtick_start` IPI 把 arm 请求转交正确的 CPU）。

**案例 2：CFS bandwidth 的 period_timer/slack_timer**（kernel/sched/fair.c）

```c
/* init_cfs_bandwidth(): */
hrtimer_init(&cfs_b->period_timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS_PINNED);
cfs_b->period_timer.function = sched_cfs_period_timer;
/* 随机错峰: 各 cgroup 的 period 不落在同一 ns 上 */
hrtimer_set_expires(&cfs_b->period_timer, get_random_u32_below(cfs_b->period));
hrtimer_init(&cfs_b->slack_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
cfs_b->slack_timer.function = sched_cfs_slack_timer;

static enum hrtimer_restart sched_cfs_period_timer(struct hrtimer *timer)
	/* 每 period 刷新配额; 回调里 hrtimer_forward_now(timer, cfs_b->period)
	   续期并处理 overrun(节流拖欠的周期数) */
static enum hrtimer_restart sched_cfs_slack_timer(struct hrtimer *timer)
	/* 周期结束还剩配额时, 把余量分发回被 throttle 的组 */
```

cgroup v2 `cpu.max` 的整套节律就是这两枚 hrtimer 在驱动（详见 scheduler 分册 sched-bwc 文档）。

**案例 3：DEADLINE 的补给定时器**（kernel/sched/deadline.c）

```c
hrtimer_init(timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);
/* setup_new_dlentity/start_dl_timer: 按 dl_se->dl_deadline 排下次补给 */
hrtimer_start(timer, act, HRTIMER_MODE_ABS_HARD);
static enum hrtimer_restart dl_task_timer(struct hrtimer *timer)
	/* 到期 → refill runtime, 重挂 throttle 的任务, pushable 重排队 */
```

DL 调度的"每周期补满 runtime"由它保证；错一枚 tick 就意味着实时任务饿死——所以 `ABS_HARD` 钉死硬中断。

> 更正一个流传甚广的旧说法：**6.6 的 schedutil 已不用 hrtimer** 做速率限制——它改为 `last_update + rate_limit_us` 的纯时间戳比较 + `irq_work`/`kthread_work` 投递（cpufreq_schedutil.c）。hrtimer 版 schedutil 属于 5.x 时代。

---

## ⚖️ 七、hrtimer 与时间轮：一表看清取舍

| 维度 | 时间轮（timer.c） | hrtimer |
| :--- | :--- | :--- |
| 时间单位 | jiffies（`unsigned long`） | ktime_t（纳秒，64 位） |
| 数据结构 | 9×64 bucket + 位图 | 8 棵红黑树/clock |
| 复杂度 | 入队/取消 O(1)，触发 O(到期数) | 入队/取消 O(log n) |
| 精度 | 1 tick + 层级粒度 | 硬件粒度（hres 时 1ns 语义） |
| 回调上下文 | TIMER_SOFTIRQ（软中断） | 默认硬中断；SOFT 变体软中断 |
| 到期语义 | 桶粒度对齐，可能晚不到不早 | `_softexpires` 下界保证 |
| 迁移 | 每 tick 随 `get_target_base` 漂移 | arming 时一次性选 base，之后不漂 |
| 适合 | 超时类：网路重传、IO、看门狗 | 周期驱动：tick 模拟、音频、DL、测速 |

选型口诀（呼应 `timers-howto.rst`）：**要精度/要绝对时刻/要硬中断确定性 → hrtimer；只是"出事才用"的超时 → 时间轮**（省内存、省锁、批处理友好）。

## 🔀 八、CPU 热插拔与"下一个事件"上界

`hrtimers_dead_cpu()`（与时间轮的 `timers_dead_cpu` 同场调用）：8 棵树逐棵 `migrate_hrtimer_list()`——逐个 `__remove_hrtimer(ENQUEUED)`（**刻意保持 ENQUEUED 态**，防他 CPU 把它当 inactive 丢了）→ 改 `timer->base` → 新 base 入队；迁完 `hrtimer_update_softirq_timer(new_base, false)` + `__hrtimer_peek_ahead_timers()` 立刻收割已到期项并重编程。

两个跨框架的"下一个事件"查询（NOHZ 睡前必问）：

* `hrtimer_get_next_event()`：**仅 !hres 模式下有意义**（hres 时返回 KTIME_MAX）——低精度 nohz 里 `cmp_next_hrtimer_event()` 用它把 hrtimer 队首并入时间轮结果。
* `hrtimer_next_event_without(exclude)`：hres 模式下排除指定 timer（NOHZ 排除 `sched_timer` 自身）后的最早到期——`tick_nohz_get_sleep_length()` 用它给 menu governor 报"真正的下一个用户事件"。

---

## 🔭 九、观测调试

**`/proc/timer_list` 在 6.6 仍存在**（常见说法"5.11 已移除"不实——被移除的是 `/proc/timer_stats`，4.11）。它以只读 0400 挂载（`kernel/time/timer_list.c` 的 `init_timer_list_procfs`）：

```bash
sudo head -100 /proc/timer_list
# 输出结构:
#   Tick Device: per-CPU clock event devices (含 broadcast)
#   clockevent 状态/rating/min/max delta
#   每个CPU: hrtimer_base 各树: .resolution / .get_time / .offset
#            active timers 列表: <expires>, <function>, <S/H>(软硬)
#   nr_events / nr_retries / nr_hangs / max_hang_time   ← 挂死自检!
```

tracepoints（include/trace/events/timer.h 的 `hrtimer:*`）：

```bash
cd /sys/kernel/tracing
echo 1 > events/hrtimer/enable
# 可用事件: hrtimer_init / hrtimer_start / hrtimer_cancel /
#           hrtimer_expire_entry / hrtimer_expire_exit
# hrtimer_start 字段: hrtimer/function/expires/softexpires/clockid/mode
# hrtimer_expire_entry 字段: hrtimer/function/now/base —— now 减 softexpires
#                            即"到期迟到量", 最直接的延迟证据

# 迟到分布(专家向):
bpftrace -e 'tracepoint:hrtimer:hrtimer_expire_entry
             /comm == "yourapp"/ { @late_ns = hist((args->now - args->softexpires)); }'

# hres 是否生效: clock_getres 应为 1ns; timer_list 里 resolution: 1 nsecs
python3 -c "import time; print(time.clock_getres(time.CLOCK_MONOTONIC))"

# 挂死告警与重试计数
sudo grep -E "nr_retries|nr_hangs|max_hang" /proc/timer_list
dmesg | grep "hrtimer: interrupt took"
```

---

## 💭 十、思考题

1. `hrtimer_interrupt` 为什么在重扫队列前先把 `expires_next` 置 `KTIME_MAX`？结合 `hrtimer_check_target()` 与迁移路径，说明这防住了哪种"吃掉定时器"的事故。
   <details><summary>提示</summary>迁移代码以 `expires_next` 为界决定"能不能把定时器放进来"。若中断处理期间它还是旧的小值，一个本应入队的定时器会因 `expires < expires_next` 被拒之门外——而硬件马上要被重编程到更晚，这枚定时器就没人记得了。KTIME_MAX 让迁移侧始终放行（中断侧马上会自己重扫树）。</details>
2. `tick_program_event` 返回非 0 触发 retry。三次重试全失败后的 100ms 强制延迟，为什么比"无限重试直到成功"更安全？`hang_detected` 置位后对后续 `__hrtimer_reprogram` 有什么影响？
   <details><summary>提示</summary>挂死场景里"下一次到期"总在过去，无限重试 = 中断风暴锁死系统。100ms 给别的中断/任务留出前进机会。hang_detected 期间 `__hrtimer_reprogram` 不再碰硬件（注释里的 T1/T2 场景：别把唯一逃生门也编程没掉）。</details>
3. SOFT 定时器的**到期时刻**仍由硬件中断保证，被推迟的只是回调。那么 `HRTIMER_MODE_REL_SOFT` 的 sleep 与普通 sleep 在"醒来的时刻"上有区别吗？真正的开销差在哪里？
   <details><summary>提示</summary>无区别——`hrtimer_wakeup` 走 SOFT 路径时照样由硬中断准点触发（`softirq_expires_next` 早于硬值时硬中断顺带 raise softirq），唤醒本身在 `hrtimer_run_softirq` 里完成。开销差在回调延迟与 RT 下的优先级归属，不在到期精度。</details>
4. `hrtimer_active()` 为什么在 seqcount 重试之外还要额外校验 `base != READ_ONCE(timer->base)`？只靠 seq 不够吗？
   <details><summary>提示</summary>seq 保护的是"state+running 组合的段一致性"；timer->base 指针的改写（迁移）不递增该 seq。读到旧 base 的旧段、而 timer 已迁走，会出现 false negative。对照 `lock_hrtimer_base` 的双重校验。</details>
5. 高精度模式下 `tick_sched_timer` 以 `HRTIMER_RESTART` + `hrtimer_forward(TICK_NSEC)` 续期。它和 NOHZ 停 tick 的交互点在哪一行？停 tick 后再次被 arm 时 expires 从哪里恢复？
   <details><summary>提示</summary>`if (ts->tick_stopped) return HRTIMER_NORESTART;`——停 tick 就是"这次到期不再续期"。恢复在 `tick_nohz_restart()`：`hrtimer_cancel` → set `ts->last_tick` → `hrtimer_forward(now, TICK_NSEC)` → `hrtimer_start_expires(ABS_PINNED_HARD)`，把相位对回整 tick 网格。</details>
6. 为什么 DL 的 `dl_task_timer` 与 hrtick 都选 `MODE_HARD`，而 CFS bandwidth 的 `period_timer` 没有显式 HARD？（提示：后者在 PREEMPT_RT 上允许软执行，代价是什么、谁承担？）
   <details><summary>提示</summary>DL/hrtick 的语义是硬实时保证，软执行会破坏 admission control；CFS bandwidth 的 period 晚几百微秒只是配额边界模糊，由组内所有任务共同承担且不违反任何承诺。选软硬本质是问"这个到期谁在等、多急"。</details>

---

*上一章：[时间轮与低分辨率定时器](04-时间轮与低分辨率定时器.md)　|　下一章：[tick 与 NOHZ](06-tick与NOHZ.md) —— `tick_device`/`tick_sched`、NOHZ_IDLE 全流程与 NOHZ_FULL 的 tick 依赖计数。*
