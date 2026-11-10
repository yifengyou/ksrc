# Introduction — 电源管理分册（power）

基于 **Linux 6.6** 的电源管理子系统深度笔记，共 5 章（已全部完成）。

## 章节导读

| 章节 | 主题 | 一句话 |
| :--- | :--- | :--- |
| [01-电源管理总体框架](docs/01-电源管理总体框架.md) | 版图与骨架 | 五大板块（CPU/设备/平台 PM、QoS、wakeup）、`dev_pm_ops` 回调矩阵、suspend 总流程 |
| [02-cpufreq](docs/02-cpufreq.md) | 调频子系统 | policy 生命周期、三代驱动接口、schedutil 落点、freq_qos、频率不变性 |
| [03-cpuidle](docs/03-cpuidle.md) | 空闲管理 | C-state 成本模型、`cpuidle_idle_call` 决策链、menu/teo 预测算法、poll idle |
| [04-runtime PM](docs/04-runtime-PM.md) | 设备级微睡眠 | usage_count/状态机/pm_wq、rpm_suspend 六道关卡、autosuspend 防抖、device link |
| [05-suspend 与 s2idle](docs/05-suspend与s2idle.md) | 系统睡眠 | 三级等级全时序、s2idle_loop 唤醒协议、wakeup source 记账、hibernate 概览 |
| [06-thermal 框架](docs/06-thermal框架.md) | 热管理 | tz/cdev/instance 三件套、trip 越线判定与 hysteresis、step_wise/power_allocator(IPA)、cpufreq_cooling 的 freq_qos clamp、arch_scale_thermal_pressure 与调度 capacity 实时联动 |

## 与其他分册的联动

* **调度器分册**：schedutil 的 util 来源（第 5 章 PELT）、thermal→capacity（第 6 章）、do_idle 与 idle 任务（第 2 章）
* **timers 分册**：`tick_nohz_get_sleep_length` 是 menu/teo 的预测主轴；broadcast timer 是深 C-state 的守夜人
* **中断分册**：s2idle 唤醒依赖 wakeup 中断的 `IRQF_NO_SUSPEND`

## 本目录 .rst 镜像导读（内核官方文档）

* `runtime_pm.rst` —— runtime PM 官方文档（第 4 章的原文对照）
* `energy-model.rst` / `opp.rst` —— Energy Model 与 OPP（EAS 的供能侧）
* `pm_qos_interface.rst` —— PM QoS 官方说明
* `freezing-of-tasks.rst` —— 任务冻结机制
* `s2ram.rst` / `swsusp.rst` / `basic-pm-debugging.rst` —— 系统睡眠与调试

## 源码索引（Linux 6.6）

```
kernel/power/{main,suspend,hibernate,qos,wakeup}.c
drivers/base/power/{main,runtime}.c
drivers/cpufreq/{cpufreq.c,cpufreq_governor.c,*.c}
kernel/sched/cpufreq_schedutil.c
drivers/cpuidle/{cpuidle.c,driver.c,governors/menu.c,governors/teo.c}
include/linux/{pm.h,pm_runtime.h,suspend.h,cpufreq.h,cpuidle.h}
```
