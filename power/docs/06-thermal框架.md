# thermal 框架：从 trip 到 capacity 缩放（Linux 6.6）

> 阅读目标：掌握 thermal 框架的三件套模型（thermal_zone/cooling_device/instance）、trip 检测协议（越线判定与 hysteresis）、三种 governor（step_wise/power_allocator/user_space）、cpufreq_cooling 的 clamp 机制（freq_qos + thermal pressure），以及**它如何实时改变调度器眼里的 CPU capacity**——这是 power 分册与 scheduler 分册的又一条交叉通道。前置：[02-cpufreq](02-cpufreq.md)、[scheduler/05-PELT](../../scheduler/docs/05-PELT与利用率追踪.md)（capacity 的消费侧）。

---

## 一、三件套模型（include/linux/thermal.h + drivers/thermal/thermal_core.h，已核对 6.6）

```
thermal_zone_device（被监测对象：SoC/外壳/电池…）
  ├── trips: struct thermal_trip[] { temperature, hysteresis, type, priv }
  │      num_trips + trips_disabled 位图
  ├── passive_delay_jiffies / polling_delay_jiffies（两类轮询间隔）
  ├── temperature / last_temperature / emul_temperature（注入测试）
  ├── governor（策略）─── thermal_governor_list（step_wise/power_allocator/user_space）
  └── instances[] ──┐
                     │ struct thermal_instance {
thermal_cooling_device │   tz, cdev, trip, upper, lower, target, weight… }
  └── ops: get_max_state / get_cur_state / set_cur_state
instance 把"某 trip 越线时该把某 cdev 拧到哪个区间 [lower,upper]"三元绑定
```

## 二、trip：类型、检测与轮询（已核对 6.6）

**类型**（6.6 已移入 `include/uapi/linux/thermal.h`——为 genetlink 复用，老代码里翻 include/linux 会扑空）：

```
THERMAL_TRIP_ACTIVE=0（主动冷却：风扇档位）/ PASSIVE（被动：降频）
/ HOT（严重告警）/ CRITICAL（强制关机： orderly_poweroff）
```

**越线判定**（thermal_core.c `handle_thermal_trip`，已核对）：比较 `last_temperature` 与 `temperature` 的**穿越方向**——`last < trip && cur >= trip` 即 up 事件（`thermal_notify_tz_trip_up` 通知 netlink + uevent）；回落穿越用 **hysteresis** 抑制抖动（两个方向的触发线拉开）。`THERMAL_TEMP_INVALID` 的 trip（传感器未就绪）直接跳过。

**驱动模式**：`thermal_zone_device_update`（传感器中断触发）或 `monitor_thermal_zone` 的 delayed_work 周期轮询（`polling_delay_jiffies`；passive trip 越线期间切 `passive_delay_jiffies` 加密采样）——两种模式由 zone 注册参数决定。

## 三、governor：从 trend 到功率分配（已核对 6.6）

```
step_wise（默认）: gov_step_wise.c
  get_target_state(instance, trend, throttle)——按趋势（升/稳/降）
  逐档拧 cdev：heating→target+1（直到 upper），稳定→不动，
  cooling→target-1；throttle 仅在真正越线时为 true
power_allocator（IPA，智能功率分配）: gov_power_allocator.c
  面向"power actor"类 cdev（能报告功耗预算的，如 cpufreq/GPU）：
  sustainable_power（可持续散热功率，未提供则 estimate_sustainable_power
  从各 actor 的 min_power 估）+ PID 控制器（temp 与 trip 的偏差）→
  divvy_up_power 按各 actor 的请求比例切分总功率→换算成各自 state
user_space: 只上报，决策交给用户态守护
注册：thermal_governor_list + def_governor；per-zone 可换
  （/sys/class/thermal/thermal_zoneX/policy）
```

## 四、cpufreq_cooling：把"降频"做成 cdev（已核对 6.6 全链）

