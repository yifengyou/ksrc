# timekeeping 与 clock_gettime（Linux 6.6）

> 阅读目标：吃透 `struct timekeeper` 与 `tk_read_base` 的每个字段；理解 `tk_core`/`shadow_timekeeper` 双缓冲与 `tk_fast_mono`/`tk_fast_raw` 的 NMI 安全 latch 协议；逐行走完 `update_wall_time()`（tick 驱动的时间累积、对数累加、闰秒与 mult 自适应调整）；弄清 NTP 的 adjtimex 如何以 slewing/stepping 两种方式作用到时钟；最后打通 `clock_gettime` 的 vDSO 快路径与系统调用慢路径，以及 jiffies 的维护与回绕语义。

代码出自 Linux 6.6：`include/linux/timekeeper_internal.h`、`kernel/time/timekeeping.c`、`kernel/time/ntp.c`、`kernel/time/vsyscall.c`、`kernel/time/tick-sched.c`。

---

## 🗄️ 一、struct timekeeper：时间的心脏

```c
// include/linux/timekeeper_internal.h (v6.6)
struct tk_read_base {                 /* 读出基——冷热分离的关键设计 */
	struct clocksource	*clock;   // 当前 clocksource(timekeeper 缓存它, 热路径)
	u64			mask;      // cs->mask 副本
	u64			cycle_last;      // 上次 update_wall_time 时的计数器锚点
	u32			mult;            // (经 NTP 调整过的)乘数——与 cs->mult 是两份!
	u32			shift;
	u64			xtime_nsec;      // 小数部分: shifted ns(以 2^shift 为单位)
	ktime_t			base;            // MONOTONIC 的 ktime 基准(整 ns)
	u64			base_real;       // REALTIME 快速读出用基准
};                                     /* 56 字节: 和 seqcount 同挤一个 cacheline */

struct timekeeper {
	struct tk_read_base	tkr_mono;     // CLOCK_MONOTONIC 读出基
	struct tk_read_base	tkr_raw;      // CLOCK_MONOTONIC_RAW 读出基(mult 不吃 NTP)
	u64			xtime_sec;        // CLOCK_REALTIME 整秒
	unsigned long		ktime_sec;       // CLOCK_MONOTONIC 整秒(无锁读用)
	struct timespec64	wall_to_monotonic;  // REALTIME→MONOTONIC 偏移(恒 ≤0)
	ktime_t			offs_real;        // = -wall_to_monotonic(ktime 形式)
	ktime_t			offs_boot;        // MONOTONIC→BOOTTIME(累计睡眠注入)
	ktime_t			offs_tai;         // MONOTONIC→TAI
	s32			tai_offset;       // UTC↔TAI 整秒差
	unsigned int		clock_was_set_seq;  // 校时事件序号(hrtimer/快路径用)
	u8			cs_was_changed_seq; // clocksource 更换序号
	ktime_t			next_leap_ktime;    // 待插入闰秒的 MONOTONIC 时刻
	u64			raw_sec;            // CLOCK_MONOTONIC_RAW 整秒
	struct timespec64	monotonic_to_boot;  // offs_boot 的 timespec 形式(vDSO 用)

	/* —— 以下为 update_wall_time 专用内部量 —— */
	u64			cycle_interval;   // 一个 NTP 间隔 = cycle_interval 个周期
	u64			xtime_interval;   // 一个间隔产生的 shifted ns (=cycle_interval*mult)
	s64			xtime_remainder;  // 与 NTP 理想间隔的差(取整残余)
	u64			raw_interval;     // raw 轴的每间隔 shifted ns
	u64			ntp_tick;         // 当前采用的 ntp_tick_length() 缓存
	s64			ntp_error;        // 本地钟与 NTP 理想时间的累积误差
	u32			ntp_error_shift;  // 时钟域↔NTP 域(2^32) 的换挡
	u32			ntp_err_mult;
	u32			skip_second_overflow;
};
```

三个结构性洞察：

