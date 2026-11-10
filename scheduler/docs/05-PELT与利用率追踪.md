# PELT 与利用率追踪（Linux 6.6）

> 阅读目标：吃透 Per-Entity Load Tracking 的衰减数学与 6.6 实现（`kernel/sched/pelt.c`）；分清 `load_avg` / `runnable_avg` / `util_avg` 三个信号的语义与消费方；理解 rq 级聚合、NOHZ 阻塞负载、迁移同步（removed load）、util_est 与 uclamp，以及它们如何喂给 schedutil/EAS/负载均衡。

---

## 一、为什么需要"每实体"的负载追踪

早期内核只有**全局近似**：rq 级的瞬时 nr_running、CPU 时间片粗粒度统计。它们回答不了三个现代问题：

1. **这个任务"值多少 CPU"**？——任务刚唤醒时 util=0，但真实需求可能 80%。负载均衡要把"重"任务搬走，必须知道每个任务的权重×活跃度。
2. **这个 CPU 未来需要多快**？——cpufreq 只看瞬时占用会震荡；需要一个有记忆、平滑、可预测的信号。
3. **小核跑得动这个任务吗**？——异构系统上必须比较任务 util 与核 capacity。

PELT（4.3，2012，Paul Turner/Ben Segall）把记账下沉到每个 `sched_entity`（任务或组），用**几何衰减的滑动平均**回答"最近的过去"。6.6 的 PELT 经历过 5.13 的大重构（"PELT decayed"：不再按 1024 周期整数对齐累加，而是任意粒度增量衰减），本章按 6.6 实现讲。

---

## 二、衰减模型：y³² ≈ ½

### 2.1 三句话定义

* 时间切成 **1024µs（~1ms）的 period**；
* 每个 period 的历史贡献乘 **y = 0.978⁻¹**（精确值使 y³² = ½）衰减——即 **32ms 半衰期**；
* `*_sum` 是无限几何级数的定点缓存（`SCHED_CAPACITY_SCALE = 1024` 定点）。

### 2.2 三个信号的精确定义（include/linux/sched.h 注释原文翻译）

对每个 `sched_entity`（任务级）：

```
load_avg     = runnable% × scale_load_down(load.weight)
               （份额语义：我"应该"占用多少权重·CPU）

runnable_avg = runnable% × SCHED_CAPACITY_SCALE
               （就绪语义：我在队列里的时间占比，含等待时间）

util_avg     = running% × SCHED_CAPACITY_SCALE
               （占用语义：我在 CPU 上的时间占比——cpufreq/EAS 只认它）
```

| 信号 | 分子（时间累计什么） | 上界 | 主要消费者 |
| :--- | :--- | :--- | :--- |
| `load_avg` | on_rq 期间 × 权重 | 任务权重（组则 shares） | 负载均衡的"重量"、fair 的组权重换算 |
| `runnable_avg` | on_rq 期间 | 1024 | newidle balance 判断"值不值得拉"、util 不足时的补充 |
| `util_avg` | on_cpu 期间 | 1024 | schedutil 频率请求、EAS 能效估算、capacity 判定 |

**runnable% vs running% 的差**就是排队时间占比：一个在 10 核机器上负载 100% 的任务（一直 runnable、10% 时间 on_cpu），`runnable_avg≈1024` 而 `util_avg≈102`。`load_avg` 均衡用它搬任务，cpufreq 用 util 提频——两套消费者对"负载"的定义本来就不同。

---

## 三、6.6 实现：pelt.c 逐函数

### 3.1 decay_load：O(1) 幂衰减

```c
static u64 decay_load(u64 val, u64 n)            /* val * y^n */
{
	if (unlikely(n > LOAD_AVG_PERIOD * 63))  /* > ~2 秒 → 0 */
		return 0;
	local_n = n;
	if (unlikely(local_n >= LOAD_AVG_PERIOD)) {   /* y^n = 1/2^(n/32) * y^(n%32) */
		val >>= local_n / LOAD_AVG_PERIOD;    /* 整数个半衰期：右移！ */
		local_n %= LOAD_AVG_PERIOD;
	}
	val = mul_u64_u32_shr(val, runnable_avg_yN_inv[local_n], 32);
	return val;                                  /* 查表乘 32 位，一次完成 */
}
```

数学技巧：`y^32=½` 允许把任意 n 次幂拆成"右移（半衰期数）+ 32 点查表（余数）"——常数时间，无迭代。

