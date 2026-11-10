# work 生命周期与 flush/cancel（Linux 6.6）

> 阅读目标：跟随一个 work 走完全程——提交侧 `INIT_WORK → queue_work_on → __queue_work` 的 pwq 选择、last_pool 粘性亲和、max_active 限流分叉；执行侧的摘取与顺序语义；然后吃透两个最精微的 API 家族：cancel（`try_to_grab_pending` 的偷取协议、`cancel_work_sync` 的等待语义）与 flush（6.6 的 **work color 机制仍然在用**——`flush_workqueue` 是颜色级联 + `wq_flusher` 链，`flush_work` 是 `wq_barrier` 插队，两条路线并存）。所有代码已对照 6.6 源码核对。

---

## 一、提交路径全链

### 1.1 从宏到函数

```c
INIT_WORK(work, fn);                          /* __INIT_WORK: data=NO_POOL,
                                                   entry 自环, func=fn, lockdep */
DECLARE_WORK(name, fn);                       /* 静态版，多一个 STATIC 位 */
queue_work(wq, work)                          /* 头文件内联 */
  └─ queue_work_on(WORK_CPU_UNBOUND, wq, work)   /* = NR_CPUS：未指定 CPU */
       └─ local_irq_save + test_and_set_bit(PENDING)   /* ★ 原子抢所有权 */
            └─ __queue_work(cpu, wq, work)    /* 仅当 PENDING 原为 0 */
```

**为什么全程关中断**（`local_irq_save`）：PENDING 抢占成功到 work 真正挂上链表之间存在窗口——期间本 CPU 的中断若对同一 work 调 `cancel_delayed_work`，`try_to_grab_pending` 会在窗口里死等（`-EAGAIN` 忙循环）；关中断把这个窗口的有界性收窄到本上下文。`__queue_work` 开头的 `lockdep_assert_irqs_disabled()` 是这条契约的执法器。

### 1.2 __queue_work：三段式（6.6 精简）

```c
static void __queue_work(int cpu, struct workqueue_struct *wq,
			 struct work_struct *work)
{
	/* ① draining/destroying 的 wq 只放行 chained work（本 wq 的 work 里
	   再 queue 本 wq），外部投递直接丢弃 + WARN_ON_ONCE */
	if (unlikely(wq->flags & (__WQ_DESTROYING | __WQ_DRAINING) &&
		     WARN_ON_ONCE(!is_chained_work(wq))))
		return;
	rcu_read_lock();
retry:
	/* ② 选 pwq：per-cpu wq → 本 CPU 的 pwq；unbound → wq_select_unbound_cpu */
	if (req_cpu == WORK_CPU_UNBOUND) {
		if (wq->flags & WQ_UNBOUND)
			cpu = wq_select_unbound_cpu(raw_smp_processor_id());
		else
			cpu = raw_smp_processor_id();
	}
	pwq = rcu_dereference(*per_cpu_ptr(wq->cpu_pwq, cpu));
	pool = pwq->pool;

	/* ③ last_pool 粘性：work 上次的池 ≠ 本次池时查防重入 */
	last_pool = get_work_pool(work);          /* data 高位的 pool id */
	if (last_pool && last_pool != pool) {
		raw_spin_lock(&last_pool->lock);
		worker = find_worker_executing_work(last_pool, work);
		if (worker && worker->current_pwq->wq == wq) {
			pwq = worker->current_pwq;    /* ★ 改投到旧池 */
			pool = pwq->pool;
		} else {
			raw_spin_unlock(&last_pool->lock);
			raw_spin_lock(&pool->lock);
		}
	} else
		raw_spin_lock(&pool->lock);

	/* unbound pwq 可能恰好已被属性更新替换释放 → 重选 */
	if (unlikely(!pwq->refcnt)) { ... goto retry; }

	/* ④ 入队：双路分叉 */
	pwq->nr_in_flight[pwq->work_color]++;
	work_flags = work_color_to_flags(pwq->work_color);

	if (likely(pwq->nr_active < pwq->max_active)) {
		if (list_empty(&pool->worklist))
			pool->watchdog_ts = jiffies;    /* watchdog 起表 */
		trace_workqueue_activate_work(work);
		pwq->nr_active++;
		insert_work(pwq, work, &pool->worklist, work_flags);
		kick_pool(pool);                    /* 唤醒 idle worker */
	} else {
		work_flags |= WORK_STRUCT_INACTIVE;
		insert_work(pwq, work, &pwq->inactive_works, work_flags);
	}
	raw_spin_unlock(&pool->lock);
	rcu_read_unlock();
}
```