1. **`cs->mult` 与 `tkr_mono->mult` 是两份数据**：前者是注册时的静态标称，后者随 `timekeeping_adjust()` 每个 tick 微调（±11% 上限）。clocksource 结构因此彻底退出热路径。
2. **56 字节的 `tk_read_base` + seqcount 恰好一个 cacheline**（头文件注释原话）——读时间的缓存友好性是被字节级设计的。
3. **`xtime_nsec` 是"移位纳秒"**：单位是 1/2^shift 纳秒。所有累积在 shifted 域完成，只在最终读出时 `>>shift`——这是整个 timekeeping 无除法的原因。

### 双缓冲与快照体系

```c
// kernel/time/timekeeping.c (v6.6)
static struct {
	seqcount_raw_spinlock_t	seq;         // 读者用 seqcount 协议
	struct timekeeper	timekeeper;        // ★ 全局唯一权威(活跃副本)
} tk_core ____cacheline_aligned;

static struct timekeeper shadow_timekeeper;   // 写侧草稿(写时复制再整体提交)

struct tk_fast {                    // NMI/trace 安全的"快照"读出基
	seqcount_latch_t	seq;        // 最低位 = 选 base[0]/base[1]
	struct tk_read_base	base[2];   // 双份, latch 切换
};
static struct tk_fast tk_fast_mono;   // ktime_get_mono_fast_ns()
static struct tk_fast tk_fast_raw;    // ktime_get_raw_fast_ns()
```

* **shadow 的用途**：`timekeeping_advance()` 全程在 shadow 上算（可能重入/失败），`write_seqcount_begin()` 后 `memcpy(real_tk, tk)` 一次性提交——活跃副本要么全旧要么全新，读者永远看不到中间态。`TK_MIRROR` 动作把活跃态拷回 shadow 供下一轮起步。
* **tk_fast 的用途**：`ktime_get_mono_fast_ns()` 服务 NMI/持锁 trace——普通 seqcount 若在写侧 `begin/end` 之间被 NMI 打断，重试循环可能死锁（NMI 里不能再等）。latch 版本读者按 seq 最低位选 base[0/1]，写侧 `update_fast_timekeeper()` 先 latch 走读者到 base[1] → 更新 base[0] → latch 回 base[1] 同步。最坏代价是几纳秒的旧斜率时间戳（函数文档的 ASCII 注释展示了 reader 6 观察到时间倒退的场景）。

### 读取协议（读者视角，一遍就懂）

```c
do {
	seq = read_seqcount_begin(&tk_core.seq);
	base = tk->tkr_mono.base;                    /* 整数基准 */
	nsecs = timekeeping_get_ns(&tk->tkr_mono);   /* 锚点之后的增量 */
} while (read_seqcount_retry(&tk_core.seq, seq));
return ktime_add_ns(base, nsecs);
```

`timekeeping_get_ns()` 内部：`tk_clock_read()`（READ_ONCE(clock) 后调 `cs->read`，防止换钟竞态崩引用）→ `clocksource_delta(now, cycle_last, mask)` → `timekeeping_delta_to_ns(tkr, delta)`：

```c
static inline u64 timekeeping_delta_to_ns(const struct tk_read_base *tkr, u64 delta)
{
	u64 nsec = delta * tkr->mult + tkr->xtime_nsec;   /* 锚点小数一起算 */
	nsec >>= tkr->shift;
	return nsec;
}
```

---

## ⏱️ 二、update_wall_time：tick 驱动的时间推进

每次 tick（或 NOHZ 醒来补账）调用 `update_wall_time()` → `timekeeping_advance(TK_ADV_TICK)`：

