# CFS 与 EEVDF 详解（Linux 6.6）

> 阅读目标：逐函数吃透 6.6 fair 调度类的核心算法——虚拟时间与权重、`avg_vruntime()`、`entity_eligible()`、`update_entity_lag()`、`place_entity()`、`update_deadline()`、`__pick_eevdf()`，以及唤醒抢占 `check_preempt_wakeup()` 的判定变化；理解 EEVDF 相比 CFS 到底修好了什么。

本章源码均出自 `kernel/sched/fair.c`（Linux 6.6），行号为近似参考，建议对照 elixir 交叉阅读。

---

## 一、公平性的数学基础：虚拟时间与权重

### 1.1 权重：nice 值的内部表示

`kernel/sched/core.c` 的 `sched_prio_to_weight[]` 表（节选）：

```c
const int sched_prio_to_weight[40] = {
 /* -20 */ 88761, 71755, 56483, 46273, 36291,
 /* -15 */ 29154, 23254, 18705, 14949, 11916,
 /* -10 */  9548,  7620,  6100,  4904,  3906,
 /*  -5 */  3121,  2501,  1991,  1586,  1277,
 /*   0 */  1024,   820,   655,   526,   423,
 /*   5 */   335,   272,   215,   172,   137,
 /*  10 */   110,    87,    70,    56,    45,
 /*  15 */    36,    29,    23,    18,    15,
};
```

规律：每差一级 nice 权重比约 1.25（精确值由表硬编码，保证跨版本稳定）。`NICE_0_LOAD = 1024`。权重进入 `struct load_weight`（`load.weight` 带一个 `scale_load_down()` 的移位封装，纯整数优化，可无视）。

### 1.2 calc_delta_fair：物理时间 → 虚拟时间

```c
static inline u64 calc_delta_fair(u64 delta, struct sched_entity *se)
{
	if (unlikely(se->load.weight != NICE_0_LOAD))
		delta = __calc_delta(delta, NICE_0_LOAD, &se->load);

	return delta;
}
```

* nice 0（权重 1024）：`vruntime` 与物理时间 1:1 推进（`delta` 原样返回）。
* 其他：`delta_virt = delta_exec * NICE_0_LOAD / weight`（`__calc_delta` 用 128 位乘除避免溢出）。

直觉：**权重越大的任务，虚拟时钟走得越慢**。两个任务权重 1024 vs 512（nice 差 2）：512 的任务 vruntime 涨速是 1024 的两倍——树中很快"跑到右边"，让 1024 多拿一倍 CPU。这就是比例公平的全部实现：不设上限、不设配额，只让"该拿更多的"虚拟时间走得慢。

---

## 二、CFS 的选法与它的裂缝（回顾）

CFS 的选择函数在 6.5 及以前：

```
pick = rb_leftmost(红黑树按 vruntime 排序)     // 永远选 vruntime 最小者
```

两道裂缝：

1. **公平有界但延迟无界**：CFS 的公平性靠"所有人 vruntime 挤在 `sched_latency`（6ms）窗口内"约束，窗口大小 = `nr_running * min_granularity`。任务一多（服务器 100+），窗口被动拉大到数百 ms——**交互任务的唤醒→上CPU延迟随负载线性恶化**，且无任何旋钮能在不破坏公平的情况下单独压它。
2. **短任务吃亏**：vruntime 最小者胜出，意味着一个只需 0.1ms 的短请求，必须排在所有"vruntime 更小"的长任务后面。没有任何机制表达"我这个请求本来就短，应该快点被处理完"。

EEVDF 用**合格性（eligibility）管公平、虚拟截止期（virtual deadline）管延迟**，同时缝合两道裂缝。

---

## 三、EEVDF 的数学

### 3.1 模型

任务 i 的每一次"想跑"称为一个**请求（request）**，长度 `r_i`（即 `se->slice`，6.6 固定为 `sysctl_sched_base_slice = 750000ns`）。在虚拟时间轴上：