**last_pool 粘性的双重目的**（源码注释原文："it might still be running there, in which case the work needs to be queued on that pool to guarantee non-reentrancy"）：

1. **非重入**：work 上一次执行还没结束（busy_hash 可查），新一次排队必须路由回那个池——否则同池防重入（第 1 章 `assign_work`）就管不到它。
2. **cache 局部性**：`WORK_CPU_UNBOUND` 时 per-cpu wq 取的是**本次提交 CPU**，但如果 work 还在旧 CPU 的池里跑，路由回旧池让两轮执行共享热 cache。`data` 高位存 pool id 正是为零成本回答"我上辈子在哪个池"。

**双路分叉**：`nr_active < max_active` → 直接进 `pool->worklist`（active，占并发名额）；否则进 `pwq->inactive_works`（INACTIVE 位，不占名额）。这是 6.5 引入的 inactive_works 方案：限流从"挂在 per-wq 的 delayed 队列"改为 pwq 一条 FIFO，激活（`pwq_activate_first_inactive`）发生在执行完成（`pwq_dec_nr_in_flight`）时——补位严格 FIFO，保序性比老方案好。

### 1.3 queue_work 的返回值与幂等

```c
bool queue_work_on(int cpu, struct workqueue_struct *wq, struct work_struct *work)
{
	bool ret = false;
	local_irq_save(flags);
	if (!test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work))) {
		__queue_work(cpu, wq, work);
		ret = true;          /* 抢到所有权：这次提交生效 */
	}
	local_irq_restore(flags);
	return ret;              /* false = 已 pending/执行中，本次提交是空操作 */
}
```

三点语义务必记牢：

- **PENDING 从入队一直保持到执行开始**（`process_one_work` 里才清）。所以"执行中"的 work 也抢不到 PENDING——`queue_work` 返回 false。
- **幂等 ≠ 丢弃**：返回 false 的那次调用**没有**重排（不改变现有排队位置，头文件 DocBox 明言 "leaves it in the same position"），已排队的那次会执行。
- **内存序保证**（头文件 DocBook 原文）：返回 true 时，调用点之前的所有 store 对执行该 work 的 CPU 可见——由 `test_and_set_bit` 的隐含 mb 与执行侧清 PENDING 的 mb 配对（第 1 章 §2.1）。

### 1.4 delayed_work：timer 的封装

```c
struct delayed_work {
	struct work_struct work;
	struct timer_list timer;          /* TIMER_IRQSAFE！ */
	struct workqueue_struct *wq;      /* 到期时投递目标 */
	int cpu;                          /* 到期时投递 CPU */
};
```

```
queue_delayed_work_on(cpu, wq, dwork, delay)
  └─ 抢 PENDING → __queue_delayed_work:
       delay==0 → __queue_work(cpu, wq, work)   /* ★ 直通：不走 timer，
                                                    避免最坏一个 tick 的错拍 */
       else     → dwork->wq=wq; dwork->cpu=cpu;
                  timer->expires = jiffies + delay;
                  cpu==UNBOUND ? add_timer(timer) : add_timer_on(timer, cpu)

timer 到期（硬中断，IRQSAFE）:
  delayed_work_timer_fn(t)
    └─ __queue_work(dwork->cpu, dwork->wq, &dwork->work)   /* irq 已关 */
```

