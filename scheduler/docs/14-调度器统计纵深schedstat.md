# 调度器统计纵深：schedstat 的 P 计数族（Linux 6.6）

> 阅读目标：分清内核调度统计的**两套账目**——`CONFIG_SCHED_INFO` 的轻量时间戳账（run_delay/pcount，喂 `/proc/<pid>/schedstat` 与 KVM steal time）与 `CONFIG_SCHEDSTATS` 的全量计数族（static key 运行时门控，喂 `/proc/schedstat` 的 cpu/domain 行、五个 sched_stat_* tracepoint、latencytop）；逐字段解剖 `/proc/schedstat` 的 cpu 行（含★第 3 槽字面量 0 的跨版本考古：v2.6.32 实为 `rq->sched_switch`）与 domain 行 36 字段；吃透 `schedstat_inc` 宏族的 static-key 提升模式。全部对照 v6.6（kernel/sched/stats.c、stats.h、core.c、include/linux/sched.h、include/linux/sched/topology.h）。前置：[00-一页速查](00-一页速查.md)（三元组速查用法）｜[13-调度器 debugfs 全景](13-调度器debugfs全景.md)（sched/debug 侧）。

---

## 一、两套账目：一个内核，两本调度流水

```
                    CONFIG_SCHED_INFO                CONFIG_SCHEDSTATS
                    （轻量·编译期常开）              （全量·运行时可开关）
─────────────────────────────────────────────────────────────────────
载体      task_struct->sched_info           task_struct->stats
          （struct sched_info，4 字段）      （struct sched_statistics，30+ 字段）
方法      时间戳差分：                        事件计数 + 累计：
          last_queued/last_arrival           wait/sleep/block/iowait/exec/migration
          → run_delay/pcount                 /wakeup 全家族计数
开关      sched_info_on() = 编译期           static key sched_schedstats
          IS_ENABLED(CONFIG_SCHED_INFO)      （core.c:4606，运行时翻）
消费者    /proc/<pid>/schedstat 三元组        /proc/schedstat 的 cpu/domain 行
          （2/3 字段）、rq->rq_sched_info、   五个 sched_stat_* tracepoint、
          ★KVM steal time（x86.c:3858 等）   latencytop、debugfs sched/debug
依赖关系  ──────────────────────────────►   Kconfig: select SCHED_INFO
                                             （lib/Kconfig.debug:1245）
```

这是本章的主线论点：**"schedstat" 一词横跨两套账目**——`/proc/<pid>/schedstat` 主要吃 SCHED_INFO 的账（外加 sched_entity 的 `sum_exec_runtime`），`/proc/schedstat` 吃 SCHEDSTATS 的账。混淆两者是排障时最常见的概念错误。

## 二、/proc/schedstat 全格式解剖（stats.c:118-175）

### 2.1 文件头与注册

```
version 15        ← SCHEDSTAT_VERSION（stats.c:116，注释原话："Bump this up
                     when changing the output format or the meaning of an
                     existing format, so that tools can adapt (or abort)"）
timestamp <jiffies>
cpu0 ...
domain0 <cpumask> ...
```

注册：`proc_create_seq("schedstat", 0, NULL, &schedstat_sops)` + `subsys_initcall`（:226-231）——挂在 proc 根（非 /proc/sys），seq_file 三段式 start/next/show。

### 2.2 cpu 行：10 个槽位（:135-142）

```
格式串（逐字）：
  "cpu%d %u 0 %u %u %u %u %llu %llu %lu"
        │  │  │  │  │  │    │     │    └ rq->rq_sched_info.pcount
        │  │  │  │  │  │    │     └────── rq->rq_sched_info.run_delay
        │  │  │  │  │  │    └──────────── rq->rq_cpu_time（ns）
        │  │  │  │  │  └───────────────── rq->ttwu_local（本地唤醒）
        │  │  │  │  └──────────────────── rq->ttwu_count（try_to_wake_up 调用数）
        │  │  │  └─────────────────────── rq->sched_goidle（切到 idle 的次数）
        │  │  └────────────────────────── rq->sched_count（schedule() 调用数）
        │  └───────────────────────────── ★字面量 0（见下节考古）
        └──────────────────────────────── rq->yld_count（sched_yield 调用数）
```

