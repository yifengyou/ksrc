# clocksource 与时钟硬件（Linux 6.6）

> 阅读目标：逐字段吃透 `struct clocksource`；掌握 mult/shift 定点换算的数学（含回绕处理与 11%/50% 两条安全边界）；理解 watchdog 的监控协议、rating 降级与 fallback 重选链（`__clocksource_unstable → clocksource_select`）；对照 x86 TSC 与 arm64 通用定时器两大参考实现，理解频率不变性为何是现代时间子系统的命根。

所有代码出自 Linux 6.6：`include/linux/clocksource.h`、`kernel/time/clocksource.c`、`arch/x86/kernel/tsc.c`、`drivers/clocksource/arm_arch_timer.c`。

---

## 🧩 一、struct clocksource 逐字段

```c
// include/linux/clocksource.h (v6.6)
struct clocksource {
	u64	(*read)(struct clocksource *cs);   // 读硬件计数器, 唯一必填的热路径回调
	u64	mask;          // 位掩码: 回绕周期-1, 如 CLOCKSOURCE_MASK(64)
	u32	mult;          // 周期→纳秒 乘数 (NTP 会微调 timekeeper 里的副本)
	u32	shift;         // 除数, 恒为 2 的幂
	u64	max_idle_ns;   // 两次读数之间允许的最大空闲时间(NOHZ 长睡眠上限)
	u32	maxadj;        // mult 允许的最大调整量 (~11%)
	u32	uncertainty_margin;  // watchdog 判不稳的豁免带宽
	u64	max_cycles;    // 乘 mult 不溢出的最大周期数
	const char	*name;
	struct list_head	list;      // 挂入 clocksource_list(按 rating 降序)
	int	rating;        // 评分, 选择依据
	enum clocksource_ids	id;     // CSID_TSC/CSID_ARM_ARCH_TIMER/... (快照比对用)
	enum vdso_clock_mode	vdso_clock_mode;  // vDSO 用哪种方式读此钟
	unsigned long	flags;         // 见下表

	int	(*enable)(struct clocksource *cs);
	void	(*disable)(struct clocksource *cs);
	void	(*suspend)(struct clocksource *cs);   // 电源管理
	void	(*resume)(struct clocksource *cs);
	void	(*mark_unstable)(struct clocksource *cs); // watchdog 判不稳时回调驱动
	void	(*tick_stable)(struct clocksource *cs);   // 每次校验通过时回调(TSC 同步点)

#ifdef CONFIG_CLOCKSOURCE_WATCHDOG
	struct list_head	wd_list;   // 挂入 watchdog_list
	u64	cs_last;                   // 上次被校验时的本钟读数
	u64	wd_last;                   // 上次校验时的 watchdog 读数
#endif
	struct module	*owner;
};
```

字段语义补充：

* **`mask` 与回绕**：计数器是模运算硬件。32 位 ACPI_PM 每 ~4.3s 回绕一次，TSC 64 位要数百年。`clocksource_delta()` 用补码减法天然处理回绕（见第三节）。
* **`mult/shift` 属于"注册时静态计算、timekeeper 动态微调"的二元体系**：注册方用 `clocks_calc_mult_shift()` 算出基准对；timekeeper 在 `tk->tkr_mono.mult` 上叠加以 ppm 计的 NTP 修正（第 3 章）。
* **`vdso_clock_mode`**：决定用户态能否绕过内核直接读钟。x86 为 `VDSO_CLOCKMODE_TSC`，arm64 为 `VDSO_CLOCKMODE_ARCHTIMER`；不满足条件时置 `VDSO_CLOCKMODE_NONE`，用户态自动退回系统调用。
* 头文件注释明确：**本结构不在 timekeeping 热路径上**（热路径字段被缓存进 `tk_read_base`），所以无需 cacheline 对齐——省内存换来的。

### rating 段位（选择 clocksource 的唯一准绳）

