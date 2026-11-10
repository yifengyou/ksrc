# io 控制器与 cpuset（Linux 6.6）

> 阅读目标：掌握 v2 io.* 控制器家族四件套的设计光谱（确定性模型/反馈延迟/绝对上限/比例权重）；吃透 iocost 的 cost model 与 vrate 反馈环、iolatency 的 scale_lat 自适应窗口；掌握 cpuset 的 v2 接口与 partition 四态（含 6.6 的 isolated）。

---

## 一、blkcg 基座（衔接 block 分册）

io 控制器全部构建在 blkcg（block cgroup）上：每个 (cgroup × 块设备) 组合一个 `blkcg_gq`，策略数据（`blkg_policy_data`）按注册的 policy 挂靠（block 分册 `03-blk-mq` 与 `05-IO调度器` 已讲其与 request 路径的集成）。v2 下接口统一挂 `io.*` 前缀：

| 文件族 | 控制器 | 机制 | 注册 policy |
| :--- | :--- | :--- | :--- |
| `io.cost.*` | iocost | **前馈成本模型 + 反馈速率修正** | `blkcg_policy_iocost` |
| `io.latency` | iolatency | **目标延迟反馈**（收缩并发窗口） | `blkcg_policy_iolatency` |
| `io.max` | blk-throttle | 绝对速率上限（token） | `blkcg_policy_throttle` |
| `io.weight` | bfq 权重 | 比例仲裁（仅 bfq 生效） | `blkcg_policy_bfq` |

四把刀解决四类问题：**确定性预算**（io.cost）、**延迟 SLO**（io.latency）、**硬隔离**（io.max）、**相对优先级**（io.weight）。

---

## 二、io.cost（iocost）：把成本模型搬进内核

思想：**把"完成一个 IO 的代价"量化为成本，按组的预算发放**，而不是事后统计（事后统计=反馈滞后=风暴已成型）。

```
成本模型（io.cost.model，可按设备定制）：
  每请求成本 = iocost_params：[scan]（寻道/元数据）+ [size]（每字节线性）
  → 换算为"一秒能发多少 credit"——即基准速率

反馈环（vrate）：
  周期性（timer）评估实际延迟 vs 目标：
    missed_ppm（超时比例）、rq_wait_pct（排队比例）
    → vrate_adj_pct[] 查表得到修正百分比（blk-iocost.c:651 的分档表）
    → vrate（虚拟速率）上调/下调——预算的发放速度随之缩放
  ★第 5 轮复查勘误：6.6 **没有** AUTOMATE（io.cost.qos 无 automate=
  参数，blk-iocost.c 全文零命中——该特性为后续版本合入）；
  6.6 的 ctrl=auto 语义仅是"使用内核静态默认值"（ioc->
  user_qos_params ? "user" : "auto"），不涉及设备能力自动校准
```

接口（io.cost.qos）：`enable=1 rpct=.. wpct=.. rl=.. wl=..`（读/写各占设备的百分比与延迟目标，单位见文档）；`io.cost.model`：`ctrl=auto/user` + 模型参数。**特性级总开关**在 `/sys/fs/cgroup/io.cost.qos` 根上配。

适用：多租户共享 NVMe/SATA 的带宽切分（比 io.max 优雅：不依赖经验数字，vrate 自动找设备的真实上限）。

---

## 二点五、iocost 的参数与运作细节（已对照 blk-iocost.c）

```
io.cost.model（每设备成本模型）：
  [model] 的默认值为**内核静态默认**（DFT_ 常量的线性模型），
  可手工覆盖：ctrl=user（★勘误：原"AUTOMATE 校准"表述不实，
  见上文复查注）
  每请求成本 = scan成本 + size × 每字节成本
  → ioc->vtime 的推进速率基准：1 credit 的"时间价值"由此定

io.cost.qos（服务质量）：
  rpct/wpct   读/写各占设备能力的百分比（预算发放速率）
  rl/wl       读/写延迟目标（反馈环的对齐锚，ns）
  enable      根 cgroup 上开总闸

反馈环（ioc_timer_fn 周期评估，blk-iocost.c:1002 一带）：
  missed_ppm（超时万分比）、rq_wait_pct（在组内排队占比）
    → 查 vrate_adj_pct[] 分档表（:651）得修正百分比
    → ioc->vrate ∈ [ioc->vrate_min, ioc->vrate_max]——两者由 QOS_MIN/
       QOS_MAX ppm 派生（默认 VRATE_MIN_PPM=10000 即 1%、
       VRATE_MAX_PPM=100000000 即 10000%，blk-iocost.c:255-256/
       :943-945；★勘误：原文"DFT_VRATE_MIN=500..10000/100"有误）
    → 组内 credit 的发放速率随 vrate 同步缩放
  效果：设备超载时全组按比例减速，不过载时逐步回升——
       "用 vrate 一个标量吸收模型误差"
```