```
lag_i   = S - s_i      （应得服务 - 已得服务；lag>0 = 被亏欠）
        = w_i * (V - v_i)

V      = 加权平均虚拟时刻 = (\Sum w_j * v_j) / W，W = \Sum w_j

eligible(i)  ⟺  lag_i ≥ 0      （被亏欠，才有资格再拿服务）

vd_i（虚拟截止期）= v_i + r_i / w_i
                   （本次请求的到达虚拟时刻 + 请求时长/权重）
```

调度规则一句话：**在当前 eligible 的任务里，挑 vd_i 最小的**。

* 公平：lag<0（拿多了）的任务直接出局（ineligible）；
* 延迟：短请求/高权重的 vd 小，自然先跑——**短任务延迟低不需要任何启发式**，是模型内生性质。

### 3.2 源码里的 V：avg_vruntime()（fair.c）

第 2 章说过 cfs_rq 缓存了加权和。`avg_vruntime()` 把缓存换算回"以 min_vruntime 为零点的平均虚拟时刻"：

```c
u64 avg_vruntime(struct cfs_rq *cfs_rq)
{
	s64 avg = cfs_rq->avg_vruntime;
	long load = cfs_rq->avg_load;

	if (curr && curr->on_rq) {          /* curr 不在树里，临时补上 */
		unsigned long weight = scale_load_down(curr->load.weight);
		avg += entity_key(cfs_rq, curr) * weight;
		load += weight;
	}

	if (load) {
		avg = div64_s64(avg, load);     /* V = 分子/分母 */
	} else {
		avg = cfs_rq->min_vruntime;
	}

	return cfs_rq->min_vruntime + avg;
}
```

注意 `div64_s64` 是**带符号**除法：分子 `\Sum (v_i - v0)*w_i` 可正可负（有任务 vruntime 小于 v0？不会——min_vruntime ≤ 所有 on_rq 的 vruntime，单项非负；但 avg_vruntime 的增量维护经过 clamp，见 `__update_min_vruntime` 的 open-coded max_vruntime）。工程细节：所有除法/乘法都移到入队/出队路径（低频），`entity_eligible()`（高频）只剩乘法+比较，**没有除法**。

### 3.3 entity_eligible()：公平性的 O(1) 判定

源码注释（6.6，逐行解读）：

```c
int entity_eligible(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	struct sched_entity *curr = cfs_rq->curr;
	s64 avg = cfs_rq->avg_vruntime;
	long load = cfs_rq->avg_load;

	if (curr && curr->on_rq) {
		unsigned long weight = scale_load_down(curr->load.weight);
		avg += entity_key(cfs_rq, curr) * weight;
		load += weight;
	}

	return avg >= entity_key(cfs_rq, se) * load;
}
```

推导（为什么这个式子等价于 lag ≥ 0）：

```
lag_i ≥ 0
⟺ V ≥ v_i
⟺ \Sum (v_j - v0)*w_j ≥ (v_i - v0) * \Sum w_j     （两边同乘 W>0，同减 v0*W）
⟺ avg_vruntime分子的整数形式 ≥ entity_key(se) * avg_load
```

**两侧都乘到整数域比较**，避免除法引入的舍入误差（注释原话：`avg_vruntime() > se->vruntime` is inaccurate due to the loss in precision）。这是全内核公平判定的最热点之一——`__pick_eevdf` 树搜索中每个节点都要调它，所以必须是纯乘法。

### 3.4 update_curr()：记账中心

`update_curr()` 是 fair 类的"心脏"，每次入队/出队/tick/换任务都要先调它：