- **IRQSAFE 的意义**：到期回调与 `try_to_grab_pending` 的 `del_timer` 竞争——irqsafe timer 保证 `del_timer()` 失败即"已到期且不在任何队列"，cancel 侧据此干净地拿走 PENDING。
- **`add_timer_on`**：delayed work 的"目标 CPU"在**入队时**锁定（timer 挂到那个 CPU 的 base），到期后 `__queue_work` 用 `dwork->cpu`——per-cpu wq 的延迟 work 会精准落回指定 CPU。
- **`mod_delayed_work_on`**：`try_to_grab_pending` 循环（含 timer 窃取）成功后重新 `__queue_delayed_work`——"改期或（重新）提交"合二为一；从任何上下文（含 IRQ）可调。
- **`queue_rcu_work`**：`call_rcu_hurry` + 宽限期后 `__queue_work`——"等所有读者放下旧数据再执行"，是 RCU 与 workqueue 的官方粘合。

---

## 二、执行侧：摘取、顺序与超时警戒

### 2.1 顺序语义全景

| 维度 | 保证 | 机制 |
| :--- | :--- | :--- |
| 同一 wq 同一 pwq 的多个 work | **FIFO**（active 与 inactive 各自 FIFO，衔接处 FIFO） | worklist 头取尾插 + inactive_works 补位 |
| 同一池上**不同** wq 的 work | FIFO 混排，无 wq 间保证 | 池共享（CMWQ 本意） |
| 同一 work 的两次提交 | 永不并发，至多一个实例在跑 | PENDING + busy_hash + last_pool 三重锁 |
| ordered wq | 全局严格 FIFO 串行 | max_active=1 且唯一 pwq |
| linked works（barrier 及后随） | 与目标 work 同 worker 接续执行 | `WORK_STRUCT_LINKED` 成组搬运 |

一个容易踩的坑：**同 wq 两个 work 的先后不等于完成先后**——它们可能被派给不同 worker 并行（max_active 允许内），A 先跑 B 后完成完全正常。需要"A 完成后才 B"要么用 ordered wq，要么 A 的函数末尾投递 B（chained）。

### 2.2 watchdog 时间戳

`pool->watchdog_ts` 在两个时刻刷新：worklist 从空变非空（`__queue_work`/`pwq_activate_inactive_work`）、worker 领到第一批活（`process_scheduled_works` 的 first）。它就是 watchdog 的"前进心跳"——第 1 章观测表的 lockup 告警判据。执行侧对它的贡献只有领工那一刻，**单个 work 跑 30s 不结束不告警**（那不是池停摆，是 work 慢，该找 CPUitsv/tracepoint）。

---

## 三、cancel 家族：偷、标记、等

### 3.1 try_to_grab_pending：五态偷取协议

`try_to_grab_pending(work, is_dwork, &flags)` 是所有 cancel 的地基（6.6 返回值表照录）：

```
返回  1  ：work 已 pending，被我从 timer 或 worklist 上偷走（irq 已关，PENDING 归我）
返回  0  ：work 空闲，我抢占了 PENDING（假装它 pending，防并发提交）
返回 -EAGAIN：瞬时抢不到（正在搬运/正被执行等）——忙重试，有界时间
返回 -ENOENT：别人正在 cancel 它（CANCELING 位）——该状态可持续任意久！
```

```
local_irq_save
├─ is_dwork && del_timer(timer) 成功 ──────────────► return 1（从 timer 上偷走）
├─ !test_and_set_bit(PENDING) ─────────────────────► return 0（空闲占用）
└─ 已 pending：get_work_pool → raw_spin_lock(pool->lock)
     ├─ data 指向本池 pwq（在队列上）：
     │    INACTIVE 位先 pwq_activate_inactive_work（防带 linked barrier 的
     │    inactive work 被直偷后 nr_active 记账错乱）
     │    list_del_init + pwq_dec_nr_in_flight
     │    set_work_pool_and_keep_pending(pool->id) ─► return 1
     └─ 否则（正在执行/已搬运）：放锁、查 CANCELING 位、
          cpu_relax ───────────────────────────────► return -EAGAIN / -ENOENT
```

### 3.2 cancel_work_sync：三段式等待