```c
// kernel/time/timekeeping.c (v6.6, 核心骨架)
static bool timekeeping_advance(enum timekeeping_adv_mode mode)
{
	struct timekeeper *real_tk = &tk_core.timekeeper;
	struct timekeeper *tk = &shadow_timekeeper;          /* ① 草稿开工 */

	offset = clocksource_delta(tk_clock_read(&tk->tkr_mono),
				   tk->tkr_mono.cycle_last, tk->tkr_mono.mask);
	if (offset < real_tk->cycle_interval && mode == TK_ADV_TICK)
		goto out;                                        /* 不满一个间隔, 免单 */

	/* ② 对数累加: shift = ilog2(offset) - ilog2(cycle_interval)
	 *    每轮吃掉 cycle_interval << shift 的一大块, 不够就 shift-- */
	shift = max(0, ilog2(offset) - ilog2(tk->cycle_interval));
	shift = min(shift, maxshift);      /* maxshift 防止 ntp_tick_length 溢出 */
	while (offset >= tk->cycle_interval)
		offset = logarithmic_accumulation(tk, offset, shift--, &clock_set);

	/* ③ 频率自适应调整(见下) */
	timekeeping_adjust(tk, offset);
	/* ④ 把小数收进整秒(可能触发闰秒) */
	clock_set |= accumulate_nsecs_to_secs(tk);

	write_seqcount_begin(&tk_core.seq);       /* ⑤ 发布窗口 */
	timekeeping_update(tk, clock_set);        /* 更新 ktime 基准/vDSO/tk_fast */
	memcpy(real_tk, tk, sizeof(*tk));         /* ★ 整体提交, 必须最后 */
	write_seqcount_end(&tk_core.seq);
	return !!clock_set;    /* 有时钟跳变 → clock_was_set_delayed() 通知 hrtimer */
}
```

NOHZ 长睡眠后一次可能要累积几十万个 cycle_interval，`logarithmic_accumulation()` 用倍增分块把循环压到 O(log)：`interval = cycle_interval << shift` 一次吃 2^shift 个 tick，同步推进 `cycle_last`、`xtime_nsec += xtime_interval << shift`、raw 轴、以及 `ntp_error`（`ntp_error += ntp_tick << shift − (xtime_interval+remainder) << (…+shift)`，度量本地钟与 NTP 理想值的偏差）。

### 闰秒：accumulate_nsecs_to_secs + second_overflow

```c
while (tk->tkr_mono.xtime_nsec >= nsecps) {        /* nsecps = 1e9 << shift */
	tk->tkr_mono.xtime_nsec -= nsecps;
	tk->xtime_sec++;
	leap = second_overflow(tk->xtime_sec);      /* kernel/time/ntp.c */
	if (unlikely(leap)) {                       /* 插闰秒: leap=-1 */
		tk->xtime_sec += leap;              /* 23:59:60: 同一秒值停留两次 */
		tk_set_wall_to_mono(tk, timespec64_sub(tk->wall_to_monotonic, ts));
		__timekeeping_set_tai_offset(tk, tk->tai_offset - leap);
		clock_set = TK_CLOCK_WAS_SET;
	}
}
```

`second_overflow()`（ntp.c）内是 `TIME_OK/INS/DEL/OOP/WAIT` 状态机：NTP 守护进程用 `adjtimex(ADJ_STATUS|STA_INS)` 宣告"今天 UTC 末尾插入闰秒"，到点后返回 -1，REALTIME 在 23:59:59 停一秒再进位；TAI 偏移同时 +1。**闰秒期间 REALTIME 秒值重复，MONOTONIC 照常推进**——基于 MONOTONIC 的测量不受影响。

### timekeeping_adjust：mult 的自适应 ±1

```c
// kernel/time/timekeeping.c (v6.6, 节选)
static void timekeeping_adjust(struct timekeeper *tk, s64 offset)
{
	if (likely(tk->ntp_tick == ntp_tick_length()))     /* 多数 tick: 纯查表 */
		mult = tk->tkr_mono.mult - tk->ntp_err_mult;
	else {                                              /* NTP 改了节拍: 重算 */
		tk->ntp_tick = ntp_tick_length();
		mult = div64_u64((tk->ntp_tick >> tk->ntp_error_shift)
				 - tk->xtime_remainder, tk->cycle_interval);
	}
	tk->ntp_err_mult = tk->ntp_error > 0 ? 1 : 0;      /* 落后了就多加 1 */
	mult += tk->ntp_err_mult;

	timekeeping_apply_adjustment(tk, offset, mult - tk->tkr_mono.mult);
	/* 上限检查: 超过 clocksource 的 maxadj(~11%) 打印
	 * "Adjusting %s more than 11%%" 警告 */
	/* xtime_nsec 下溢兜底: 借 1 秒, xtime_sec--, skip_second_overflow=1 */
}
```

