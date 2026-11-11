# 调度器 debugfs 全景（Linux 6.6）

> 定位：scheduler 分册的观测中枢章——/sys/kernel/debug/sched/ 的**全部文件清单**（含 root domain 的 domains/ 子树）、动态抢占（preempt 文件三模式）、SCHED_FEAT 开关面、以及与 /proc/sched_debug 的分工。前置：[00-一页速查](00-一页速查.md)、[04-调度时机与上下文切换](04-调度时机与上下文切换.md)。

---

## 一、文件全表（kernel/sched/debug.c:342-375，已核对 6.6）

```
/sys/kernel/debug/sched/（debugfs_create_dir("sched") :342）
├── features          0644  SCHED_FEAT 开关（写名字±切换）
├── verbose           0644  sched_debug 的详细度（unsafe fops）
├── preempt           0644  ★动态抢占模式（§二）
├── base_slice_ns     0644  直接绑 sysctl_sched_base_slice（EEVDF
│                           基准片——00 速查同名项）
├── latency_warn_ms   0644  重调度延迟告警阈值（配 latency_warn_once）
├── tunable_scaling   0644  lognone/logarithmic/linear（负载越重
│                           片越长的缩放模式）
├── migration_cost_ns 0644  唤醒粘滞窗（00 速查同名）
├── nr_migrate        0644  单次均衡的最多任务数
├── numa_balancing/         四+一参数（§三）
│   ├── scan_delay_ms / scan_period_min_ms / scan_period_max_ms
│   ├── scan_size_mb
│   └── hot_threshold_ms  ★ch10 补漏的第五参数（§三）
├── debug             0444  全量 dump（等价 /proc/sched_debug）
└── domains/                调度域树镜像（§四）
```

## 二、动态抢占：preempt 文件（:245-258，已核对）

```
cat preempt 输出（当前模式加括号）：
  none (voluntary) full
三模式（CONFIG_PREEMPT_DYNAMIC；preempt= boot 参数同源）：
  none      无抢占（服务器吞吐型）
  voluntary 主动抢占点（传统 voluntary）
  full      全抢占（桌面/低延迟）
运行期可写切换（0644）——切换的是 preempt_schedule 与
  preempt_schedule_notthunk?（静态调用/静态键改道——04 章
  的抢占模型在运行时"换插槽"）
```

## 三、numa_balancing 第五参数：hot_threshold_ms（本轮新发现）

```
ch10 只列了四参数（delay/min/max/size）——★本表补漏：
sysctl_numa_balancing_hot_threshold（debug.c:372 注册；
fair.c:1381 默认 MSEC_PER_SEC=1000ms，:1863 用于 memory
tiering 的 demote 门槛 def_th——"热到什么程度才值得升回快层"
判定），memory tiering 模式（kernel.numa_balancing=2+）的
配套开关
```

## 四、domains/ 子树（:414-465，已核对）

```
/sys/kernel/debug/sched/domains/cpuN/domainX/
  ├── name/flags/groups_flags（0444 只读——调度域与调度组的
  │   实际标志位——06 章 SD_* 标志的"现值可读版"）
  └── min_interval/max_interval 等 u32（per-domain 参数镜像）
用途：验证 cpuset/tolology 生成的域结构是否如预期（隔离 CPU
  是否真的没建域——cpuset partition root 的现场验证）
```

## 五、与 /proc/sched_debug 的分工

```
/proc/sched_debug（CONFIG_SCHED_DEBUG 门控）：文本全量 dump
  （runqueue/任务树/域树）——一次性观察
debugfs/sched/*：逐参数可写（运行期调优）+ domains 的机器
  可读镜像——交互调优
等价物：debugfs 的 "debug" 文件（0444）与 /proc/sched_debug
  同源输出
```

## 六、观测速查

```
cat /sys/kernel/debug/sched/preempt          # 当前抢占模式
cat /sys/kernel/debug/sched/features         # 全部 SCHED_FEAT 现值
ls /sys/kernel/debug/sched/domains/cpu0/     # 域层级实况
排查链（延迟告警实战）：
  echo 100 > latency_warn_ms && echo 1 > latency_warn_once
  → dmesg "resched_latency" 定位长临界区任务（04 章配对）
```

## 七、思考题

1. preempt 运行期切换为什么可行？（对照 04 章抢占模型：静态调用改道的机制。）
2. tunable_scaling 三档对 base_slice 的实际影响？（负载重时片的自动放宽。）
3. hot_threshold_ms 为什么在 memory tiering 语境才有意义？（对照 ch10 的模式 2 与 demotion。）
4. domains/cpuN/domainX/flags 与 06 章 SD_* 宏的关系？（现值 vs 声明。）
5. latency_warn 的"once"语义设计？（风暴抑制 vs 持续监控的取舍。）

---

> 回看：[00-一页速查](00-一页速查.md)｜[04-调度时机与上下文切换](04-调度时机与上下文切换.md)｜[06-调度域与负载均衡](06-调度域与负载均衡.md)｜[10-NUMA balancing](10-NUMA_balancing.md)