★ **第 3 槽的字面量 0——跨版本考古（本仓双标签实证）**：v2.6.32 的 `kernel/sched_stats.h` 中该位置是**真实字段 `rq->sched_switch`**（上下文切换计数，格式串 `"cpu%d %u %u %u ..."` 无 0）；该字段删除后（v3.10 起至 v6.6 逐字不变）改为硬编码 0 **保住列位**——工具解析时直接跳过。这正是 version 号机制存在的意义：删字段不挪列。

### 2.3 domain 行：36 个统计字段（:151-170）

```
domain<N> <cpumask 的 %*pb 十六进制位图>
  │
  ├─ 按 idle_type 三轮（enum cpu_idle_type：CPU_IDLE / CPU_NOT_IDLE /
  │  CPU_NEWLY_IDLE——include/linux/sched/idle.h:7-12），每轮 8 字段：
  │    lb_count     该域 load_balance 尝试次数
  │    lb_balanced   判定已均衡的次数
  │    lb_failed     均衡失败次数
  │    lb_imbalance  累计失衡量（权重和）
  │    lb_gained     迁入任务数
  │    lb_hot_gained 其中"热"任务数（cache-hot）
  │    lb_nobusyq   找不到忙队列的次数
  │    lb_nobusyg   找不到忙组的次数
  │  （3 × 8 = 24 字段）
  │
  └─ 尾部 12 字段：
       alb_count / alb_failed / alb_pushed    active load balance 三连
       sbe_count / sbe_balanced / sbe_pushed  SD_BALANCE_EXEC（exec 均衡）
       sbf_count / sbf_balanced / sbf_pushed  SD_BALANCE_FORK（fork 均衡）
       ttwu_wake_remote / ttwu_move_affine / ttwu_move_balance
                                              唤醒侧跨域/亲和迁移/均衡迁移
```

字段定义全在 `struct sched_domain`（include/linux/sched/topology.h:113-143，`#ifdef CONFIG_SCHEDSTATS` 块），注释分组与上表一一对应（"load_balance() stats"/"Active load balancing"/"SD_BALANCE_EXEC stats"/"SD_BALANCE_FORK stats"/"try_to_wake_up() stats"）。递增样例：`schedstat_inc(sd->lb_count[idle])`（fair.c:11075）、`schedstat_inc(sd->alb_count)`（:11413）。

## 三、rq 级计数的递增点：static-key 提升模式

### 3.1 ttwu_stat（core.c:3717-3758）——教科书级写法

```c
static void ttwu_stat(struct task_struct *p, int cpu, int wake_flags)
{
	if (!schedstat_enabled())      /* ★外层一次性判 static key */
		return;
	...
	if (cpu == rq->cpu) {
		__schedstat_inc(rq->ttwu_local);          /* 内层用 __ 前缀 */
		__schedstat_inc(p->stats.nr_wakeups_local); /* （免重复判 key） */
	} else {
		__schedstat_inc(p->stats.nr_wakeups_remote);
		for_each_domain(rq->cpu, sd) {
			if (cpumask_test_cpu(cpu, sched_domain_span(sd))) {
				__schedstat_inc(sd->ttwu_wake_remote);
				break;          /* 只记首个包含唤醒 CPU 的域 */
			}
		}
	}
	if (wake_flags & WF_MIGRATED)
		__schedstat_inc(p->stats.nr_wakeups_migrate);
	__schedstat_inc(rq->ttwu_count);
	__schedstat_inc(p->stats.nr_wakeups);
	if (wake_flags & WF_SYNC)
		__schedstat_inc(p->stats.nr_wakeups_sync);
}
```

