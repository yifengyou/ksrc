# 实时与 Deadline 调度（Linux 6.6）

> 阅读目标：掌握 rt_sched_class 的选队/抢占/push-pull/带宽节制全景，理解 `SCHED_FIFO/RR` 的强抢占语义及其陷阱；吃透 dl_sched_class 的 CBS（Constant Bandwidth Server）补给、EDF 排序、准入控制与 GRUB 带宽回收；建立"实时确定性 = 调度器 + 抢占模型 + 内存 + 锁 + 中断"的系统观。

---

## 一、RT 的世界观：优先级即真理

fair 类的世界里"人人有权"，RT 类的世界里**优先级即真理**：只要高优先级 RT 任务可运行，一切（fair 任务、同优先级以下 RT、甚至内核的大部分活动）都让路。

```
用户接口：sched_setscheduler(p, SCHED_FIFO/RR, { .sched_priority = 1..99 })
          rt_priority: 99 最高，1 最低（0 等价于让出 RT 身份）
内部映射：prio = MAX_RT_PRIO - 1 - rt_priority = 100 - 1 - rt_priority
          即 rt_priority 99 → prio 0，rt_priority 1 → prio 98
          （prio 越小越优先——与用户语义恰好反向，第 2 章）
```

### 1.1 数据结构回顾（第 2 章）：O(1) 选队

```c
struct rt_prio_array {
	DECLARE_BITMAP(bitmap, MAX_RT_PRIO+1);   /* 优先级位图（含哨兵位）*/
	struct list_head queue[MAX_RT_PRIO];     /* 每级一个 FIFO 链表 */
};
```

`pick_next_task_rt()`：`sched_find_first_bit(bitmap)` 定位最高优先级 → 链头出队。enqueue/dequeue 同步维护 `rt_rq->highest_prio.curr/next`（为 SMP 迁移决策缓存的最值）。

RR 与 FIFO 的唯一区别：RR 在 `task_tick_rt` 里扣 `time_slice`（`sched_rr_timeslice`，默认 100ms，`/proc/sys/kernel/sched_rr_timeslice_ms`），到期把同优先级链表尾插（`requeue_rt_entity`）；FIFO 无限运行。

### 1.2 抢占判定：一句话逻辑

```c
static void check_preempt_curr_rt(struct rq *rq, struct task_struct *p, int flags)
{
	if (p->prio < rq->curr->prio) {          /* 新来的优先级更高 → 立即抢 */
		resched_curr(rq);
		return;
	}
#ifdef CONFIG_SMP
	/* 同优先级微调：新任务不可迁移而 curr 可迁移 → 也抢，
	   给 push_rt_tasks 创造把 curr 推走的机会（防两个同优先级互相死等）*/
	if (p->prio == rq->curr->prio && !test_tsk_need_resched(rq->curr))
		check_preempt_equal_prio(rq, p);
#endif
}
```

DL 的对应逻辑（deadline.c）：`dl_entity_preempt(&p->dl, &rq->curr->dl)`——**绝对 deadline 更小者赢**（EDF 判定），平局走 `check_preempt_equal_dl`。

---

## 二、SMP 的 RT 调度：push 与 pull

RT 的强优先级在多核上制造一个经典问题：高优先级任务在 CPU0 的队列里睡觉，CPU1 却跑着低优先级 RT。需要**主动搬运**（RT 任务不像 fair 有 load_avg，均衡器帮不上忙）：

### 2.1 push：把本队列多余的推出去

```
enqueue_task_rt / switched_to_rt / …
  └─ queue_balance_callback(rq, &per_cpu(rt_push_head, cpu), push_rt_tasks)
       （延迟回调：rq 锁内挂账，出锁后 __balance_callbacks 执行，第 4 章）
push_rt_tasks(rq)
  while ((next = pick_highest_pushable_task(rq)))   /* pushable_tasks plist 首个 */
      if ((cpu = find_lowest_rq(next)) >= 0 && cpu != rq->cpu)
          __push_rt_task(rq, next, cpu)  /* stop_one_cpu? no—直迁移 + IPI resched */
      else break
```