### 3.2 accumulate_sum：三段式增量（6.6 重构核心）

任意两次更新之间隔了 `delta` 微秒（可能含若干完整 period）。源码自图：

```
           d1          d2           d3
           ^           ^            ^
         |<->|<----------------->|<--->|
 ... |---x---|------| ... |------|-----x (now)
        ↑上次更新（period_contrib 断点）        ↑本次

u' = (u + d1)*y^p  +  1024*\Sum_{n=1}^{p-1} y^n  +  d3*y^0
    └── 旧缓存衰减 ──┘  └──── 中间完整周期的"满格"贡献 ────┘ └ 新增 ┘
```

```c
static __always_inline u32
accumulate_sum(u64 delta, struct sched_avg *sa,
	       unsigned long load, unsigned long runnable, int running)
{
	u32 contrib = (u32)delta;
	u64 periods;

	delta += sa->period_contrib;
	periods = delta / 1024;              /* 跨过的完整周期数 p */

	if (periods) {                        /* 先衰减旧缓存 */
		sa->load_sum     = decay_load(sa->load_sum, periods);
		sa->runnable_sum = decay_load(sa->runnable_sum, periods);
		sa->util_sum     = decay_load(sa->util_sum, periods);

		delta %= 1024;
		if (load)
			contrib = __accumulate_pelt_segments(periods,
					1024 - sa->period_contrib, delta);
	}
	sa->period_contrib = delta;

	if (load)
		sa->load_sum += load * contrib;
	if (runnable)
		sa->runnable_sum += runnable * contrib << SCHED_CAPACITY_SHIFT;
	if (running)
		sa->util_sum += contrib << SCHED_CAPACITY_SHIFT;

	return periods;
}
```

`__accumulate_pelt_segments()` 用预推导的闭式（`c2 = LOAD_AVG_MAX - decay_load(LOAD_AVG_MAX,p) - 1024`）算中间段，同样是查表级开销。**相比 5.12 及以前的实现**：不再要求更新对齐到 period 边界、不再有 `period_contrib` 进位循环，任意 `delta` 一次算完——这是 `clock_pelt` 任意粒度推进的前提。

### 3.3 ___update_load_avg：sum → avg 的降维

```c
static __always_inline void
___update_load_avg(struct sched_avg *sa, unsigned long load)
{
	u32 divider = get_pelt_divider(sa);      /* ~47742（LOAD_AVG_MAX-1024+period_contrib） */

	sa->load_avg     = sa->load_sum / divider;
	sa->runnable_avg = sa->runnable_sum / divider;
	sa->util_avg     = sa->util_sum / divider;
}
```

`*_avg` 就是 `*_sum` 除以几何级数稳态和（`LOAD_AVG_MAX ≈ 47742 ≈ 1024 × 46.7`）。util_sum 是 u32 而非 u64——因为 util 贡献不含权重，上限就是 `1024<<10 × 47742/1024 ≈ 2³¹`，64 位纯属浪费（6.6 的一个省内存优化）。

### 3.4 更新时机：update_load_avg 的三种状态

`kernel/sched/fair.c` 的 `update_load_avg(cfs_rq, se, flags)` 在每次入队/出队/tick/迁移时调用，内部对 se 与 cfs_rq 各跑一遍 `__update_load_avg_se/_cfs_rq`。三个布尔（load/runnable/running）按实体当前状态取值：

* **任务 on_rq + on_cpu**：三项全累加（它的 load/runnable/util 都在涨）。
* **任务 on_rq 但在排队**：load、runnable 累加，util 不动（没占 CPU）。
* **任务睡眠（blocked）**：`load_sum` 停止累加新贡献但**继续按时间衰减**——衰减函数在 `update_blocked_averages()` 里被周期调用，让睡眠任务的残留负载逐渐归零。这就是"blocked load"。

---

## 四、rq 级聚合与 NOHZ

### 4.1 聚合链

```
任务 se.avg ──(每次更新同步加上/减去自身贡献)──► cfs_rq.avg
cfs_rq.avg ──(组调度沿层级 propagate)──► 上层 cfs_rq.avg ──► rq 级 cfs util
                                                     │
                              rq->cfs.avg.util_avg ───┤
                              rq->cfs.avg.runnable_avg┤──► cfs_rq_util/EAS 输入
                              + rt avg / dl avg ──────┴──► effective_cpu_util()
```