宏族两套（stats.h:36-45）：`schedstat_inc(var)`（自带 `if (schedstat_enabled())`）与 `__schedstat_inc(var)`（裸增）——热路径把 static key 判断**提升**到函数入口一次完成，体内全部用裸版。

### 3.2 其余递增点（均已核对）

| 计数 | 位置 | 语境 |
| :--- | :--- | :--- |
| `rq->sched_count` | core.c:5961（`__schedule()` 尾部） | 每次 schedule() 调用；同点紧邻 `profile_hit(SCHED_PROFILING,...)` |
| `rq->sched_goidle` | idle.c:416 | 选到 idle 任务时 |
| `rq->yld_count` | core.c:8535（`sys_sched_yield`）/ :8969（`yield_to`） | 显式让出 CPU |

## 四、任务级 sched_statistics：30+ 字段全表（include/linux/sched.h:506-548）

```c
struct sched_statistics {          /* ____cacheline_aligned 单独缓存行 */
	u64 wait_start, wait_max, wait_count, wait_sum;   /* 就绪队列等待 */
	u64 iowait_count, iowait_sum;                      /* 其中 D 状态部分 */
	u64 sleep_start, sleep_max;  s64 sum_sleep_runtime;/* 自愿睡眠 */
	u64 block_start, block_max;  s64 sum_block_runtime;/* 非自愿阻塞 */
	u64 exec_max, slice_max;                            /* 执行/切片峰值 */
	u64 nr_migrations_cold;                             /* 冷迁移 */
	u64 nr_failed_migrations_affine/running/hot;        /* 迁移失败三分因 */
	u64 nr_forced_migrations;                           /* 强制迁移 */
	u64 nr_wakeups;                                     /* 总唤醒（ttwu_stat）*/
	u64 nr_wakeups_sync/migrate/local/remote/affine/affine_attempts/passive/idle;
#ifdef CONFIG_SCHED_CORE
	u64 core_forceidle_sum;                             /* core scheduling 强制空闲 */
#endif
};
```

挂在 **task_struct**（:822 `struct sched_statistics stats;`，与 uclamp/preempt_notifiers 同区）——而非 sched_entity（每任务一份，per-cgroup 任务的组实体不再单独记账）。

**状态机驱动**（stats.c:6-107，`__update_stats_*` 三函数）：

```
wait_start = rq_clock(rq)（入队时）
   │ dequeue/sleep/block 分岔：
   ├─ __update_stats_wait_end：delta = now - wait_start
   │    └ task_on_rq_migrating？→ 保留 wait_start（跨 CPU 迁移期间
   │      继续累计等待，:26-30 注释："Preserve migrating task's wait time"）
   │    否则：wait_max = max(wait_max, delta); wait_count++; wait_sum += delta
   │    并发 trace_sched_stat_wait(p, delta)
   ├─ __update_stats_enqueue_sleeper：sleep 分支 → sum_sleep_runtime +=
   │    delta、sleep_max 更新、trace_sched_stat_sleep、
   │    account_scheduler_latency（latencytop 入口）
   │    block 分支 → sum_block_runtime、trace_sched_stat_blocked、
   │    ★iowait 判定：p->in_iowait → iowait_sum/count +
   │      trace_sched_stat_iowait
   └─ profile 联动：prof_on == SLEEP_PROFILING 时
      profile_hits(SLEEP_PROFILING, get_wchan(p), delta >> 20)
      （注释原话："shift by 20 to get a milliseconds-range estimation"）
```

## 五、CONFIG_SCHED_INFO 轻量账：时间戳四函数（stats.h:198-289）

```c
struct sched_info {                /* include/linux/sched.h */
	unsigned long pcount;          /* 上 CPU 次数 */
	unsigned long long run_delay;  /* 累计就绪等待 */
	unsigned long long last_arrival, last_queued;  /* 两个时间戳 */
};
```