```c
static void update_curr(struct cfs_rq *cfs_rq)
{
	struct sched_entity *curr = cfs_rq->curr;
	u64 now = rq_clock_task(rq_of(cfs_rq));
	u64 delta_exec;

	delta_exec = now - curr->exec_start;
	if (unlikely((s64)delta_exec <= 0))
		return;
	curr->exec_start = now;
	...
	curr->sum_exec_runtime += delta_exec;
	curr->vruntime += calc_delta_fair(delta_exec, curr);
	update_deadline(cfs_rq, curr);        /* ★ 6.6 新：deadline 到期即请求 resched */
	update_min_vruntime(cfs_rq);          /* 推进 min_vruntime（clamp 到 avg 之下）*/
	...
	account_cfs_rq_runtime(cfs_rq, delta_exec);   /* CFS bandwidth 记账 */
}
```

6.6 的关键结构变化：**抢占决策从 tick（`check_preempt_tick`，6.5 及以前）搬进了 `update_deadline()`**——每次记账时内联判断"当前请求是否已过期"。

### 3.5 update_deadline()：切片到期即 resched

```c
static void update_deadline(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	if ((s64)(se->vruntime - se->deadline) < 0)
		return;                                   /* 请求未到期，继续 */

	se->slice = sysctl_sched_base_slice;          /* 新请求 = 新 slice */

	/*
	 * EEVDF: vd_i = ve_i + r_i / w_i
	 */
	se->deadline = se->vruntime + calc_delta_fair(se->slice, se);

	/*
	 * The task has consumed its request, reschedule.
	 */
	if (cfs_rq->nr_running > 1) {
		resched_curr(rq_of(cfs_rq));              /* ★ 到期立刻标记抢占 */
		clear_buddies(cfs_rq, se);
	}
}
```

行为对比：

| | CFS（≤6.5） | EEVDF（6.6） |
| :--- | :--- | :--- |
| 到期检查 | `entity_tick` → `check_preempt_tick`（tick 驱动，粒度 = HZ） | `update_curr` → `update_deadline`（记账驱动，粒度 = 任意进锁时机） |
| 到期后动作 | 设 need_resched，等下一个抢占点 | 同样设 need_resched，但检查频率高得多 |
| slice 语义 | 时间片（fairness 的一部分） | **请求长度**（延迟承诺的载体） |

> 💡 一句话：**EEVDF 的"时间片"不再是公平性的旋钮，而是延迟的旋钮**。750000ns（0.75ms）的默认请求长度意味着最坏调度延迟约等于 max(slice, 抢占点粒度)，与任务数无关。

---

## 四、睡眠与唤醒：lag 的守恒（place_entity 全解）

任务睡眠出队时 lag 信息若不保存，唤醒后的补偿只能靠拍脑袋（CFS 的"减半惩罚"正是如此）。EEVDF 的做法是**保存 lag、唤醒时如数奉还**，同时数学上防止"加减实体移动平均 V"造成的漂移。

### 4.1 第一步：出队时截断并保存 lag —— update_entity_lag()

```c
static void update_entity_lag(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	s64 lag, limit;

	lag = avg_vruntime(cfs_rq) - se->vruntime;

	limit = calc_delta_fair(max_t(u64, 2*se->slice, TICK_NSEC), se);
	se->vlag = clamp(lag, -limit, limit);
}
```

出队（睡眠）路径调用它。两个细节：

1. **clamp 的量纲**：lag 本身是"虚拟时间"差，但公平的比较基准应是"够跑多久"。`calc_delta_fair(2*slice 或 1 tick)` 把上限换算成该任务自己的虚拟时间刻度——重任务（大权重）的虚拟刻度慢，同样的物理上限在虚拟域数值更大。
2. **clamp 的动机**（源码注释）：加减/重权实体都会平移 V，一次大 lag 会让别的任务的 lag 突变，极端时可无限放大。EEVDF 稳态理论界是 `-r_max < lag < max(r_max, q)`，截断是工程近似。

### 4.2 第二步：唤醒入位 —— place_entity()（6.6 完整逻辑）