`pushable_tasks`（plist，按优先级）：**正在本 CPU 运行的任务不进表**，表里是"本队列可让出的次优任务"。`find_lowest_rq` 的评分：优先挑 (a) 亲和允许 (b) 优先级更低（rt_rq->highest_prio.curr > next->prio）的 **最闲 CPU**（idle > 有低优先级 RT > 有 fair）。

### 2.2 pull：把别处的高优先级拉回来

```
CPU 即将跑非 RT 任务（dequeue 后）
  └─ balance_rt()（fair balance 的 RT 版，在 pick 路径）
       └─ pull_rt_task(rq)
            for_each_cpu(cpu, rto_mask)          /* root_domain 的 RT 过载位图 */
                src_rq = cpu_rq(cpu)
                /* 双锁协议 + barrier（need_resched / pushable 状态可见性）*/
                if (p = pick_highest_pushable_task(src_rq)) {
                    若 p 优先级高于本队列 → detach + migrate
                }
```

`rto_mask`（root_domain）：某 CPU 的 rt_rq 有多于一个可运行 RT 任务（`overloaded`）时置位——pull 只扫"确实有货"的 CPU。

### 2.3 IPI push：让拥有者自己推（RT_PUSH_IPI 特性）

pull 的默认实现是**所有 idle-ish CPU 逐个抢 src_rq 锁**——N 个 CPU 抢一个队列的"惊群"。`RT_PUSH_IPI`（x86/ARM 默认开）反转方向：pull 失败方不再自旋抢锁，而是给拥有者 CPU 发一个 IPI（`rto_push_work` irq_work），让对方在自己的上下文里做 push 决策。锁竞争从 N:1 变成 1:1。

### 2.4 唤醒放置：select_task_rq_rt

RT 的唤醒不找"最闲"的核，而是：**(a) 唤醒者所在核（若亲和允许且优先级够抢）→ (b) 任务最近用过的核 → (c) 找优先级最低的核**。逻辑自洽：既然 RT 是抢占语义，"哪里有人可抢"就比"哪里队列短"更重要。

---

## 三、RT 带宽节制（throttling）：防止 RT 饿死世界

一个 SCHED_FIFO 99 的死循环任务可以让整个系统的看门狗、RCU、kworker 全部停摆——内核的底线防御是 RT bandwidth（`/proc/sys/kernel/sched_rt_runtime_us`，默认 **950000**，period 1000000，即 RT 群体最多占 95% CPU）：

```
period 计时器 sched_rt_period_timer（hrtimer，周期 sched_rt_period_us=1s）
  └─ do_sched_rt_period_timer(runtime, period)
       对每个组/每 CPU：
         if (rt_rq 的 rt_time 超过 rt_runtime 配额)
             rt_rq->rt_throttled = 1 → sched_rt_rq_dequeue（整组下架）
         else 补足配额、解除 throttle
account: update_curr_rt() 每次 tick/切换记账 rt_time
```

细节：

* throttle 的是 **rt_rq（可能是组）**，不是任务；`RT_RUNTIME_SHARE` 特性（6.6 默认关）允许繁忙 CPU 借空闲 CPU 的剩余配额——关掉的原因是配额借用导致周期末的不可预测抖动。
* `-1` 表示不限制（危险！自担 RT 任务失控系统冻结风险）。
* group 场景每个 task_group 有独立配额（cgroup v1 的 rt runtime 接口；v2 未提供 RT 配额接口）。

**RT 任务的确定性敌人清单**（不是 sched 类能解决的）：中断关闭区（irqsoff）、不可抢占内核段（PREEMPT 模型）、rt_mutex 反转链、页错误、内存分配回收、cache/TLB 冷启动、SMT 兄弟挤占（6.6 的 `group_smt_balance` 只帮 fair）。真正的硬实时业界实践是 `PREEMPT_RT` + CPU 隔离（isolcpus/cpuset）+ irq affinity + `memlock` + mlock + …。

---

## 四、DL：把时间预算变成合同

`SCHED_DEADLINE`（3.14，Dario Faggioli/Juri Lelli）用 CBS 算法提供**可证明的**（admission 之内）服务保证。用户合同：