| 段位 | 含义 | 例子 |
| :--- | :--- | :--- |
| 1–99 | 不适合实际使用，仅启动/测试 | jiffies（**rating 1**，kernel/time/jiffies.c） |
| 100–199 | 基本可用，非理想 | ACPI_PM、PIT |
| 200–299 | 良好 | tsc-early（**299**） |
| 300–399 | 期望采用 | tsc（**300**）、HPET |
| 400–499 | 完美，必须采用 | arm64 `arch_sys_counter`（**400**） |

### flags 位

| 位 | 值 | 语义 |
| :--- | :--- | :--- |
| `CLOCK_SOURCE_IS_CONTINUOUS` | 0x01 | 自由运行、不可被编程/停止 → 是 hrtimer 的前提 |
| `CLOCK_SOURCE_MUST_VERIFY` | 0x02 | 该钟**必须**被 watchdog 校验（TSC 两兄弟都置位） |
| `CLOCK_SOURCE_WATCHDOG` | 0x10 | watchdog 内部记账：已采到基线样本 |
| `CLOCK_SOURCE_VALID_FOR_HRES` | 0x20 | 校验通过 → 允许进高精度模式 |
| `CLOCK_SOURCE_UNSTABLE` | 0x40 | 已判不稳，rating 将被清零 |
| `CLOCK_SOURCE_SUSPEND_NONSTOP` | 0x80 | suspend 期间不停 → 可用于测量睡眠时长（x86 nonstop_tsc） |
| `CLOCK_SOURCE_RESELECT` | 0x100 | 请 kthread 重新选择 |
| `CLOCK_SOURCE_VERIFY_PERCPU` | 0x200 | 需跨 CPU 同步性校验（tsc 置位） |

---

## 🔢 二、mult/shift 定点换算数学

### 2.1 基本换算：cycles → ns

内核全程避免除法（早期 CPU 除法 ~40 周期，乘法 3 周期），把"每周期多少纳秒"编码为分数 `mult / 2^shift`：

```c
// include/linux/clocksource.h (v6.6)
static inline s64 clocksource_cyc2ns(u64 cycles, u32 mult, u32 shift)
{
	return ((u64) cycles * mult) >> shift;
}
```

推导（`clocksource_freq2mult()` 注释原文）：设频率 `freq = cyc/from`（from 单位与 freq 配套，如 Hz 时 from=1s），要求 `mult/2^shift = ns/cyc`，则

```
mult = (from << shift) / freq          （clocksource_hz2mult: from=NSEC_PER_SEC）
```

例：TSC 2.4GHz（=2.4 cyc/ns），即 ns/cyc = 1/2.4 ≈ 0.4167。取 shift=24：mult = (1e9 << 24)/2.4e9 ≈ 69905067，即 69905067/2^24 ≈ 0.416666——24 位小数精度，误差 < 2^-24 ≈ 0.06ppm，恰是 `__clocksource_update_freq_scale()` 注释里"~0.06ppm granularity for NTP"的来源。

### 2.2 回绕安全：clocksource_delta()

```c
// include/linux/timekeeping_internal.h (思想, 6.6 于此及相关内联展开)
static inline u64 clocksource_delta(u64 now, u64 last, u64 mask)
{
	return (now - last) & mask;     /* 补码减法 + 截断 = 模 2^bits 减法 */
}
```

32 位 ACPI_PM（mask=0xFFFFFFFF）读数从 0xFFFFFFFF 回绕到 3 时，`3 - 0xFFFFFFFF` 的 64 位结果是个巨大的负数，但 `& 0xFFFFFFFF` 后得到 4——正是真实经过的周期数。**约束**：两次读数间隔不能超过回绕周期的一半，否则无法区分"前进了 4"还是"后退了一大圈"。这正是 `max_idle_ns` 的由来。

### 2.3 clocks_calc_mult_shift：为给定频率找最优对