```c
static void place_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
	u64 vslice, vruntime = avg_vruntime(cfs_rq);
	s64 lag = 0;

	se->slice = sysctl_sched_base_slice;
	vslice = calc_delta_fair(se->slice, se);      /* 本任务的虚拟请求长度 */

	/* —— 补偿 #1：lag 守恒（SCHED_FEAT(PLACE_LAG, true)）—— */
	if (sched_feat(PLACE_LAG) && cfs_rq->nr_running) {
		struct sched_entity *curr = cfs_rq->curr;
		unsigned long load;

		lag = se->vlag;

		/* 关键：新实体加入会稀释加权平均，先做"反向稀释"修正 */
		load = cfs_rq->avg_load;
		if (curr && curr->on_rq)
			load += scale_load_down(curr->load.weight);

		lag *= load + scale_load_down(se->load.weight);   /* lag' = lag*(W+w_i) */
		if (WARN_ON_ONCE(!load))
			load = 1;
		lag = div_s64(lag, load);                          /* ÷ W */
	}

	se->vruntime = vruntime - lag;                 /* 按修正后的 lag 入位 */

	/* —— 补偿 #2：新任务（fork）的半个请求 —— */
	if (sched_feat(PLACE_DEADLINE_INITIAL) && (flags & ENQUEUE_INITIAL))
		vslice /= 2;                        /* 见下文 */

	/* EEVDF: vd_i = ve_i + r_i/w_i */
	se->deadline = se->vruntime + vslice;
}
```

**为什么需要"反向稀释"**（源码注释的推导，值得逐行看）：

```
设入队前 V = (\Sum w_j v_j)/W。
新实体带 vlag = vl_i 入队后，它自己的 v_i = V - vl_i，于是新的加权平均：

V' = (W*V + w_i*(V - vl_i)) / (W + w_i) = V - w_i*vl_i/(W + w_i)

即 V 向新实体的 vruntime 方向移动了一点。此时新实体的实际 lag：

vl'_i = V' - v_i = vl_i - w_i*vl_i/(W + w_i) < vl_i    （lag 被"稀释"掉一截）

要保持 lag 守恒，入队前应预放大：vl_i = vl'_i * (W + w_i) / W
—— 正是代码里 lag *= (load + w_i); lag /= load;
```

这是 EEVDF 论文没写、但工程上必须处理的细节：**不补偿的话，反复"睡眠-唤醒"的任务每次丢一点 lag，长跑后系统性吃亏**（变成变相的睡眠惩罚）。

**fork 的半个请求（PLACE_DEADLINE_INITIAL）**：新任务的 vruntime 放在 V 处（lag=0），deadline = V + vslice/2——注释原话"existing tasks will be, on average, halfway through their slice"，所以新任务也先排半个队，避免 fork 风暴瞬间插队。`sysctl_sched_child_runs_first`（默认 0）另控制 fork 后父/子谁先跑。

### 4.3 与 CFS 唤醒补偿对比

| | CFS（≤6.5） | EEVDF（6.6） |
| :--- | :--- | :--- |
| 睡眠保存 | 无（睡眠惩罚在唤醒时重算） | `se->vlag` 显式保存（clamp 到 ±2*slice/1tick） |
| 唤醒入位 | `vruntime = max(se->vruntime, cq->min_vruntime - 惩罚/2)` | `vruntime = avg_vruntime - 修正后的lag` |
| 长睡后果 | 惩罚一半 → 睡得越久越吃亏 | lag 已被 clamp，睡多久都一样 |
| 理论性质 | 无守恒保证 | \Sum lag_i = 0（lag 守恒） |

**长睡眠任务醒来不再被歧视**是 6.6 可实测的行为变化：周期性 100ms 醒 0.1ms 的监控任务，6.6 里醒来立刻 eligible（lag≈上限内），在 6.5 里则可能被 min_vruntime 抛离很远后追半天。

---

## 五、选择函数：__pick_eevdf() 与增广红黑树

### 5.1 数据结构：为什么是"增广"

树仍按 `entity_key`（vruntime）排序，但 EEVDF 要查的是"eligible 集合中 deadline 最小"。为此每个节点维护 `se->min_deadline`（以该节点为根的**子树内**最小 deadline），用内核通用增广红黑树框架维护：