```c
struct sched_attr {
	__u32 size, sched_policy;
	__u64 sched_runtime;    /* 每个 job 最多需要多少 CPU 时间 */
	__u64 sched_deadline;   /* 相对截止期（job 完成前）*/
	__u64 sched_period;     /* 周期性任务的实例间隔（可等于 deadline）*/
	...
};
/* sched_setattr(p, &{SCHED_DEADLINE, runtime=10ms, deadline=100ms, period=100ms})
   ⇒ 承诺：每 100ms 保你 10ms，且 10ms 在 100ms 内可用完 */
```

### 4.1 CBS：动态优先级的带宽服务器

每个任务被建模为"服务器"：账本只有两个数——剩余 `runtime` 与绝对 `deadline`。

```
update_curr_dl（记账，类比 update_curr）
  runtime -= delta_exec            （此时若 rq 频率低于 max，
                                     delta 还要按比例放大：慢核上同样指令花更多预算）
  if (runtime <= 0)
      throttle_dl_task → 下架 + start_dl_timer（dl_timer 到期 = 下个周期开始）

dl_task_timer（hrtimer 回调，任务私有时钟）
  └─ replenish_dl_entity：
       deadline += k*dl_period（跳过已错过的周期，"放弃迟到的执行"）
       runtime = dl_runtime（预算重置）
       入队 + push 判断 + resched
```

两个精妙设计：

* **`update_dl_entity`（入队/唤醒时的补给规则）**：任务睡眠后早醒/晚醒，deadline 与 runtime 按 CBS 规则重整（`dl_overflow` 检查），保证"零星唤醒不会把预算攒起来换插队"。
* **负 runtime 允许**：瞬时超支（抢占/频率错觉）先记账继续跑，由下个周期惩罚——避免 hard cut 引起抖动。

### 4.2 EDF 排序与抢占

`dl_rq->root`（rbtree 按绝对 deadline）的最左节点即下一个；`dl_entity_preempt(a,b)` 判定 "a 的 deadline 早于 b 且 a 处于可运行状态"。唤醒的 DL 任务 deadline 更早 → `resched_curr` 立刻抢——**DL 抢 DL 也毫不含糊**（与 RT 只认优先级不同，DL 的"优先级"随时间流动）。

### 4.3 准入控制（admission）

系统只承诺付得起的合同：

```
sched_setscheduler → __setscheduler_parameters → …
  __sched_setscheduler 的 DL 路径：
    dl_bw = to_ratio(runtime, period)        /* 每任务带宽分数（1024 定点）*/
    __dl_add(dl_bv) 前检查：
      root_domain->dl_bw（总带宽池）+ 新任务 ≤ 1024 × (cpuset 关联的 CPU 数)
      /* 每核可卖 100%；全网池按 cpuset span 分账 */
    超了 → 拒绝（EBUSY 或 EPERM 视 flags.SCHED_FLAG_SCAPE... 6.6: -1 表示 CLAMP？）
```

观测：`/sys/kernel/debug/sched/deadline*/`（root domain 的 `dl_bw`）、`/proc/sched_debug` 的 `dl_rq` 段（`running_bw`/`this_bw`）。

### 4.4 GRUB：把没花完的预算借出去

CBS 的浪费：某任务 10ms 预算只用了 3ms 就睡——剩下 7ms 死锁在账本里。GRUB（Greedy Reclamation of Unused Bandwidth，`SCHED_FLAG_RECLAIM`）：

```
running_bw = "醒着"的 DL 任务带宽和（睡着的记 this_bw）
inactive timer（每任务第二个 hrtimer）在 0-lag 时刻把带宽从 running 挪出
reclaim 任务可用额度：runtime 上限按 extra_bw/running_bw 比例放宽
```

效果：DL 集群里"弹性任务"（可 reclaim）自动填满空闲，而**硬合同任务的保证不受损**（GRUB 数学上保证不侵占非弹性任务带宽）。

### 4.5 DL 与 SMP：push + cpudl

与 RT push/pull 同构，但决策量换成 deadline：