```
enqueue_task ──► sched_info_enqueue（core.c:2097）
                  仅当 last_queued==0 才打点（"first time queued"）
dequeue_task ──► sched_info_dequeue（:2117）
                  run_delay += now - last_queued；清戳
__schedule  ──► sched_info_switch（:5183）
                  prev != rq->idle → sched_info_depart
                     （rq_cpu_time 累计；仍 RUNNING 则重新打点）
                  next != rq->idle → sched_info_arrive
                     （run_delay += delta; pcount++; last_arrival = now）
```

三处注释直击设计本质：① dequeue 的跨 CPU 时钟偏移（stats.h:199-203 原话：*"We are interested in knowing how long it was from the first time a task was queued...The delta taken on each CPU would annul the skew"*——逐 CPU 差分抵消 rq_clock 不同步）；② idle 豁免（:279-281 原话：*"It's not interesting to record stats about how efficient we were at scheduling the idle process"*）；③ enqueue 只记首次（:241-244："only updates the timestamp if it is already not set"）。

**隐藏消费者——KVM steal time**：`sched_info_on()`（include/linux/sched/stat.h:25，编译期返回）被 arch/x86/kvm/x86.c（:3858/:4535/:4610）等用于 guest steal time 计费的先决判定——`CONFIG_SCHED_INFO=n` 会改变 KVM 向 guest 暴露的 PV_TIME 特性可用性。

## 六、开关矩阵：谁在什么条件下翻 static key

```
                    CONFIG_SCHEDSTATS=y 前提下（lib/Kconfig.debug:1245：
                    depends on DEBUG_KERNEL && PROC_FS；select SCHED_INFO；
                    help 原话 "avoid the very slight overhead"）
cmdline   schedstats=enable|disable   __setup（core.c:4643），解析失败
          告警原话 "Unable to parse schedstats=\n"
sysctl    kernel.sched_schedstats     handler sysctl_schedstats（:4646）：
          ★写需 CAP_SYS_ADMIN，否则 -EPERM
static key DEFINE_STATIC_KEY_FALSE(sched_schedstats)（:4606）
          set_schedstats（:4608）→ static_branch_enable/disable
强开      force_schedstat_enabled（:4616）——两个调用方：
          kernel/latencytop.c:75 与 kernel/profile.c:67
          （开启时打印 "kernel profiling enabled schedstats, disable
           via kernel.sched_schedstats.\n"）
被动依赖  check_schedstat_required（stats.h:56-68）：若五个
          sched_stat_* tracepoint 有活跃者而 schedstats 未开，则
          printk_deferred_once 原话提示——tracepoint 需要计数账做参数
```

tracepoint 五件套（include/trace/events/sched.h）：`sched_stat_wait/sleep/iowait/blocked`（同模板 `(tsk, delay)`，经 `DEFINE_EVENT_SCHEDSTAT`——**非 SCHEDSTATS 配置下整组变 NOP**）+ `sched_stat_runtime (tsk, runtime, vruntime)`。

## 七、消费方全景与数据源辨析

```
/proc/<pid>/schedstat（fs/proc/base.c:476-488）：
  !sched_info_on() → "0 0 0\n"
  否则 "%llu %llu %lu\n"：
    ① task->se.sum_exec_runtime      ← sched_entity（CFS 执行账）
    ② task->sched_info.run_delay     ← SCHED_INFO（等待账）
    ③ task->sched_info.pcount        ← SCHED_INFO（上 CPU 计数）
  ★三元组横跨两本账+一个实体字段——"schedstat"命名的混淆之源
  （fs/proc/base.c 与 stats.c 无任何代码共享，仅名字相近）

/proc/schedstat：cpu 行的 run_delay/pcount 是 rq 级聚合
  （rq->rq_sched_info，由 rq_sched_info_arrive/depart 旁路累计）

debugfs sched/debug：P(sched_count)/P(sched_goidle)（debug.c:808-809）
  ——同一批 rq 计数的另一出口（详见本分册 13 章）

KVM：sched_info_on() 门控 steal time（第五节）
latencytop：account_scheduler_latency 吃 __update_stats_enqueue_sleeper
  的 sleep/block delta（第四节）；且反向 force 开启 schedstats（第六节）
```