```c
RB_DECLARE_CALLBACKS(static, min_deadline_cb, struct sched_entity,
		     run_node, min_deadline, entity_eligible... )

static void __enqueue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	avg_vruntime_add(cfs_rq, se);
	se->min_deadline = se->deadline;
	rb_add_augmented_cached(&se->run_node, &cfs_rq->tasks_timeline,
				__entity_less, &min_deadline_cb);
}
```

插入/删除 O(log n) 顺带把增广字段沿路径重算（`rb_insert_augmented_cached` 的 propagate 回调）。省下每次 pick 时的全树扫描。

### 5.2 __pick_eevdf()：两遍搜索（6.6 精简注释版）

```c
static struct sched_entity *__pick_eevdf(struct cfs_rq *cfs_rq)
{
	struct rb_node *node = cfs_rq->tasks_timeline.rb_root.rb_node;
	struct sched_entity *curr = cfs_rq->curr;
	struct sched_entity *best = NULL;        /* 已见 eligible 中 deadline 最小 */
	struct sched_entity *best_left = NULL;   /* 悬而未决的左子树代表 */

	if (curr && (!curr->on_rq || !entity_eligible(cfs_rq, curr)))
		curr = NULL;
	best = curr;

	/* RUN_TO_PARITY：curr 还有正 lag 未耗完时直接续跑（防 ping-pong） */
	if (sched_feat(RUN_TO_PARITY) && curr && curr->vlag == curr->deadline)
		return curr;

	while (node) {
		struct sched_entity *se = __node_2_se(node);

		/* 不 eligible？eligibility 随 vruntime 单调递增（越左越 eligible），
		   被判不合格时左移大概率仍然不合格，直接下钻左子树 */
		if (!entity_eligible(cfs_rq, se)) {
			node = node->rb_left;
			continue;
		}

		if (!best || deadline_gt(deadline, best, se))
			best = se;                       /* 维护"已见"最优 */

		/* 左子树全体 eligible → 其 min_deadline 可整树代表，
		   记录 best_left 后可直接转入第二阶段 */
		if (node->rb_left) {
			struct sched_entity *left = __node_2_se(node->rb_left);
			if (!best_left || deadline_gt(min_deadline, best_left, left))
				best_left = left;
			if (left->min_deadline == se->min_deadline)
				break;                       /* 最优必在左子树深处 */
		}

		/* 本节点即子树最优（deadline == min_deadline）→ 无需看右子树 */
		if (se->deadline == se->min_deadline)
			break;

		node = node->rb_right;               /* 否则最优在右子树 */
	}

	/* 阶段一找到"整树最优"：best_left 的 min_deadline 不优于 best */
	if (!best_left || (s64)(best_left->min_deadline - best->deadline) > 0)
		return best;

	/* 阶段二：在 best_left 子树内沿 min_deadline 指引下钻 */
	node = &best_left->run_node;
	while (node) {
		struct sched_entity *se = __node_2_se(node);
		if (se->deadline == se->min_deadline)
			return se;
		if (node->rb_left &&
		    __node_2_se(node->rb_left)->min_deadline == se->min_deadline) {
			node = node->rb_left;
			continue;
		}
		node = node->rb_right;
	}
	return NULL;
}
```

算法骨架：**阶段一**在"部分树可能 ineligible"的情况下，边判合格边用增广字段剪枝，找到一个候选下界；**阶段二**在已知"整树 eligible"的子树里纯沿 `min_deadline == 父指引`下钻。均摊 O(log n)。失败兜底 `pick_eevdf()`：打印 `EEVDF scheduling fail` 并回退最左节点（理论上不可达，属防御代码——6.6 早期曾真实触发过，后修复）。