`timekeeping_apply_adjustment()` 里那段著名的推导（6.6 源码注释）：mult+1 后同一段周期算出的时间多了，为保"当前时刻"连续，必须 `xtime_nsec -= offset`——把未来的加速提前预支掉。整套机制的稳态就是**每 tick 对 mult 做最多 ±1 的步进**，把本地钟斜率平滑拉向 NTP 期望斜率，同时把误差反馈进 `ntp_error` 决定下一步方向。纯整数、无除法（除法只在 `ntp_tick_length()` 变化时出现一次）。

---

## 🔄 三、NTP 同步：adjtimex 如何改写时间

### 3.1 状态与入口

```c
// kernel/time/ntp.c (v6.6, 变量集)
static s64	time_freq;        // 频率偏移 (scaled ns/s), 来自 ADJ_FREQUENCY
static s64	time_offset;      // 相位偏移 (scaled ns), 来自 ADJ_OFFSET
static long	time_adjust;     // adjtime() 的待摊销量
static s64	ntp_tick_adj;     // 启动参数 ntp_tick_adj= 注入的常量修正
static u64	tick_length;      // ★ 本秒的 NTP 间隔长度(2^32 域), timekeeping 直接消费
static u64	tick_length_base; // 无相位修正的基线
```

`adjtimex()` → `do_adjtimex()`（timekeeping.c）拿住 `timekeeper_lock` + seqcount 写锁后调 `__do_adjtimex()`（ntp.c）：

* `ADJ_FREQUENCY`：`time_freq = txc->freq * PPM_SCALE`（±500ppm 限幅 `MAXFREQ_SCALED`）。
* `ADJ_OFFSET`：进 `ntp_update_offset()`——Dave Mills 的 PLL/FLL 二元控制：短期用 PLL（相位按 `shift_right(offset, SHIFT_PLL + time_constant)` 分期偿还），长间隔（>MAXSEC 或 STA_FLL）切换 FLL（用相位/时间比直接修频率）；同时 `time_offset` 记下待偿相位。
* `ADJ_TICK`：`tick_usec` 直接改 tick 长度（粗调，±10% 限幅）。
* 之后统一 `ntp_update_frequency()`：

```c
second_length  = (tick_usec * NSEC_PER_USEC * USER_HZ) << NTP_SCALE_SHIFT;
second_length += ntp_tick_adj + time_freq;           /* 常量+频率修正 */
new_base       = div_u64(second_length, NTP_INTERVAL_FREQ);  /* ÷HZ */
tick_length   += new_base - tick_length_base;        /* 立即生效,不等整秒 */
```

每个整秒，`second_overflow()` 重新分配：`tick_length = tick_length_base + ntp_offset_chunk(time_offset)`，把相位误差按时间常数分期注入；顺带处理 `time_adjust`（见下）与闰秒状态机。`ntp_tick_length()` 就是返回 `tick_length`——上一节 timekeeping 消费的正是它。

### 3.2 slewing vs stepping：两条修正路线

| | slewing（捋顺） | stepping（跳变） |
| :--- | :--- | :--- |
| 入口 | `adjtimex(ADJ_OFFSET/ADJ_FREQUENCY/ADJ_TICK)` | `settimeofday`/`clock_settime`、`adjtimex(ADJ_SETOFFSET)` |
| 落点 | `tick_length`（每秒长度±）→ `timekeeping_adjust` | `do_settimeofday64()`/`timekeeping_inject_offset()` 直接改 `xtime_sec`+`offs_real` |
| 时间连续性 | 单调、连续，±500ppm 内缓移 | 墙钟瞬间跳，**MONOTONIC 基准永不动**（只动 offs_real） |
| 典型使用者 | chrony/ntpd 日常追频 | 开机粗校、大偏差重对齐、PTP jump |
| `adjtime(delta)` 特例 | `time_adjust` 记账，`second_overflow()` 每秒最多摊 `MAX_TICKADJ=500us` | — |

