# cpu 控制器（Linux 6.6）

> 阅读目标：掌握 v2 cpu 控制器的完整接口族与换算链（cpu.weight → shares → se.load.weight）；理解 cpu.max 的 quota/period/burst 三参数语义（机制细节引用 scheduler 分册，不重复）；掌握 cpu.idle（组级 SCHED_IDLE）、uclamp 组接口；用 cpu.stat 与 cpu.pressure 做 throttle 排障。

---

## 一、接口文件族（kernel/sched/core.c 的 cpu_files，已核对 6.6）

| 文件（v2 前缀 cpu.） | 默认 | 语义 |
| :--- | :--- | :--- |
| `weight` | 100 | 相对权重（1~10000，`CGROUP_WEIGHT_MIN/MAX`） |
| `weight.nice` | 0 | 同一旋钮的 nice 视角（-20~19 双向换算） |
| `idle` | 0 | 组级 SCHED_IDLE（0/1）：整组降到 idle 策略语义 |
| `max` | `max 100000` | 带宽上限：`"$MAX $PERIOD"`（µs；`max`=不限） |
| `max.burst` | 0 | 突发额度：允许跨 period 累积/借用的上限 |
| `uclamp.min` / `uclamp.max` | — | 组级 util clamp（`CONFIG_UCLAMP_TASK_GROUP`） |
| `stat` | — | 用量与带宽统计（下文逐列） |
| `pressure` | — | CPU PSI（some/full） |

> v1 的 `cpu.shares`/`cpu.cfs_quota_us`/`cpu.cfs_period_us` 在 v2 收敛为 `cpu.weight` 与 `cpu.max` 一对——**权重（比例）与带宽（绝对上限）是两把独立的刀**：weight 管争抢时的分配比例，max 管不管争抢都越不过的硬顶。

---

## 二、weight 换算链：从 100 到 1024

```c
/* kernel/sched/core.c (v6.6, 已核对) */
static int cpu_weight_write_u64(..., u64 weight)
{
	if (weight < CGROUP_WEIGHT_MIN || weight > CGROUP_WEIGHT_MAX)  /* 1~10000 */
		return -ERANGE;
	weight = DIV_ROUND_CLOSEST_ULL(weight * 1024, CGROUP_WEIGHT_DFL); /* ×1024/100 */
	return sched_group_set_shares(css_tg(css), scale_load(weight));
}
```

```
cpu.weight 100（默认）→ shares 1024 == nice 0 的基准权重 NICE_0_LOAD
cpu.weight 200        → 2048（等效 nice ≈ -5）
cpu.weight 50         → 512（等效 nice ≈ +2 上下）
```

shares 进入 `task_group->se[].load.weight` 后就是 scheduler 分册第 3 章的世界：**EEVDF 的虚拟时钟斜率与 entity_eligible 的加权平均都由它决定**——weight 大的组 vruntime 涨得慢、lag 更容易为正，抢占份额自然变大。两个工程含义：

1. **weight 是"无上限的相对值"**：两个组 100:100 与 10000:10000 行为完全一样——单独调大某个组不影响与"未配置组"（默认 100）的关系之外的世界。
2. **层级折算**：子组的实际权重还要乘上父组在祖先层的占比（`update_cfs_group` 沿层级换算，scheduler 分册第 2 章）——嵌套容器的"稀释"由此而来。

`cpu.weight.nice` 只是换个写法：内部走 `PRIO_TO_NICE` 反查表后走同一路径（接口服务于"复用 nice 直觉"的用户）。

---

## 三、cpu.max：quota / period / burst

```c
/* tg_set_cfs_bandwidth（core.c，6.6 精要） */
if (quota != RUNTIME_INF && (burst > quota || burst + quota > max_cfs_runtime))
	return -EINVAL;          /* burst 不能超过 quota 本身 */
period ∈ [1ms, 1s]（min_cfs_quota_period/max_cfs_quota_period）
```

机制全链（throttle 状态机、5ms 本地分桶 `sched_cfs_bandwidth_slice`、slack timer 的疏浚）在 **scheduler 分册第 3 章第七节**已逐函数剖析，此处只补治理视角的三个实践点：