```c
static bool __cancel_work_timer(struct work_struct *work, bool is_dwork)
{
	do {
		ret = try_to_grab_pending(work, is_dwork, &flags);
		if (unlikely(ret == -ENOENT)) {
			/* 别人在 cancel 同一 work：在 cancel_waitq 上排独占等待，
			   自定义 wake 函数按 work 指针匹配——防惊群 */
			prepare_to_wait_exclusive(&cancel_waitq, &cwait.wait,
						  TASK_UNINTERRUPTIBLE);
			if (work_is_canceling(work)) schedule();
			finish_wait(&cancel_waitq, &cwait.wait);
		}
	} while (unlikely(ret < 0));

	mark_work_canceling(work);       /* data 置 CANCELING：警告后来者 */
	local_irq_restore(flags);

	if (wq_online)
		__flush_work(work, true);  /* ★ 唯一的睡眠点：见 §4.3 */
	clear_work_data(work);           /* data 复位 NO_POOL，PENDING 归零 */
	smp_mb();
	if (waitqueue_active(&cancel_waitq))
		__wake_up(&cancel_waitq, TASK_NORMAL, 1, work);  /* 传递接力棒 */
	return ret;      /* true=原本 pending（被我取消）；false=原本空闲 */
}
```

**语义要点**：

- 返回后保证 work **不 pending 且不执行**（除非它在此期间又被别人重新 queue——API 无法替你锁住世界，需要的话调用方自持锁）。
- "-ENOENT 等待"解决的是**多个 canceler 汇聚**：第二个 canceler 等第一个完成并接过完成通知。注释里的 PREEMPT_NONE 推演（flush 会假成功、grab 会 -ENOENT、CPU 被我占着对方清不了 CANCELING）是理解为什么用 waitqueue 而非 flush 的好材料。
- **自续 work 的正确取消姿势**：`cancel_work_sync` 对"函数里再 queue 自己"的 work 依然收敛——因为 canceler 持有 CANCELING 状态时，work 内的 `queue_work(self)` 会成功抢到 PENDING 并重新入队！**必须在 work 函数里配条件开关**（`if (!cancelling) queue_work(...)`），否则 cancel 返回后它又活了。文档明确把这列为调用方责任。

**原子上下文边界**：

| API | 可用上下文 | 睡眠点 |
| :--- | :--- | :--- |
| `cancel_work` / `cancel_delayed_work`（非 sync，6.6 新增前者） | **任意**（含 IRQ） | 无（grab 失败即返回 false） |
| `mod_delayed_work_on` | 任意 | 无 |
| `cancel_work_sync` / `cancel_delayed_work_sync` | 仅进程上下文 | `__flush_work` 的 completion |
| `flush_work` / `flush_workqueue` | 仅进程上下文 | 同上 |

非 sync 版只保证"不再排队"不保证"不在跑"——注释明言"The work callback function may still be running on return"。

---

## 四、flush 家族：6.6 的双轨制

> ⚠️ 先纠正一个流传甚广的说法：**6.6 的 work color 并没有被删**。`pool_workqueue` 仍有 `work_color/flush_color/nr_in_flight[16]`，`workqueue_struct` 仍有 `first_flusher/flusher_queue/flusher_overflow`。颜色机制要到 6.9 才被 barrier 方案替换。6.6 的现状是**双轨**：整队 flush 走颜色，单个 flush 走 barrier。

### 4.1 颜色模型速览

每个 work 入队时打上 pwq 当前 `work_color`（16 色循环取模），`nr_in_flight[color]` 计数。`flush_workqueue` 的目标可以表述为："**所有在我调用之前入队的 work 都执行完**"。实现：flusher 声明"当前色"为自己的 `flush_color`，把 wq 的 `work_color` 推进一格（新 work 换色）；每个 pwq 等到 `nr_in_flight[flush_color]` 归零即完成；全部 pwq 完成 → `nr_pwqs_to_flush` 归零 → complete 首个 flusher。**同一时刻 wq 最多推进 14 个未完成颜色**（16 色扣除当前/flush 两个），不够时后来的 flusher 进 `flusher_overflow` 等待——这是 flusher_queue/overflow 两条链存在的原因。