```
注册（cpufreq_cooling_register）:
  为 policy 建 freq_qos_request 挂 policy->constraints（cpufreq 分册
  的 QoS 层！），state↔freq 表按 policy->freq_table 逆序展开
生效（cpufreq_set_cur_state）:
  state → get_state_freq(state) → freq_qos_update_request(qos_req, freq)
  ——不直接改频，只收紧 cpufreq 的上限（policy 侧 schedutil 仍自主决策）
  ★随后 arch_update_thermal_pressure(cpus, frequency)：
  把"被热封顶的频率"换算成 per-CPU 的 thermal_pressure 信号
```

## 五、与调度器的实时联动（cross-分册通道）

```
arch_update_thermal_pressure(cpus, freq)（include/linux/sched/topology.h
声明；arm64 在 topology.c 实现 per-CPU 存储与衰减窗口）
消费侧（6.6 逐行核对）：
  kernel/sched/core.c:5655  rq->cpu_capacity 计算里扣 thermal_pressure
  kernel/sched/fair.c:4816  capacity_orig_thermal = capacity_orig
                            - arch_scale_thermal_pressure(cpu)
  kernel/sched/fair.c:9076  负载均衡路径同样扣减
效果：被热 clamp 的 CPU 在 EAS/负载均衡眼里"瞬间变小"——任务自然
  迁往未受热的核；PELT 的 util 与 thermal 缩放的 capacity 一起决定
  频率是否还有上调空间（SCHED_CAPACITY_SCALE=1024 的满额不存在了）
```

## 六、netlink 与用户态（已核对 uapi）

```
THERMAL_GENL_FAMILY "thermal"：sampling 组（周期温度流）+ event 组
（trip 越线/恢复）；thermal_notify_tz_trip_up/down 直接触发
uevent：/sys/class/thermal/thermal_zoneX/uevent（type=xxx, event=n）
消费方：thermald、Android thermal HAL、桌面 DE 的风扇曲线
```

## 七、观测与实战

```
/sys/class/thermal/
  thermal_zoneX/{type,temp,mode,policy,trips_*}   # trips 6.6 起带 hysteresis
  cooling_deviceX/{type,cur_state,max_state}
  thermal_zoneX/cdevX_trip_Y/{cur_state,upper,lower}  # instance 级
tracefs: thermal:thermal_zone_trip
调试注入：thermal_zoneX/emul_temp 写入模拟温度（复现 trip 逻辑）
soc 度数异常排查链：temp 源（HWMON/ACPI）→ trips 表 → governor 策略
  → cdev 实际 state（cur_state 对比 max_state）→ freq_qos 是否生效
  （cpufreq/policyX/scaling_max_freq）→ thermal_pressure（EAS 侧）
```

## 八、思考题

1. `freq_qos_update_request` 收紧的是 policy 上限而非直接改频——schedutil 下被 clamp 的核为什么仍可能短时跑高频？（对照 [02-cpufreq](02-cpufreq.md) 的 QoS 与 util 请求叠加。）
2. `hysteresis` 只作用在"回落穿越"方向，为什么不在两个方向对称生效？（提示：升温保护要快、降温恢复要稳——抖动保护的不对称代价。）
3. power_allocator 需要 cdev 实现 power actor 接口——风扇（只有档位无功耗表）如何参与 IPA？（对照 cdev_is_power_actor 的判定与退路。）
4. `arch_scale_thermal_pressure` 扣减 capacity 后，`util_avg` 超过缩放 capacity 的核会怎样？（推演：迁移压力与 misfit task 判定。）
5. step_wise 与 power_allocator 在"多核同热源"场景的差异？（提示：前者各 cdev 独立拧档，后者按 actor 功率比例统一切分。）

---

> 回看：[02-cpufreq调频框架](02-cpufreq.md)｜[05-suspend与s2idle](05-suspend与s2idle.md)｜[scheduler/05-PELT](../../scheduler/docs/05-PELT与利用率追踪.md)