```
pushable_dl_tasks_root（rbtree，最早 deadline 在最左）
push_dl_task → cpudl（root_domain 的"空闲 CPU + 其最早 deadline"堆）
   选择：目标 CPU 空闲，或其最早 deadline 比 candidate 晚
pull 对应 dlo_mask（本 CPU 有 ≥2 个可运行 DL）
```

---

## 五、三类调度类的横向对比（总结）

| 维度 | fair (EEVDF) | rt (FIFO/RR) | dl (EDF+CBS) |
| :--- | :--- | :--- | :--- |
| 排序依据 | vruntime / vdeadline | 固定优先级 | 绝对 deadline（流动） |
| 公平/保证 | 比例公平（lag 守恒） | 无（优先级即真理） | 带宽合同（CBS 可证） |
| 抢占 | slice 到期/更早 vdeadline | 更高 prio（EDF：更早 deadline） | 更早 deadline |
| 饥饿防护 | 模型内生 | 无（靠 throttling 兜底） | 无（准入控制兜底） |
| SMP 迁移 | PELT + sched_domain 均衡 | push/pull（plist/rto_mask） | push/pull（EDF 树/dlo_mask） |
| 带宽控制 | cpu.max（cgroup，throttle） | sched_rt_runtime_us | 准入 + dl_timer 补给 |
| cpufreq 输入 | util_avg/util_est/uclamp | util_rt 记账（avg_rt） | running_bw（拉满优先） |
| 典型用户 | 一切 | 内核线程/音频/电信 | 工业/媒体管线 |

---

## 六、实战与观测

```
设置：chrt -f -p 80 <pid>            # FIFO 80
      chrt -d --sched-runtime 10000000 --sched-deadline 30000000 \
              --sched-period 30000000 -p 0 <pid>    # DL 合同 10/30ms

监控：/proc/sched_debug  → 每核 rt_rq/dl_rq 段（nr_running、throttled、
                            running_bw、earliest_dl.curr/next）
      tracepoint:sched:sched_stat_runtime / sched_switch 的 next prio
      /proc/sys/kernel/sched_rt_runtime_us、sched_rr_timeslice_ms
      rt-throttling 事件：/sys/kernel/tracing/events/sched/…（6.6 无专门
      tracepoint，可打 sched_pi_setprio + ftrace function 定位）

延迟体检（RT 上线前必做）：
      cyclictest -m -Sp98 -i 1000 -h 800    # 最大延迟应 << deadline
      结合 preemptoff/irqsoff/wakeup_rt tracer 找最坏路径
```

> ⚠️ 一个经典陷阱：`SCHED_FIFO` 任务在**频率未锁定**的 CPU 上，schedutil 会因 util_rt 拉频，但 DVFS 爬坡有 µs~ms 延迟；闭环控制类任务要么锁频，要么把频率延迟计入 deadline 预算。

---

## 七、思考题

1. RT throttling 的默认 95%/1s 意味着：极端情况下系统仍有 5% 的"呼吸窗口"。这 5% 为什么必需？把 `sched_rt_runtime_us` 设为 1000000（100%）的机器，最坏会发生什么序列？（提示：migration 线程、RCU、kcompactd、watchdog。）
2. `check_preempt_equal_prio`：同优先级 + 新任务不可迁移时让 curr 也被"抢"。用 push/pull 的语言描述这一步之后两边的队列各发生了什么。
3. DL 任务为什么在慢频率 CPU 上的记账会被放大（`scale_exec_delta`）？如果内核不做这个缩放，"每周期 10ms"的合同在 DVFS 系统上会怎样失效？
4. GRUB 与 uclamp_max 的"压 util"策略在动机上有何本质区别？（保合同 vs 省电/限热。）
5. 设计题：为"500µs 周期的运动控制环 + 日志 flush + web 服务"的三类负载设计 policy/cpuset/中断亲和布局，并指出每类最坏延迟的守门人分别是调度器里的哪个机制。

---

> 进阶阅读（本仓库）：`scheduler/sched-deadline.rst`（DL 官方文档，含 admission 公式）、`scheduler/sched-rt-group.rst`、`scheduler/sched-bwc.rst`（fair 带宽）、以及调度器分册其余各章。