工程含义：**监控/日志时间戳用 MONOTONIC 或 CLOCK_TAI 才能对 stepping 免疫**；REALTIME 上做差值测量在 chrony 大步进时会得到负数或巨大正数。

---

## 📖 四、clock_gettime 的完整路径

### 4.1 vDSO 快路径（细看数据协议）

内核在每次 `timekeeping_update()` 时经 `update_vsyscall()`（kernel/time/vsyscall.c）把读出所需的最小集合写进**每个 CPU 的 `vdso_data` 页**：

```c
// kernel/time/vsyscall.c :: update_vdso_data() (v6.6, 节选)
vdata[CS_HRES_COARSE].cycle_last = tk->tkr_mono.cycle_last;
vdata[CS_HRES_COARSE].mask       = tk->tkr_mono.mask;
vdata[CS_HRES_COARSE].mult       = tk->tkr_mono.mult;   /* NTP 调整后的! */
vdata[CS_HRES_COARSE].shift      = tk->tkr_mono.shift;
/* CLOCK_MONOTONIC 基准: (xtime_sec + wall_to_monotonic) 秒 + shifted nsec */
vdso_ts->sec  = tk->xtime_sec + tk->wall_to_monotonic.tv_sec;
vdso_ts->nsec = tk->tkr_mono.xtime_nsec + (wall_to_monotonic.tv_nsec << shift);
/* BOOTTIME 在 MONOTONIC 上再加 monotonic_to_boot; CS_RAW 页装 tkr_raw 四件套 */
```

用户态算法（lib/vdso 通用实现，x86/arm64 共享）：

```
do {
    seq     = vdso_data.seq;               /* ① 拿版本 */
    mode    = vdso_data.clock_mode;        /* ② 拿读钟方式 */
    cycles  = 读硬件(TSC→rdtsc_ordered / ARCHTIMER→cntvct) /* ③ */
    if (seq & 1 || mode 不可用) 走慢路径;
    base    = basetime[which_clock];       /* ④ 该时钟的 sec/nsec 基准 */
    delta   = (cycles - cycle_last) & mask;
    nsec    = ((delta * mult) >> shift) + base.nsec;   /* 与内核同一公式 */
} while (seq != vdso_data.seq);            /* ⑤ 版本变了重来 */
```

* **精确模式**（默认）：③ 读硬件计数器，纳秒级。
* **粗模式**：`CLOCK_*_COARSE` 直接用 ④ 的 base（跳过 ③），精度一 tick、零硬件访问——这就是内核侧 `ktime_get_coarse_real_ts64()`（只读 `tk_xtime(tk)`）的用户态镜像。
* `clock_mode == VDSO_CLOCKMODE_NONE`（clocksource 无 vDSO 资格 / errata）→ 整体退回系统调用。

### 4.2 系统调用慢路径

```
clock_gettime(which_clock, tp)
  → SYSCALL_DEFINE2(clock_gettime)                  kernel/time/posix-timers.c
      → clockid_to_kclock(id)                       /* 负 id: CPU/FD 时钟;
                                                       正 id: posix_clocks[] 分派 */
      → kc->clock_get_timespec(which_clock, &ts)
          CLOCK_REALTIME      → ktime_get_real_ts64()   ┐
          CLOCK_MONOTONIC     → ktime_get_ts64()        │ 同一套 seqcount 协议,
          CLOCK_BOOTTIME      → ktime_get_boottime()    │ 区别只在 base 加哪个 offset
          CLOCK_MONOTONIC_RAW → ktime_get_raw_ts64()    ┘ (ktime_get_with_offset)
```

