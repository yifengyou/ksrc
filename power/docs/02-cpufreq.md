# cpufreq 调频子系统（Linux 6.6）

> 阅读目标：掌握 cpufreq 的三层架构（policy/driver/governor）；吃透 policy 的生命周期（注册→sysfs→governor 接管）、三代驱动接口（setpolicy/target_index/fast_switch+adjust_perf）的演进动机、切频的慢/快双路径与通知链；理解 schedutil 与调度器的集成细节、freq_qos 约束网络、频率不变性（`arch_set_freq_scale`）对全系统的意义。

---

## 一、三层架构

cpufreq 把"电压频率怎么算"拆成三个角色，配置即文件：

```
┌─────────────────────────────────────────────────────────┐
│  governor（策略：何时要多少频率）                          │
│  performance / powersave / userspace / ondemand /        │
│  conservative / schedutil(调度器联动, kernel/sched/)      │
│  /sys/.../scaling_governor 切换                          │
├─────────────────────────────────────────────────────────┤
│  policy（每"频控域"一份的状态与约束）                      │
│  struct cpufreq_policy：cpus/related_cpus、min/max/cur、  │
│  freq_table、freq_qos、governor_data、rwsem…             │
│  /sys/devices/system/cpu/cpuN/cpufreq/                   │
├─────────────────────────────────────────────────────────┤
│  driver（硬件：怎么真正切频）                              │
│  intel_pstate / amd-pstate / acpi-cpufreq / cppc_cpufreq │
│  cpufreq-dt / qcom-cpufreq-hw / mediatek-cpufreq-hw / …  │
└─────────────────────────────────────────────────────────┘
```

"频控域（policy）"是理解一切的起点：现代 SoC 一个 cluster/PPIN 内所有核共享一个电压域，**切一个核的频率等于切全体**——`policy->related_cpus` 就是这个域的 CPU 集合（含离线核，`cpus` 是其在线子集）。域由 driver 的 `init()` 通过 `cpumask` 声明，ACPI `_PSD`/DT 的 `shared-domains` 是其固件描述来源。

---

## 二、struct cpufreq_policy 与生命周期（drivers/cpufreq/cpufreq.c）

```c
struct cpufreq_policy {                 /* include/linux/cpufreq.h, 6.6 */
	cpumask_var_t		cpus;          /* 在线成员 */
	cpumask_var_t		related_cpus;  /* 全部成员（含 offline）*/
	cpumask_var_t		real_cpus;     /* related ∩ present */

	unsigned int		shared_type;   /* 域协调类型（HW/ANY/ALL）*/
	unsigned int		cpu;           /* 管理者 CPU（必须在线）*/

	unsigned int		min, max;      /* kHz，freq_qos 聚合结果 */
	unsigned int		cur;           /* kHz */
	unsigned int		suspend_freq;

	struct cpufreq_governor	*governor;
	void			*governor_data;

	struct freq_constraints	constraints;    /* freq_qos 约束网络 */
	struct cpufreq_frequency_table *freq_table;  /* 离散频点表 */

	struct rw_semaphore	rwsem;          /* policy 大锁 */
	bool			fast_switch_possible;
	bool			fast_switch_enabled;
	...
};
```

注册链条（谁在什么时候把这一切搭起来）：

```
驱动 initcall: cpufreq_register_driver(&xxx_driver)      drivers/cpufreq/cpufreq.c
  └─ subsys_interface_register(&cpufreq_interface)
       └─ 对每个已注册 CPU 设备调用 cpufreq_add_dev()
            ├─ cpufreq_policy_alloc(cpu)         kzalloc policy + freq_qos 初始化
            ├─ driver->init(policy)              ★驱动声明：域范围、freq_table、
            │                                      shared_type、fast_switch_possible
            ├─ freq_table 注册（若驱动未给，从 OPP 或 ACPI 构建）
            ├─ cpufreq_add_dev_interface(policy) sysfs 目录（一域一份 symlink）
            │    └─ cpufreq_init_governor + governor->start
            └─ 首次 apply（limits → 切到 min/max 约束内的合理频点）
```

