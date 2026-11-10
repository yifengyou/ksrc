# 时间子系统（timers）——Linux 6.6 深度笔记

> 本分册基于 **Linux 6.6** 源码（kernel/time/、fs/timerfd.c、arch/x86/kernel/tsc.c、drivers/clocksource/arm_arch_timer.c 等），所有结构体、常量与调用顺序均与 v6.6 源码逐行核对。中文正文、真实代码、ASCII 图、对比表、观测调试、思考题——风格与 scheduler / interrupt 分册一致。

时间子系统回答三个问题：**现在几点**（timekeeping）、**何时叫我**（五套定时器框架）、**时间如何推进**（tick 与 NOHZ）。它是调度器、网络超时、RCU、功耗管理共同的地基。

---

## 📚 章节导读

| 章 | 标题 | 回答的核心问题 |
| :--- | :--- | :--- |
| [00](docs/00-一页速查.md) | 一页速查 | 五件套/三层 API 梯度/轮 9×6bit/8 基 hrtimer/NOHZ/观测（各章浓缩） |
| [01](docs/01-时间子系统总体架构.md) | 时间子系统总体架构 | 分层地图、start_kernel 初始化顺序、gettimeofday/nanosleep 全路径、几条时间轴 |
| [02](docs/02-clocksource与时钟硬件.md) | clocksource 与时钟硬件 | `struct clocksource` 逐字段、mult/shift 定点数学、watchdog 降级链、x86 TSC vs arm64 通用定时器 |
| [03](docs/03-timekeeping与clock_gettime.md) | timekeeping 与 clock_gettime | `struct timekeeper`、shadow/tk_fast 双缓冲、update_wall_time、NTP 修正、vDSO 数据协议、jiffies 维护 |
| [04](docs/04-时间轮与低分辨率定时器.md) | 时间轮与低分辨率定时器 | 非级联轮的层级粒度（LVL_GRAN）、bucket 计算、触发链、NOHZ 迁延到期 |
| [05](docs/05-hrtimer高精度定时器.md) | hrtimer 高精度定时器 | 红黑树与 8 个 clock_base、`hrtimer_interrupt` reprogram/hang 流程、高/低精度切换、SOFT 模式、调度器三案例 |
| [06](docs/06-tick与NOHZ.md) | tick 与 NOHZ | `tick_device`/`tick_sched`、NOHZ_IDLE 停表/补偿全流程、NOHZ_FULL 依赖计数、broadcast、CONFIG_HZ 取舍 |
| [07](docs/07-posix定时器.md) | posix 定时器 | `k_itimer` 与 `k_clock` 分派、clockid 编码、CPU 定时器、timerfd、alarmtimer、SIGEV_SIGNAL 的 overrun 协议 |

## 🗺️ 学习路线图

```
阶段一: 读时间(上半场)                     阶段二: 定时(中场)                阶段三: 推进与省电(下半场)
┌─────────────────────────┐      ┌─────────────────────────┐      ┌─────────────────────────┐
│ 01 总体架构 (先建地图)    │      │ 04 时间轮               │      │ 06 tick 与 NOHZ         │
│    ↓                    │      │   (超时语义/jiffies 粒度) │      │   (tick 的生与死)        │
│ 02 clocksource          │ ──→  │ 05 hrtimer              │ ──→  │ 07 posix 定时器         │
│   (谁提供计数)           │      │   (精度与硬中断)          │      │   (用户态五套接口选型)    │
│    ↓                    │      │    ↓                    │      │                         │
│ 03 timekeeping          │      │ (05 依赖 03 的 ktime 语义 │      │ 依赖: 全部              │
│   (谁维护时间轴)          │      │  和 02 的 clocksource)   │      │                         │
└─────────────────────────┘      └─────────────────────────┘      └─────────────────────────┘
```

* **最小通关路线**（复习/面试向）：01 → 03（跳过 NTP 细节）→ 05 → 06。
* **驱动/嵌入式工程师**：02 → 06（broadcast 与 clockevent 注册）→ 04。
* **性能/延迟工程师**：03（vDSO）→ 05（hrtimer 观测）→ 06（NOHZ_FULL）→ 07（timerfd 与 overrun）。
* 每章末尾思考题附提示，建议至少口头过一遍再进入下一章。

## 📖 .rst 镜像导读（本目录）

本目录保留内核官方文档的**英文原文镜像**（Documentation/timers/），与本分册互补——**官方文档讲"怎么用/为什么这么设计"，本分册讲"源码里到底怎么跑"**：