## 八、观测实战

```bash
# 开关与现状
cat /proc/sys/kernel/sched_schedstats        # 0/1
echo 1 > /proc/sys/kernel/sched_schedstats   # 需 root（CAP_SYS_ADMIN）

# cpu 行读法（第 3 槽是格式遗产，跳过）
awk '/^cpu/ {printf "%s sched=%d idle=%d ttwu=%d(local %d) run_time=%.3fms delay=%.3fms pcount=%d\n", \
  $1,$4,$5,$6,$7,$8/1e6,$9/1e6,$10}' /proc/schedstat

# domain 行：负载均衡健康度三比值
#   lb_balanced/lb_count  越高越好（大部分扫描判定已均衡）
#   lb_failed/lb_count    持续偏高 → 域内失衡或迁移受阻（对照
#                         nr_failed_migrations_affine/hot 区分原因）
#   alb_*                 active LB 活跃通常意味着调度压力已传到
#                         stop_machine 级别的搬运

# 任务级：wait/sleep/iowait 分解（tracepoint 或 delayacct 对比）
perf record -e sched:sched_stat_iowait -e sched:sched_stat_sleep -a sleep 5
# 注意：开这些 tracepoint 前若 schedstats 未开会触发第六节的
# "require the kernel parameter schedstats=enable" 提示

# 版本核对（工具解析前先看头两行）
head -2 /proc/schedstat     # version 15 + timestamp
```

## 九、思考题

1. `schedstat_inc` 与 `__schedstat_inc` 为什么要成对存在？如果把 ttwu_stat 体内的 `__schedstat_inc` 全换成带门控版本，性能上差在哪个量级？（提示：static branch 的补丁点 vs 分支预测。）
2. cpu 行第 3 槽保留字面量 0 而不是删掉一列，牺牲了什么换来了什么？（提示：SCHEDSTAT_VERSION 的存在意义与用户态工具的列号硬编码。）
3. `/proc/<pid>/schedstat` 的 `sum_exec_runtime` 来自 `task->se`——对 RT/DL 任务这个字段还准确吗？（提示：se 只在 fair class 被 update_curr 维护。）
4. sched_info_dequeue 用"逐 CPU 差分"抵消 rq_clock 偏移——如果只在最终离开时做一次全局差分会怎样？（提示：跨 CPU 迁移场景下 last_queued 与 now 来自不同 rq 的 clock。）
5. KVM 依赖 `sched_info_on()`（编译期）而非 schedstats 的 static key（运行期）——为什么 steal time 不能接受运行期才打开的账目？（提示：guest 侧的时间连续性 vs host 侧统计窗口。）
6. domain 行的 `lb_nobusyq`（找不到忙队列）与 `lb_balanced`（判定已均衡）都表示"没搬成任务"——两者的语义差在哪个环节？（提示：一个在找 busiest_rq 阶段返回空，一个在算出 imbalance 后放弃。）

---

> 关联：[00-一页速查](00-一页速查.md)（三元组速查）｜[13-调度器 debugfs 全景](13-调度器debugfs全景.md)（sched/debug 同源计数）｜[06-调度域与负载均衡](06-调度域与负载均衡.md)（lb_* 计数的机制背景）｜[04-调度时机与上下文切换](04-调度时机与上下文切换.md)（delayacct_blkio_start 所在的切换路径——delayacct 是另一套任务延迟账）｜[trace/07-procfs 观测索引](../../trace/docs/07-procfs观测索引.md)（/proc/schedstat 表项）｜[trace/08-sysctl 调参索引](../../trace/docs/08-sysctl调参索引.md)（sched_schedstats 注册）