热插拔联动：CPU offline 时若它是 `policy->cpu`，管理权迁移到域内其他核（`cpufreq_offline`→`__cpufreq_governor_stop`/更换管理者）；全部 offline 则 policy 保留（`related_cpus` 记住成员），以支持再上线。

### 2.1 freq_table 与频点解析

硬件只认离散频点（P-state），governor 输出的是连续 kHz。二者之间：

```c
unsigned int cpufreq_driver_resolve_freq(struct cpufreq_policy *policy,
					 unsigned int target_freq);
/* 语义：在 [policy->min, policy->max] 内找"最接近 target"的频点索引。
   6.6 的实现缓存上次结果（policy->cached_resolved_idx）——相邻请求
   大多解析到同一频点，直接短路。 */
```

`cpufreq_stats`（可选模块）为每频点记录驻留时长：`/sys/.../stats/time_in_state`——功耗分析的第一数据源。

---

## 三、三代驱动接口：演进动机

```c
struct cpufreq_driver {
	int  (*init)(struct cpufreq_policy *policy);
	int  (*verify)(struct cpufreq_policy_data *policy);   /* 校验新 limits 合法性 */

	/* 第一代：硬件自管（intel_pstate 早期 active、部分 -hw 驱动）*/
	int  (*setpolicy)(struct cpufreq_policy *policy);     /* 把 min/max 直接扔给硬件 */

	/* 第二代：内核驱动切换（主流慢路径）*/
	int  (*target_index)(struct cpufreq_policy *policy, unsigned int index);
	unsigned int (*fast_switch)(struct cpufreq_policy *policy,
				    unsigned int target_freq);  /* 第三代：无锁快路径 */

	/* 第三代+：schedutil 专属的"性能区间"语义 */
	void (*adjust_perf)(unsigned int cpu, unsigned long min_perf,
			    unsigned long target_perf, unsigned long capacity);

	int  (*get_intermediate)(...);  /* 切频过渡频点（如先到中转频率再换电压）*/
	int  (*target_intermediate)(...);
	unsigned int (*get)(unsigned int cpu);   /* 读硬件当前频率（vs cur 的防漂移校验）*/
	void (*update_limits)(unsigned int cpu); /* 被通知重读硬件限制（如 HWP 变化）*/
	...
};
```

| 代 | 接口 | 硬件 | 延迟 | 谁在用 |
| :--- | :--- | :--- | :--- | :--- |
| 1 | `setpolicy` | 硬件自治（HWP） | 硬件内部 µs | intel_pstate active |
| 2 | `target_index` | 内核写 MSR/SMC/寄存器 | 数 µs~ms（含 notifier） | acpi-cpufreq、cpufreq-dt、cppc |
| 3 | `fast_switch` | 同上但路径无锁无通知 | 数 µs | 支持 schedutil 高频请求 |
| 3+ | `adjust_perf` | 硬件接受 [min,target] 区间 | 一次调用 | intel_pstate + schedutil |

演进逻辑：**governor 请求频率的频率本身也在升高**——sampling 型 ondemand 每 10ms 问一次，schedutil 每次 PELT 更新都可能问（唤醒密集时 >1000 次/s）。慢路径的 notifier 链 + workqueue + sysfs 更新撑不住，于是切出无锁 `fast_switch`（限 `cpufreq_driver.flags |= CPUFREQ_NEED_REQ_LIMITS`…细则：仅 governor 在上下文内直调）。

### 3.1 慢路径切频全链（经典 target_index）

```
__cpufreq_driver_target(policy, target_freq, relation)
  └─ cpufreq_driver_resolve_freq → index
  └─ cpufreq_freq_transition_begin(policy, &freqs)     ★序障开始
       └─ srcu_notifier_call_chain(CPUFREQ_PRECHANGE)  /* 慢速消费者（如 loops_per_jiffy
                                                          的 adjust_jiffies 修正）先做铺垫 */
  └─ driver->target_index(policy, index)               /* 真正写硬件：先电压后频率（升频）
                                                          或先频率后电压（降频）—— DVFS 序 */
  └─ arch_set_freq_scale(policy->related_cpus, freq)   ★频率不变性锚点（见第五节）
  └─ cpufreq_freq_transition_end(policy, &freqs)
       └─ srcu_notifier_call_chain(CPUFREQ_POSTCHANGE) + sysfs cur 更新
```

