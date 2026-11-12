# idle 注入与 intel_powerclamp（Linux 6.6）

> 阅读目标：掌握 thermal 的**第三种降温手段**——idle 注入（压占空比而非降频）：通用框架 `idle_inject`（per-CPU kthread + `play_idle_precise`）、`intel_powerclamp`（已迁至该框架上：反馈环/模块参数/mwait 选档）、以及非 Intel 平台的兄弟件 `cpuidle_cooling`。前置：[06-thermal 框架](06-thermal框架.md)（cdev 家族）、[03-cpuidle](03-cpuidle.md)（被注入的 idle 状态）。

---

## 一、为什么还需要第三种手段

```
cpufreq_cooling：压频率上限——但现代工艺下 Leakage 显著，
  降频后单位时间功耗占比未必线性下降（热设计余量可能不够）
idle 注入：直接强制 CPU 睡（占空比控制）——功耗随"真停机时间"
  线性下降，是热保护的最强一档
代价：吞吐量直损 + 唤醒延迟抖动（对 RT 敏感负载慎用）
```

## 二、通用框架：idle_inject（drivers/powercap/idle_inject.c，已核对 6.6）

```
struct idle_inject_device { hrtimer timer, idle_duration_us,
  run_duration_us, cpumask, update 回调 }
执行单元：per-CPU kthread（idle_inject_thread { task_struct,
  should_run }）周期到达 → play_idle_precise(duration_ns,
  latency_ns)（include/linux/cpu.h:200——受 latency 上限保护的
  强制 idle：调度器视角 = 本 CPU 上一个"不可运行"的普通任务）
节拍：hrtimer 每 (idle + run) 一个周期；下一周期的时长由
  idle_inject_set_duration() 调整——update 回调在每周期边界
  被调（本周期结果 → 下一周期参数——反馈窗口一拍）
注册面：idle_inject_register(cpumask)/start/stop/unregister
```

## 三、intel_powerclamp（drivers/thermal/intel/intel_powerclamp.c，已核对 6.6）

```
★架构事实：6.6 的 powerclamp 已跑在 idle_inject 框架上
  （static struct idle_inject_device *ii_dev）——老资料里
  "自管理 clamp 线程 + TSC 对齐"的历史实现已成过去

反馈环 idle_inject_update()（每周期回调，已核对）：
  每 window_size 个周期触发一次：
    powerclamp_adjust_controls(target_ratio, guard, window_size_now)
    ——滑动窗口统计实际注入占比 vs 目标，修正"跳过"决策
    （外部中断打扰了注入 → 该周期结果打折）
    idle_inject_set_duration(ii_dev, runtime, duration)  ← 下一拍

模块参数（0644，带运行期护栏）：
  duration    默认 6 jiffies（DEFAULT_DURATION_JIFFIES）
  max_idle    默认 50%（DEFAULT_MAX_IDLE=50，u8，MAX_TARGET_RATIO 封顶）
  window_size 默认 2（"sliding window in number of clamping cycles"）
  cpumask     注入 CPU 集（运行期可改，check_invalid 护栏）
其它：find_target_mwait（挑 mwait 提示位——往深 idle 状态注入）；
  thermal_cooling_device_ops{get_max/cur_state, set_cur_state}
  注册为 cdev（thermal 章的 governor 统一调度它）；
  X86_MATCH_VENDOR_FEATURE(INTEL, X86_FEATURE_MWAIT)——非 Intel 免进
```

## 四、兄弟件：cpuidle_cooling（非 Intel 平台）

```
drivers/thermal/cpuidle_cooling.c：同样架在 idle_inject 上
  （per idle_inject_device），按 cpuidle driver 生成"generic
  processor cooling"——ARM/其它平台的热防护标配
两者分工：powerclamp 带 Intel 专属的 mwait/反馈精调；
  cpuidle_cooling 走通用路径（named "cpuidle_cooling" 注册）
```

## 五、三种 cdev 对比（补全 thermal 章的家谱）

| | cpufreq_cooling | devfreq_cooling | idle 注入（powerclamp/cpuidle_cooling） |
| :--- | :--- | :--- | :--- |
| 手段 | 压 CPU 频率上限 | 压设备频率 | 压占空比（强制 idle） |
| 功效曲线 | 受 Leakage 拖累 | 设备相关 | 线性（真停机） |
| 吞吐代价 | 中 | 中 | 高 |
| 版本注 | 6.6 现役 | 6.6 现役 | 6.6 已框架化 |

## 六、观测与实战

```
/sys/module/intel_powerclamp/parameters/{duration,max_idle,
  window_size,cpumask}                # 全部运行期可调（0644）
/sys/class/thermal/cooling_device*/type   # intel_powerclamp /
  cpuidle_cooling 的注册名
验证注入生效： turbostat 的 %idle 异常抬升 + PkgWatt 下降曲线；
  /proc/timer_list 无异常（hrtimer 周期属正常）
排障：注入比例达不到 target → 外部中断/锁频率（powerclamp 的
  window 反馈已在补偿——看 max_idle 与 guard 参数）
```

## 七、思考题

1. idle 注入的 kthread 本身占 CPU——为什么它跑得越多系统越"凉"？（提示：play_idle 的本质是调度出一个不可运行的长任务。）
2. 反馈环为何按 `window_size` 周期调整而非每拍？（对照 cpufreq governor 的 step_wise 滞回。）
3. `max_idle=50` 的封顶语义是什么？为什么不给到 100%？（推演：100% 注入 = 系统假死面。）
4. mwait 提示位的选择如何影响注入功耗？（对照 [03-cpuidle](03-cpuidle.md) 的 C-state 深浅。）
5. 与 cpufreq_cooling 组合使用时的次序由谁决定？（对照 thermal 章的 governor——step_wise 会同时拧多个 cdev。）

---

> 回看：[06-thermal 框架](06-thermal框架.md)｜[03-cpuidle](03-cpuidle.md)｜[07-devfreq 与 pm_qos](07-devfreq与pm_qos.md)