以 `ktime_get_with_offset(TK_OFFS_REAL)` 为例：seq 循环里 `base = ktime_add(tk->tkr_mono.base, *offset)`（offset 指向 `tk->offs_real`），再 `+ timekeeping_get_ns()`——**一次读硬件、一次 seq 校验、两次加法**，与 vDSO 逐指令同构，只是多了系统调用/分派/copy 的固定开销。

> 版本注：一些老资料写 `ksys_clock_gettime`——那是 5.x 时代 io_uring/内核内部复用的包装；6.6 中直接是 `SYSCALL_DEFINE2(clock_gettime)` + `k_clock` 分派表，语义不变。

---

## 🔢 五、jiffies 的维护与 64 位回绕

jiffies 是"tick 计数"语义的时间轴，与 wall clock 完全解耦（`CONFIG_HZ` 决定粒度）：

```c
// kernel/time/timer.c
__visible u64 jiffies_64 __cacheline_aligned_in_smp = INITIAL_JIFFIES;
/* INITIAL_JIFFIES = (unsigned long)(unsigned int)(-300*HZ)
 * 故意把 32 位回绕点拉到开机后 ~5 分钟, 让回绕 bug 早暴露 */

// kernel/time/timekeeping.c
void do_timer(unsigned long ticks)          /* 仅 do_timer_cpu 执行 */
{
	jiffies_64 += ticks;
	calc_global_load();                     /* 顺带算全局负载 */
}
```

### tick_do_update_jiffies64：NOHZ 下的补偿推进

```c
// kernel/time/tick-sched.c (v6.6, 骨架)
static void tick_do_update_jiffies64(ktime_t now)
{
	/* 64 位: 无锁快查(时间未到下个整 tick 就走人)
	 * smp_load_acquire(tick_next_period) 与写侧 smp_store_release 配对 */
	if (ktime_before(now, smp_load_acquire(&tick_next_period)))
		return;
	raw_spin_lock(&jiffies_lock);
	if (ktime_before(now, tick_next_period)) { unlock; return; }  /* 重验 */
	write_seqcount_begin(&jiffies_seq);

	delta = ktime_sub(now, tick_next_period);
	if (delta >= TICK_NSEC)                       /* 长睡补偿: 整除出 tick 数 */
		ticks += ktime_divns(delta, TICK_NSEC);
	last_jiffies_update += incr * ticks;          /* 理想 tick 网格 */
	jiffies_64 += ticks;
	tick_next_period = last_jiffies_update + TICK_NSEC;  /* release 存储 */
	write_seqcount_end(&jiffies_seq);

	calc_global_load();
	update_wall_time();                           /* ★ jiffies 与 wall time 同步推进 */
}
```

设计细节：

* **`tick_next_period` 是理想网格锚**：NOHZ 睡了 3.7 个 tick，醒来 jiffies **只 +3**（按网格对齐），第 4 个 tick 的时间没到不给——jiffies 永远不虚报。
* **`jiffies_seq`（seqcount）护的是 32 位系统**：`jiffies_64` 是两个 32 位字的非原子写；64 位上快查无锁靠 acquire/release。
* **谁有资格推进**：全局变量 `tick_do_timer_cpu` 指定唯一 CPU（初始 `TICK_DO_TIMER_BOOT`）；进 NOHZ 睡眠时主动弃权为 `TICK_DO_TIMER_NONE`（`tick_nohz_stop_tick`），由"下一个醒来的 CPU"接管（`tick_sched_do_timer`）——防的是所有 CPU 一起抢 `jiffies_lock` 的惊群。
* **回绕比较**：`time_after(a,b)` 展开为 `(long)((b)-(a)) < 0`，依赖无符号回绕差的符号位判断先后——因此 **jiffies 比较永远用宏，绝不能直接 `<` 比较**。64 位 `jiffies_64` 回绕周期以百年计，但仍统一走 `time_after64` 等宏。

