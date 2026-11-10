# Introduction — cgroup 资源治理分册

基于 **Linux 6.6** 的 cgroup 资源治理深度笔记。本分册站在**治理视角**（接口语义、层级传播、排障实战），与既有分册的机制深挖互补——CFS 带宽细节在 [scheduler 分册](../scheduler/docs/03-CFS与EEVDF详解.md)，blkcg 的块层集成在 [block 分册](../block/README.md)，页记账与回收在 [mm 分册](../mm/README.md)。

## 章节导读

| 章节 | 主题 | 一句话 |
| :--- | :--- | :--- |
| [01-v2 层级与线程模式](docs/01-v2层级与线程模式.md) | 框架底座 | 统一层级动机、cgroup/css/subsys 四层结构、kernfs、线程模式四态、内部进程约束 |
| [02-cpu 控制器](docs/02-cpu控制器.md) | 比例与硬顶 | weight→shares 换算链、cpu.max 的 quota/period/burst、cpu.idle、throttle 排障、PSI |
| [03-memory 控制器](docs/03-memory控制器.md) | 四道闸门 | obj_cgroup 两层记账、min/low/high/max 行为差异、memcg 回收与组内 OOM、memory.stat 逐列、stock 批处理 |
| [04-io 控制器与 cpuset](docs/04-io控制器与cpuset.md) | 四把刀 | io.cost 的 vrate 反馈环、io.latency 的 scale_lat 迟滞、io.max 双级桶、cpuset partition 四态（含 6.6 isolated）与完整隔离栈 |

## 学习路线

```
01 框架（先建立层级世界观）
   ├─→ 02 cpu   ──→ 对照 scheduler 分册第 3 章（机制）
   ├─→ 03 memory ──→ 对照 mm 分册（页/回收）
   └─→ 04 io+cpuset ──→ 对照 block 分册（blkcg）
```

## 源码索引（Linux 6.6）

```
kernel/cgroup/cgroup.c          层级框架（~7000 行，两大主战场之一）
kernel/cgroup/cpuset.c          cpuset
mm/memcontrol.c                 memory 控制器（~7000 行）
mm/page_counter.c               限额/保护/事件的层级账本
kernel/sched/core.c（cpu_files 段） cpu 控制器接口
block/blk-iocost.c / blk-iolatency.c / blk-throttle.c  io 控制器
kernel/sched/psi.c              压力失速信息
include/linux/cgroup-defs.h     四层结构定义
```

## 官方文档镜像

仓库 `admin-guide/cgroup-v2.rst`（内核自带 v2 全量接口说明）——本分册做源码级深挖与排障实战互补。