与 io.max 的本质差：io.max 的"10MB/s"是**拍脑袋的绝对数**（盘闲时浪费、盘忙时仍挤）；iocost 的"rpct=30"是**相对设备真实能力的比例**，且 vrate 修正让模型误差自动收敛。

## 二点六、iolatency 的完成延迟分桶（已对照 blk-iolatency.c）

```
每次完成：lat = 完成 - 提交 → 按线性桶（LAT_BUCKETS=64? 常量表）累计
周期评估（iolatency_timer_fn）：
  计算百分位（io.latency.percentile 读出的即此）——默认看 50th? 可配
  target 未达 → inflight 窗口收紧（组的提交排队）
  达标且余裕 → 缓慢放宽（迟滞，见 scale_lat 单调逻辑 :576-581）
```

`io.latency` 文件写法：`"8:16 target=5"`（ms 单位接口）；`io.latency.percentile` 配观测分位。与 iocost 的分工：**iolatency 管的是"别让我的完成延迟超线"**（防御型），iocost 管的是"把设备能力按比例分掉"（分配型）——两者可同树并存但通常二选一（反馈变量都是延迟，叠加易互扰）。

## 二点七、blk-throttle 的两级执行

`io.max` 在**两层**各自发桶（已核对 blk-throttle.c 的 limit 表）：

```
层1: 组的 tg（per-cgroup）——rbps/wbps/riops/wiops 的 token 桶
层2: 若父组也设限——请求同时受父桶约束（先过父桶再过子桶）
```

与 cpu.max 的"层级折算后单点执行"对比：块设备的串行化点在**提交端**（每设备一个队列），两层桶在提交路径串联执行即可，无需 CPU 那种"全局账本+层级传播"的复杂记账——IO 控制器选了更笨但更可预测的模型。

## 三、io.latency（iolatency）：延迟目标的负反馈

不做速率预算，直接盯**完成延迟**：`io.latency` 写入目标（如 `8:16 target=5`——设备 8:16 上目标 5ms）。

```
iolat（每组每设备）：
  每次 IO 完成 → 按延迟入桶（lat_to_buckets，NR_LAT_BUCKETS? 线性桶）
  周期评估（iolatency_timer_fn）：
    若超目标 → 收紧"允许的并发深度"（inflight 窗口）
    若轻松达标 → 放宽窗口（scale_lat 单调自适应，blk-iolatency.c 已核对：
      scale_lat 只降不升——收紧快、放宽慢的迟滞设计，防震荡）
  IO 提交时：inflight 超窗口 → 在组内排队等完成事件放行
```

`scale_lat` 的自适应还有一个全局下探语义（源码 576-581 行：组内观察到的最小延迟下修 scale_lat——窗口跟着设备的真实最快响应走）。适用：为"共享盘上的交互服务"保延迟（数据库 WAL 盘混合备份流量）。

---

## 四、io.max（blk-throttle）：绝对上限

四旋钮（已核对接口）：`rbps/wbps/riops/wiops`（读/写的字节与 IOPS 上限），`io.max` 写法 `8:16 rbps=10M wbps=max riops=100`。

机制：token 桶——每次分配预算按时间片补充；超速的请求在 `tg->protection`（per-cgroup 队列）排队，到点放行。细节两点：

* 层级语义：throttle 在**叶子层**逐组执行（子组各自桶），父组的限额独立再管一遍（与 scheduler 的层级折算不同——io.max 是"两层各自都掐"）。
* `io.max` 对**同步直读**（如 O_DIRECT 数据库）会产生用户可见的停顿——它管"提交速率"而非"完成速率"，所以 io.latency 常是延迟敏感场景更好的选择。

---

## 五、io.weight：bfq 专享

`io.weight`（1~10000，默认 100）只在设备用 **bfq 调度器**时生效（bfq-cgroup 的 `bfq_group_sched` 注册；`BFQ_WEIGHT_CONVERSION` 把 cgroup weight 折算为 bfq 内部权重）。6.6 现实约束：NVMe（默认 none/mq-deadline）上 io.weight **无效**——机械盘 + bfq 的桌面/单盘场景才是它的主场。多队列时代的权重仲裁事实上由 io.cost 接棒。

---

## 六、cpuset：CPU/内存的拓扑治理

v2 文件族：

| 文件 | 语义 |
| :--- | :--- |
| `cpuset.cpus` / `cpuset.mems` | 请求的 CPU/NUMA 节点集合 |
| `cpuset.effective_cpus` / `effective_mems` | **实际生效**集合（与父约束的交集结果） |
| `cpuset.cpus.exclusive` | 声明独占集合（partition 的前置） |
| `cpuset.cpus.partition` | 分区状态（下文四态） |
| `cpuset.mems.empty`? | 内存无交集事件（cgroup.events 补充） |