* **`max 100000`（=100ms/100ms）**：period 上限 1s、下限 1ms；quota 写 `max` 表示不限（RUNTIME_INF）。容器场景常见 `200000 100000`（2 核）。
* **burst**：默认每组每 period 只能领 quota；burst 允许把**没用完的部分存起来**（上限 `max.burst`），下个 period 超支时先吃存款——处理"均值合规、峰值超载"的波动负载（web 服务 P99 毛刺的经典药方）。对应 `cpu.stat` 的 `nr_bursts/burst_usec` 两列。
* **分配粒度**：quota 按 `sched_cfs_bandwidth_slice`（默认 5000µs）切成桶分发给各 CPU 的 cfs_rq（`distribute_cfs_runtime`）——**多核组的单核峰值 = min(quota, slice×并发核数)**，不是 quota/核数的想当然。

---

## 四、cpu.idle：组级 SCHED_IDLE（6.6）

```c
sched_group_set_idle(tg, 1)   /* cgroup.cgroup.idle = 1 */
```

把整组任务的公平语义降为 SCHED_IDLE（scheduler 分册第 1 章的 idle_nr_running 单独计数路径）：**组内任务只在别人不跑时才获得显著份额**。与 cpu.max 的区别：max 是"到量掐断"，idle 是"永远排最后"——离线批处理与在线服务的混部场景，idle 比 max 更温和（不产生 throttle 毛刺，只在争抢时让路）。继承：组的 idle 影响其在祖先层的调度身份（idle_h_nr_running 沿层级传播）。

---

## 五、cpu.stat 与排障

```
cpu.stat（6.6，两段 seq_show 拼合，已核对输出列）：
  usage_usec             组内累计 CPU 时间（用户+内核）
  user_usec / system_usec  细分
  nr_periods             经历的带宽周期数
  nr_throttled           被掐断的周期数
  throttled_usec         累计被掐时长
  nr_bursts / burst_usec 突发消耗统计
```

**"CPU 使用率只有 50% 却大量 throttle"** ——最高频的容器之谜，三步破案：

```
1. 单核峰值超 quota：4 核组 quota=100ms/100ms（1 核），业务两线程同时全速跑
   单核 100ms 周期 → 开局即领 5ms×N 桶，很快耗尽 → throttle，
   而全组平均利用率可能仍只有 50%。解法：quota 提到匹配并发（或限并发）。
2. period 边界对齐：多组同 period 同时到点，桶分发滞后产生微毛刺
   （nr_throttled 高但 throttled_usec 低）→ 多为噪音，看数量级。
3. burst 未配：波动负载存款耗尽即断 → nr_bursts 与 nr_throttled 同步增长
   → 配 max.burst。
```

诊断配套：

```
perf trace -e sched:sched_stat_runtime? （组视角更直接的是）
watch -n1 cat /sys/fs/cgroup/.../cpu.stat          # 增量读法
bpftrace 对 tracepoint sched:sched_switch 过滤组路径   # 组内切换全景
systemd-cgtop                                      # 组级 top
```

---

## 六、cpu.pressure：PSI 的组级视图

`cpu.pressure`（PSI，`kernel/sched/psi.c`）两行输出：

```
some avg10=1.23 avg60=0.80 avg300=0.42 total=123456789
full avg10=0.00 ...      full=6789012
```

* **some**：至少一个任务在等 CPU 的时间占比（饥饿感）
* **full**：**所有**非空闲任务同时等 CPU（整组停摆的时间占比）——cpu 的 full 要求"此 CPU 上该组无一可跑却仍被掐"，比 io/memory 的 full 少见得多

10/60/300 秒指数窗口 + total 累计（µs）。触发器（`poll()`）可挂在阈值上——容器编排（Kubernetes 的 cgroup PSI 驱动驱逐）即此。总开关 `CONFIG_PSI_DEFAULT_DISABLED`/`psi=1`。

> PSI 的算法本质（per-CPU 的 10ms 追踪窗、遮挡计数 overlap、avg 的几何衰减）值得专章——已列入孵化区。

---

## 七、机制侧的骨架（衔接 scheduler 分册）

本章聚焦治理接口；机制的完整剖析在 scheduler 分册，此处给出衔接地图与 cpu 侧的 cgroup 骨架事实：

