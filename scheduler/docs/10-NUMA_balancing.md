# NUMA balancing：PTE 采点与迁移建议（Linux 6.6）

> 阅读目标：掌握 NUMA 亲和的自动维护机制——三步协议（**PTE 陷阱 → 缺页记账 → 迁移/调度决策**）、扫描节奏的自适应公式、sysctl/debugfs 调参面、vmstat 观测计数器与开关时机。这是 scheduler 分册（[06-调度域](06-调度域与负载均衡.md)）与 mm 分册（缺页路径）的交叉机制。前置：06 章、[mm/00-一页速查](../../mm/docs/00-一页速查.md)。

---

## 一、问题：NUMA 亲和靠谁维护

```
首次分配只管"当下在哪个节点"；任务迁移/负载变化后，热点数据
落在远端节点 → 本地命中率衰减（访问延迟 ~2×）
NUMA balancing（3.8 起自动）：调度器+内存子系统的协同——
  无需应用配合，周期性"采点"统计任务的真实访存分布，
  然后把任务调去数据侧（调度）或把数据搬来任务侧（页迁移）
```

## 二、三步协议（kernel/sched/fair.c + mm/memory.c，已核对 6.6）

```
① PTE 陷阱（task_numa_work:3207——task_work 回调）：
  到达 mm->numa_next_scan（首次 = now + scan_delay）后：
  以 numa_balancing_scan_size（默认 256MB）为步长遍历 vma，
  change_prot_numa(vma, start, end)（:3345）批量把 PTE 改为
  PROT_NUMA（PROT_READ+NUMA 语义位——mm/mprotect.c 的
  MM_CP_PROT_NUMA 路径）→ 后续任何访问触发缺页
② 缺页记账（mm/memory.c do_numa_page:4739，:4987 挂进
  handle_pte_fault）：
  恢复真实权限 + 记账到 mm->numa_faults——四象限：
  private/shared × local/remote（NR_NUMA_HINT_FAULT_TYPES=2，
  fair.c:1519-1549）+ last_cpupid 归因（PTE 携带的"上次使用者"）
③ 决策：本地/远端分数倾斜 →
  任务侧：负载均衡偏向数据所在节点（06 章的 NUMA 熔断）
  页侧：热页走 migrate_misplaced_page（mm/migrate.c——异步迁移）
```

## 三、扫描节奏自适应（已核对）

```
初始：task_numa_work 首跑置 numa_scan_period=task_scan_start
  （受 sysctl_numa_balancing_scan_delay=1000 起步延迟影响）
调整（fair.c:2633-2650）：
  远端多 → period 减半加速采样；本地多 → 加倍（最大翻倍至
  numa_scan_period_max）——NUMA_PERIOD_SLOTS=10 的比例槽：
  lr_ratio = local×10/(local+remote)、ps_ratio = private×10/
  (private+shared)，slot 差驱动 period 加减
量程：scan_period_min=1000ms … scan_period_max=60000ms
  （fair.c:1371-1372 原值）
```

## 四、开关与调参（已核对）

```
sysctl kernel.numa_balancing：
  0=关 / 1=开 / 2+ = memory tiering 模式
  （sysctl_numa_balancing_mode & NUMA_BALANCING_MEMORY_TIERING——
  6.6 现役的分层内存联动，fair.c:1850）
debugfs（/sys/kernel/debug/sched/numa/，debug.c:368-371）：
  scan_delay_ms / scan_period_min_ms / scan_period_max_ms /
  scan_size_mb —— 四参数运行期可调
```

## 五、观测

```
grep numa_ /proc/vmstat   # 四计数器（vmstat.c:1307-1311 已核对）：
  numa_pte_updates        陷阱改写次数（开销主项）
  numa_hint_faults        采样缺页总数
  numa_hint_faults_local  本地命中（local/remote 命中率分母）
  numa_pages_migrated     页迁移数
cat /proc/<pid>/numa_maps # 每段的节点分布（应用侧对账）
症状速查：pte_updates 巨大而 faults 少 → 采样过密（调大 size/
  period）；pages_migrated 持续高 → 放置策略抖动（检查内存压力）
```

## 六、代价与开关时机

```
收益侧：多实例数据库/JVM（任务与堆随机漂移后自动回拢）
代价侧：① TLB 失效风暴（批量改 PTE）② 单线程重访存任务被
  采样打爆 ③ 虚拟化/CI 短命任务来不及回本
实践：CPU 密集无远端访存 → 关；memory tiering（CXL/多级）→
  模式 2 与 demotion 协同（[mm 分册] 联动）
```

## 七、思考题

1. PROT_NUMA 为什么复用 PROT_READ 语义位而不是新 PTE 位？（提示：x86 PTE 位稀缺；fault 后恢复原权限的记账链。）
2. 采样（hint fault）与真实统计的偏差在什么负载下最大？（提示：工作集远大于 scan_size 时的抽样误差。）
3. 任务调走 vs 页搬来——决策依据的差别？（对照：私有页跟任务、共享页少数服从多数。）
4. period 自适应的翻倍收敛为什么不线性？（对照 NUMA_PERIOD_SLOTS 槽位差。）
5. memory tiering 模式下 NUMA balancing 与页 demotion 的分工？（提示：谁负责"冷页下沉"。）

---

> 回看：[06-调度域与负载均衡](06-调度域与负载均衡.md)｜[mm/00-一页速查](../../mm/docs/00-一页速查.md)｜[cgroup/04-cpuset](../../cgroup/docs/04-io控制器与cpuset.md)
