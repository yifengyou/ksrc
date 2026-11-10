# PSI：压力失速信息（Linux 6.6）

> 阅读目标：掌握 PSI（Pressure Stall Information，4.20，Johannes Weiner）的完整机制——七态失速模型（SOME/FULL 的语义分野）、per-CPU 任务状态记账（TSK_* 标志与 PSI_ONCPU 特殊位）、2 秒窗口的三档指数聚合、**memstall 双态**（含"在 CPU 上等内存"的 MEMSTALL_RUNNING）、rtpoll 触发器与 /proc/pressure 接口。这是 cgroup 分册 `{cpu,memory,io}.pressure` 与 memcg 回收观测的底层答案。前置：[02-核心数据结构](02-核心数据结构.md)、[cgroup/05-memcg 回收](../../cgroup/docs/05-memcg回收深挖.md)。

---

## 一、问题：负载均值与 free -m 都回答不了的事

```
"CPU 使用率 100%"——是全速推进还是有人干等？
PSI 的答案：量化"因缺资源而浪费的时间"：
  SOME：至少一个任务在等（部分失速——延迟问题）
  FULL：所有任务都在等（完全失速——吞吐/容量问题）
例：io.full=30% = 每 100 秒里有 30 秒全组无人推进（盘打满）
```

## 二、七态模型（include/linux/psi_types.h:57-69，已核对 6.6）

```c
enum psi_states {
	PSI_IO_SOME, PSI_IO_FULL,
	PSI_MEM_SOME, PSI_MEM_FULL,
	PSI_CPU_SOME, PSI_CPU_FULL,        /* cpu 无 FULL 语义争议？
	                                     ——6.6 有 CPU_FULL（后述）*/
#ifdef CONFIG_IRQ_TIME_ACCOUNTING
	PSI_IRQ_FULL,                      /* ★6.6 新增：IRQ 挤占的 FULL */
#endif
	PSI_NONIDLE,                       /* 仅 per-CPU：CPU 非空闲占比权重 */
	NR_PSI_STATES,
};
#define PSI_ONCPU  (1 << NR_PSI_STATES)  /* state_mask 里的特殊位 */
```

## 三、记账：per-CPU 的任务状态（psi.c，已核对）

```
任务侧五标志：TSK_IOWAIT / TSK_MEMSTALL / TSK_RUNNING /
  TSK_MEMSTALL_RUNNING / TSK_ONCPU
记账热路径 psi_task_change/psi_task_switch（无锁 per-CPU 累加）：
  状态迁移 = tasks[] 计数迁移 + 时间累积（PSI_ONCPU 位在
  state_mask 而非 tasks[]——"on CPU"是 CPU 视角而非任务排队位）
★memstall 双态的精妙：psi_memstall_enter/leave 标记
  current->in_memstall——memcg 回收里"在 CPU 上跑着等内存"
  （shrink 持锁自旋等页）记 TSK_MEMSTALL_RUNNING：算 MEM_FULL
  但不算 CPU 空闲（:865-868 原句——"a task in a cgroup is
  in_memstall, the corresponding groupc…"）
```

## 四、聚合：2 秒窗的三档指数（psi.c:157-160，已核对）

```
#define PSI_FREQ  (2*HZ+1)     /* 2 秒聚合间隔 */
#define EXP_10s   1677         /* 1/exp(2s/10s) 定点 */
#define EXP_60s   1981
#define EXP_300s  2034
avg[10s/60s/300s] = calc_load_n(...) 指数衰减（与 loadavg 同款数学
  ——scheduler 分册 01 章的 CALC_LOAD 亲戚）
psi_group：parent（层级聚合——cgroup 的 pressure 即子组加权）、
  per-CPU pcpu（psi_group_cpu）、avg_total[NR_PSI_STATES-1]
```

## 五、触发器：rtpoll（6.6 已从"poll"改名）

```
写 /proc/pressure/<res> 或 cgroup 的 <res>.pressure：
  "some 150000 1000000"（阈值 150ms 窗口 1s，≤1s 粒度）
→ psi_trigger_create → rtpoll_triggers 链 + rtpoll_min_period
  （组内最短周期驱动 rtpoll_next_update 排程）
触发后 poll() 可读（监控代理的根因告警：不用再扫 avg 曲线）
```

## 六、接口与观测

```
全局：/proc/pressure/{cpu,memory,io}（"some avg10=X avg60=X
  avg300=X total=Y"格式）
cgroup v2：cpu.pressure / memory.pressure / io.pressure（同格式，
  层级加权）——cgroup 速查表与本分册 00-一页速查的引用出处
开关：CONFIG_PSI；boot 参数 psi=0/1（psi_disabled 静态键——
  关闭时记账路径零开销）；CONFIG_PSI_DEFAULT_DISABLED
排障速用：
  memory.full 高 → memcg high 罚睡或全局回收（[cgroup/05]）
  io.full 高 → 设备打满（[block 分册] 的 io.stat 对查）
  cpu.some 高而 full≈0 → 调度延迟而非容量（EEVDF 优先级问题）
```

## 七、思考题

1. SOME vs FULL 的监控语义差：延迟 SLO 盯哪个、容量规划盯哪个？（提示：交互体验∝SOME、吞吐上限∝FULL。）
2. MEMSTALL_RUNNING 为什么要从 CPU_FULL 里"借走"时间？（对照 ：865 注释——回收占着 CPU 干等，这算 CPU 忙还是 CPU 闲？）
3. PSI_IRQ_FULL 的账怎么记？（提示：hardirq 计账里"挤占"的语义。）
4. 层级聚合下子组 pressure 之和可以超过父组吗？（avg_total 的权重合并。）
5. rtpoll 触发器比用户态轮询 avg 省在哪？（内核侧窗口精确判定 vs 采样误差。）

---

> 回看：[02-核心数据结构](02-核心数据结构.md)｜[cgroup/05-memcg 回收深挖](../../cgroup/docs/05-memcg回收深挖.md)｜[cgroup/00-一页速查](../../cgroup/docs/00-一页速查.md)｜[block 分册](../../block/README.md)
