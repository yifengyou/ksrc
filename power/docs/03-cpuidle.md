# cpuidle 空闲管理（Linux 6.6）

> 阅读目标：掌握 C-state 的成本模型（exit_latency/target_residency）与 `struct cpuidle_state/driver/device` 三件套；逐行吃透 `cpuidle_idle_call()` 的决策链（含 s2idle 旁路）；深挖 menu 与 teo 两个 governor 的预测算法（典型间隔、校正因子、hits/intercepts）；理解 poll idle、tick 停启、RCU idle 与 PM QoS 约束如何共同决定"这一拍睡多深"。

---

## 一、C-state：深度即成本

CPU 空闲状态（x86 命名 C1/C1E/C6…、arm64 名为 cluster idle/retention/off）是一张"越深越省电、越深越难醒"的菜单。两个参数决定一切（6.6 用 ns 原生字段）：

```
exit_latency_ns     从该状态回到执行态的唤醒延迟（浅状态 ~1µs，深状态可到百µs 级）
target_residency_ns 睡够这个时长，"省的电"才赚回"进出开销"——比它短的睡眠
                    选这个状态反而更耗电
power_usage         该状态功耗（相对值，供 governor 参考）
```

决策的永恒权衡：**预测这次空闲能睡多久 vs 每个状态的收益门槛**。预测偏保守（睡短选浅）浪费电，预测偏激进（睡长选深）则唤醒延迟超标——governor 的全部艺术就是把这两个错误率调到最低。

```c
struct cpuidle_state {                  /* include/linux/cpuidle.h, 6.6 */
	char name[CPUIDLE_NAME_LEN];
	char desc[CPUIDLE_DESC_LEN];

	s64 exit_latency_ns;
	s64 target_residency_ns;
	unsigned int flags;                 /* CPUIDLE_FLAG_* */
	unsigned int exit_latency;          /* 兼容的 µs 版本 */
	int power_usage;                    /* mW */
	unsigned int target_residency;      /* µs 版本 */

	int (*enter)(struct cpuidle_device *dev,
		     struct cpuidle_driver *drv, int index);
	int (*enter_dead)(struct cpuidle_device *dev, int index);   /* CPU hotplug 下线时 */
	int (*enter_s2idle)(struct cpuidle_device *dev,
			    struct cpuidle_driver *drv, int index); /* s2idle 专用（不得开中断）*/
};
```

常用 flags：`CPUIDLE_FLAG_POLLING`（状态 0 的忙等语义）、`CPUIDLE_FLAG_COUPLED`（arm 老式 cluster 同步进入）、`CPUIDLE_FLAG_TIMER_STOP`（**进此状态本地 timer 停摆**——必须依赖 broadcast timer 唤醒，见 timers 分册）、`CPUIDLE_FLAG_RCU_IDLE`（进入前需 RCU 静默化）。

### 1.1 驱动层：谁提供这份菜单

```
intel_idle       x86 现代 CPU 的硬件 C-state 直连（MSR MWAIT hint）
acpi_idle        ACPI _CST 表描述（BIOS 提供）
psci idle        arm64：PSCI CPU_SUSPEND + 系统级 Suspend-to-Idle（DT/固件）
cpuidle-arm/qcom…  各平台驱动
```

`struct cpuidle_driver` 全局或 per-CPU（`CPUIDLE_DRIVER_STATE_START`）注册；`struct cpuidle_device` per-CPU（`states_usage` 记录每状态使用次数/驻留分布，sysfs 的数据源）。

---

## 二、cpuidle_idle_call()：idle 循环的决策链（kernel/sched/idle.c，6.6）

调度器分册第 2 章讲过：每 CPU 的 idle 任务进入 `do_idle()` 循环。核心段：

