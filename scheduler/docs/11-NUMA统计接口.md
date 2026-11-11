# NUMA 统计接口：三层观测源（Linux 6.6）

> 定位：[10-NUMA balancing](10-NUMA_balancing.md) 的观测落地——任务级（/proc/<pid>/sched 的 NUMA 块）、地址空间级（numa_maps 逐 VMA）、节点级（numastat 六计数器）三层统计源的数据语义与排障组合。前置：10 章。

---

## 一、三层统计源地图

```
任务级    /proc/<pid>/sched          "这个任务在哪 faults、迁了多少"（采样口径）
地址空间级 /proc/<pid>/numa_maps      "每段 VMA 的页分布在哪"（放置口径）
节点级    /sys/devices/system/node/nodeN/numastat   "本节点分配命中/落空"（全局口径）
```

## 二、任务级：/proc/<pid>/sched 的 NUMA 块（kernel/sched/debug.c，已核对）

```
sched_show_numa（:975-986）输出：
  mm->numa_scan_seq        采样轮次（10 章的扫描进度）
  numa_pages_migrated      已迁移页数（对账 vmstat 的
                           numa_pages_migrated）
  se.nr_migrations         任务自身迁移次数（:1009）
  print_numa_stats（:962-971 原句）逐节点：
    "numa_faults node=%d task_private=%lu task_shared=%lu
     group_private=%lu group_shared=%lu"
    ——四象限正是 10 章记账模型的直读（private/shared ×
    task/group）；节点间对比即"数据在哪边"的答案
```

## 三、地址空间级：/proc/<pid>/numa_maps（fs/proc/task_mmu.c，已核对）

```
show_numa_map（:1940-2015）逐 VMA 一行：
  <起始地址> <策略前缀>（default/bind/interleave/huge... 由
    mpol 决定——walk 期间走 mpol_lookup）+ 条件字段族：
  dirty= / mapped= / mapmax= / swapcache= / active= /
    writeback=（各自只在非零时打印）
  N0=… N1=…                      ★每节点页分布（:2008-2009）
  kernelpagesize_kB=             页粒度（:2011——THP 大页时非 4）
用法：与 10 章"采样"对账——numa_maps 是**放置事实**（页真在哪），
  sched 的 numa_faults 是**访问倾向**（采样到的缺页在哪）——
  两者背离 = 迁移决策的输入
```

## 四、节点级：numastat（include/linux/mmzone.h:119-127 注释原样）

```
/sys/devices/system/node/nodeN/numastat：
  numa_hit           allocated in intended node    （应得所得）
  numa_miss          allocated in non intended node（应得未得）
  numa_foreign       was intended here, hit elsewhere（本节点
                     的配额被别处拿走——miss 的对偶）
  interleave_hit     interleaver preferred this zone
  numa_local         allocation from local node    （本地请求）
  numa_other         allocation from other node    （外来请求）
解读：hit/(hit+miss)=本节点命中率；local vs other=谁在用本节点；
  foreign 高=放置策略与实际容量错位
姊妹文件：nodeN/meminfo（Node 上的内存构成）与 10 章的
  /proc/vmstat numa_* 计数器互补
```

## 五、组合排障（10 章症状速查的数据源落地）

```
症状"本地命中率低"：
  ① numa_maps 看页分布（放置错？）
  ② sched 的 task_private 按节点排（访问在哪个节点？）
  ③ numastat 的 foreign/miss（全局放置压力？）
症状"迁移不收敛"：
  ① sched 的 numa_scan_seq 增长率（采样过密？→ 10 章 size/period）
  ② numa_maps 前后快照 diff（页真的在动吗）
  ③ nr_migrations vs numa_pages_migrated（任务在跑页不动？）
工具衔接：numactl --hardware（拓扑与 meminfo 汇总）、
  mpstat -P ALL（节点侧 CPU 视角）
```

## 六、思考题

1. numa_faults（采样）与 numa_maps（放置）背离时迁移决策听谁的？（对照 10 章三步协议的输入。）
2. numa_foreign 为什么是 numa_miss 的"对偶"而不是同义？（提示：立场不同——本节点视角 vs 请求方视角。）
3. group_private/task_private 的分野解决了什么盲区？（单任务视角看不到的共享页归属。）
4. numa_maps 的条件字段族（仅非零打印）对大 VM 的可读性设计。（对照 /proc/smaps 的全量风格。）
5. interleave_hit 单列的语义是什么？（提示：interleave 策略的"预期分布"与命中率的特殊关系。）

---

> 回看：[10-NUMA balancing](10-NUMA_balancing.md)｜[mm/00-一页速查](../../mm/docs/00-一页速查.md)｜[06-调度域与负载均衡](06-调度域与负载均衡.md)