* **css_alloc**：`cpu_cgroup_css_alloc` 为每个组创建 `task_group`（sched/core.c），同时分配它在**每个 CPU** 上的 `sched_entity` 与 `cfs_rq`（组在每个 CPU 的队列里都有一个实体投影——组调度树的"每 CPU 一棵子树"结构，scheduler 分册第 2 章图示）。
* **带宽发放**：period 到点（`sched_cfs_period_timer`，hrtimer）→ `refill_cfs_bandwidth` 灌满全局池 → `distribute_cfs_runtime` 按各 cfs_rq 的欠额比例把 `sched_cfs_bandwidth_slice`（默认 5000µs，`/proc/sys/kernel/sched_cfs_bandwidth_slice`）切给各 CPU。
* **疏浚（slack）**：`__return_cfs_rq_runtime` 把本地用剩的桶在 period 末尾前 5ms 归还全局池（`runtime_expired_within` 的两个回收时机）——多核组里"闲核的配额流向忙核"靠它。
* **uclamp 组接口**：`cpu.uclamp.min/max` 是任务级 uclamp 的组级底/顶——组内任务的 effective clamp 先取任务与组的并/交（`uclamp_eff_value`），再入 schedutil 的频率请求（power 分册第 2 章）。6.6 中它是**默认关闭**的 `CONFIG_UCLAMP_TASK_GROUP`。

## 八、v1 ↔ v2 接口映射（迁移速查）

| v1 | v2 | 备注 |
| :--- | :--- | :--- |
| `cpu.shares`（默认 1024） | `cpu.weight`（默认 100） | 换算见第二节 |
| `cpu.cfs_quota_us` + `cpu.cfs_period_us` | `cpu.max`（"quota period" 一对写） | 默认 `max 100000` |
| `cpu.cfs_burst_us`（5.14+） | `cpu.max.burst` | |
| `cpu.stat`（nr_periods 等） | `cpu.stat`（并入 usage_ 三列） | |
| `cpuacct.usage` | `cpu.stat` 的 `usage_usec` | v2 无独立 cpuacct |
| `cpu.rt_runtime_us` 等 | （无 v2 对应） | RT 组调度留在 v1（6.6 现状） |

## 九、实战案例二则

**案例 A：批处理容器拖慢在线**。症状：夜间批处理上线后在线 P99 劣化 30%，两组 cpu.max 均未触顶。定位链：

```
1. 两组 cpu.pressure：批处理 some avg10 涨到 40% —— 它在挨饿（正常，max 限着）
2. 但在线组 some 也 15% —— 在线也被挤压
3. 共同祖先（物理核）视角：批处理的 weight 与在线相同（都 100）
   → 批处理虽被 max 限了均值，但每次分桶后在"可用窗口"里与在线同权重争抢
4. 处置：批处理 cpu.weight 降到 10（或干脆 cpu.idle=1）+ 保留 max 兜底
   → 夜间 P99 回落
```

**案例 B：period 与毛刺**。`cpu.max = 100000 100000`（单核满额）的组出现周期性 100ms 边界的微 throttle。原因：quota 恰等于 period 且单核满速——桶耗尽与 refill 边界严格对齐，一拍不剩。处置：quota 微调 105% 或启用 burst 5000——用 5% 冗余吸收对齐抖动。

---

## 九点五、线程模式与 RT 组调度的现状（易错点）

* **cpu 控制器在 6.6 不支持 threaded 粒度**：声明 `.threaded = true` 的控制器只有 debug（git grep v6.6 证实）；5.x 曾用 nog 机制让 cpu 部分支持线程粒度，**该机制在 6.6 已删除**。需要按线程限速时走业务层方案（per-thread nice/affinity）而非 cgroup。
* **RT 组调度留在了 v1**：`cpu.rt_runtime_us`/`cpu.rt_period_us`（scheduler 分册第 7 章的 RT bandwidth 组版）在 v2 **没有对应接口**（6.6 现状）——需要给 RT 任务分组限额的场合（音频管线、工业控制）仍是 v1 的 `cpu,rt` 层级。

## 九点六、PSI 的算法内幕（已对照 kernel/sched/psi.c）

