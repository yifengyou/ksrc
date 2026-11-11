# procfs 观测索引：/proc 文件 → 内核源（Linux 6.6）

> 定位：跨分册引用的 `/proc` 文件的**来源索引**——每个文件由哪个源文件创建（proc_create 逐一对 v6.6 核对）、数据回调是谁、配合哪个分册章节。含 ★/proc/sched_debug 移除勘误。前置：[01-内核观测体系总览](01-内核观测体系总览.md)。

---

## 一、全表：/proc 顶层文件 → 源（本轮逐一对源核对）

| /proc 文件 | 创建点（v6.6） | 数据源 | 配合分册 |
| :--- | :--- | :--- | :--- |
| `interrupts` | fs/proc/interrupts.c:39 | int_seq_ops | [interrupt](../../interrupt/docs/00-一页速查.md) |
| `softirqs` | fs/proc/softirqs.c:33 | show_softirqs | [interrupt](../../interrupt/docs/05-softirq与tasklet.md) |
| `timer_list`（0400） | kernel/time/timer_list.c:353 | timer_list_sops | [timers](../../timers/docs/00-一页速查.md) |
| `pressure/`（目录） | kernel/sched/psi.c:1653（proc_mkdir） | psi_*_show | [scheduler/09-PSI](../../scheduler/docs/09-PSI压力失速信息.md) |
| `vmstat`（0444） | mm/vmstat.c:2140 | vmstat_op | [mm/00-速查](../../mm/docs/00-一页速查.md) |
| `buddyinfo`（0444） | mm/vmstat.c:2138 | fragmentation_op | [mm](../../mm/buddy/buddy.md) |
| `pagetypeinfo`（0400） | mm/vmstat.c:2139 | pagetypeinfo_op | 同上 |
| `zoneinfo`（0444） | mm/vmstat.c:2141 | zoneinfo_op | [mm](../../mm/zone/zone.md) |
| `meminfo` | fs/proc/meminfo.c:178 | meminfo_proc_show | [mm/00-速查](../../mm/docs/00-一页速查.md) |
| `slabinfo` | mm/slab_common.c:1387 | slabinfo_proc_ops | [mm/slub](../../mm/slub/slub.md) |
| `diskstats` | block/genhd.c:1304 | diskstats_op | [block/06](../../block/docs/06-完成路径与统计.md) |
| `cgroups` | kernel/cgroup/cgroup.c:6162 | proc_cgroupstats_show | [cgroup/01](../../cgroup/docs/01-v2层级与线程模式.md) |
| `iomem` | kernel/resource.c:146 | resource_op（iomem_resource） | [kdump/02](../../ras/kdump/docs/02-crashkernel预留与elfcorehdr.md) |
| `keys` | security/keys/proc.c:47 | proc_keys_ops | [module/04](../../module/docs/04-模块签名链.md) |
| `schedstat` | kernel/sched/stats.c:228 | schedstat_sops | [scheduler/00-速查](../../scheduler/docs/00-一页速查.md) |
| `cmdline` / `version` | fs/proc/cmdline.c:19 / version.c:23 | cmdline/version_proc_show | [init/01](../../init/docs/01-内核启动总览.md) |
| `self/`、`<pid>/` 族 | fs/proc/base.c（ONE 宏族） | 见 §二 | 各分册 |

## 二、per-PID 文件族（fs/proc/base.c 的 ONE(...) 注册）

```
/proc/<pid>/status（Seccomp/Seccomp_filters——syscalls/02 章）
  /sched（numa_faults 四象限——scheduler/11 章）、/schedstat
  （三元组——scheduler/00-速查勘误后口径）
  /maps、/numa_maps（fs/proc/task_mmu.c——mm/11 统计接口章）
  /cgroup（多行=v1 判定——cgroup/09 迁移手册）、/syscall、/stat
```

## 三、★勘误中心：/proc/sched_debug 已不存在

```
时间线（本轮按 tag 逐版本核实）：v5.10 的 kernel/sched/debug.c
  尚有 proc_create 注册；v5.15 起 grep 归零——已被 **debugfs 的
  /sys/kernel/debug/sched/debug** 取代（0444，同款 dump）。
老教程/工具（老版 sched_tool 等）引用的 /proc/sched_debug 在
  6.6 直接 ENOENT。全仓 9 处旧引用已勘误（详见
  [scheduler/13](../../scheduler/docs/13-调度器debugfs全景.md) §五）。
```

## 四、/proc/sys（sysctl 树）与 debugfs 的边界

```
/proc/sys/*：kernel/sysctl.c 注册的表驱动树（可写参数——
  scheduler/00-速查的 sched_* 族在此）
debugfs/*：非稳定接口（观测+实验调参——各分册 debugfs 章）
/proc 顶层（本表）：稳定观测接口（工具链依赖面）
```

## 五、思考题

1. /proc 顶层与 /proc/sys 的稳定性承诺差异？（工具链兼容 vs 内核调参。）
2. timer_list 为何 0400 而非 0444？（信息量与安全考量——含内核地址。）
3. diskstats 在 block/genhd.c 而非 fs/proc/——注册时机的耦合点？（磁盘注册的动态性。）
4. sched_debug 从 /proc 迁往 debugfs 的信号意义？（观测接口的"稳定承诺"降级。）
5. per-PID 的 ONE() 宏族为什么集中在一个文件？（进程生命周期统一管理的入口面。）

---

> 回看：[01-内核观测体系总览](01-内核观测体系总览.md)｜[scheduler/13-debugfs 全景](../../scheduler/docs/13-调度器debugfs全景.md)｜各分册速查表