---

## 🐞 六、timekeeping_debug：自我怀疑的机制

`CONFIG_DEBUG_TIMEKEEPING=y` 时（timekeeping.c 内嵌）：

* `timekeeping_check_update()`：单次累积 offset 超过 `max_cycles` → `"WARNING: ... Cycle offset (%lld) is larger than allowed ... time overflow danger ... Your kernel is sick, but tries to cope by capping time updates"`；超过一半打 INFO（"still fine, but feeling a bit nervous"）。
* `timekeeping_get_delta()`：内嵌二层 seqcount 抓取锚点快照，检测**回读变小**（underflow：clocksource 停振/复位）与**超过 max_cycles**（overflow），各以 5 分钟限速打印 `"Underflow in clocksource '%s' observed, time update ignored"`。这类消息几乎总是固件/虚拟化层的时钟跳变前兆。
* 6.6 的 timekeeping.c 同目录还有 `timekeeping_debug.c`：`tk_debug_account_sleep_time()` 把每次 suspend 注入的睡眠时长直方图打到 dmesg（`midpoint` 分桶）。

---

## 🌙 七、suspend/resume：睡眠时间的补账

`timekeeping_suspend()/resume()`（syscore_ops，最早期执行）的三级时钟偏好（源码注释原文顺序）：

```
1) SUSPEND_NONSTOP 的 clocksource   (x86 nonstop_tsc; arm64 系统计数器)
2) persistent clock                 (read_persistent_clock64, RTC 类)
3) RTC 设备                         (rtc_suspend/resume 兜底, CONFIG_RTC_HCTOSYS)
```

suspend：`timekeeping_forward_now()` 收尾 → `clocksource_start_suspend_timing()` 让 suspend clocksource 开始计时 → `halt_fast_timekeeper()` 把 tk_fast 换成 dummy（挂起后读真钟是未定义行为）。resume：`clocksource_stop_suspend_timing()` 算出 ns → `__timekeeping_inject_sleeptime()` 同时加 `xtime_sec`、反向减 `wall_to_monotonic`、累加 `offs_boot`——**REALTIME 与 BOOTTIME 补上睡眠，MONOTONIC 不补**（它定义上不含睡眠）。最后 `tick_resume()` + `timerfd_resume()`（后者把"cancel on set"语义等价地通知一遍 timerfd）。

---

## 🔭 八、观测调试

```bash
# NTP 内核状态全景 (adjtimex syscall 的用户态镜像; util-linux 自带)
adjtimex --print | head -30
# 关注: time_offset/time_freq(scaled), status(STA_* 位), 常量 constant,
#       tai, 以及 time.tv_sec/sec——校时/追频的直接证据

# chrony 视角(若在用): 内核纪律状态与残余偏差
chronyc tracking; chronyc sourcestats

# jiffies 与 tick 体检
grep -E "LOC|timer" /proc/interrupts        # 本地定时器中断计数
cat /proc/uptime                             # 与 CLOCK_BOOTTIME 交叉验证睡眠注入

# 各时钟一致性 & NTP 路径回归(kernel 自测)
make -C tools/testing/selftests/timers
tools/testing/selftests/timers/raw_skew          # MONOTONIC_RAW vs MONOTONIC 频偏
tools/testing/selftests/timers/inconsistency-check
tools/testing/selftests/timers/adjtick           # ADJ_TICK 路径
tools/testing/selftests/timers/valid-adjtimex
tools/testing/selftests/timers/leap-a-day        # 闰秒状态机(测试环境!)

# vDSO 是否命中(零系统调用=命中)
strace -c -e trace=clock_gettime ./your_app
perf stat -e syscalls:sys_enter_clock_gettime ./your_app

# DEBUG_TIMEKEEPING 告警
dmesg | grep -i "timekeeping\|cycle offset\|underflow\|overflow"
```

---

## 💭 九、思考题