### 4.2 __flush_workqueue：三幕剧（6.6 骨架）

```
第一幕【选色入队】（wq->mutex 持有）
  next_color = work_color+1
  next_color != flush_color（色未用尽）？
    ├─ 是：this_flusher.flush_color = wq->work_color; wq->work_color = next_color
    │     无 first_flusher？→ 我当 first_flusher，
    │       flush_workqueue_prep_pwqs(wq, flush_color, work_color):
    │         对每个 pwq：nr_in_flight[flush_color]>0 → pwq->flush_color=flush_color,
    │                                              nr_pwqs_to_flush++
    │         （全为 0 → 无需等待，立即完成——"nothing to flush"快路径）
    │     有 → 挂 flusher_queue 排队（同色可多 flusher）
    └─ 否（色用尽）：挂 flusher_overflow
  check_flush_dependency()   /* lockdep：PF_MEMALLOC/MEM_RECLAIM 依赖环预警 */
  mutex_unlock
第二幕【等待】
  wait_for_completion(&this_flusher.done)
  ← 完成信号链：最后一个 in-flight 色归零的 pwq → pwq_dec_nr_in_flight
      → atomic_dec_and_test(nr_pwqs_to_flush) → complete(first_flusher)
  非 first_flusher 到此返回
第三幕【级联】（first_flusher 专属，重新拿 mutex）
  while (true) {
      complete 所有与当前 flush_color 同色的 flusher_queue 等待者
      flush_color 推进一格
      flusher_overflow 非空 → 全部赋新色、work_color 再推进、转入 flusher_queue
      flusher_queue 空 → 收工
      否则 → 队头升任新 first_flusher，prep_pwqs 武装其颜色；
             该色已空 → 立即完成，继续级联
  }
```

**为什么后到的 flusher 能"搭车"**：它等的是更晚的颜色（覆盖面是超集），所以完成通知可以由级联链统一派发——无数 flusher 的等待被压缩成 O(颜色数) 的接力。`drain_workqueue()` 在此之上再套一层循环：置 `__WQ_DRAINING`（拒外来新 work，只放行 chained）→ 反复 `__flush_workqueue` 直到每个 pwq `nr_active==0 && inactive_works` 空 → 10 次未净开始 warn（10、100、…1000 次）。`destroy_workqueue` 的排空正是它（第 4 章）。

### 4.3 flush_work：wq_barrier 插队（单 work 屏障）

```c
struct wq_barrier {
	struct work_struct work;        /* 匿身成普通 work */
	struct completion done;
	struct task_struct *task;       /* 纯记录：谁在等 */
};
static void wq_barrier_func(struct work_struct *work)   /* 执行=complete */
{ complete(&barr->done); }
```

```
flush_work(work)
  └─ start_flush_work:
       get_work_pool → pool->lock
       work 还在队列上（data 指向本池 pwq）？
         └─ pwq = 该 pwq；insert_wq_barrier(pwq, barr, work, worker=NULL)
            插入点 = target->entry.next（紧跟目标 work）
            继承目标 LINKED 位并给目标置 LINKED → 成组搬运不散架
       work 正在执行？（busy_hash 查到 worker）
         └─ pwq = worker->current_pwq；insert_wq_barrier(..., worker=worker)
            插入点 = worker->scheduled.next → 排进执行者本人的接力队
       两头都扑空 → already_gone：work 已完成，flush_work 返回 false
  wait_for_completion(&barr.done)
```

细节三则（源码注释均有明示）：