```c
static void cpuidle_idle_call(void)              /* kernel/sched/idle.c */
{
	struct cpuidle_device *dev = cpuidle_get_device();
	struct cpuidle_driver *drv = cpuidle_get_cpu_driver(dev);
	int next_state, entered_state;

	if (need_resched()) {                        /* 竞态复查：唤醒先于入睡 */
		local_irq_enable();
		return;
	}

	if (cpuidle_not_available(drv, dev)) {       /* 无 cpuidle：走架构默认 */
		tick_nohz_idle_stop_tick();
		default_idle_call();                     /* arch_cpu_idle（safe_halt 等）*/
		goto exit_idle;
	}

	if (idle_should_enter_s2idle() || dev->forced_idle_latency_limit_ns) {
		/* ① s2idle 旁路：系统已冻结，绕过 governor 直选最深可进状态
              （或 powerclamp 的 forced_idle_latency 强制浅睡注入） */
		...
		tick_nohz_idle_stop_tick();
		next_state = cpuidle_find_deepest_state(drv, dev, max_latency_ns);
		call_cpuidle(drv, dev, next_state);
	} else {
		bool stop_tick = true;

		/* ② 正常路径：governor 决策 */
		next_state = cpuidle_select(drv, dev, &stop_tick);

		if (stop_tick || tick_nohz_tick_stopped())
			tick_nohz_idle_stop_tick();      /* 停 tick（若 governor 判定会睡深）*/
		else
			tick_nohz_idle_retain_tick();    /* 保留 tick（如选了 poll 态）*/

		entered_state = call_cpuidle(drv, dev, next_state);
		cpuidle_reflect(dev, entered_state); /* 把结果反馈给 governor 学习 */
	}

exit_idle:
	__current_set_polling();
	...
}
```

三个容易被忽视的语义：

1. **`cpuidle_reflect` 是学习闭环**：governor 在 select 时给预测（期望时长/索引），醒来后 reflect 对照实际驻留修正模型（menu 的校正因子、teo 的命中/拦截统计）。
2. **`stop_tick` 由 governor 决定**：预测睡得浅（比如选 poll）就不停 tick，省得醒来后补账；预测深睡则停 tick 交由 broadcast/唤醒源负责叫醒（timers 分册第 6 章）。
3. **s2idle 旁路**：系统睡眠时（第 1 章）所有 CPU 都从这里进最深状态，governor 不参与——决策权在 `cpuidle_find_deepest_state`（受 wakeup latency 约束）。

`call_cpuidle → cpuidle_enter → cpuidle_enter_state`（drivers/cpuidle/cpuidle.c）：记录进入时刻 → `ct_cpuidle_enter`（**RCU 进入 EQS**，rcu_idle_enter 的 6.6 形态）→ `state->enter(dev, drv, index)`（架构指令：`mwait`/`wfi`/PSCI 调用）→ 醒来统计 `last_residency_ns`。

---

## 三、menu governor：基于历史间隔的预测器（drivers/cpuidle/governors/menu.c）

menu 是 x86/多数平台的默认 governor。它的模型一句话：**"这次空闲大概率像最近的几次"**。

### 3.1 数据结构：8 桶滑动窗口

```c
#define INTERVALS 8                    /* 历史 8 个空闲间隔 */
struct menu_device {
	unsigned int next_timer_us;        /* 下一个 timer 到期时长（硬上界）*/
	unsigned int bucket;               /* 校正因子分桶（10 个：交互性×iowait）*/
	unsigned int correction_factor[10];/* 每"桶"独立的学习修正（百分比）*/
	unsigned int intervals[INTERVALS]; /* 最近 8 次空闲的实际时长（µs）*/
	unsigned int interval_ptr;         /* 环形写指针 */
	int      needs_update;
	...
};
```

### 3.2 menu_select 主流程（已核对 6.6 源码，精简注释）

