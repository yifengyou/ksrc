# sysctl 调参索引：/proc/sys → 内核源（Linux 6.6）

> 定位：[07-procfs 观测索引](07-procfs观测索引.md) 的姊妹篇——分册章节引用的 sysctl 项 → **注册表位置**（逐一对 v6.6 核对）+ ★sched_base_slice_ns/migration_cost_ns 幻影勘误。前置：07 章。

---

## 一、注册机制速记

```
6.6 的 sysctl 注册已去中心化：kernel/sysctl.c 只保留老基表，
  各子系统用 register_sysctl(_init)("kernel"/"vm", xxx_sysctls)
  自带 ctl_table——查任何项的真相要 grep 子系统而非只看 sysctl.c
```

## 二、kernel.* 索引（分册引用项，逐一对源）

| sysctl | 注册表（v6.6） | 配合分册 |
| :--- | :--- | :--- |
| `sched_rt_period_us`/`sched_rt_runtime_us` | kernel/sched/rt.c:35/:42 | [scheduler/07](../../scheduler/docs/07-实时与Deadline调度.md) |
| `sched_util_clamp_min/max`、`sched_util_clamp_min_rt_default` | core.c:4683-4697 | [cgroup/06-uclamp](../../cgroup/docs/06-uclamp深挖.md) |
| `sched_schedstats` | core.c:4672 | [scheduler/00](../../scheduler/docs/00-一页速查.md) |
| `sched_energy_aware` | topology.c:245 | [scheduler/06](../../scheduler/docs/06-调度域与负载均衡.md) |
| `sched_autogroup_enabled` | autogroup.c:14 | [scheduler/00](../../scheduler/docs/00-一页速查.md) |
| `numa_balancing`（0/1/tiering） | core.c:4706 | [scheduler/10](../../scheduler/docs/10-NUMA_balancing.md) |
| `numa_balancing_promote_rate_limit_MBps` | fair.c:163 区 | [scheduler/10](../../scheduler/docs/10-NUMA_balancing.md) |
| `sched_child_runs_first`/`sched_cfs_bandwidth_slice_us` | fair.c:147-163 | [cgroup/02](../../cgroup/docs/02-cpu控制器.md) |
| `sched_dl_period_min/max_us` 族 | deadline.c:51 | [scheduler/07](../../scheduler/docs/07-实时与Deadline调度.md) |
| `printk`（四值） | kernel/printk/sysctl.c:25 | [printk/02](../../printk/docs/02-console驱动模型.md) |
| `kexec_load_disabled` | kernel/kexec_core.c:987 | [kdump/01](../../ras/kdump/docs/01-kexec加载与跳转.md) |

## 三、vm.* 索引（逐一对源）

| sysctl | 注册表（v6.6） | 配合分册 |
| :--- | :--- | :--- |
| `swappiness` | kernel/sysctl.c:2091 | [mm/ksm](../../mm/ksm/ksm与zswap.md) |
| `overcommit_memory` | kernel/sysctl.c:2051 | [mm](../../mm/README.md) |
| `drop_caches` | kernel/sysctl.c:2111 | [mm/00-速查](../../mm/docs/00-一页速查.md) |
| `dirty_ratio`/`dirty_background_ratio`/`_bytes` 族 | mm/page-writeback.c:2228-2254 | [mm/pagecache](../../mm/pagecache/pagecache与writeback.md) |
| `dirty_expire_centisecs` | mm/page-writeback.c:2269 | 同上 |
| `min_free_kbytes` | mm/page_alloc.c:5940 | [mm/buddy](../../mm/buddy/buddy.md) |

## 四、★勘误中心：两个 scheduler "幻影 sysctl"

```
第 53 轮对源发现（与 52 轮 /proc/sched_debug 同类）：
  ✗ sched_base_slice_ns      ——6.6 sysctl 表 grep 零命中；
                              仅存于 debugfs（sched/base_slice_ns，
                              ch13 全表）
  ✗ sched_migration_cost_ns  ——同上，仅 debugfs
修正：scheduler/00-一页速查 §7 已重写（现存 9 项逐项附注册点；
  base_slice/migration_cost 调参路径改指 13 章 debugfs 表）
教训延续：老资料把"内核变量"与"sysctl 暴露"混为一谈——
  本索引的每一行都以注册表 procname 为准。
```

## 五、思考题

1. register_sysctl 去中心化后，`man sysctl` 类文档为何频繁过时？（对照 §四幻影的成因。）
2. min_free_kbytes 注册在 page_alloc.c 而非 sysctl.c——模块自带的表如何合并进 /proc/sys 树？
3. dirty_bytes 与 dirty_ratio 互斥覆盖的语义？（写一个清另一个。）
4. sched_cfs_bandwidth_slice_us 与 cgroup 的 cpu.max 谁优先？（组级配额内的再切分。）
5. kexec_load_disabled 为何不可逆（写 1 后无法归零）？（对照 module 签名链的 bool_enable_only 同型设计。）

---

> 回看：[07-procfs 观测索引](07-procfs观测索引.md)｜[scheduler/13-debugfs 全景](../../scheduler/docs/13-调度器debugfs全景.md)｜各分册速查