1. `timekeeping_advance()` 在 `write_seqcount_begin()` 与 `memcpy(real_tk, tk)` 之间还调用 `timekeeping_update()`（更新 vDSO/tk_fast）。为什么这些副作用必须放在 seqcount 写区**内部**？放外面会坏什么？
   <details><summary>提示</summary>vDSO 页与 tk_fast 里的 cycle_last/mult 必须与 real_tk 的同批数据对齐；若读者已看到新 vDSO 数据但 real_tk 仍旧，二者用不同 mult 解释同一段 cycles，时间读数会撕裂。seqcount 写区正是"这批数据属于同一次更新"的原子性边界。</details>
2. `timekeeping_adjust()` 里 `ntp_err_mult` 只取 0/1（落后 +1，否则 0）。为什么不对称地也允许 −1"减速"？什么场景确实需要减速，靠什么实现？
   <details><summary>提示</summary>mult 基线来自 `ntp_tick_length()`（已含 NTP 期望斜率），本地钟只需"追上去"；快于期望时重算基线本身会变小（`tk->ntp_tick == ntp_tick_length()` 失配路径），−1 步进会被基线下调吸收。真正的减速上限由 clocksource 的 maxadj（11%）把守并打印警告。</details>
3. chrony 正在 slew 修正时，一个用 `CLOCK_REALTIME` 做差值测量的程序会看到什么？换 `CLOCK_MONOTONIC`/`CLOCK_MONOTONIC_RAW`/`CLOCK_TAI` 分别看到什么？
   <details><summary>提示</summary>slew 期间 REALTIME 斜率 ±500ppm 偏移（差值系统性偏 0.05%）；MONOTONIC 同样吃 mult 调整（它就是基准轴）；MONOTONIC_RAW 完全不受影响（tkr_raw 独立累计）；TAI 与 REALTIME 差固定整秒，斜率行为同 MONOTONIC 基准+offset。</details>
4. `tk_fast_mono` 的文档承认 update 期间读者可能拿到旧斜率导致时间戳**倒退**几纳秒。哪些子系统必须容忍这种倒退、哪些绝对不能？各举一例。
   <details><summary>提示</summary>用户：printk 时间戳/NMI 采样（容忍，事后可排序）；不能用的场景：需要严格单调序的持久化序号/锁的过期判断——那些应基于自己的序号源。普通 `ktime_get()`（tk_core+seqcount）不受此影响。</details>
5. `tick_do_update_jiffies64()` 的快路径在 64 位上无锁，靠 `smp_load_acquire(&tick_next_period)`。若把它换成普通 load，最坏会发生什么？
   <details><summary>提示</summary>写侧顺序是 jiffies_64 += ticks; smp_store_release(tick_next_period, nextp)。读者普通 load 可能先看到新 tick_next_period 而后（弱序）读旧 jiffies——直接走人则本 tick 的 jiffies 延迟可见；更糟的是 acquire 语义丢失后读者后续对 jiffies 的读取可能被重排。对照 32 位路径必须走 jiffies_seq 的原因。</details>
6. 为什么 `do_settimeofday64()` 改 REALTIME 时必须同步反向修改 `wall_to_monotonic`，而 `__timekeeping_inject_sleeptime()` 却要**正向**修改它？
   <details><summary>提示</summary>不变式是 MONOTONIC = REALTIME + wall_to_monotonic 且 MONOTONIC 不可回退。settimeofday 把墙钟前拨 d：MONOTONIC 不动，故 wtm 需减 d（tk_set_wall_to_mono 注释先验证 offs_real == -wtm 再改）。睡眠注入 d 秒时墙钟确实前进了 d，MONOTONIC 仍不动（不含睡眠），故 wtm 也要减 d——两者同向，本质都是"墙钟动了而基准没动"。</details>

---

*上一章：[clocksource 与时钟硬件](02-clocksource与时钟硬件.md)　|　下一章：[时间轮与低分辨率定时器](04-时间轮与低分辨率定时器.md) —— 6.6 非级联时间轮的层级粒度、bucket 计算与 NOHZ 迁延到期。*