```c
// kernel/time/clocksource.c (v6.6, 节选)
void clocks_calc_mult_shift(u32 *mult, u32 *shift, u32 from,
			    u32 to, u32 maxsec)
{
	u64 tmp;
	u32 sft, sftacc = 32;

	/* shift 上限由 maxsec 保证的换算范围决定 */
	tmp = ((u64)maxsec * from) >> 32;
	while (tmp) { tmp >>= 1; sftacc--; }

	/* 从 shift=32 往下找: 第一个满足 mult < 2^sftacc(不溢出)的即为最优精度 */
	for (sft = 32; sft > 0; sft--) {
		tmp = (u64) to << sft;
		tmp += from / 2;              /* 四舍五入 */
		do_div(tmp, from);
		if ((tmp >> sftacc) == 0)
			break;
	}
	*mult = tmp;
	*shift = sft;
}
```

`maxsec`（保证不溢出的换算秒数）由注册方决定：`sec = mask / freq`，但对 mask > 32 位的钟**封顶 600 秒**——精度与范围互换（注释：40 位以上 + 4GHz 时 shift=24，即 0.06ppm）。

### 2.4 两条安全边界：11% 与 50%

* **`maxadj`（11%）**：NTP 对 mult 的修正上限。`clocksource_max_adjustment()`：`mult * 11 / 100`。NTP 的合法调整量本就 ≤ ±500ppm，11% 余量是为了吸收硬件频偏 + 厂商标称误差。修正后 mult 若溢出，`__clocksource_update_freq_scale()` 会把 mult/shift 同时右移一位（精度减半）重试。
* **`max_idle_ns`（50%）**：`clocks_calc_max_nsecs()` 先算"乘 (mult+maxadj) 不溢出 64 位"的 `max_cycles = min(ULLONG_MAX/(mult+maxadj), mask)`，再取 `clocksource_cyc2ns(max_cycles, mult - maxadj, shift)`（**用最低速斜率**算，防负向调整后欠账），最后 **>>1 砍半**——注释明言是留出检测"坏间隔"的余量。NOHZ 空闲 CPU 最长睡眠即被 `timekeeping_max_deferment()` 用此值限制（第 6 章）。
* **`uncertainty_margin`**：watchdog 允许的偏差带宽。未指定时取"一个周期"（`NSEC_PER_SEC/(scale*freq)`），下限 `2*WATCHDOG_MAX_SKEW`；无频率信息时保守取 `WATCHDOG_THRESHOLD`（31.25ms）。

---

## 📋 三、注册链与选择链

### 3.1 __clocksource_register_scale：驱动的入口

```c
// kernel/time/clocksource.c (v6.6, 节选)
int __clocksource_register_scale(struct clocksource *cs, u32 scale, u32 freq)
{
	clocksource_arch_init(cs);
	/* ① 静态量就位: mult/shift, uncertainty_margin, maxadj, max_cycles,
	 *    max_idle_ns —— 即上一节全部数学 */
	__clocksource_update_freq_scale(cs, scale, freq);

	mutex_lock(&clocksource_mutex);
	clocksource_watchdog_lock(&flags);
	/* ② 按 rating 降序插入 clocksource_list */
	clocksource_enqueue(cs);
	/* ③ MUST_VERIFY 的钟进 watchdog_list; 其余若 CONTINUOUS 直接获得 HRES 资格 */
	clocksource_enqueue_watchdog(cs);
	clocksource_watchdog_unlock(&flags);

	/* ④ 选主钟 + 选 watchdog 本尊 + 选 suspend 计时钟(三套独立选择!) */
	clocksource_select();                 // → timekeeping_notify → 换 timekeeper 的钟
	clocksource_select_watchdog(false);   // → rating 最高且非 MUST_VERIFY 者
	__clocksource_suspend_select(cs);     // → SUSPEND_NONSTOP 中 rating 最高者
	mutex_unlock(&clocksource_mutex);
	return 0;
}
```

日常封装：`clocksource_register_hz(cs, hz)` / `clocksource_register_khz(cs, khz)`。

### 3.2 clocksource_select：换钟的 stop_machine 仪式

