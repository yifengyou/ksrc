# Introduction — cgroup 资源治理分册

基于 **Linux 6.6** 的 cgroup 资源治理深度笔记。本分册站在**治理视角**（接口语义、层级传播、排障实战），与既有分册的机制深挖互补——CFS 带宽细节在 [scheduler 分册](../scheduler/docs/03-CFS与EEVDF详解.md)，blkcg 的块层集成在 [block 分册](../block/README.md)，页记账与回收在 [mm 分册](../mm/README.md)。

## 章节导读

| 章节 | 主题 | 一句话 |
| :--- | :--- | :--- |
| [00-一页速查](docs/00-一页速查.md) | 速查 | v2 机制/cpu.weight 范围/memory 双限语义/io+cpuset/uclamp 夹逼/PSI（各章浓缩） |
| [01-v2 层级与线程模式](docs/01-v2层级与线程模式.md) | 框架底座 | 统一层级动机、cgroup/css/subsys 四层结构、kernfs、线程模式四态、内部进程约束 |
| [02-cpu 控制器](docs/02-cpu控制器.md) | 比例与硬顶 | weight→shares 换算链、cpu.max 的 quota/period/burst、cpu.idle、throttle 排障、PSI |
| [03-memory 控制器](docs/03-memory控制器.md) | 四道闸门 | obj_cgroup 两层记账、min/low/high/max 行为差异、memcg 回收与组内 OOM、memory.stat 逐列、stock 批处理 |
| [04-io 控制器与 cpuset](docs/04-io控制器与cpuset.md) | 四把刀 | io.cost 的 vrate 反馈环、io.latency 的 scale_lat 迟滞、io.max 双级桶、cpuset partition 四态（含 6.6 isolated）与完整隔离栈 |
| [05-memcg 回收深挖](docs/05-memcg回收深挖.md) | 执行路径 | try_charge 全链（high 罚睡 vs max OOM 双语义）、memcg lruvec 与 MGLRU（walk_mm 扫 PTE 老化）、shrink_folio_list 决策机、MEMCG_AWARE shrinker、memory.reclaim/PSI 观测 |
| [06-uclamp 深挖](docs/06-uclamp深挖.md) | 组级 effective | 四层来源（任务/组/系统/RT 默认）、uclamp_tg_restrict 的 clamp() 夹逼公式、桶聚合 O(1)（UCLAMP_BUCKETS=5）、sched_uclamp_used 静态键零开销、schedutil/EAS 双消费点 |
| [07-css_set 深挖](docs/07-css_set深挖.md) | 数据结构 | 组合去重枢纽（同组合任务共享实例）、find_existing 的 template 构造（只换动到的 root）、find_css_set 全函数、put_css_set_locked 释放链与 kfree_rcu、迁移 mg 双链挂点 |
| [08-v1 淘汰史](docs/08-v1淘汰史.md) | 历史纵深 | v1 多挂载树模型、6.6 兼容层 1308 行盘点（release_agent/CVE-2022-0492/clone_children/cgroup_no_v1）、v2 砍除对照表（devices→BPF；net_cls/net_prio 冻结勘误）、淘汰节奏 |
| [09-v1 到 v2 迁移手册](docs/09-v1到v2迁移手册.md) | 实操桥 | 控制器文件映射全表、五个语义陷阱（soft→low 反转/memsw 拆分/use_hierarchy 隐含/无内部进程/tasks 限制）、cgroup2 挂载四选项（★memory_recursiveprot 实名）、systemd 对照、七步 playbook |

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