`CPUFREQ_TRANSITION_NOTIFIER` 是 cpufreq 的对外广播（谁听：printk 的 delay 校准、`loops_per_jiffy`、部分网络驱动）。fast_switch **不广播**——这是它快的原因，也是它的契约：不能有 notifier 依赖的消费者走这条路。

---

## 四、governor 家族与 schedutil 深挖

6.6 内置 governor（`drivers/cpufreq/`）与外部实现：

| governor | 决策 | 状态 |
| :--- | :--- | :--- |
| performance | 恒 max | 内置 |
| powersave | 恒 min | 内置（多核域名下基本无意义） |
| userspace | 用户态直写 `scaling_setspeed` | 内置 |
| ondemand | 采样负载（默认 10ms 窗口 idle<阈值 则升频，hysteresis 降频） | 采样型，5.x 起淡出 |
| conservative | ondemand 的平缓版（步进升频） | 同上 |
| **schedutil** | **直接吃调度器 util**（kernel/sched/cpufreq_schedutil.c） | 6.6 事实默认 |

框架统一接口（`struct cpufreq_governor`，已核对源码）：

```c
struct cpufreq_governor {
	char name[CPUFREQ_NAME_LEN];
	int  (*init)(struct cpufreq_policy *policy);
	int  (*start)(struct cpufreq_policy *policy);   /* 接管：注册 util 更新回调 */
	void (*stop)(struct cpufreq_policy *policy);
	void (*limits)(struct cpufreq_policy *policy);  /* min/max 变化时立即生效 */
	ssize_t (*show_setspeed)(...); int (*store_setspeed)(...);
	struct list_head governor_list;  struct module *owner;  u8 flags;
};
```

### 4.1 schedutil 的落点（承接调度器分册第 5 章）

调度器侧的推导（util → freq 映射 `map_util_perf` 1.25 余量、`rate_limit_us` 节流、uclamp 上下限）已在第 5 章展开；本章补驱动侧三个工程细节：

1. **per-CPU vs shared policy**：一域一 policy 时，任一核的 util 更新都会请求整个域的频率——`sugov_get_util` 聚合域内各核 util（取 max）后统一决策，防止"核 A 高频、核 B 低频"的不一致请求互踩。
2. **fast_switch 直通**：`sugov_update_*` 在中断/调度上下文里直调 `driver->fast_switch`（写 MSR），无锁无通知；不支持 fast_switch 的驱动退回 kthread worker（`sugov_work`）异步慢切。
3. **`adjust_perf`（intel_pstate 专属）**：schedutil 不给单一频率，给 `[min_perf, target_perf, capacity]` 三元组——HWP 硬件在区间内自选， turbo/温控参与更自然。`amd-pstate` 同样以 CPPC 的 min/desired/max 三元组对接。

### 4.2 intel_pstate 的两种模式（排障必备）

```
active（默认，Intel 平台）：HWP 硬件自治。governor 选项被降级为
    performance/powersave（实为 EPP 偏好）；
    /sys/.../energy_performance_preference = balance_performance/performance/…
    （EPP 提示写入 IA32_HWP_REQUEST，硬件据此在区间内选频）
passive（intel_pstate=passive）：驱动退化为 target_index 型，governor 恢复全集，
    schedutil 可用
```

"为什么我机器上 scaling_governor 只有 performance/powersave"——九成是 intel_pstate active 模式所致，`intel_pstate=passive` 或 `intel_pstate=disable`（回落 acpi-cpufreq）可改行为。

---

## 五、freq_qos 与频率不变性：两处全局影响

### 5.1 freq_qos：policy 上的约束网络

`policy->constraints`（`struct freq_constraints`）聚集多来源请求，输出最终的 `policy->min/max`：