```c
// kernel/time/clocksource.c (v6.6, 关键路径)
__clocksource_select(skipcur)
    → clocksource_find_best(oneshot, skipcur)   // 按序扫链表头就是最优;
                                                // oneshot 模式跳过无 HRES 资格者
    → override 检查                              // /sys/.../current_clocksource 或
                                                // clocksource= 启动参数
    → timekeeping_notify(best)                   // kernel/time/timekeeping.c
        → stop_machine(change_clocksource, clock, NULL)  // ★ 全世界停下
            → timekeeping_forward_now(tk)        // 把旧钟时间收尾
            → tk_setup_internals(tk, new)        // 重算 cycle_interval/xtime_interval...
        → tick_clock_notify()                    // 通知 tick: 重新评估 hres 资格
    → pr_info("Switched to clocksource %s\n", best->name);
```

为何 `stop_machine`：`tk_core.seq` 与 `timekeeper_lock` 保护的是数据一致性，但**所有 CPU 可能同时在各自的 `tk_read_base` 上调用 `cs->read()`**——换钟瞬间若某 CPU 仍拿旧 read 函数读新钟（或反之）就是崩溃。全机器静止是唯一廉价正确的做法（clocksource 切换是稀有事件）。

---

## 🐕 四、watchdog：谁来看住看时间的人

一个 rate 高的钟也可能悄悄坏掉（固件 bug、SMI 吞周期、虚拟化窃时）。watchdog 用**另一个钟**交叉验证。

### 4.1 常量与结构

```c
// kernel/time/clocksource.c (v6.6)
#define WATCHDOG_INTERVAL (HZ >> 1)          // 校验周期: 0.5 秒
#define WATCHDOG_THRESHOLD (NSEC_PER_SEC >> 5)   // 31.25ms, 兼作默认 uncertainty
#define MAX_SKEW_USEC (125 * WATCHDOG_INTERVAL / HZ)  // 默认 62.5us, 源自 NTP 500ppm
```

* **watchdog 本尊的选择**（`clocksource_select_watchdog()`）：rating 最高、且**没有** `MUST_VERIFY` 标志的钟——它自己不需要被验证（x86 上通常是 HPET 或 ACPI_PM，TSC 因 MUST_VERIFY 永远当不了 watchdog）。
* **被监控者**：`watchdog_list` 上所有 `MUST_VERIFY` 的钟（TSC 两代）。
* 载体是一个普通的 `struct timer_list watchdog_timer`，**每次到期迁移到下一个 CPU**（`next_cpu = cpumask_next(...)`），避免单 CPU 的 SMI/停顿造成系统性误判。

### 4.2 cs_watchdog_read：三明治读法与重试

```c
// kernel/time/clocksource.c (v6.6, 核心逻辑)
local_irq_disable();
*wdnow  = watchdog->read(watchdog);   // ① watchdog 读数
*csnow  = cs->read(cs);               // ② 被测钟读数
wd_end  = watchdog->read(watchdog);   // ③ 再读 watchdog
wd_end2 = watchdog->read(watchdog);   // ④ 连续再读一次
local_irq_enable();

wd_delay = cyc2ns(wd_end - wd_now);   // 整个三明治耗时
if (wd_delay <= WATCHDOG_MAX_SKEW)
    return WD_READ_SUCCESS;           // 耗时正常, csnow 有效

/* 否则: SMI/NMI/抢占干扰了测量 */
wd_seq_delay = cyc2ns(wd_end2 - wd_end);
if (wd_seq_delay > WATCHDOG_MAX_SKEW/2)
    return WD_READ_SKIP;              // 系统太忙, 跳过本轮(顺带休眠 5 分钟)
/* 重试, 最多 max_cswd_read_retries(默认 2) 次 */
```

三明治的智慧：只有当 watchdog 自己证明"这段时间没发生怪事"，`csnow` 才可信。**不能反过来**——watchdog 读数有效性由 `wd_seq_delay`（连续两次读的差，天然排除 SMI）佐证。