`RUN_TO_PARITY` 的语义（配合 `set_next_entity()` 的 HACK 精确理解）：任务被 pick 时，`set_next_entity` 会把**当时的 deadline 暂存进 vlag**（源码注释："stash a copy of deadline at the point of pick in vlag, which isn't used until dequeue"）。于是 `curr->vlag == curr->deadline` 成立 ⟺ **当前请求（slice）尚未被耗尽**（`update_deadline` 还没分配新 deadline）。此时即使树里出现了 deadline 更早的其他实体，也允许 curr 把本轮请求跑完再让位——避免两个任务在对方 slice 中途反复互相抢占（cache/上下文切换开销放大）。副作用是 vlag 字段的**双重复用**：运行中暂存 deadline，出队时存 lag。

### 5.3 入队与出队路径的完整链条

```
enqueue_task_fair(rq, p, flags)
  └─ for_each_sched_entity(se)                  /* 沿组层级向上 */
       ├─ enqueue_entity(cfs_rq, se, flags)
       │    ├─ update_curr(cfs_rq)              /* 先记账（含 curr 时先 place） */
       │    ├─ update_load_avg(...)             /* PELT 同步（第5章） */
       │    ├─ se_update_runnable / update_cfs_group /* 组权重换算 */
       │    ├─ place_entity(cfs_rq, se, flags)  /* ★ lag 入位（见第四节） */
       │    ├─ account_entity_enqueue            /* load/nr_running 加总 */
       │    └─ __enqueue_entity                  /* 增广树插入 + avg_vruntime_add */
       └─ 检查 throttle / 触发层级传播（h_nr_running 等）
```

出队对称（`dequeue_entity` → `update_entity_lag` 在 sleep 场景保存 vlag → `__dequeue_entity`）。

---

## 六、唤醒抢占：check_preempt_wakeup()

新任务入队后是否立刻抢占 CPU 上的 curr？6.6 的判定收敛为一个问题——**"挑出来的还会是当前任务吗？"**：

```c
static void check_preempt_wakeup(struct rq *rq, struct task_struct *p, int wake_flags)
{
	...
	/* 快速短路：curr 已被标记 resched / curr 是 SCHED_IDLE 且 p 不是 */
	...
	/* BATCH/IDLE 不参与唤醒抢占 */
	if (unlikely(p->policy != SCHED_NORMAL) || !sched_feat(WAKEUP_PREEMPTION))
		return;
	...
	find_matching_se(&se, &pse);              /* 组调度下找公共祖先比较 */
	...
	cfs_rq = cfs_rq_of(se);
	update_curr(cfs_rq);

	/* ★ 6.6 核心判定：让 EEVDF 自己回答"现在该跑谁" */
	if (pick_eevdf(cfs_rq) == pse)
		goto preempt;

	return;

preempt:
	resched_curr(rq);
}
```

对比 6.5 的 `wakeup_preempt_entity()`（比较 vruntime 差与 `wakeup_granularity` 阈值）——那个函数连同阈值参数在 6.6 中被整体删除。**抢占判定与选择判定统一**是 EEVDF 实装的一大架构收益：不再存在"选择说 A、抢占说 B"的二元决策缝隙。

> 细节：`SCHED_BATCH` 的"不唤醒抢占"正由此路径实现（`p->policy != SCHED_NORMAL` 提前返回）——它只放弃**抢占别人的权力**，自身公平性不受影响。

---

## 七、组调度与 CFS bandwidth（索引）

fair 类的公平树可承载 `task_group` 层级（`CONFIG_FAIR_GROUP_SCHED`，cgroup v2 cpu 控制器的内核基础）：

* `tg->shares`（v2 `cpu.weight`，默认 100）→ 组实体的 `load.weight`；`update_cfs_group()` 按"组内活跃度"（`h_nr_running` 与 runnable_weight）把 shares 换算为组 se 的瞬时权重。
* `cpu.max`（quota/period）→ `CFS_BANDWIDTH`：period 计时器（`sched_cfs_period_timer`）刷新配额，`account_cfs_rq_runtime()` 在 `update_curr()` 里扣减，超额的 cfs_rq 整体 throttle（`throttled_hierarchy`）直到下个 period 或 `runtime_remaining` 由 `distribute_cfs_runtime` 补充。机制细节见本目录内核文档 `sched-bwc.rst`。
* EEVDF 对组调度完全透明：组就是一个虚拟任务，其 vruntime/lag/deadline 同样由 `place_entity`/`update_curr` 维护——这是 EEVDF 能一次替换 CFS 的关键工程原因。