**partition 四态**（kernel/cgroup/cpuset.c:193-199，已核对 6.6 注释）：

```
member    普通组（默认）——参与父域的负载均衡
root      分区根：子树独立成一域，exclusive cpus 从父域摘除，
          但域内仍做调度域/负载均衡
isolated  ★6.6：更激进的分区根——连负载均衡也不做（SD 构建时跳过），
          CPU 完全按 affinity 自治（实时/DPDK 场景的"驱离一切内核打扰"）
invalid   结构失效（exclusive 集合与父约束冲突等）——修复后回落 root
```

与 scheduler 分册第 6 章的衔接：`update_partition_sd_lb`（新 lb 布尔即 `prs != PRS_ISOLATED`，cpuset.c:1311 已核对）触发 `partition_sched_domains_locked` 重建调度域——**root/isolated 的本质差别就是"要不要在这批 CPU 上建 sched_domain"**。isolated 域上没有域→没有周期均衡→没有唤醒扫描→tick 也可 nohz_full（配合 `nohz_full=` 参数形成完整隔离栈）。

---

## 六点五、cpuset 的生效语义、热插拔与完整隔离栈

```
请求 vs 生效：
  cpuset.cpus（请求）∩ 父的 effective_cpus → cpuset.effective_cpus（生效）
  无交集 → cgroup.events 报 cpus_empty=1（组的任务被"软搁置"：
           无 CPU 可跑的组只能等父级修复）
热插拔联动：
  CPU 下线 → update_tasks_cpumask 沿子树重算；exclusive 集合里少了 CPU
  → 分区降级 invalid（PRS_INVALID_ROOT）并打日志——守护脚本应监听
  /sys/fs/cgroup/*/cpuset.cpus.partition 的值变化
```

**isolated 的完整隔离栈**（与既有分册的拼装）：

```
cpuset.cpus.partition=isolated   调度域不建（本节）——无周期均衡/唤醒扫描
+ nohz_full=<cpus>               tick 关停（timers 分册第 6 章）
+ irqaffinity=                   中断驱离（interrupt 分册第 6 章）
+ rcu_nocbs=                     RCU 回调迁出（RCU 分册）
+ isolcpus=nohz,domain           （传统参数位，等价物的历史形态）
= 一颗"只跑我 affinity 圈定任务"的核——DPDK/高频交易的标配底座
```

## 七、实战

```
# 四控制器的典型配置姿势
echo "+io" > /sys/fs/cgroup/parent/cgroup.subtree_control
echo '8:16 enable=1 rpct=30 wpct=10' > /sys/fs/cgroup/parent/db/io.cost.qos
echo '8:16 target=5'                 > /sys/fs/cgroup/parent/app/io.latency
echo '8:16 wbps=50M'                 > /sys/fs/cgroup/parent/batch/io.max

# cpuset 实时分区
echo 4-7  > /sys/fs/cgroup/rt/cpuset.cpus.exclusive   # 先声明独占
echo root > /sys/fs/cgroup/rt/cpuset.cpus.partition    # 再切状态
# （isolated 同款；生效检查 effective 与 dmesg 的 sched_domain 变更日志）

# 观测
io.stat（每设备 ios/bytes 计数）、io.latency 的 lat_percentile
iostat -x 对照四控制器的限速是否被命中
```

---

## 八、思考题

1. iocost 用"前馈成本 + 轻量反馈（vrate）"，iolatency 用"纯反馈（窗口）"。分别给出一种对方机制处理不好的负载形态。（提示：重顺序写 vs 突发小随机读。）
2. `scale_lat` 为什么"只降不升"？如果对称升降，观测到的震荡形态是什么？（对照迟滞控制的一般原理。）
3. io.max 在叶子与父组双层各自执行——与 cpu.max 的"层级折算后单点执行"对比，为什么 io 选择了双层？（提示：块设备队列的串行化点 vs CPU 的全局记账。）
4. isolated 分区上如果还跑着内核线程（kworker/RCU），谁负责把它们请走？（对照 scheduler 分册的 per-CPU kthread 约束与 nohz_full 的 housekeeping。）
5. 设计：一台 8 核 + 单 NVMe 的机器，三个租户（交互 API：P99 IO 5ms；数仓批写：越快越好；备份：夜间窗口）——给出 io.* 与 cpuset 的组合配置与每项理由。

---

> 本分册 4 章至此完整。回看：[01 v2 层级与线程模式](01-v2层级与线程模式.md)｜[02 cpu 控制器](02-cpu控制器.md)｜[03 memory 控制器](03-memory控制器.md)｜[04 io 控制器与 cpuset](04-io控制器与cpuset.md)