注意 **util 信号是分层的**：`cpu_util_cfs`（rq->cfs 的 util_avg）+ `cpu_util_rt`（rq->avg_rt）+ `cpu_util_dl`（running_bw 折算）在 `effective_cpu_util()`（kernel/sched/core.c，6.6）里按用途组合。

### 4.2 blocked load 的衰减义务

睡眠任务的 load_avg 仍挂在 cfs_rq 上（均衡会把它当"这颗 CPU 的负载"），必须有人定期替它衰减：

* **普通 CPU**：`update_blocked_averages(cpu)` 在 tick 的 `nohz_idle_balance`/periodic balance 路径里调用。
* **NOHZ_IDLE CPU**：没有 tick！→ `nohz_csd`（call_single_data）+ `has_blocked_load` 标记：**还有未衰减负载的 idle CPU 集合**由别的 CPU 在自己的 tick 里检查（`_nohz_idle_balance`），必要时 IPI 通知对方自己衰减（`NOHZ_NEXT_KICK` 风格的渐进收尾）。
* **NOHZ_FULL CPU**：`nohz_full` 上任务在跑但不进内核 tick——`remote_update_load`？6.6 的处理是让入队/出队路径自身承担衰减（6.6 重构的收益之一）。

### 4.3 迁移同步：removed 链表

任务从 CPU0 迁到 CPU1：它的 PELT 状态随行，但**CPU0 的 cfs_rq 级聚合**不能瞬间扣掉（`avg` 是增量维护的，若当场减去"衰减了半天的份额"，负载凭空蒸发/蒸发前漂移）。流程（fair.c）：

```
detach_task()          → migrate_task_rq_fair: detach_entity_load_avg()
                         把该 se 的 load/util/runnable 三个 avg 挂入
                         cfs_rq->removed 链表（持 cfs_rq 的 removed.lock）
下一次 update_load_avg(): remove_entity_load_avg()
                         从 removed 取出，按"当前值"从 cfs_rq->avg 里记账性扣除
```

效果：旧 CPU 的聚合负载**平滑地**反映离开的任务（下次更新时一次修正），新 CPU 侧 `attach_entity_load_avg()` 立即并入。`SCHED_FEAT(ATTACH_AGE_LOAD, true)`：任务长期睡眠后迁移，其 PELT 先"老化"再并入，避免陈旧高负载污染新 CPU 的均衡决策。

---

## 五、util_est：给 cpufreq 的"预测修正"

`util_avg` 的 32ms 半衰期对 cpufreq 太慢：任务从睡眠醒来冲满核，util_avg 要几十 ms 才爬上去，期间频率滞后（性能坑）。`struct util_est`（6.6）：

```c
struct util_est {
	unsigned int		enqueued;     /* 队列内任务的 util_avg 最大值（近似峰值）*/
	unsigned int		ewma;         /* enqueued 的指数滑动平均 */
#define UTIL_EST_WEIGHT_SHIFT	2     /* ewma = 3/4*ewma + 1/4*enqueued */
#define UTIL_AVG_UNCHANGED	0x80000000
} __attribute__((__aligned__(sizeof(u64))));
```

* `enqueued`：出队（睡眠）时更新为该任务的 util_avg（`util_est_dequeue` 的 `ewma` 累计）——唤醒后 rq 的 util_est 立即包含它，**不等 avg 爬坡**。
* `cfs_rq->avg.util_est.enqueued` = 队上所有任务的 enqueued 之和（上限钳制）。
* **消费方**：`effective_cpu_util(FREQUENCY_UTIL)` 在 util_avg 与 util_est.enqueued 之间取大——schedutil 的频率请求对"突发任务集合"的响应提前 30~100ms。
* `SCHED_FEAT(UTIL_EST_FASTUP)`：上调（更重的任务入队）立即反映，下调仍走平滑。

---

## 六、uclamp：util 的人为包络

PELT 是纯观测，uclamp（5.3+）给系统管理员/框架**改写**观测的能力：

```
最终 util = clamp( Σ任务util_est, uclamp_min[rq], uclamp_max[rq] )
```