### 4.3 clocksource_watchdog：每个 0.5s 的巡检

对 `watchdog_list` 上每口钟：

```c
delta = clocksource_delta(wdnow, cs->wd_last, watchdog->mask);
wd_nsec = clocksource_cyc2ns(delta, watchdog->mult, watchdog->shift);
delta = clocksource_delta(csnow, cs->cs_last, cs->mask);
cs_nsec = clocksource_cyc2ns(delta, cs->mult, cs->shift);
cs->cs_last = csnow;  cs->wd_last = wdnow;

md = cs->uncertainty_margin + watchdog->uncertainty_margin;
if (abs(cs_nsec - wd_nsec) > md) {
    pr_warn("timekeeping watchdog on CPU%d: Marking clocksource '%s' as unstable ...", ...);
    __clocksource_unstable(cs);
}
```

即：**两钟各自报道的 0.5 秒时长差**超过两者不确定度之和 → 判死。此外巡检还做两件事：给通过者点亮 `CLOCK_SOURCE_VALID_FOR_HRES`（触发 `tick_clock_notify`，tick 层借此升级高精度模式）；给当前主钟调用 `tick_stable()`（TSC 用它积累跨 CPU 同步证据）。

### 4.4 降级与 fallback 链

```
__clocksource_unstable(cs)
  → cs->flags |= CLOCK_SOURCE_UNSTABLE; 清 VALID_FOR_HRES
  → schedule_work(&watchdog_work)
      → kthread_run(clocksource_watchdog_kthread, NULL, "kwatchdog")
          → __clocksource_watchdog_kthread()
              → 对所有 UNSTABLE: list_del(wd_list) + __clocksource_change_rating(cs, 0)
                                   /* 从链表摘下, rating 归零, 重新插回队尾 */
              → clocksource_select()          // ★ 重选主钟 → fallback 到次优
```

于是 dmesg 里经典的死亡三连：`Marking clocksource 'tsc' as unstable` → `Switched to clocksource hpet` → 系统精度从纳秒退到 ~100ns+ 且 vDSO 失效（HPET 无 vDSO 模式）。x86 侧还有主动路径：`tsc_cs_mark_unstable()`（如检测到 non-constant TSC）同样汇入 `clocksource_mark_unstable()`。

### 4.5 跨 CPU 同步校验（tsc 专属）

`CLOCK_SOURCE_VERIFY_PERCPU`（tsc 置位）：kthread 会对不稳的主钟执行 `clocksource_verify_percpu()`——选 `verify_n_cpus`（默认 8）个 CPU，本 CPU 读 `csnow_begin`、IPI 让对方读 `csnow_mid`、再读本 CPU `csnow_end`，检查对方读数是否落在 [begin,end] 区间内，报告 "CPUs ahead/behind"。这是 2019 年为多 socket TSC 不同步问题加的诊断。

---

## ⚡ 五、x86 TSC：从可疑部件到主力时钟

```c
// arch/x86/kernel/tsc.c (v6.6)
static struct clocksource clocksource_tsc_early = {
	.name		= "tsc-early",
	.rating		= 299,                       // 让位于正式 tsc
	.uncertainty_margin = 32 * NSEC_PER_MSEC, // 早期对多 socket 同步存疑
	.read		= read_tsc,
	.mask		= CLOCKSOURCE_MASK(64),
	.flags		= CLOCK_SOURCE_IS_CONTINUOUS | CLOCK_SOURCE_MUST_VERIFY,
	.vdso_clock_mode	= VDSO_CLOCKMODE_TSC,
	.enable		= tsc_cs_enable,  .resume = tsc_resume,
	.mark_unstable	= tsc_cs_mark_unstable,
	.tick_stable	= tsc_cs_tick_stable,
};

static struct clocksource clocksource_tsc = {
	.name		= "tsc",
	.rating		= 300,
	.read		= read_tsc,
	.mask		= CLOCKSOURCE_MASK(64),
	.flags		= CLOCK_SOURCE_IS_CONTINUOUS |
			  CLOCK_SOURCE_VALID_FOR_HRES |      // 注释: 必须提前给,
			  CLOCK_SOURCE_MUST_VERIFY |         // 否则 tsc-early 注销时无人接班
			  CLOCK_SOURCE_VERIFY_PERCPU,
	.vdso_clock_mode	= VDSO_CLOCKMODE_TSC,
	...
};
```