| 镜像 | 官方标题（原文） | 对应章 | 定位 |
| :--- | :--- | :--- | :--- |
| `index.rst` | Timers（目录索引） | — | 原目录索引 |
| `timers-howto.rst` | delays — Information on the various kernel delay / sleep mechanisms | 04、07 | "该用哪种延迟/睡眠方式"官方决策树（udelay→msleep→ssleep 等），驱动写手必读 |
| `hrtimers.rst` | hrtimers — subsystem for high-resolution kernel timers | 05 | hrtimer 设计与 API 的引入文档 |
| `highres.rst` | High resolution timers and dynamic ticks design notes | 05、06 | hrtimers + dynticks 的历史设计笔记（OLS 2006） |
| `no_hz.rst` | NO_HZ: Reducing Scheduling-Clock Ticks | 06 | NOHZ_IDLE/NOHZ_FULL 的 Kconfig/启动参数/语义权威说明 |
| `timekeeping.rst` | Clock sources, Clock events, sched_clock() and delay timers | 02 | clocksource/clockevent/sched_clock 抽象层的官方阐述 |
| `hpet.rst` | High Precision Event Timer Driver for Linux | 02 | HPET 硬件规格与驱动 |

## 🔭 贯穿全分册的观测工具速查

```bash
cat /sys/devices/system/clocksource/clocksource0/current_clocksource   # 当前时钟
sudo cat /proc/timer_list                        # 全系统定时器/设备总览(6.6 仍在)
grep LOC /proc/interrupts                        # 本地 tick 计数
adjtimex --print                                 # NTP 内核状态
cat /sys/devices/system/cpu/nohz_full/cpulist    # NOHZ_FULL 配置
cat /proc/<pid>/timers; cat /proc/<pid>/fdinfo/<fd>   # 进程定时器/timerfd
cd /sys/kernel/tracing && echo 1 > events/timer/enable   # timer/hrtimer/tick tracepoints
```

## ⚖️ 贯穿全分册的三个设计张力

理解这三个"取舍"，比记住任何单个函数更能建立时间子系统的直觉：

1. **精度 vs 开销**：clocksource 的 mult/shift、时间轮的分层粒度、hrtimer 的 SOFT 模式、coarse 时钟——全是"用可控的精度损失换可测的系统开销"。
2. **连续性 vs 可信度**：MONOTONIC 基准轴永不回退（校时只动 offset），但基准轴自己会坏（watchdog 因此存在）；tk_fast 干脆承认"快照可能倒退几纳秒"。工程上没有免费的绝对真理。
3. **按需中断 vs 补账复杂度**：NOHZ 停掉 tick 省下的是真金白银的功耗，代价是 jiffies/wall time/idle 记账/负载统计/时间轮 clk 五套独立的补偿逻辑——**每一章的第 3~6 节都在讲某个"停表之后谁记得补账"**。

## ❓ 常见误区速查（读前自测）

* "jiffies 每 tick 加一"——NOHZ 下是补账式推进，睡醒一次性加 N。
* "hrtimer 精度 1ns"——1ns 是**语义分辨率**（`hrtimer_resolution`），实际精度由 clockevent 决定（通常数十~数百 ns 起步）。
* "`/proc/timer_list` 已被移除"——6.6 仍在（0400，root）；被移除的是 `/proc/timer_stats`（4.11）。
* "gettimeofday 走系统调用"——绝大多数走 vDSO，`strace` 里零记录才是常态。
* "时钟越准越好"——watchdog 判不稳会把你打回 HPET；CMOS/RTC 只在 suspend 记账用。
* "6.6 里叫 del_timer"——已更名 `timer_delete`/`timer_delete_sync`（老名是 inline 别名），并新增 `timer_shutdown*`。

## 🔗 与其他分册的联系

* **scheduler 分册**：`scheduler_tick`/hrtick/CFS bandwidth 定时器/DL 补给/PELT 与 NOHZ 的互动——见 05、06 章交叉引用处。
* **interrupt 分册**：`update_process_times` 位于 `irq_exit` 派发的软中断路径上；clockevent 到期即一次本地中断（`irq_enter_rcu` 的 `tick_irq_enter` 补 jiffies）。
* 后续若有 **电源管理分册**：menu governor、broadcast、alarmtimer 的 suspend 链（06、07 章）是直接前置知识。

## 🧭 本分册未覆盖（诚实声明）

以下主题与时间子系统相邻但未展开，避免注水：

* **clockevents 框架本身**（clockevents.c 的状态机/rating 选择）只在 05/06 章按需涉及；独立的 clockevent 驱动编写请参考 `include/linux/clockchips.h` 与现有驱动。
* **sched_clock**（kernel/time/sched_clock.c 的早期启动时钟）只在 01 章初始化序列提及。
* **PTP/动态 posix 时钟**（`clock_posix_dynamic`、fd_to_clockid）在 07 章点到编码为止。
* **时间命名空间**（time namespace，`timens_*` 调用）散见于 03/07 章的调用链，未独立成章。
* 各架构 clocksource/clockevent 驱动的注册细节（Timer OF/ACPI probe）请直接读 `drivers/clocksource/`。

## ⚠️ 阅读约定

* 未标注出处的结构体/函数均可在 v6.6 对应文件找到；引用格式 `// 路径 (v6.6)`。
* "6.6 已改名"类提示（如 `del_timer → timer_delete`）只写主线路径，老别名注明为 inline 兼容。
* 实验命令假定 root 或有 sudo；tracepoint 需 debugfs 挂载于 `/sys/kernel/tracing`。