* 每任务 `p->uclamp_req[UCLAMP_MIN/MAX]`（`sched_setattr` 可设）；cgroup v2 `cpu.uclamp.min/max` 聚合到组；rq 级生效值取**任务集合的 max(min)/min(max)**（`uclamp_eff`）。
* **UCLAMP_MIN**：把小任务的 util 抬高 → schedutil 不降频 → 交互延迟保障（Android 的 `top-app` boost 即此）。
* **UCLAMP_MAX**：把大任务的 util 压低 → 允许离线/后台任务跑在小核低频，兼顾能效与发热（框架化混部常用）。
* 实现：**bucket 化**（5 个 bucket/级，`struct uclamp_bucket{value,bitmap}`）——插入删除 O(1)，避免每次唤醒全队列重算 clamp。

---

## 七、schedutil：util 如何变成频率

`schedutil`（4.7，今天默认 governor）是调度器的"御用"调频器（`kernel/sched/cpufreq_schedutil.c`）：

```
update_load_avg / util_est 更新
  └─ cfs_rq_util_change() → cpufreq_update_util()
       └─ sugov_update_*
            ├─ sugov_get_util(cpu)
            │    next_f = 有效util 映射：freq = 1.25 × util×max_freq/capacity
            │    （1.25 headroom：留出 util_avg 滞后余量）
            │    基准 = effective_cpu_util(cpu, cpu_util_cfs(cpu),
            │                FREQUENCY_UTIL, NULL)
            │    = max(util_est.enqueued, util_avg)  + rt/dl 份额
            │      再 clamp 到 uclamp 区间
            ├─ 变化未超节流窗口（tunables->rate_limit_us，默认由
            │    cpufreq_policy_transition_delay_us(policy) 按硬件切频延迟推算，
            │    sysfs 可调）→ 忽略
            └─ sugov_iowait_boost 等 → fast_switch（硬件快速路径）或普通切频
```

两个要点：**频率不变性**（`rq_clock_pelt` 已按 `arch_scale_freq_capacity` 缩放）保证降频时 util 不虚高——这正是 util_avg 能与 capacity 直接比较的原因；**DL 的特殊性**：deadline 任务直接按 deadline 反推所需算力（`cpu_util_dl = running_bw`），schedutil 对 DL 密集负载的反应是"尽力拉满直到满足时限"。

---

## 八、观测与调试

```
/proc/<pid>/sched          → se 的 load_avg/util_avg? （打印 avg 经 scale 后的值）
/sys/kernel/debug/sched/debug
   cfs_rq[cpuN] 段:  .load_avg .runnable_avg .util_avg .util_est
                     （6.6 debugfs 的 cfs_rq 打印即 PELT 三信号现场）
/proc/loadavg              → 全局近似（calc_global_load，非 PELT）
tracepoint:sched:sched_load_avg_cpu   → 每次 rq 级 PELT 更新（可画 util 曲线）
tracepoint:sched:sched_load_avg_task  → 任务级
/sys/devices/system/cpu/cpuN/cpufreq/scaling_governor → 确认 schedutil
/sys/devices/system/cpu/cpuN/cpufreq/... → 当前频率 vs /sys/.../cpu_capacity
```

实验建议：写一个 50% 占空比的自旋任务（`schbench`/自写）：

```
perf stat -e cycles 观察频率跟随；同时 bpftrace 画 sched_load_avg_cpu 的
util_avg 与 scaling_cur_freq —— 应看到 1.25×util 的映射与 rate_limit 平滑。
```

---

## 九、思考题

1. `runnable_avg` 与 `util_avg` 的差在什么 workload 下最大？负载均衡（第 6 章）应该搬"runnable 高 util 低"还是"util 高"的任务？提示：均衡目标是削峰等待 vs 均摊算力。
2. NOHZ idle CPU 的 blocked load 若长期无人衰减，均衡会犯什么方向的错误？（把空负载 CPU 误判为忙/闲？）对应哪个 sysctl/行为能观察？
3. `decay_load` 为什么在 `n > LOAD_AVG_PERIOD*63` 时直接返回 0？47742×y²⁰¹⁶ 有多小？这限制了 PELT 的最长"记忆"，两次更新最多能隔多久不出错？（联系 nohz idle 的 max_gap 检查。）
4. util_est 的 `enqueued` 在任务**出队时**更新。为什么不在入队时？提示：入队时 util_avg 还没恢复，出队时的值才是"它刚干完活的真实水平"。
5. uclamp_max 若设置低于任务真实 util，schedutil 会欠频、任务会跑得更久（物理时间）——uclamp_max 到底省了什么？什么场景下反而更费电？

---

> 下一章：[调度域与负载均衡](06-调度域与负载均衡.md) —— sched_domain 层级、周期/空闲均衡与 NUMA balancing。