- **barrier 不占并发名额**：`work_flags |= WORK_STRUCT_INACTIVE`——注释 "The barrier work item does not participate in pwq->nr_active"。它只在被搬运到 worklist/scheduled 时才真正跑，INACTIVE 位防止它干扰 max_active 记账（这也解释了 6.6 里"INACTIVE 的 work 不一定在 inactive_works 上"这个注释怪谈）。
- **排进的 barrier 不可取消**：`try_to_grab_pending` 无法判断带 LINKED 的链中段 work 是否可安全摘除——所以 flush 期间被 cancel 的目标 work（`__cancel_work_timer → __flush_work(work, true)`）同样用 barrier 等执行完毕，这是 cancel 的"等"的实现。
- **颜色继承**：barrier 取目标 work 的 color（执行中则 `worker->current_color`），保证它不把 pwq 的 flush 色计数挂账挂错。

`flush_delayed_work`：先 `del_timer_sync`（成功则立刻 `__queue_work` 提前兑现）再 `flush_work`——语义"把延迟 work 立即兑现并等它跑完"。`flush_rcu_work`：pending 则先 `rcu_barrier()` 等宽限期再 flush。

### 4.4 API 选型表

| 需求 | 首选 | 备注 |
| :--- | :--- | :--- |
| 顺手提交一个短活 | `schedule_work(work)` | = `queue_work(system_wq, work)`；别提交长活（见第 4 章） |
| 指定 CPU | `schedule_work_on(cpu, work)` / `queue_work_on` | 调用方保证 CPU 在线 |
| 延迟/周期 | `queue_delayed_work` + `mod_delayed_work` | mod 兼具改期与防重 |
| 等"我提交的这单"跑完 | `flush_work(work)` | 返回 false=早已跑完 |
| 等"这个 wq 全部历史订单" | `flush_workqueue(wq)` | **自建 wq 才允许**；flush system_wq 会触发编译期/运行时警告（`__warn_flushing_systemwide_wq`） |
| 撤单 | `cancel_delayed_work`（原子上下文）/ `cancel_work_sync`（进程上下文） | 自续 work 必须配合条件开关 |
| 卸载前清空 | `destroy_workqueue`（内含 drain） | 别手动 drain 后再 destroy |

两个"混合形态"辅助 API（同文件导出，生命周期视角值得一并记住）：

- `execute_in_process_context(fn, ew)`：**进程上下文就地执行，否则排队**——中断里调则 `schedule_work`（返回 1），进程里调则直接调 fn（返回 0）。适合"能同步就同步、不能就异步"的收尾逻辑，免于判上下文的样板代码。
- `work_on_cpu(cpu, fn, arg)`：把 fn 派到指定 CPU 的 system_wq 上执行并**同步等结果**（返回 fn 的返回值）；`work_on_cpu_safe` 版自带 `cpus_read_lock` 防 CPU 下线。本质是"借 workqueue 车道跑一次跨 CPU 调用"——持锁约束与 flush_work 同级（不许持会阻塞 fn 的锁）。

---

## 五、观测：sysfs 与 tracepoint

### 5.1 sysfs（真实路径，已核对 6.6）

> 📌 注意：workqueue 的 sysfs **不在** `/sys/kernel/workqueue`（那是网络上陈年资料的常见讹传），自 3.9 起就是总线设备模型：

```
/sys/bus/workqueue/devices/<wq_name>/      # 仅 WQ_SYSFS 的 wq 出现
    per_cpu            # RO: 1=per-cpu wq
    max_active         # RW: 可运行时调整（ordered wq 拒绝）
    nice               # RW (unbound): worker 的 nice
    cpumask            # RW (unbound): worker 允许的 CPU
    affinity_scope     # RW (unbound): default/cache/numa/...（第 3 章）
    affinity_strict    # RW (unbound): 是否严格限制在 scope 内

/sys/devices/virtual/workqueue/cpumask     # RW: 全局 unbound 池总开关
/sys/module/workqueue/parameters/          # 模块参数（部分依赖 CONFIG）
    watchdog_thresh         # WQ_WATCHDOG: 默认 30（秒），0=禁用，运行时可改
    cpu_intensive_thresh_us # 自动 CPU-intensive 阈值，默认 10ms（按 BogoMIPS 放大）
    default_affinity_scope  # 6.6: 默认 affinity scope，默认 cache
    power_efficient         # RO: WQ_POWER_EFFICIENT 是否激活
    debug_force_rr_cpu      # DEBUG_WQ_FORCE_RR_CPU: 强制 unbound 轮转
```

