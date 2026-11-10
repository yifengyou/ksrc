# Introduction — 进程调度分册

本分册深入剖析 Linux 6.6 的进程调度子系统，按"演进 → 数据结构 → 算法 → 时机 → 追踪 → 均衡 → 实时"的顺序层层展开。

## 章节导读

| 章节 | 主题 | 一句话 |
| :--- | :--- | :--- |
| [00-一页速查](docs/00-一页速查.md) | 速查 | 五件套/调度类链/EEVDF-PELT-RT 速记/sysctl 与观测命令总表（各章浓缩） |
| [01-调度器演进与总体架构](docs/01-调度器演进与总体架构.md) | O(n)→O(1)→CFS→EEVDF | 调度类的多态框架与五类调度事件全景 |
| [02-核心数据结构](docs/02-核心数据结构.md) | task_struct/se/rq/cfs_rq/root_domain | 逐字段精读（含 6.6 EEVDF 新字段） |
| [03-CFS与EEVDF详解](docs/03-CFS与EEVDF详解.md) | vruntime/lag/deadline | 公平与延迟统一模型的逐函数实现 |
| [04-调度时机与上下文切换](docs/04-调度时机与上下文切换.md) | ttwu/__schedule/switch_to | 唤醒内存序、lazy TLB、抢占点地图 |
| [05-PELT与利用率追踪](docs/05-PELT与利用率追踪.md) | load/runnable/util_avg | 几何衰减、util_est、uclamp、schedutil |
| [06-调度域与负载均衡](docs/06-调度域与负载均衡.md) | sched_domain/load_balance | group_type×migration_type、misfit、EAS、NUMA |
| [07-实时与Deadline调度](docs/07-实时与Deadline调度.md) | FIFO/RR/DL | push/pull、throttling、CBS/GRUB、准入 |

## 专题子目录

* [sched-batch](sched-batch/)：腾讯 `SCHED_BT` 离在线混部调度（RUE）
* [xsched.md](xsched.md)：阿里类混部调度实践
* 其余 `*.rst` 为内核官方文档镜像，最有价值的几篇：
  * `sched-design-CFS.rst` — CFS 设计文档（对照第 3 章阅读，注意其描述基于 6.5 之前的实现）
  * `sched-deadline.rst` — DEADLINE 官方文档（最权威的 admission 公式推导）
  * `sched-capacity.rst` / `sched-energy.rst` — capacity 与 EAS 体系（对照第 6 章）
  * `sched-util-clamp.rst` / `schedutil.rst` — uclamp 与 schedutil（对照第 5 章）
  * `sched-bwc.rst` — CFS 带宽控制（cgroup cpu.max 的实现）
  * `sched-rt-group.rst` — RT 组调度

## 源码索引（Linux 6.6）

```
kernel/sched/core.c        核心：__schedule/ttwu/setscheduler/PI（~12000 行）
kernel/sched/fair.c        EEVDF：13000 行，公平性主战场
kernel/sched/rt.c          RT：push/pull/throttle
kernel/sched/deadline.c    DL：CBS/GRUB/准入
kernel/sched/idle.c        idle 类与 do_idle
kernel/sched/stop_task.c   stop 类（migration 线程）
kernel/sched/topology.c    sched_domain 构建
kernel/sched/pelt.c        PELT 衰减实现
kernel/sched/sched.h       内部头：rq/cfs_rq/sched_class（本分册引用最多的文件）
include/linux/sched.h      task_struct/sched_entity（对外 ABI）
include/linux/sched/topology.h  sched_domain/SD flags
```

## 推荐实验环境

```
CONFIG_SCHED_DEBUG=y CONFIG_SCHEDSTATS=y CONFIG_DEBUG_FS=y   # sched_debug 观测
CONFIG_PREEMPT_DYNAMIC=y CONFIG_PREEMPT=y                    # 抢占模型对比
CONFIG_FAIR_GROUP_SCHED=y CONFIG_CFS_BANDWIDTH=y             # cgroup CPU
CONFIG_UCLAMP_TASK=y CONFIG_ENERGY_MODEL?                    # 能效（需要 EM 描述）
工具：perf / bpftrace / schbench / hackbench / cyclictest / runqlat
```

## 相关分册

* 内存管理（[mm/](../mm/README.md)）— fork 时的 mm 复制、页错误与调度
* 中断子系统（[interrupt/](../interrupt/README.md)）— irq 线程的调度身份
* RCU（[RCU/](../RCU/README.md)）— context_switch 与 grace period
* 内核同步（[locking/](../locking/SUMMARY.md)）— preempt_count 与锁的交互
