# debugfs 全景索引：/sys/kernel/debug → 内核源（Linux 6.6）

> 定位：索引三部曲收官（[07-procfs](07-procfs观测索引.md) → [08-sysctl](08-sysctl调参索引.md) → 本篇）——分册引用的 debugfs 目录 → 创建点逐一对源 + 四文件系统象限 + 两大勘误重申。前置：07/08 章。

---

## 一、性质声明（用前必读）

```
debugfs 是"内核内部的调试面板"——官方明示无 ABI 稳定承诺
  （文件可随版本增删改名；工具不应依赖）
★tracing 不是 debugfs：kernel/trace 用独立的 tracefs_* API 族
  （trace.c:8881/:9140 的 tracefs_create_dir——挂载在
  /sys/kernel/tracing；debugfs 下的 tracing 只是兼容入口）
```

## 二、索引全表（分册引用项，逐一对源）

| debugfs 目录 | 创建点（v6.6） | 配合分册 |
| :--- | :--- | :--- |
| `sched/` | kernel/sched/debug.c:342 | [scheduler/13 全文件表](../../scheduler/docs/13-调度器debugfs全景.md) |
| `sched/numa_balancing/` | debug.c:366 | [scheduler/10](../../scheduler/docs/10-NUMA_balancing.md) |
| `zswap/` | mm/zswap.c:1543 | [mm/ksm-zswap](../../mm/ksm/ksm与zswap.md) |
| `shrinker/` | mm/vmscan.c（shrinker_debugfs_init） | [cgroup/05-memcg](../../cgroup/docs/05-memcg回收深挖.md) |
| `extfrag/` | mm/vmstat.c:2265 | [mm/00-速查](../../mm/docs/00-一页速查.md) |
| `bdi/` | mm/backing-dev.c:42-46 | [mm/pagecache](../../mm/pagecache/pagecache与writeback.md) |
| `wakeup_sources` | drivers/base/power/wakeup.c:1178 | [power/05-suspend](../../power/docs/05-suspend与s2idle.md) |
| `kvm/` | virt/kvm/kvm_main.c:113-114（**EXPORTED**） | [virt/kvm/01](../../virt/kvm/docs/01-vcpu线程模型与主循环.md) |
| `clk/` | drivers/clk/clk.c:3661（clk_summary 常用） | 通用补充 |
| （无 `workqueue/`） | **6.6 不存在**——sysfs 与 tracepoint 为现役观测面 | [workqueue/00 勘误](../../workqueue/docs/00-一页速查.md) |

## 三、勘误重申（索引体系的两大成果）

```
① /proc/sched_debug 已于 5.15 前后移除——现役等价物即本表
  sched/debug（0444）：见 [07 章 §三] 与 scheduler/13 §五
② sched_base_slice_ns / sched_migration_cost_ns 并非 sysctl
  ——仅存 sched/ 下的同名文件：见 [08 章 §四]
```

## 四、四文件系统象限（观测面的完整地图）

| 文件系统 | 挂载点 | 稳定性 | 职责 |
| :--- | :--- | :--- | :--- |
| procfs | /proc | 稳定（工具链依赖面） | 系统状态快照（07 章索引） |
| sysctl | /proc/sys | 稳定 | 可写调参（08 章索引） |
| debugfs | /sys/kernel/debug | **无承诺** | 子系统内部面板（本篇） |
| tracefs | /sys/kernel/tracing | 半稳定（事件名约定） | 追踪事件与实例 |

## 五、思考题

1. kvm_debugfs_dir 为什么 EXPORT——而 sched 的目录指针不导出？（模块化边界：KVM 模块要挂自己的子目录。）
2. tracefs 从 debugfs 分家的动机？（提示：容器/生产环境不想挂 debugfs 却需要 tracing。）
3. shrinker/ 的 per-shrinker 文件为什么用 shrinker 名而不是 id？（对照可读性与稳定性取舍。）
4. bdi/ 按设备名建目录——热插拔时的生命周期谁管？（bdi_register/unregister 对。）
5. "无 ABI 承诺"对工具链的实际含义？（对照 07 章 procfs 的对比。）

---

> 回看：[07-procfs 观测索引](07-procfs观测索引.md)｜[08-sysctl 调参索引](08-sysctl调参索引.md)｜[scheduler/13-debugfs 全景](../../scheduler/docs/13-调度器debugfs全景.md)