```c
static int menu_select(struct cpuidle_driver *drv, struct cpuidle_device *dev,
		       bool *stop_tick)
{
	s64 latency_req = cpuidle_governor_latency_req(dev->cpu); /* PM QoS！
	                                    /dev/cpu_dma_latency 的最紧值（第 1 章）*/
	/* ① 典型间隔预测：8 次历史的中位数（带方差过滤） */
	predicted_ns = get_typical_interval(data) * NSEC_PER_USEC;

	if (predicted_ns > RESIDENCY_THRESHOLD_NS) {
		/* ② 预测值得认真对待 → 再问 timer 硬上界 */
		delta = tick_nohz_get_sleep_length(&delta_tick);  /* 到下个 timer 的时长 */
		data->next_timer_ns = delta;
		data->bucket = which_bucket(next_timer_ns, nr_iowaiters);
		/* ③ 校正学习：预测 = timer_us × correction_factor[bucket]
		   （bucket 按"时长档位 × 是否 io 等待者"分桶——io 等待的空闲
		     常被中断提前打断，独立修正防止系统性高估）*/
		timer_us = ... data->next_timer_ns * data->correction_factor[data->bucket] ...;
		predicted_ns = min(timer_us * NSEC_PER_USEC, predicted_ns);  /* 两者取小 */
	} else {
		/* ④ 历史说"睡不长"：next_timer 置 KTIME_MAX（不指望 timer 叫醒），
		   delta_tick 记半个周期（保守估计 tick 会来） */
		data->next_timer_ns = KTIME_MAX;
		delta_tick = TICK_NSEC / 2;
		data->bucket = which_bucket(KTIME_MAX, nr_iowaiters);
	}

	/* ⑤ 交互性约束：预测时长 < 交互阈值（约 50ms? 按 latency_req 换算）时，
	   强制至少给"交互响应"留出 exit_latency 预算 */
	if (data->next_timer_ns < TICK_NSEC? ... interactivity_req 计算）...

	/* ⑥ 从深到浅扫描（默认期望深睡）：
	   第一个满足 (predicted_ns ≥ target_residency) 且
	              (latency_req ≥ exit_latency) 的状态入选；
	   前半段要求同时满足"醒来无 timer 事件"，后半段放松（浅睡无妨） */
	...
}
```

`get_typical_interval` 的统计口径（已核对）：对 8 个历史值求均值/方差，`avg*avg > variance*36`（即变异系数 < 1/6）且样本覆盖 ≥ 6/8 时才采纳中位数为"典型值"——**历史不稳定宁可不用**（退化为纯 timer 上界预测）。

### 3.3 menu_update：醒来后的反思

`cpuidle_reflect → menu_update`：拿实际驻留对照预测。若实际 < 预测的 1/2（**被打断得太早**），把当前 bucket 的 correction_factor 下调（learning：`if (actual_factor < correction_factor) correction_factor--` 类的保守递减——6.6 的实现是逐步 1 步修正，防止震荡）；实际更长则缓慢上调。这就是 menu 的"自学习"：**每个 (时长档, iowait) 组合独立收敛到一个校正系数**。

---

## 四、teo governor：定时器事件导向（Timer Events Oriented）

teo（`drivers/cpuidle/governors/teo.c`）针对 menu 的弱点：**menu 的"典型间隔"在事件稀疏的机器上样本少、失真**。teo 反转模型：以"下一个 timer 何时来"为主轴，用历史统计回答"timer 到点时我到底在不在深睡里"。

每个状态（bin）记三个量（已核对源码）：

```
hits        历史：选了它且睡到了 timer（预测正确）
intercepts 历史：选了它但比 timer 更早被别的源打断（选深了/被意外唤醒）
recent      最近一次窗口的 intercepts 计数（窗口 = 每次选中变化时清零）
```

`teo_select` 的骨架：

```
1) teo_update（反思上一次：实际驻留 vs 当时 timer 上界 → 判定 hit/intercept 记账）
2) duration_ns = tick_nohz_get_sleep_length()      本次预测主轴
3) 依"timer 上界落点 + 历史命中分布"选 bin：
   若最深满足 residency 的 bin 的 (hits+intercepts) 主要来自 hits → 果断深睡；
   若 intercepts 显著（timer 前常被拦截）→ 回退一个"被拦截概率低"的较浅 bin；
   alt_intercepts / alt_recent 两个候选二次仲裁（处理浅 bin 也被拦截的
   分布空洞——6.6 注释里有完整的决策表）
4) cpu_utilized（CPU 上还有 runnable 任务痕迹）时收缩到浅态防过度深睡
```

一句话对比：

| | menu | teo |
| :--- | :--- | :--- |
| 预测主轴 | 历史空闲间隔的中位数（timer 只做上界） | timer 到期时长（历史回答"会否被提前打断"） |
| 学习量 | 每 bucket 一个标量校正因子 | 每 bin 的 hits/intercepts 分布 |
| 弱点 | 稀疏负载下样本失真 | timer 密集负载下退化为"浅睡循环" |
| 适用 | 桌面/交互混合负载 | 服务器/嵌入式的事件驱动负载 |