要点：

1. **两段式注册**：early 版在枚举 PCI 之前就要供 timekeeping 使用；正式版经 CPUID 频率解析（`0x15`/`0x16` 叶：core/bus 晶振比）后由 `tsc_init()`（late_time_init 阶段）接管。
2. **`read_tsc` → `rdtsc_ordered()`**：按 CPU 能力选择最强屏障——有 `X86_FEATURE_RDTSCP` 用 `rdtscp; lfence`（保证"读之前所有 load 完成"），否则 `lfence; rdtsc`（保证"读之后才执行后续指令"）。没有屏障的裸 rdtsc 会被乱序引擎搬移，时间戳就是错的。
3. **constant_tsc**（CPUID.80000007:EDX[8] + 频率解析成功）：TSC 频率与 P-state 无关 → mult 长期有效。不具备时 x86 侧直接不注册 tsc clocksource。
4. **nonstop_tsc**（同 CPUID 位 + 验证）：deep idle/suspend 不停振 → `clocksource_tsc.flags |= CLOCK_SOURCE_SUSPEND_NONSTOP`，进而可能成为 **suspend clocksource**，resume 后用它精确注入睡眠时长（免去 RTC 粗测）。
5. **`tsc_cs_mark_unstable`** 会把 WATCHDOG 标志也清掉并打印 `Clocksource tsc unstable`——x86 场景下最常见的"时钟降级"起点。

## 🦾 六、arm64 通用定时器：教科书式的对称

```c
// drivers/clocksource/arm_arch_timer.c (v6.6)
static struct clocksource clocksource_counter = {
	.name	= "arch_sys_counter",
	.id	= CSID_ARM_ARCH_COUNTER,
	.rating	= 400,                          // "完美"档: 必须采用
	.read	= arch_counter_read,
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS,
	.vdso_clock_mode = VDSO_CLOCKMODE_ARCHTIMER,  // 默认(vdso_default)
};
/* mask 不在静态初始化里: 注册前由
 *   clocksource_counter.mask = CLOCKSOURCE_MASK(width);   // width 读自硬件,
 *   if (!arch_counter_suspend_stop)                          // 规范 56 位,
 *       flags |= CLOCK_SOURCE_SUSPEND_NONSTOP;               // "arm,no-tick-in-suspend"
 *   clocksource_register_hz(&clocksource_counter, arch_timer_rate);  // DT 可推翻 NONSTOP
 * 动态补齐 */
```

* **读数**：`arch_counter_get_cntvct()` → `read_sysreg(cntvct_el0)`。系统计数器由 SoC 侧独立供电的模块提供，所有 CPU 读到同一物理计数器——**天然跨核一致**，没有 x86 的多 socket 同步难题，也无需 watchdog 强制校验（无 MUST_VERIFY）。
* **精度顾虑**：读 EL0 寄存器前后需要 `isb` 序列化；有 ERRATA 时换稳定读法 `__arch_counter_get_cntvct_stable`（两次读 + isb 闭环）。
* **Errata 全家桶**：Freescale A-008585、HiSilicon 161010101、ARM 858921、Allwinner A64——全都是"读 CNTVCT 偶发跳变"类 bug，各自套一个检测/重读 workaround（`fsl_a008585_read_cntvct_el0` 等）。**一旦套上 workaround，`clocksource_counter.vdso_clock_mode = VDSO_CLOCKMODE_NONE`**——用户态不能再直读硬件，全部退回系统调用。这是"读起来最简单的钟"暗藏的最大工程代价。

