# devfreq 与 pm_qos：设备级调频（Linux 6.6）

> 阅读目标：掌握 cpufreq 之外的世界——GPU/DRAM/存储/总线等可调频设备的统一框架 `devfreq`（实例结构、决策环、五 governor、监测双路径）与设备级功耗约束聚合 `dev_pm_qos`。前置：[02-cpufreq](02-cpufreq.md)（对照 CPU 侧）、[06-thermal](06-thermal框架.md)（devfreq_cooling 联动）。

---

## 一、定位：cpufreq 的"外设版"

```
cpufreq：policy/per-CPU 域、schedutil 与 PELT 联动——CPU 专属
devfreq：任意 dev 的框架（drivers/devfreq/），governor 概念复用
  但无调度器信号——负载统计全靠设备自己的 status
```

## 二、核心结构（include/linux/devfreq.h:180，已核对 6.6）

```c
struct devfreq {
	struct mutex lock;
	struct device dev;              /* ★内嵌 device：天然落在
	                                   /sys/class/devfreq/<名>/ */
	struct devfreq_dev_profile *profile;   /* 驱动提供：target 回调、
	                                           polling_ms、freq_table */
	const struct devfreq_governor *governor;
	struct opp_table *opp_table;
	struct notifier_block nb;             /* OPP/QoS 变更通知 */
	struct delayed_work work;             /* 轮询监测 */
	unsigned long *freq_table;  unsigned int max_state;
	unsigned long previous_freq;
	struct devfreq_dev_status last_status;
	struct dev_pm_qos_request user_min_freq_req;   /* ★sysfs min/max
	struct dev_pm_qos_request user_max_freq_req;      写入即 QoS 请求 */
	unsigned long scaling_min_freq, scaling_max_freq;
};
struct devfreq_governor {                 /* drivers/devfreq/governor.h */
	const char name[];  const u64 attrs, flags;
	int (*get_target_freq)(struct devfreq *, unsigned long *freq);
	int (*event_handler)(struct devfreq *, unsigned int event, void *data);
};
```

## 三、决策环：devfreq_update_target（已核对 6.6）

```
update_devfreq()（governor 的标准回写口，导出符号）
  → devfreq_update_target(devfreq, 0):
    ① governor->get_target_freq(devfreq, &freq)     ← 策略出价
    ② devfreq_get_freq_range(devfreq, &min, &max)   ← QoS 聚合出的区间
    ③ freq 越界则夹回（min 侧清 LEAST_UPPER_BOUND 标志——GLB/LUB
       的保守方向语义）
    ④ profile->target(dev, &freq)                   ← 驱动落地
       （通常走 OPP：devfreq_recommended_opp）
    ⑤ devfreq_notify_transition：SRCU 通知链
       （DEVFREQ_TRANSITION_NOTIFIER——对外暴露频率迁移）
```

## 四、五个 governor（宏名已核对）

| 名 | 语义 | 要点 |
| :--- | :--- | :--- |
| `simple_ondemand` | 按负载升降 | upthreshold=90 / downdifferential=5（**宏名 `DFSO_DOWNDIFFERENCTIAL`——内核源码真拼错**，彩蛋）可经 data 覆盖 |
| `performance` / `powersave` | 定格 | max / min |
| `userspace` | 用户说了算 | sysfs 写 target_freq |
| `passive` | 跟随父 | 双父源：**parent devfreq**（`get_target_freq_by_required_opp`——OPP 的 required-op 属性跨设备映射）；**parent cpufreq**（`get_target_freq_with_cpufreq`——外设跟随 CPU 频率的常见玩法，GPU/总线） |

## 五、触发：轮询与事件双路径

```
① 轮询：devfreq_monitor（DEFERRABLE_WORK，profile->polling_ms 周期，
   trace_devfreq_monitor tracepoint）——驱动只报 status，框架定期决策
② 事件：驱动在状态突变处主动 update_devfreq()（如 GPU 帧完成）
   ——零轮询延迟，代价是驱动要理解框架
```

## 六、pm_qos：设备级约束聚合（include/linux/pm_qos.h，已核对）

```
约束类型：DEV_PM_QOS_MIN_FREQUENCY / MAX_FREQUENCY（还有
  LATENCY/FLAGS 族）——per-device 的 plist 聚合（多请求取最严）
devfreq 侧的三类请求源：
  ① 用户：sysfs min/max_freq 写入 → user_min/max_freq_req
     （scaling_min/max_freq 缓存，QoS nb 同步）
  ② 框架/其它子系统：如 thermal 的 devfreq_cooling
     （[06-thermal 章](06-thermal框架.md)的 cdev 之一——
     热约束经 pm_qos 传到设备频域）
  ③ PM runtime/latency 类：resume 期间的 QoS 恢复
```

**分层全景**：cpufreq（CPU+freq_qos）与 devfreq（设备+dev_pm_qos）是**同构的两套**——都是"governor 出价 + QoS 定区间 + 驱动落地 + 通知链广播"；thermal 分别经 cpufreq_cooling/devfreq_cooling 接入两侧（thermal 章的两种 cdev）。

## 七、观测与实战

```
/sys/class/devfreq/<device>/
  governor  cur_freq  min_freq  max_freq  polling_interval
  available_governors  trans_stat（迁移矩阵/时间/次数）
tracefs: devfreq:devfreq_monitor / devfreq_frequency
调试：trans_stat 高频抖动 → governor 参数（upthreshold/微分项）；
  min_freq 卡死 → 查 QoS 请求者（thermal? 用户残留?）
```

## 八、思考题

1. devfreq 内嵌 `struct device` 的设计红利？（提示：sysfs/lifecycle/PM runtime 全免费。）
2. passive 的"跟随 cpufreq"与"跟随 devfreq 父"分别依赖什么映射机制？（OPP required-op 属性 vs 直接查询。）
3. `simple_ondemand` 的 90/5 与 CPU 侧 schedutil 的 util 反馈有何本质差异？（对照：无调度器信号，只能事后统计。）
4. min_freq 被外部 QoS 顶高时 governor 出价仍低——谁兜底？（对照决策环第 ③ 步的夹逼。）
5. 为什么 devfreq_cooling 走 pm_qos 而不是直接调 target？（提示：与 cpufreq_cooling 的 freq_qos 对齐——约束语义 vs 命令语义。）

---

> 回看：[02-cpufreq](02-cpufreq.md)｜[06-thermal 框架](06-thermal框架.md)｜[04-runtime PM](04-runtime-PM.md)
