# HWP 与 EPP 调优实战（Linux 6.6）

> 阅读目标：掌握 Intel 硬件自主 P-state（HWP）的寄存器模型与调优面——`MSR_IA32_HWP_REQUEST` 的四字段、EPP 五档偏好（raw 值语义）、EPB 回退、驱动模式（active/passive）与 EPP 的交互、`hwp_dynamic_boost`。前置：[02-cpufreq](02-cpufreq.md)（governor 世界观）、[07-devfreq](07-devfreq与pm_qos.md)（对照设备侧）。

---

## 一、HWP：把调频决策搬进硬件

```
开启：写 MSR_PM_ENABLE（msr-index.h:468——★无 IA32 前缀，勘误：原稿误写
  MSR_IA32_PM_ENABLE）后，硬件在 [min,max] 区间内自选
  P-state——OS 从"命令者"降级为"建议者"（区间 + 偏好）
请求字（MSR_HWP_REQUEST 位域，arch/x86/include/asm/msr-index.h 已核对）：
  bits  0-7  HWP_MIN_PERF      区间下限
  bits  8-15 HWP_MAX_PERF      区间上限
  bits 16-23 HWP_DESIRED_PERF  建议（硬件可不理）
  bits 24-31 EPP               能耗-性能偏好 ★调优主旋钮
能力字（HWP_CAPABILITIES）：highest / guaranteed / lowest——
  guaranteed 即 base 频率（turbo 档的参照系）
CPUID 旗标：X86_FEATURE_HWP/HWP_NOTIFY/HWP_ACT_WINDOW/HWP_EPP
  （cpufeatures.h 14*32+7~10 已核对）——没有 HWP_EPP 的老硬件
  走 EPB 回退（下节）
```

## 二、EPP：偏好的四档（intel_pstate.c，已核对）

```
energy_perf_strings[]（sysfs 可写值）   raw 值（msr-index.h）
  performance            0x00   性能优先（功耗上限内尽力提频）
  balance_performance    0x80   默认档的常见值——偏性能的平衡
  balance_power          0xC0   偏节能的平衡（★sysfs 字符串是
                                balance_power 而非枚举名
                                EPP_INDEX_BALANCE_POWERSAVE——原稿
                                误写 balance_powersave，勘误已注）
  power                  0xFF   节能优先（最低满足负载的频率）
  default                ——    跟随策略/policy 推导
读回路径：intel_pstate_get_energy_pref_index 把 MSR/缓存里的
  raw epp 反查字符串（epp_values[] 表已核对）；写路径
  store_energy_performance → intel_pstate_set_energy_pref_index →
  更新 hwp_req_cached 的 EPP 字段并 intel_pstate_hwp_set 落 MSR
EPB 回退：无 X86_FEATURE_HWP_EPP 时用 MSR_IA32_ENERGY_PERF_BIAS
  （0-15；源码注释原句"When there is no EPP present, HWP uses
  EPB settings"）——x86_energy_perf_policy 工具的老朋友
```

## 三、驱动模式与 EPP 的交互

```
active 模式：intel_pstate 直控——policy 变化触发
  intel_pstate_hwp_set（源码原句已核对）：
    if (policy == CPUFREQ_POLICY_PERFORMANCE)
        min = max;               ← performance 策略把下限拉满
    epp_policy 缓存避免重复写 EPP
passive 模式：governor（schedutil）出 desired；HWP 在区间内兜底
  ——EPP 仍然有效（硬件在"建议"与"偏好"间自己权衡）
```

## 四、hwp_dynamic_boost：周期性上探

```
/sys/devices/system/cpu/intel_pstate/hwp_dynamic_boost（6.6 现役）：
  开启后 intel_pstate_hwp_boost_up/down 在空闲-繁忙跳变时短暂
  抬 max_perf——交互场景的"响应性小灶"（代价：功耗尖峰）
  注释原句：REQUEST MSR "may be updated by intel_pstate_hwp_boost_up()
  or _down() at any time"
```

## 五、调优实战矩阵

| 场景 | 推荐 | 理由 |
| :--- | :--- | :--- |
| 计算/编译 | `performance` 或 policy=performance（min=max） | 拉满吞吐 |
| 交互桌面 | `balance_performance` + hwp_dynamic_boost | 响应优先 |
| 长时稳态服务 | `balance_power` | 能效曲线最优段 |
| 电池/静音 | `power` + max 限制 | 压 turbo |
| 混合负载（cpufreq+thermal） | EPP + thermal 的 max clamp（[06 章](06-thermal框架.md)） | 偏好与区间分层 |

测量方法：`turbostat` 的 `Bzy_MHz / PkgWatt / Bzy_MHz~PkgWatt 比值` 三档对照——同一 workload 扫五档 EPP 画出机器自己的能耗-性能曲线（比任何攻略都准）。

## 六、观测与排障

```
cat /sys/devices/system/cpu/cpu0/cpufreq/
    energy_performance_available_preferences
    energy_performance_preference          # 当前档
cat /sys/devices/system/cpu/intel_pstate/status   # active/passive/off
cat /sys/devices/system/cpu/intel_pstate/hwp_dynamic_boost
turbostat --interval 1                     # HWPdesired/BusyMHz/PkgWatt
msr 工具直接读 MSR_HWP_REQUEST（验证缓存是否同步）
排障链：改 EPP 无效果 → 查 status 是否 off（acpi-cpufreq 顶替，
  无 EPP 面）；turbo 上不去 → thermal/power cap（RAPL）而非 EPP
```

## 七、思考题

1. `HWP_DESIRED_PERF` 只是"建议"——active 模式下 OS 究竟还剩多少控制力？（对照 min/max/EPP 三字段的硬软之分。）
2. EPB（0-15）与 EPP（四档）如何映射？谁做翻译？（提示：intel_pstate 的 get/set_epp 两条路径。）
3. `performance` 策略把 min=max——为什么这比"仅调 EPP=performance"更激进？（区间 vs 偏好两层的不同弹性。）
4. hwp_dynamic_boost 与 schedutil 的 util 信号是什么关系？（对照 passive 模式：一个硬件周期性上探、一个软件即时出价。）
5. 五档 EPP 的实测曲线为什么必然机器相关？（提示：guaranteed/highest 的物理比与散热设计。）

---

> 回看：[02-cpufreq](02-cpufreq.md)｜[06-thermal 框架](06-thermal框架.md)｜[07-devfreq 与 pm_qos](07-devfreq与pm_qos.md)