```
每个 CPU 周期性 update（追踪窗）：
  任务级状态位 (oncpu/memstall/running/iowait) 聚合为 per-CPU 的
  4 状态时间累积；"some"= ≥1 任务在等，"full"= 全部非空闲任务同时在等
  total 以 µs 计；跨 CPU 聚合走 rstat 上卷（与 cgroup.stat 同款读时付费）
窗口平均（avg10/60/300）：
  三个几何窗口；半过期窗口用历史增长速率外推
  （window_update 的 prev_growth，psi.c:411 注释——优雅的部分估计）
触发器（用户态 poll 压力阈值）：
  非特权触发器窗口最小 2s（psi.c:1316 注释：防聚合风暴）
```

与治理的关系：`cpu.pressure` 的 avg 是"该组被 CPU 饥饿的时间占比"——比使用率更贴近 SLO（使用率 100% 但压力 0% = 健康满负荷；使用率 60% 但 some=30% = 调度饥饿）。

## 九点七、systemd 侧的对应物（运维速查）

| systemd 指令 | cgroup v2 写入 |
| :--- | :--- |
| `CPUWeight=` | `cpu.weight` |
| `CPUQuota=200%` | `cpu.max = 200000 100000` |
| `AllowedCPUs=` | cpuset.cpus（v2 经 `Delegate=` 暴露） |
| `MemoryMax=` / `MemoryHigh=` | `memory.max` / `memory.high` |
| `IOWeight=` / `IODeviceLatencyTargetSec=` | `io.weight` / `io.latency`（第 4 章） |

`Delegate=yes` 决定哪些文件族交给单元自管（子树里写 subtree_control 不再受权限约束）——容器运行时的标准姿势。

## 九点八、refill/steal 的周期时间线（补路线图指定素材）

```
period T (100ms) 的生命周期（跨核组, quota=200ms, 4 CPU）：

T+0     sched_cfs_period_timer（hrtimer，挂 tg->cfs_bandwidth）
        └─ refill_cfs_bandwidth：runtime = quota；slack 窗口关闭
T+0~    distribute_cfs_runtime：遍历组内欠额的 cfs_rq，
        按欠额比例发 5ms 桶（slice）——CPU0..3 各领 5ms
T+5ms   CPU3 先用完 → 桶耗尽 → throttle（dequeue 整子树）
        nr_throttled++，throttled_clock 开始计时
T+8ms   CPU0 还剩 2ms 时本核空闲 → __return_cfs_rq_runtime
        把 2ms 归还全局池 → CPU3 在 period 尾段被复查唤醒（slack 疏浚）
T+95ms  period 尾 5ms：全部桶强制回收（runtime_expires）
T+100ms refill 下一轮（unthrottle 全体）
```

两个可观测推论：① `throttled_usec` 的大头是"全组耗尽到 refill"的尾部等待（减毛刺 → 提 quota 或缩 period）；② 单核偷跑由集中检查点 `__account_cfs_rq_runtime` 的 steal 逻辑补课。

```
读数示例（容器 4 核 quota=200ms）：
  nr_periods=1000  nr_throttled=170  throttled_usec=1.2e9
  → 17% 周期被掐，累计 1.2s；平均每次 ~7ms
  → P99 毛刺若 ≈ 半周期分布 → 毛刺源基本锁定为带宽而非排队
```

## 十、思考题

1. 两个子组 weight 1:10000，父组自身在根下 weight=100。孙辈的份额由哪两次换算决定？给出"孙辈实际份额 = f(自身 weight, 父 weight, 祖 weight)"的式子（对照 scheduler 分册第 2 章的 update_cfs_group）。
2. `cpu.max` 的 quota 设为 0 会发生什么？（对照 `tg_set_cfs_bandwidth` 的 0 语义与 RUNTIME_INF 的区别。）
3. burst 与"提高 quota"都消灭 throttle 毛刺，各自的代价曲线是什么？什么时候 burst 严格更优？
4. cpu.pressure 的 `full` 为什么在 CPU 域比 io/memory 域少见？（提示：full 的定义要求"无一个任务在跑"，而组内任务可能散在多核。）
5. 设计：离线训练容器（8 核饱和计算，可让路）与在线推理容器（P99 <50ms）同机。给出 cpu.weight/cpu.max/cpu.idle/PSI 告警的四元配置与理由。

---

> 下一章：[memory 控制器](03-memory控制器.md) —— 两层记账（obj_cgroup→mem_cgroup）、page_counter 的四道闸门、memcg 回收与 OOM。