### 双架构对照表

| 维度 | x86 TSC | arm64 arch_sys_counter |
| :--- | :--- | :--- |
| 读取 | `rdtsc_ordered`（屏障随 CPUID 能力） | `read_sysreg(cntvct_el0)` + isb |
| 计数器归属 | 每 CPU 一个，须软件保证同步 | 全系统一个，硬件保证同步 |
| rating | 300（tsc）/ 299（tsc-early） | 400 |
| watchdog | MUST_VERIFY，全程被监控 | 无 MUST_VERIFY（可当 watchdog 本尊） |
| 频率来源 | CPUID 0x15/0x16 + 晶振计算 | CNTFRQ_EL0（固件提供） |
| suspend | nonstop_tsc 才 SUSPEND_NONSTOP | 计数器通常独立供电，常 NONSTOP |
| vDSO | VDSO_CLOCKMODE_TSC | VDSO_CLOCKMODE_ARCHTIMER（errata 时 NONE） |
| 已知坑 | 多 socket 频率/相位漂移、SMI | 各家 Errata 跳变 |

---

## 📐 七、频率不变性为什么重要

**频率不变（frequency invariance）**：计数器频率不随 CPU 自身调频而变。它对 clocksource 的意义分三层：

1. **mult 的长期有效性**：mult 是注册时按标称频率算好的常数。若 P-state 切换会改变 TSC 频率，那么同一 mult 在不同频率下就是错的——时间会系统性漂移，直到 watchdog 用别的钟测出偏差把它判死。constant_tsc 之前的老 x86 正是这样常年 tsc-unstable。
2. **调度器的利用率刻度**：PELT/util_avg 假设"1 个时钟周期的工作量在 1GHz 与 3GHz 下相同"。频率不变计数器（x86 用 TSC + APERF/MPERF 折算 `arch_scale_freq_capacity`，arm64 用 AMU/CTR 定标）是 `freq invariant util` 的基石，进而驱动 schedutil 调频决策。clocksource 层的"频率不变"与调度器的"算力不变"同源同义。
3. **虚拟化与嵌套**：云上 vCPU 被偷走的时间在 TSC 上表现为"停走"；KVM 用 `kvmclock`/`MSR_KVM_STEAL_TIME` 修正。寄宿机 TSC 必须频率不变，guest 才敢直接 rdtsc——这是云厂商死守 invariant TSC 的原因。

反例最直观：HPET 频率固定 14.3MHz（很稳）但读一次是慢速 MMIO（上 µs 级）；TSC 读一次 ~20 cycles。**频率不变 + 读取便宜 + 跨核一致**三者齐备才配得上 rating 400——arm64 通用定时器是目前唯一满配的主流实现。

---

## 🔭 八、观测调试

```bash
# 主钟与候选（available 会随 oneshot 模式过滤掉无 HRES 资格者）
cat /sys/devices/system/clocksource/clocksource0/current_clocksource
cat /sys/devices/system/clocksource/clocksource0/available_clocksource

# 手动切换（写 current_clocksource 即 override_name + clocksource_select）
echo hpet > /sys/devices/system/clocksource/clocksource0/current_clocksource

# 强制卸下一口钟（研究 fallback 链: 会触发 select_fallback 换主钟+换watchdog）
echo tsc > /sys/devices/system/clocksource/clocksource0/unbind_clocksource

# dmesg 关注三类行
dmesg | grep -E "Switched to clocksource|Marking clocksource|clocksource .* unstable"

# watchdog 自身行为的回归测试模块（CONFIG_CLOCKSOURCE_WATCHDOG + clocksource-wdtest.c）
modprobe clocksource_wdtest

# /proc/timer_list 头部: 当前 clocksource、watchdog、tick 设备一览
sudo head -40 /proc/timer_list

# 运行时校验读取开销（纳秒级读钟基准）
perf stat -e cycles -r 10 python3 -c "import time; [time.clock_gettime_ns(1) for _ in range(1000000)]"
# 对比 current_clocksource=tsc 与 hpet 的差距(通常 >10x, 因后者走系统调用+MMIO)
```