```
thermal cpufreq_cooling（温控压缩 max）
userspace scaling_min/max_freq（sysfs）
devfreq/pm_qos 的间接请求
        ↓ 聚合（最紧者胜）
governor->limits(policy) → apply → cur 被夹进 [min,max]
```

经典用法：`echo 1.2GHz > scaling_min_freq` 锁底频保交互延迟；thermal daemon 动态压 max 控温。

### 5.2 arch_set_freq_scale：为什么 PELT 必须知道频率

切频后 `arch_set_freq_scale(policy->related_cpus, freq)`（已核对调用点）把"当前频率/最大频率"写进 per-CPU 的 `freq_scale`。下游消费者：

```
arch_scale_freq_capacity() → PELT 的 rq_clock_pelt 缩放（调度器分册第 5 章）
                          → capacity_of() → 负载均衡/EAS/misfit（第 6 章）
                          → util 与 capacity 保持同一刻度
```

若驱动忘了调（自研驱动常见 bug）：降频后 util_avg 不变 → schedutil 认为还需要高频 → **正反馈锁死在高频**。这是第三方 cpufreq 驱动的头号正确性检查项。

---

## 六、sysfs 全解与实战

```
/sys/devices/system/cpu/cpuN/cpufreq/
    affected_cpus / related_cpus          频控域成员
    scaling_driver / scaling_governor     驱动与策略（governor 切换直接写此文件）
    scaling_cur_freq                      当前频率（policy->cur）
    scaling_min_freq / scaling_max_freq   约束（写 → freq_qos 请求）
    scaling_available_governors           可选列表
    cpuinfo_min/max_freq                  硬件界（含 turbo 上限）
    cpuinfo_transition_latency            切频延迟（schedutil rate_limit 的默认来源）
    stats/time_in_state                   各频点驻留（cpufreq_stats）
    energy_performance_preference         intel_pstate active 的 EPP

观测工具链：
turbostat 1  → Bzy_MHz（真实基频）、Busy%、PkgWatt（功耗）、C-state 分布
cpupower frequency-info / monitor
perf stat -e 'power/p-state-*/' 6.6 tracepoint? （平台相关）
```

调优速查：

| 目标 | 操作 |
| :--- | :--- |
| 交互延迟优先 | governor=performance 或 EPP=performance（注意功耗） |
| 服务器均衡 | schedutil + uclamp_max 压制离线任务（调度器分册第 5 章） |
| 控温 | thermal daemon 写 scaling_max_freq；或 passive + schedutil |
| 虚机高频抖动排查 | host 侧 turbostat 看 P-state 抖动 + guest `scaling_cur_freq` 采样 |

---

## 七、思考题

1. policy 是"一域一份"而非"一核一份"。域内 A 核想 3GHz、B 核想 1GHz，最终谁赢？这个聚合发生在哪两层（governor 聚合 util vs policy 只有一个 cur）？
2. fast_switch 绕过了 notifier 链。`loops_per_jiffy`（BogoMIPS）依赖 `CPUFREQ_POSTCHANGE` 修正，走 fast_switch 的系统 BogoMIPS 会怎样漂？内核用哪个机制兜底（提示：`adjust_jiffies` 的调用条件与 `l_p_j_ref` 缓存）？
3. `arch_set_freq_scale` 失效导致的"schedutil 锁高频"环路，用哪些观测值可以确认（util_avg、scaling_cur_freq、freq_scale 的三角关系）？
4. 为什么 `cpufreq_driver_resolve_freq` 要缓存上次解析结果？什么样的 governor 行为模式让这个缓存的命中率极高？
5. 设计：某 ARM64 平台 cluster0（4×A55，policy A）+ cluster1（4×A76，policy B）。在 EAS 不可用（无 Energy Model）时，如何用 governor+EPP+scaling_max 组合逼近"小核跑轻载、大核只跑重载"的效果？局限在哪里？

---

> 下一章：[cpuidle 空闲管理](03-cpuidle.md) —— C-state、menu/teo governor 与 idle 循环的决策链。