---

## 八、行为差异实测建议

在 6.5/6.6 双内核环境对比以下实验（hackbench、 schbench、以及自写 wakeup-latency 微基准）：

```
1) 负载敏感性：hackbench -g 64 -f 24   （任务数 >> 核数）
   6.6 的延迟分布尾部显著收窄（slice 到期即 resched，tick 粒度不再封顶）

2) 短任务延迟：64 个重 CPU 任务 + 1 个每 10ms 醒 0.2ms 的探测任务
   用 runqlat / sched_wakeup→sched_switch tracepoint 度量唤醒延迟

3) nice 语义回归：nice 0 vs nice 5 双任务单核对跑，CPU 比应稳定在 ~2.44:1
   （6.6 与 6.5 都应通过，此实验验证基础公平性没有回归）

4) 睡眠公平：A 任务 busy loop；B 任务每 500ms 睡醒一次
   6.6 中 B 的每次醒来不应出现"追赶期"（对比 /proc/PID/sched 的 vruntime 差）
```

`/proc/<pid>/sched` 的关键字段速查（6.6）：`vruntime`（换算成 ms 的虚拟时刻）、`slice`（当前请求）、`sum_exec_runtime`、`nr_migrations`；`/proc/sched_debug` 的 cfs_rq 段可看 `avg_vruntime`、`min_vruntime`、`.deadline`（树内最左实体的截止期）。

---

## 九、遗留与展望（从 6.6 望向后续）

* 6.6 的 EEVDF 是"最小替换"：slice 还是全局常量。6.7+ 引入 `se->slice` 的可变长（`sched_slice` 按层级/权重细化）与 `DELAY_DEQUEUE`、`ESTIMATED` 等特性开关，方向是进一步压尾延迟。
* `sched_ext`（6.12）：BPF 实现调度类，与 fair 类并存可切换。理解 6.6 EEVDF 是阅读 sched_ext 代码（大量复用 pick/put/place 语义）的前提。
* 多代社区争论的"CPU 紧急度"（`se->util_avg` 参与 pick）尚未进入 fair 主线；util 仍只影响均衡与频率。

---

## 十、思考题

1. `update_deadline()` 在 `cfs_rq->nr_running > 1` 时才 resched。单任务独占核时 slice 到期为什么不需要？提示：没有竞争者时"下一个 EEVDF 最优"仍是它自己。
2. `place_entity` 的反向稀释公式在 `cfs_rq->nr_running == 0` 时为什么不适用（代码用 `if (nr_running)` 跳过）？空队列时 avg_vruntime 的分母 W 是多少？此时直接 `vruntime = min_vruntime - lag` 会有什么问题？提示：min_vruntime 已含所有历史任务的时间。
3. 增广树维护 `min_deadline` 而不是 `min_vlag` 或 `min_key`。若用 min_key（最左键）做增广能替代 entity_eligible 判断吗？提示：eligibility 依赖的是"加权平均位置"而非树序，两者不等价。
4. `RUN_TO_PARITY` 关掉（sched_features 可运行时改）会观察到什么现象？什么 workload 受益于关掉它？提示：两个高权重任务对跑时的上下文切换频率。
5. CFS bandwidth 的 throttle 使组实体整体出队（`throttle_cfs_rq`）。被 throttle 的组的 lag 保存在哪里？解除 throttle 时它的公平性如何恢复？对照 `unthrottle_cfs_rq` 路径回答。

---

> 下一章：[调度时机与上下文切换](04-调度时机与上下文切换.md) —— 从 `try_to_wake_up()` 到 `switch_to()` 汇编边界。