### 5.2 tracepoint（4 个，`include/trace/events/workqueue.h`）

| 事件 | 字段 | 用途 |
| :--- | :--- | :--- |
| `workqueue:workqueue_queue_work` | work, function, req_cpu, cpu(实际池), pwq_pool_id | 提交审计：谁在往哪个池投什么 |
| `workqueue:workqueue_activate_work` | work, function | 从 inactive 转 active（限流放行时刻） |
| `workqueue:workqueue_execute_start` | work, function | worker 开始执行 |
| `workqueue:workqueue_execute_end` | work, function | 执行结束 |

**实战：找出哪个 work 占 worker 最久**（bpftrace 一行流）：

```
bpftrace -e '
tracepoint:workqueue:workqueue_execute_start { t[args->work] = nsecs }
tracepoint:workqueue:workqueue_execute_end /t[args->work]/ {
    $us = (nsecs - t[args->work]) / 1000;
    @dur_us[args->function] = max($us); @cnt[args->function] = count();
    delete(t[args->work]); }'
```

perf 版（离线分析，事件语义相同）：`perf record -e workqueue:workqueue_execute_start -e workqueue:workqueue_execute_end -a -- sleep 60 && perf script`，再按 work 指针列配对求差。

注意 `execute_start/end` 之间 work 可能**睡眠**（等 IO/锁）——差值是"驻留时间"而非纯 CPU 时间；要区分二者，配 `wq_monitor.py` 的 CPUtime/CPUitsv 列，或对 kworker pid 采 `sched:sched_stat_runtime`。**inactive 等待时间**用 `queue_work → activate_work` 的差值度量（`perf record -e workqueue:*` 后 `perf script` 自行配对）。

**hung worker 联动**：hung task detector（默认 120s）会对 D 状态 kworker 打印 backtrace，其输出自动带 `print_worker_info` 的 `Workqueue: <wq> <fn>` 行——两个子系统在这里天然联手；而 wq 自己的 watchdog（30s，watchdog_thresh 可调）盯的是**池停摆**而非单 work 慢，注意甄别。

---

## 六、思考题

1. `queue_work` 返回 false 有哪三种可能状态？对"提交后必须执行一次"的驱动逻辑（如中断下半部合并），返回 false 是否意味着可以安心不处理？提示：执行中 vs 排队中 vs 在 timer 上。
2. last_pool 粘性在 work **执行结束后**仍把下次排队拉回旧池（pool id 留在 data 里）。这带来什么收益、什么风险？什么时候 data 里的 pool id 才会被清掉？提示：`clear_work_data` 的调用者。
3. 6.6 的 `cancel_work`（非 sync 版）为什么是 6.5 才姗姗来迟的新 API？对比 `cancel_delayed_work` 已经存在多年——从 `try_to_grab_pending` 的 `is_dwork` 分支找原因。
4. flush_workqueue 的颜色用尽（16 个未完成的 flush 色并列）时，新 flusher 进 overflow 队等待。构造这个场景需要什么样的负载？overflow 中的 flusher 与 flusher_queue 中的在完成顺序上有何不同待遇？
5. `flush_work` 对"正在执行"的 work 把 barrier 插进 `worker->scheduled.next`。若该 work 函数随后又往同一 worker 的 scheduled 链上 punt 了新 work（busy_hash 冲突路径），barrier 与新 work 的相对位置如何？flush 的"之后"语义是否仍然成立？
6. 用 `perf script` 配对 queue_work/execute_start 两个事件，统计你的内核里 system_wq 上 work 从提交到开跑的分位数延迟。哪个分位最能暴露"限流（inactive）等待"？提示：P99 与 mean 的差。

---

> 下一章：[unbound 池与 affinity scope](03-unbound池与affinity-scope.md) —— 长任务的归宿：attrs 如何索引池、6.6 的 CPU pod/affn_strict 新模型、以及 unbound_cpus 的治理链。