arm64 社区近年在推动 teo 成为默认；x86 仍是 menu。`cpuidle.governor=teo` 可切换。

---

## 五、poll idle：状态 0 的特殊性

`CPUIDLE_FLAG_POLLING` 的状态 0 不是真睡——**关中断自旋等 need_resched**：

```
进入：local_irq_disable + 循环 { cpu_relax; if (need_resched) break; }
收益：唤醒延迟 ≈ 0（无任何恢复路径）
成本：满频耗电 + SMT 兄弟降速
```

menu 的短预测 + poll 的配合：预测空闲 < 一定阈值（`dev->poll_limit_ns`，动态学习）时直接选 poll。`poll_time_limit`（device 字段）在 poll 驻留超限后强制禁用一段时间——防止"永远预测短"的活锁。sysfs `state0/disable` 可全局关掉。

---

## 六、约束、观测与调优

### 6.1 三层约束

```
PM QoS（全局）：/dev/cpu_dma_latency ≤ latency_req → governor 只在满足
              exit_latency 的状态里选（menu/teo 入口第一件事就是取它）
sysfs per-state：/sys/.../cpuidle/stateN/disable = 1 直接禁用某状态
                stateN/residency/latency 可改（改的是决策参数，不是硬件）
menu 内部学习：correction_factor 的收敛
```

### 6.2 观测

```
/sys/devices/system/cpu/cpuN/cpuidle/state*/
    name / desc / time（累计驻留 µs）/ usage（进入次数）
    above / below（6.6：浅于预期/深于预期的次数——governor 健康度指标）
    disable / residency / latency（可写）
powertop        各状态驻留 + wakeup 源计数（用户态集成观测）
turbostat       PkgWatt + C-state 分布（第 2 章工具链）
tracepoint:     power:cpu_idle（旧）/ 6.6 仍在；bpftrace 可画"预测 vs 实际"分布
```

### 6.3 经典调优场景

| 症状 | 诊断 | 处置 |
| :--- | :--- | :--- |
| 功耗高、深 C-state 进不去 | stateN/time 全为 0；`above` 高 | 找频繁唤醒者（powertop timer stats）；或 timer 精度问题（timers 分册 lazy timer） |
| 交互延迟毛刺 | latency_req 无约束 + 深状态 exit_latency 大 | 应用按住 `/dev/cpu_dma_latency`；或 disable 深状态 |
| 吞吐反常低于预期 | SMT 上 poll 态兄弟互踩 | state0/disable=1 |
| 虚机深睡失效 | host 侧 vmexit 频繁打断 | host guestia 联调（virt 分册） |

---

## 七、思考题

1. `get_typical_interval` 要求"变异系数 < 1/6 且样本覆盖 ≥ 6/8"才采纳。一个网络转发核（事件泊松到达）为什么永远过不了这道门？它最终走哪条预测支路？
2. menu 的 correction_factor 按 `(时长档, iowait)` 分桶学习。为什么 iowait 空闲必须独立修正？（提示：块设备完成中断的到达模式 vs timer。）
3. teo 的 "intercepts" 高但 "recent" 低（历史被拦截、最近没有）时该选深还是浅？对照 6.6 teo.c 注释里的决策表回答。
4. `CPUIDLE_FLAG_TIMER_STOP` 状态与 NOHZ 的交互：停了 tick 又停了本地 timer，谁保证下次唤醒？（broadcast timer 的链路，见 timers 分册第 6 章。）
5. 设计：为"低功耗传感器节点，每 100ms 唤醒 2ms 处理，允许 5ms 唤醒延迟"配置 cpuidle（状态表、governor、QoS），并算出理论占空比与最坏唤醒延迟。

---

> 下一章（下轮续写）：[runtime PM 与设备电源管理](04-runtime-PM.md) —— usage_counter/disable_depth 状态机与 autosuspend。
> 再下一章：[suspend-to-RAM 与 s2idle 全流程](05-suspend与s2idle.md)。