诊断口诀：时间莫名漂移 → 先 `dmesg` 找 unstable 记录 → 看是否发生了 TSC→HPET 降级 → 降级则查 SMI（`turbostat` 的 SMIs 列）/固件/虚拟化窃时 → 实在无解用 `clocksource=hpet` 启动参数钉死。

---

## 💭 九、思考题

1. `clocksource_cyc2ns()` 注释强调"只用于相对周期"。若拿绝对计数直接乘 mult，64 位 TSC 会在多长时间后溢出？`clocks_calc_mult_shift()` 的 `maxsec` 参数如何量化这个风险？
   <details><summary>提示</summary>mult≈7e7（shift 24）时，2^64/mult ≈ 2.6e11 周期 ÷ 2.4e9 cyc/s ≈ 110 秒。maxsec 的 600 秒封顶正是覆盖"绝对值换算不溢出"的区间；timekeeper 的做法则是把 cycle_last 锚点随时前移，只用 delta。</details>
2. watchdog 为什么选"rating 最高且**非** MUST_VERIFY"的钟当本尊，而不是选 rating 最低的差钟？HPET（rating 250+）当 watchdog 时，它自己坏了怎么办？
   <details><summary>提示</summary>watchdog 必须比被测钟更可信才有裁判资格；MUST_VERIFY 的钟自己都待审。watchdog 本尊没有更高层的裁判——这是体系的公理层，只能靠"多钟一致性"事后暴露（如 clocksource-wdtest 场景）。它不稳时 clocksource_select_watchdog(true) 会换人（unbind 路径）。</details>
3. 一台 x86 机器 dmesg 出现 `Marking clocksource 'tsc' as unstable`，但 10 分钟后没有任何后续切换记录。可能发生了什么？
   <details><summary>提示</summary>看 4.4 节链条：__clocksource_unstable → workqueue → kthread。若 boot 未完成（finished_booting=0）只挂账不触发；另外若是 override 钉住了 hpet，select 也不会换。也可查 watchdog_work 是否因 stop_machine 与 CPU hotplug 锁序问题被推迟（clocksource_watchdog_work 注释）。</details>
4. arm64 上应用 `clock_gettime` 的开销忽然从 ~20ns 涨到 ~300ns。结合第六节，第一个要查什么？
   <details><summary>提示</summary>vDSO 是否被禁用：dmesg 搜 errata 名（"Freescale"/"HiSilicon"/"ARM errata 858921"/"Allwinner"）。套上读数 workaround 后 vdso_clock_mode=NONE，全部退回系统调用。</details>
5. 设计一个实验：用 `unbind` + 用户态读钟对比，量化 TSC 与 HPET 在 vDSO 可用性上的差异（前者有 VDSO_CLOCKMODE_TSC，后者无）。预期结果是什么？
   <details><summary>提示</summary>current=tsc：读钟不陷入（strace 零 syscall 记录）；切 hpet：每次 clock_gettime 出现一次系统调用。可写死循环 1e7 次读钟计时对比，再 strace -c 验证系统调用次数。</details>
6. 为什么 `max_idle_ns` 的计算要用 `mult - maxadj`（最慢斜率）而不是 `mult` 或 `mult + maxadj`？
   <details><summary>提示</summary>这个值约束的是"醒着后用缓存 mult 换算 delta 不溢出/不超回绕"。NOHZ 睡眠期间 NTP 可能把斜率往任一方向调 ±11%；按最慢斜率算出的 ns 上界，在任何合法调整下都不至于让真实时间超出可表达范围。</details>

---

*上一章：[时间子系统总体架构](01-时间子系统总体架构.md)　|　下一章：[timekeeping 与 clock_gettime](03-timekeeping与clock_gettime.md) —— `struct timekeeper`、shadow 双缓冲、NTP 修正与 vDSO 数据协议。*
