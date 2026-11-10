# sched_ext 前瞻：BPF 可插拔调度器（基准 v6.12，6.6 基线外）

> ⚠️ 基线声明：本篇是**唯一的基线外前瞻章**——事实基准为 **v6.12**（sched_ext 合入版本；6.6 树中无任何 sched_ext 代码）。所有结构/常量/路径均对照 v6.12 标签核对，与 [03-CFS与EEVDF详解](03-CFS与EEVDF详解.md)（6.6）形成"固定策略 vs 可插拔"的对照。

---

## 一、动机：调度器为什么需要"可插拔"

```
EEVDF（6.6）再精细，也是单一策略：交互、批处理、游戏、LLM 推理
的负载形态差异巨大，任何内核内置折中都只能"整体次优"
历史包袱：调度器改动必须走主线长周期（EEVDF 理论 1995 → 合入
  2023，28 年）；实时性需求的策略实验根本等不起
sched_ext（SCX）：把 sched_class 的核心决策点暴露为 BPF
  struct_ops——用户态可加载自定义调度器，**失速自动回退 CFS**
（安全网使实验可上生产）
```

## 二、核心：struct sched_ext_ops（kernel/sched/ext.c:205，v6.12 已核对）

```
约 30 个回调，按职责分组（全部 v6.12 逐项核对）：
  任务生命周期：init_task/exit_task、enable/disable
  唤醒与入队：select_cpu（s32 返回目标 CPU——注释原句"Decision
    made here isn't final. @p may be moved to any CPU while it is
    getting dispatched"）、enqueue/dequeue
  派发主循环：dispatch(cpu, prev)——每 CPU 无任务可跑时调用，
    BPF 侧用 scx_bpf_dispatch() 把任务投往 DSQ
  执行通知：runnable/running/stopping/quiescent、tick
  让位与序：yield(from, to)、core_sched_before（核心调度序）
  属性：set_weight/set_cpumask
  空闲与 CPU 管理：update_idle、cpu_acquire/cpu_release、
    cpu_online/cpu_offline
  cgroup 族：cgroup_init/exit/prep_move/move/cancel_move/set_weight
  诊断：dump/dump_cpu/dump_task
注册：BPF 程序以 struct_ops 形式挂载（bpf_sched_ext）——
  验证器保证回调安全（不可睡眠等约束）
```

## 三、DSQ：调度队列的乐高（include/linux/sched/ext.h，已核对）

```
三类队列：
  内建全局：SCX_DSQ_GLOBAL（= FLAG_BUILTIN|1）
  内建本地：每 CPU 一个 local DSQ（consume 的终点）
  用户自建：scx_bpf_create_dsq(id, node)/destroy_dsq
协议：dispatch 回调里 scx_bpf_dispatch(p, dsq, slice, enq_flags)
  （可带 vtime 版本）；CPU 空闲时 scx_bpf_consume(dsq) 拉取到本地
  —— scx_bpf_pick_idle_cpu 辅助选核
对照 EEVDF：全局一棵增广红黑树 vs SCX 的"多队列 + 派发协议"
（复杂度从算法移到了策略设计自由度上）
```

## 四、安全网：watchdog 与回退（已核对）

```
SCX_WATCHDOG_MAX_TIMEOUT = 30*HZ（ext.h:14）：
  任务 runnable 超过 last_runnable+timeout 未跑（:3324 判定）→
  调度器失速 → bypass 模式：内核接管所有决策，退回 CFS/EEVDF
  路径（poll 间隔 = timeout/2，:3353）
旗标（ext.c:96-123 已核对）：
  SCX_OPS_KEEP_BUILTIN_IDLE=1<<0  内建 idle 选择兜底
  SCX_OPS_ENQ_LAST=1<<1 / ENQ_EXITING=1<<2
  SCX_OPS_SWITCH_PARTIAL=1<<3  ★部分任务留 CFS（默认）vs
    switch_all 全量接管（sysfs 可切）
```

## 五、用户态生态（v6.12 tools/sched_ext/，已核对）

```
内核树内四个 C 示例 + 工具：
  scx_simple——全局 FIFO 最小实现
  scx_qmap——按 priority 分五级 DSQ
  scx_central——单 CPU 集中派发（时序确定性实验）
  scx_flatcg——层级 cgroup 扁平化
  scx_show_state.py——状态速览
★如实注记：scx_rusty/scx_lavd 等生产级 Rust 调度器在
  外部 sched-ext 仓库（chromiumos/工具链生态），非内核树
```

## 六、与 EEVDF 对照（跨版本表）

| | EEVDF（6.6 基线） | sched_ext（v6.12+） |
| :--- | :--- | :--- |
| 策略位置 | 内核编译期固定 | BPF 运行时可换 |
| 决策核心 | lag/deadline 单一算法 | ops 表自由组合 DSQ 协议 |
| 安全性 | 主线审查即保证 | watchdog 30s + bypass 回退 |
| 部署 | 无需用户态 | 需守护进程持续加载 |
| 适合 | 通用服务器稳态 | 特化负载实验/产品（Android/ChromeOS 生产采用） |

## 七、观测（v6.12 sysfs，已核对）

```
/sys/kernel/sched_ext/（scx_root_kobj，ext.c:1002）：
  state（调度器名/错误态）、switch_all（接管范围）等属性
  scx_show_state.py 一键解读
```

## 八、思考题

1. select_cpu 的"Decision made here isn't final"与 EEVDF 的唤醒抢占如何对应？（提示：两级决策 vs 单点决策。）
2. watchdog 30 秒对交互负载意味着什么？为什么阈值不能小到毫秒级？（对照误判代价：正常运行被误 bypass。）
3. SWITCH_PARTIAL 默认而非全量——哪些任务必须留在 CFS？（提示：SCX 调度器自身的守护进程失速时谁来调度它。）
4. scx_central 的"单 CPU 派发"牺牲了什么换来了什么？（对照调度域分层的意义——[06-调度域与负载均衡](06-调度域与负载均衡.md)。）
5. 若把 EEVDF 实现为 SCX 调度器（scx 平台已有类似实验），BPF 边界会损失哪些能力？（提示：per-CPU 锁语义/时钟精度。）

---

> 回看：[03-CFS与EEVDF详解](03-CFS与EEVDF详解.md)（6.6 基线）｜[00-一页速查](00-一页速查.md)｜官方镜像：../scheduler/sched-ext.rst（若已同步）
