# CMWQ 设计与 worker pool（Linux 6.6）

> 阅读目标：吃透 workqueue 子系统的设计动机与骨架——从 legacy workqueue 的困境理解 CMWQ（Concurrency Managed Workqueue，2.6.36，Tejun Heo）要解决什么；逐字段精读核心四件套 `work_struct` / `pool_workqueue` / `worker_pool` / `worker`（真实定义，已核对 6.6 源码）；掌握 `worker_thread()` 主循环与 `process_one_work()` 的执行协议；理解"并发管理"这一核心算法——池里的 worker 何时生、何时死、何时多睡一个；最后理清 worker 与调度器的共生关系。

---

## 一、演进史：为什么需要 CMWQ

### 1.1 从 keventd 到 legacy workqueue

内核从 2.5 时代就有"异步执行上下文"的需求：中断下半部、延迟处理、进程上下文回调。最早的实现是 2.4 的 keventd（Ingo Molnar，2002）——一个内核线程跑一个全局任务队列。2.5/2.6 演化出 `workqueue` API：`INIT_WORK()` + `schedule_work()` 把 `work_struct` 排到队列上，由 worker 线程在**进程上下文**执行——这是 workqueue 与 tasklet/softirq 的本质分界（后两者运行在原子上下文，不能睡眠）。

legacy 时代（2.6.0 ~ 2.6.35）的使用方式：

```c
/* 三种"工厂"，产出三类线程模型 */
create_workqueue(name);        /* MT（multi-thread）：每个 CPU 一个专属线程 */
create_singlethread_workqueue(name); /* ST（single-thread）：全系统一个线程 */
schedule_work(work);           /* 排到全局 keventd 队列（后来的 system_wq）*/
```

每个 wq 维护**自己私有**的 worker 池——这是问题的根源。

### 1.2 三重困境

**① 线程爆炸**：MT wq 每个 CPU 一个线程。内核里几十个 MT wq 用户 × 64 核机器 = 数千 kworker。`Documentation/core-api/workqueue.rst`（6.6 自带文档）原话：*"some systems saturated the default 32k PID space just booting up"*——开机就把 32k 的 PID 空间吃光。

**② 并发度反而不足**：私有池意味着互相隔离——ST wq 全系统只有一个执行上下文，MT wq 每 CPU 只有一个。任何一个 work 睡眠（等 IO、等锁），它身后的整条队列全部堵死。libata 曾被迫选 ST wq 跑轮询 PIO，并接受"任意时刻只能有一个轮询 PIO 在推进"的限制——文档明确以它为反面教材。

**③ 冻结与死锁**：suspend 时 freezer 冻结线程，而 wq 用户大多没标 freezable；更糟的是"执行上下文不够"天然滋生死锁（详见第 4 章）：work A 等 work B，而 B 排在同一个执行上下文里被 A 堵着。

### 1.3 CMWQ 的答案（2.6.36，Tejun Heo）

CMWQ 把模型倒转：**池与队列分离、池全局共享、并发按需伸缩**。

```
legacy:   workqueue ──── 私有 worker 池（线程数创建时固定）
CMWQ:     workqueue ── pwq ──> 共享 worker_pool <── pwq ── workqueue
                                          ↑
                              池内 worker 数随负载动态伸缩
```

四个设计支柱：

1. **worker_pool 与 workqueue 解耦**：每个 CPU 恒有两个标准池（普通/高优先级，`NR_STD_WORKER_POOLS = 2`）；unbound 池按需动态创建。所有 wq 通过 `pool_workqueue`（pwq）"租用"池。
2. **并发管理（concurrency management）**：池的 `nr_running` 追踪"没有睡眠的 worker 数"。per-CPU 池默认**只保持一个 running worker**——work 睡了，池自动唤醒/创建下一个 worker 接力。并发度 = "正在干活且不睡眠"的数量，天然贴合负载。
3. **按需生灭**：有活且无人可干 → 建线程；空闲 5 分钟 → 自杀（`IDLE_WORKER_TIMEOUT = 300*HZ`）。
4. **max_active 限流**：每个 pwq 限制同时在执行的 work 数（默认 256），既防失控又保序。

> 📌 CMWQ 之后 API 基本没再变过：`alloc_workqueue()` 是唯一正门；`create_*_workqueue()` 宏仍在头文件里，但被标记 legacy（`__WQ_LEGACY`），新代码禁用。6.6 的演进集中在 unbound 亲和（第 3 章）与执行可观测性（wq_monitor.py）。

---

## 二、核心对象四件套（真实定义，`kernel/workqueue.c` 6.6）

### 2.1 work_struct：一个原子长整型装下全部状态

```c
struct work_struct {                 /* include/linux/workqueue.h */
	atomic_long_t data;          /* 编码见下，PENDING/pwq 指针/pool id */
	struct list_head entry;      /* 挂 pool->worklist / pwq->inactive_works
	                                    / worker->scheduled 的链表节点 */
	work_func_t func;            /* 执行函数 */
#ifdef CONFIG_LOCKDEP
	struct lockdep_map lockdep_map;  /* "(work_completion)" 死锁检测用 */
#endif
};
```

`data` 一个字长（64 位）的编码是 workqueue 最精巧的部分（`WORK_STRUCT_*` 位定义，6.6）：

```
                     在队列中（WORK_STRUCT_PWQ=1）          离队后（PWQ=0）
 bit:  63 ............ 8 | 7  6  5  4 | 3  2  1 | 0        63 ..... 5 | 4 | 0
      ┌─────────────────┬────────────┴───┬──────┴───┐     ┌───────────┴───┬───┐
      │  pwq 指针（256B │  work color    │LK│PWQ│IA│PEND│     │ pool id      │CAN│ 0
      │  对齐，低 8 位空）│  (4bit,16 色)  │  │  │  │   │     │ (≤31bit)     │   │
      └─────────────────┴────────────────┴──┴──┴──┴───┘     └──────────────┴───┴───┘
        WORK_STRUCT_FLAG_MASK = 低 8 位            WORK_OFFQ_CANCELING(bit4)
        （开 DEBUG_OBJECTS_WORK 时 color 左移 1bit、bit4=STATIC）
```

- **bit0 `WORK_STRUCT_PENDING`**：排队/执行中的"所有权锁"。`queue_work()` 用 `test_and_set_bit` 抢占它——抢不到说明已在队列里，直接返回 `false`。这就是 queue 的幂等语义。
- **bit1 `WORK_STRUCT_INACTIVE`**（6.5 引入）：work 在 `pwq->inactive_works` 上排队（超过 max_active 限流）。注意与"在 pool->worklist 上"互斥。
- **bit2 `WORK_STRUCT_PWQ`**：data 里是 pwq 指针（在队列上）还是 pool id（离队）。
- **bit3 `WORK_STRUCT_LINKED`**：本 work 后面链着 linked work（如 flush 的 barrier），搬运时须成组（`move_linked_works()`）。
- **color bits（bit4~7）**：16 色循环计数，**仅服务于 flush_workqueue 的屏障语义**（6.6 仍然存在，第 2 章详解；6.9 起才被移除）。
- **离队后**：bit4 变 `WORK_OFFQ_CANCELING`（正在被 cancel，别人不得偷 PENDING）；高位存**最后所在 pool 的 id**——这是跨池重入防护与亲和粘性的关键（第 2 章）。

`set_work_pool_and_clear_pending()` 清 PENDING 时的两道显式屏障（`smp_wmb()` + `smp_mb()`）与 `queue_work()` 的 `test_and_set_bit` 隐含 mb 配对——源码里有 30 行注释推演"投机 LOAD 导致漏执行"的竞态，是内核内存序教学的经典案例。

### 2.2 pool_workqueue：wq 与池的"租约"

```c
struct pool_workqueue {               /* kernel/workqueue.c，6.6 */
	struct worker_pool	*pool;     /* I: 关联的池 */
	struct workqueue_struct *wq;  /* I: 所属 wq */
	int		work_color;      /* L: 当前颜色 */
	int		flush_color;     /* L: 正在 flush 的颜色，-1=无 */
	int		refcnt;          /* L: 引用计数（挂队/执行各持一份）*/
	int		nr_in_flight[WORK_NR_COLORS]; /* L: 每色在飞 work 数 */

	int		nr_active;       /* L: 当前执行中的 work 数（限流水位）*/
	int		max_active;      /* L: 执行上限（= wq->saved_max_active）*/
	struct list_head inactive_works; /* L: 溢出排队区（超 max_active）*/

	struct list_head pwqs_node;  /* WR: 挂 wq->pwqs 链 */
	struct list_head mayday_node;/* MD: 挂 wq->maydays（求救链）*/

	u64		stats[PWQ_NR_STATS]; /* 6.6: wq_monitor.py 的数据源 */
	struct kthread_work release_work; /* pwq 的延迟释放（bounce 到专用线程）*/
	struct rcu_head	rcu;
} __aligned(1 << WORK_STRUCT_FLAG_BITS);   /* ★ 256B 对齐——data 低 8 位放标志 */
```

pwq 是限流与隔离的单位：**同一 wq 在不同池上的 max_active 各自独立**。`nr_active >= max_active` 时新 work 进 `inactive_works`（带 `WORK_STRUCT_INACTIVE` 位），执行中的 work 结束（`pwq_dec_nr_in_flight()`）时按 FIFO 补位。ordered wq 的"串行"正是靠 max_active=1 + 全系统唯一 pwq 实现的。

> ⚠️ **work_color/flush_color/nr_in_flight[] 在 6.6 仍然存在**——网络上大量资料说"color 已删"，那是 6.9 的变更。6.6 的 `__flush_workqueue()` 完整保留颜色级联协议（第 2 章逐行分析）。`stats[]` 与 `release_work` 则是 6.5/6.6 的新成员：前者喂给 `tools/workqueue/wq_monitor.py`（drgn 脚本，统计 started/completed/CPUtime/mayday 等 7 项），后者把 pwq 释放从 pool->lock 临界区弹到专用 kthread_worker，避免 A-A 死锁。

### 2.3 worker_pool：共享的执行资源

```c
struct worker_pool {                  /* kernel/workqueue.c，6.6 */
	raw_spinlock_t	lock;            /* 池锁——保护下面所有 L: 字段 */
	int		cpu;                 /* I: <0 = unbound 池 */
	int		node;                /* I: NUMA 分配提示 */
	int		id;                  /* I: pool id（进 worker_pool_idr）*/
	unsigned int	flags;              /* L: POOL_MANAGER_ACTIVE/DISASSOCIATED */

	unsigned long	watchdog_ts;     /* L: 前进时间戳（watchdog 判卡死）*/
	bool		cpu_stall;           /* WD: 该 CPU 池是否卡死 */

	/* 并发管理核心：本池"未睡眠"的 worker 数（含抢占未睡的）*/
	int		nr_running;          /* 特殊锁规则，见 §4.1 */

	struct list_head worklist;       /* L: 待执行队列（跨 wq 混排 FIFO）*/
	int		nr_workers;           /* L: 总 worker 数 */
	int		nr_idle;              /* L: 空闲 worker 数 */
	struct list_head idle_list;      /* L: 空闲 worker（LIFO，新睡的放头）*/
	struct timer_list idle_timer;    /* L: 300s 空闲清算定时器 */
	struct work_struct idle_cull_work; /* L: 6.6: 清算工作项（sleepable）*/
	struct timer_list mayday_timer;  /* L: 建线程超时求救定时器 */

	DECLARE_HASHTABLE(busy_hash, 6); /* L: 忙 worker 哈希表（64 桶），
	                                     key = 正在执行的 work 地址 */

	struct worker	*manager;       /* L: 纯 informational */
	struct list_head workers;        /* A: 池的全部 worker（attach 锁保护）*/
	struct list_head dying_workers;  /* A: 6.6: 待死 worker 名单 */
	struct completion *detach_completion; /* 最后一个 worker 分离后 complete */

	struct ida	worker_ida;       /* worker 编号（线程命名用）*/
	struct workqueue_attrs *attrs;  /* I: nice/cpumask/pod mask */
	struct hlist_node hash_node;    /* PL: unbound 池哈希节点（按 attrs）*/
	int		refcnt;               /* PL: unbound 池引用计数 */
	struct rcu_head	rcu;               /* RCU 延迟释放 */
};
```

关键点：

- **`worklist` 是池级 FIFO**，不同 wq 的 work 混排——这是"共享池"的直接体现。wq 想要自己的顺序语义必须靠 ordered 或 max_active=1。
- **`busy_hash`**：64 桶哈希，key 是 worker 正在执行的 work 地址。用途只有一个：**防同 work 并发重入**（§3.2）。
- **idle_list 是 LIFO**：`list_add()` 头插。刚睡下的 worker 优先被唤醒——它的 cache 最热。
- **per-CPU 池静态分配**：`DEFINE_PER_CPU_SHARED_ALIGNED(struct worker_pool [2], cpu_worker_pools)`，`[0]` 普通 nice=0、`[1]` 高优 nice=-20。unbound 池动态创建，按 attrs 哈希共享（第 3 章）。

### 2.4 worker：干活的穷哥们

```c
struct worker {                       /* kernel/workqueue_internal.h，6.6 */
	/* idle 时挂 pool->idle_list（entry），busy 时挂 busy_hash（hentry）*/
	union {
		struct list_head entry;    /* L: while idle */
		struct hlist_node hentry;  /* L: while busy */
	};

	struct work_struct	*current_work; /* K: 正在执行的 work */
	work_func_t		current_func;   /* K: 及其函数 */
	struct pool_workqueue	*current_pwq;  /* K: 及其 pwq */
	u64			current_at;     /* K: 本次执行起点（sum_exec_runtime）*/
	unsigned int		current_color;  /* K: 该 work 的颜色 */

	int		sleeping;             /* S: 6.6 新增——是否在睡（sched 钩子用）*/
	work_func_t	last_func;       /* K: 上一个执行的函数（psi 用）*/

	struct list_head scheduled;       /* L: 分派给我的 work（防重入 punt 队）*/
	struct task_struct	*task;      /* I: kthread 任务 */
	struct worker_pool	*pool;     /* A: 所属池 */
	struct list_head	node;       /* A: 挂 pool->workers */
	unsigned long	last_active;     /* K: 最后活跃时刻（idle 清算依据）*/
	unsigned int	flags;            /* L: WORKER_* 状态 */
	int			id;                 /* I: 编号（线程名）*/
	char			desc[24];       /* set_worker_desc() 的调试描述 */
	struct workqueue_struct	*rescue_wq; /* 仅 rescuer：救谁 */
};
```

worker 状态标志（6.6 真实定义，注意**没有 WORKER_RUNNING**——老内核的该标志已在重构中删除，"是否在跑"改由 `pool->nr_running` 计数与 `worker->sleeping` 表达）：

| 标志 | 值 | 含义 |
| :--- | :--- | :--- |
| `WORKER_DIE` | 1<<1 | 自毁指令（自杀路径看到即退出） |
| `WORKER_IDLE` | 1<<2 | 在 idle_list 上 |
| `WORKER_PREP` | 1<<3 | 预备态（刚创建/刚睡醒，尚未参与并发管理） |
| `WORKER_CPU_INTENSIVE` | 1<<6 | 豁免并发管理（WQ_CPU_INTENSIVE 或自动检测） |
| `WORKER_UNBOUND` | 1<<7 | 不参与并发管理（unbound 池/DISASSOCIATED） |
| `WORKER_REBOUND` | 1<<8 | CPU 回线上待恢复 CM 的过渡标志 |

`WORKER_NOT_RUNNING = PREP | CPU_INTENSIVE | UNBOUND | REBOUND`——任一置位即脱离并发管理，`worker_set/clr_flags()` 在状态跨越该集合边界时增减 `pool->nr_running`。

### 2.5 四件套关系图

```
                          struct workqueue_struct (system_wq, flags, cpu_pwq[])
                          ┌───────────────────────────────────────────────┐
        queue_work()      │  pwqs ──┐   list(全部 wq)   unbound_attrs      │
      ─────────────────►  │         ▼                                       │
   work(PENDING=1)        │  ┌──────────────┐   ┌──────────────┐            │
      │                   │  │ pool_workqueue│   │ pool_workqueue│ (per-cpu: │
      ▼                   │  │ nr_active=1   │   │ nr_active=2   │  每 CPU 一个;│
 ┌──────────┐             │  │ max_active=256│   │ max_active=256│  unbound:  │
 │   work   │             │  └──────┬───────┘   └──────┬───────┘  每 pod 一个)│
 │ data:pwq │             └─────────┼──────────────────┼────────────────┘
 └──────────┘                       │ I: pool          │ I: pool
      │ insert_work()               ▼                  ▼
      ▼                  ┌─────────────────────────────────────────┐
 ┌────────────────────┐  │          struct worker_pool             │
 │ pool->worklist     │  │  worklist(混排FIFO)  nr_running  nr_idle │
 │ [w1][w2][w3]...    │  │  idle_list(LIFO)     busy_hash(64桶)    │
 └────────────────────┘  │  attrs(nice,cpumask)  watchdog_ts       │
      ▲    混排           └───────────┬───────────┬────────────────┘
      │                            A:│pool       A:│pool
      │                              ▼             ▼
      │                        ┌─────────┐   ┌─────────┐
      │   kick_pool() 唤醒     │ worker  │   │ worker  │   ← struct worker
      └───────────────────────►│ (idle)  │   │ (busy)  │     current_work,
         wake_up_process()     │ task ───┼──►│scheduled│     scheduled 队
                              └─────────┘   └─────────┘
                                kworker/0:1    kworker/0:0
```

提交侧：work → 找 pwq（`wq->cpu_pwq[cpu]`）→ 挂 pool->worklist 或 pwq->inactive_works → kick_pool 唤醒 idle worker。执行侧：worker 从 worklist 摘 work → `current_work` 归属 + busy_hash 登记 → 调 `work->func(work)`。

---

## 三、worker 主循环与执行协议

### 3.1 worker_thread()：骨架（6.6 精简）

```c
static int worker_thread(void *__worker)
{
	struct worker *worker = __worker;
	struct worker_pool *pool = worker->pool;

	set_pf_worker(true);            /* PF_WQ_WORKER：调度器钩子的识别位 */
woke_up:
	raw_spin_lock_irq(&pool->lock);

	/* ① 自毁检查：idle 清算/池销毁把 WORKER_DIE 置位并唤醒我 */
	if (unlikely(worker->flags & WORKER_DIE)) {
		raw_spin_unlock_irq(&pool->lock);
		set_pf_worker(false);
		set_task_comm(worker->task, "kworker/dying");
		ida_free(&pool->worker_ida, worker->id);
		worker_detach_from_pool(worker);   /* 从 pool->workers 摘除 */
		kfree(worker);                     /* 在自己栈上自杀，kthread 框架收尸 */
		return 0;
	}

	worker_leave_idle(worker);       /* 摘出 idle_list，nr_idle-- */
recheck:
	/* ② 没活干？去睡 */
	if (!need_more_worker(pool))
		goto sleep;

	/* ③ 有活但无人可干（没 idle worker）→ 我来当 manager 建线程 */
	if (unlikely(!may_start_working(pool)) && manage_workers(worker))
		goto recheck;            /* 管理期间世界变了，重新判定 */

	WARN_ON_ONCE(!list_empty(&worker->scheduled));

	/* ④ 预备完成：开始参与并发管理（清 PREP/REBOUND → nr_running++）*/
	worker_clr_flags(worker, WORKER_PREP | WORKER_REBOUND);

	/* ⑤ 干活循环 */
	do {
		struct work_struct *work =
			list_first_entry(&pool->worklist, struct work_struct, entry);

		if (assign_work(work, worker, NULL))       /* 见 §3.2 */
			process_scheduled_works(worker);       /* 逐个 process_one_work */
	} while (keep_working(pool));      /* worklist 非空 && nr_running<=1 */

	worker_set_flags(worker, WORKER_PREP);         /* 让出 CM 身份 */
sleep:
	/* ⑥ 睡眠协议：先置状态再放锁（唤醒者持锁或在本 CPU，不会丢事件）*/
	worker_enter_idle(worker);         /* idle_list LIFO 头插，arm idle_timer */
	__set_current_state(TASK_IDLE);    /* TASK_UNINTERRUPTIBLE|TASK_NOLOAD */
	raw_spin_unlock_irq(&pool->lock);
	schedule();                        /* sched_submit_task→wq_worker_sleeping */
	goto woke_up;
}
```

> 💡 **睡眠用 `TASK_IDLE` 而非 `TASK_UNINTERRUPTIBLE`**：`TASK_NOLOAD` 位让它**不计入 loadavg**——否则上千个空闲 kworker 会把 /proc/loadavg 永久顶到高位。这是调度器分册（`04-调度时机与上下文切换.md` §4.2）讲的 D 状态记账规则在 workqueue 的镜像应用。

### 3.2 assign_work：busy_hash 防重入

worker 从 `pool->worklist` 摘 work 前先过 `assign_work()`：

```c
static bool assign_work(struct work_struct *work, struct worker *worker,
			struct work_struct **nextp)
{
	struct worker_pool *pool = worker->pool;
	struct worker *collision;

	collision = find_worker_executing_work(pool, work); /* busy_hash 查 */
	if (unlikely(collision)) {
		/* 同池已有 worker 在执行同地址的 work → 挪到它的 scheduled 队 */
		move_linked_works(work, &collision->scheduled, nextp);
		return false;
	}
	move_linked_works(work, &worker->scheduled, nextp);
	return true;
}
```

`find_worker_executing_work()` 用**work 地址 + 函数指针双重匹配**busy_hash——注释里坦白了原因：work 结构在其函数开头就可能被释放、地址被回收复用，只比地址会误伤无辜（"if somebody wants to shoot oneself in the foot that badly..."）。防重入是**跨两级的**：跨池由 `__queue_work` 的 last_pool 粘性保证（第 2 章），同池由这里的 busy_hash 保证。`worker->scheduled` 链表因此有双重身份：正常分派队列 + 重入 punt 队列。

### 3.3 process_one_work()：执行协议九步（6.6）

```
① 摘取声明（pool->lock 持有）:
     hash_add(busy_hash, &worker->hentry, work地址)   ← 防重入登记
     worker->current_work/func/pwq/current_at/current_color  ← 归属声明
     worker->desc = wq->name（可被 set_worker_desc() 覆盖）
     list_del_init(&work->entry)
② WQ_CPU_INTENSIVE → worker_set_flags(WORKER_CPU_INTENSIVE)
     （脱离并发管理：nr_running--，此后调度器随便抢占）
③ kick_pool(pool)
     （对 per-CPU 池是 no-op：nr_running≥1；
       对 NOT_RUNNING worker 是"链接执行"的引擎——再拉一个 idle worker 接力）
④ set_work_pool_and_clear_pending(work, pool->id)
     ← 清 PENDING 的 smp_wmb+smp_mb 在锁内+关中断下与 queue 侧配对
⑤ raw_spin_unlock_irq(&pool->lock)
⑥ lock_map_acquire(wq->lockdep_map) + lock_map_acquire(work 拷贝)
     （lockdep 两层"锁"：wq 级防 flush 依赖环、work 级防重复入队死锁）
⑦ trace_workqueue_execute_start; worker->current_func(work);
     trace_workqueue_execute_end;
⑧ 泄漏体检: in_atomic() || lockdep_depth>0 →
     "BUG: workqueue leaked lock or atomic" + 持锁清单 + dump_stack
     cond_resched()  ← 防 !PREEMPT 内核上自续 work + stop_machine 互饿死
⑨ 收尾（重新持锁）: 清 CPU_INTENSIVE、last_func 记录、hash_del、
     清 current_*、pwq_dec_nr_in_flight(pwq, work_data)
     （nr_active-- → 补位激活 inactive_works 头部 → flush 色归零则 complete）
```

第 ⑧ 步的泄漏体检是"work 函数必须退出时与进入时状态一致（无原子、无持锁）"的运行时强制器——驱动里最常撞上的 workqueue 告警就来自它。

---

## 四、并发管理：worker 的生与死

### 4.1 判定函数群（pool->lock 下调用，6.6 原文）

```c
/* 需要更多 worker 吗？= 有活 && 没有在跑的（睡眠的不算！）*/
static bool need_more_worker(struct worker_pool *pool)
{
	return !list_empty(&pool->worklist) && !pool->nr_running;
}

/* 我能开工吗？（被唤醒的 worker 问）= 有 idle 的就行 */
static bool may_start_working(struct worker_pool *pool) { return pool->nr_idle; }

/* 我要继续干吗？（正在跑的 worker 问）*/
static bool keep_working(struct worker_pool *pool)
{
	return !list_empty(&pool->worklist) && (pool->nr_running <= 1);
}

/* 需要建新线程吗？= 有活 && 没人能立即干 */
static bool need_to_create_worker(struct worker_pool *pool)
{
	return need_more_worker(pool) && !may_start_working(pool);
}

/* 空闲过剩吗？= idle>2 且 (idle-2)*4 >= busy */
static bool too_many_workers(struct worker_pool *pool)
{
	bool managing = pool->flags & POOL_MANAGER_ACTIVE;
	int nr_idle = pool->nr_idle + managing;   /* manager 视作 idle */
	int nr_busy = pool->nr_workers - nr_idle;
	return nr_idle > 2 && (nr_idle - 2) * MAX_IDLE_WORKERS_RATIO >= nr_busy;
}
```

**`nr_running` 的诡异锁规则**（源码注释原文）：加——在本 CPU 上禁抢占的进程上下文；减/清零——同样上下文但持 pool->lock。读者拿锁后必能看到归零瞬间的一致值。维护它的不是 workqueue 自己，而是**调度器钩子**（§5.1）——worker 每次 `schedule()` 睡下前 `wq_worker_sleeping()` 减、醒来 `wq_worker_running()` 加。被抢占**不算**睡（仍 running，继续占并发额度）——这是"CPU_INTENSIVE"名称的由来：那种 work 只占 CPU 不睡，把并发额度焊死。

### 4.2 建线程：manage_workers → maybe_create_worker

`worker_thread()` 主循环第 ③ 步：被唤醒的 worker 发现"有活且无 idle 可派工"（`!may_start_working`），就兼任 **manager**（`POOL_MANAGER_ACTIVE` 防重入，一池至多一个）：

```c
static void maybe_create_worker(struct worker_pool *pool)  /* 6.6 精简 */
{
restart:
	raw_spin_unlock_irq(&pool->lock);
	mod_timer(&pool->mayday_timer, jiffies + MAYDAY_INITIAL_TIMEOUT); /*10ms*/
	while (true) {
		if (create_worker(pool) || !need_to_create_worker(pool))
			break;
		schedule_timeout_interruptible(CREATE_COOLDOWN);  /* 失败冷却 1s */
		if (!need_to_create_worker(pool))
			break;
	}
	del_timer_sync(&pool->mayday_timer);
	raw_spin_lock_irq(&pool->lock);
	if (need_to_create_worker(pool))   /* 放锁期间世界又变了 */
		goto restart;
}
```

**后置条件**（源码注释明示）：返回时 `need_to_create_worker()==false` 且 `may_start_working()==true`——即**池必有至少一个 idle worker**。所谓"预热"就体现在这里：不是常驻 keep_idle 定时器，而是 manager 在放行干活循环前同步兜底。`create_worker()` 全流程：`ida_alloc` 编号 → 命名（per-CPU `kworker/<cpu>:<id>[H]`，unbound `kworker/u<poolid>:<id>`）→ `kthread_create_on_node`（NUMA 本地分配）→ `set_user_nice(attrs->nice)` → `kthread_bind_mask(允许的 CPU)` → attach → `worker_enter_idle` + 显式 `wake_up_process`（防 kthread 完成量挂起触发 hung 检查）。

**mayday 求救**：`create_worker` 失败超过 10ms（`MAYDAY_INITIAL_TIMEOUT = HZ/100`，下限 2 tick），`pool_mayday_timeout` 对 worklist 上**每个 work** 发 `send_mayday()`——把其 pwq 挂上 `wq->maydays` 并唤醒该 wq 的 **rescuer**（`WQ_MEM_RECLAIM` 专属，`kworker/R-<name>`，nice -20）。典型场景：GFP_KERNEL 分配失败本身在等回收，而回收工作恰堵在这个池上——rescuer 保证前向推进（第 4 章展开）。

### 4.3 杀线程：300 秒空闲清算（6.6 重构版）

```
worker_enter_idle():
    idle_list LIFO 头插; last_active = jiffies
    若 too_many_workers() && idle_timer 未挂 → mod_timer(idle_timer, +300s)
                                （注释：worker 在 idle/active 间抖动时
                                  不做繁复的 timer 摘除——300s 够长，懒得起见）

idle_worker_timeout():              ← TIMER_DEFERRABLE
    重新评估 too_many_workers()
    检查 idle_list 尾部（最老的 idle）last_active+300s 是否已过
    未过 → mod_timer 顺延；已过 → queue_work(system_unbound_wq, idle_cull_work)

idle_cull_fn():                     ← 6.6：从 timer 回调改为 work item！
    mutex_lock(wq_pool_attach_mutex) + pool->lock
    while (too_many_workers(pool)):
        worker = idle_list 尾部
        expires = worker->last_active + IDLE_WORKER_TIMEOUT
        未到期 → mod_timer + break
        set_worker_dying(worker, &cull_list):
            nr_workers--; nr_idle--; WORKER_DIE 置位
            挪入 dying_workers 名单 + cull_list
    wake_dying_workers(cull_list)  ← 解绑亲和后唤醒——worker 在自己栈上走 §3.1①自毁
```

> 💡 为什么要 timer + work 两段？`set_worker_dying` 之后要对 worker 做 `set_cpus_allowed_ptr`（可能睡眠）——timer 回调里不能睡。6.6 把清算逻辑搬进 work item（跑在 `system_unbound_wq`），顺带解决了"不想打扰 isolated CPU"的亲和重置问题。`IDLE_WORKER_TIMEOUT = 300*HZ`（5 分钟）与 `MAX_IDLE_WORKERS_RATIO = 4`（idle : busy ≤ 1:4，且白送 2 个）共同决定池的稳态规模。

---

## 五、与调度器的共生（呼应调度器分册）

### 5.1 三个调度器钩子（`kernel/sched/core.c` 调用点已核对）

| 钩子 | 调用点（core.c） | 干什么 |
| :--- | :--- | :--- |
| `wq_worker_sleeping(tsk)` | `sched_submit_task()`（schedule 前） | busy 且非 NOT_RUNNING → `nr_running--`，必要时 `kick_pool` 补人 |
| `wq_worker_running(tsk)` | `sched_update_worker()`（ttwu/继续跑） | `nr_running++`；`current_at` 重置（CPU 占用计时重新起算） |
| `wq_worker_tick(curr)` | `scheduler_tick()`（每 tick） | CPU 时间记账 `PWQ_STAT_CPU_TIME`；**6.6 新能力**：CM worker 不间断霸占 CPU 超过 `wq_cpu_intensive_thresh_us`（默认 10ms，慢 CPU 按 BogoMIPS 放大到最多 1s）→ 自动 `worker_set_flags(WORKER_CPU_INTENSIVE)` 脱离并发管理 + `kick_pool` 放行他人 |

6.6 的自动 CPU-intensive 检测解决的是老问题：**一个不睡眠的 CM work 会把整个池的并发额度焊死**。以前只能靠作者自觉标 `WQ_CPU_INTENSIVE`；现在 tick 会在 10ms 后没收它的 CM 身份。开 `CONFIG_WQ_CPU_INTENSIVE_REPORT` 还会打印 `workqueue: %ps hogged CPU for >...us ... consider switching to WQ_UNBOUND`（指数退避，第 4 次违例起上报）。

`worker->last_func`（`wq_worker_last_func()`）供 psi 判断周期聚合 worker 是否该关闭——调度器与 workqueue 的耦合细致到 psi 的节能细节。

### 5.2 worker 的调度身份

- **per-CPU worker**：`kthread_set_per_cpu(task, cpu)` 标记 per-CPU kthread 身份——调度器把它计入 `sum_nr_running` 的 idle 判定，且在给 idle CPU 派活（`select_idle_sibling`/nohz）时区别对待；绑死单 CPU，热插拔下线时 `unbind_workers()` 转 unbound。
- **nice 即优先级**：普通池 nice=0；`WQ_HIGHPRI` 用的是**每 CPU 第二个标准池**（nice = `MIN_NICE` = **-20**），不是独立调度类——kworker 全部是 CFS 任务，只是权重不同。RT 任务的及时性**不受** kworker 阻塞影响，但 kworker 本身可能被 RT 无限推迟。
- **CPU_INTENSIVE 的豁免本质**：从"池的并发额度（nr_running）"里除名，还给孩子调度器——抢占、迁移、多核并行都随调度器意思。per-CPU 池语义退化为"unbound 但绑核"。
- **TASK_IDLE 睡眠**（§3.1）：不污染 loadavg、不可被信号唤醒（`TASK_KILLABLE` 语义都不给，内核线程无信号）。

### 5.3 一张图看懂并发管理

```
时刻  pool->worklist   nr_running   动作                    池内状态
----  --------------   ----------   ----------------------  ----------------
t0    [w1,w2,w3]       0            need_more_worker=1 →    idle: k0,k1
                                     kick 唤醒 k0（LIFO 头）
t1    [w2,w3]          1            k0 执行 w1              k0 busy
t2    [w2,w3]          1            w1 里 mutex 睡了
          ↓ sched_submit_task → wq_worker_sleeping
t3    [w2,w3]          0            need_more_worker=1 →    idle: k1
                                     kick 唤醒 k1 执行 w2    k0睡 k1busy
t4    [w3]             1            w2 也睡了 → 唤醒……池按
                                     "不睡眠的在跑数" 自适应伸缩
t5    []               0            全部睡完→最后一个跑完的
                                     worker 进 idle_list
  5 分钟无新活 → idle_cull 清算多余 worker（保底 idle>2 才裁）
```

CMWQ 的"并发管理"一句话：**池希望任一时刻恰好有一个不睡眠的 worker 在跑**；有人睡觉立刻补位（`keep_working` 的 `nr_running<=1`），睡醒的人若已有人接力则自觉再睡。

---

## 六、观测与调试

| 想知道什么 | 工具 | 说明 |
| :--- | :--- | :--- |
| 池/wq 全量状态 dump | `sysrq-w` / `echo w > /proc/sysrq-trigger` | `show_all_workqueues()`：每 wq 的 pwq、in-flight/pending/inactive 及函数指针，每 pool 的 worker 数与 idle 名单 |
| 某个卡死 kworker 在干嘛 | `sysrq-t` 输出里的 `Workqueue: <wq> <func> (<desc>)` | `print_worker_info()`；desc 来自 `set_worker_desc()`（驱动可自定义，如块层记录设备名） |
| 运行时监控（6.6 官方） | `drgn tools/workqueue/wq_monitor.py` | 每 wq 的 total/inflight/CPUtime/CPUitsv/CMW/mayday/rescued 计数 |
| 一次性结构 dump | `drgn tools/workqueue/wq_dump.py` | 全部 wq/pool/pwq 结构与 attrs |
| worker 当前身份 | `cat /proc/<pid>/comm` | `wq_worker_comm()`：`kworker/0:1+events`（+正在跑的 wq）/`-`已跑完的 |
| 池级卡死检测 | `workqueue.watchdog_thresh`（默认 30s） | "BUG: workqueue lockup - pool N ... stuck for Ns!" + 全量 dump + 霸核 worker 回栈 |

经典告警速查：

| 告警 | 原因 | 处置 |
| :--- | :--- | :--- |
| `BUG: workqueue lockup` | 池头 work 换代超 30s：CM work 霸核（睡不着的死循环）、或全部 worker 睡在彼此依赖的锁上 | 看 dump 的 in-flight 函数 + show_cpu_pools_hogs 的回栈 |
| `hogged CPU for >10000us` | 6.6 自动检测：CM work 长时间不睡眠 | 把该 work 换到 `WQ_UNBOUND` 或自建 unbound wq |
| `kworker/dying` 大量出现 | 负载尖峰后的正常清算（300s 延迟） | 通常无害；频繁出现说明池频繁过冲 |

---

## 七、思考题

1. `need_more_worker()` 对 unbound 池**永远为真**（只要 worklist 非空）——为什么？这对 unbound 池的 worker 数量曲线意味着什么？提示：unbound worker 的 `WORKER_NOT_RUNNING` 身份与 `nr_running` 记账。
2. 假设一个 per-CPU 池上有 8 个 worker 全部睡在同一个 mutex 上，而持锁者在另一个 CPU 的池里也睡了。`need_more_worker` 会怎样驱动池扩张？扩张到哪里停？mayday 何时介入？
3. `worker_thread()` 睡眠协议"先 `__set_current_state` 再放 pool->lock"依赖什么前提才不丢唤醒？对照调度器分册 `try_to_wake_up` 的 wakelist 路径：唤醒者若**不持** pool->lock 也不在本 CPU，哪里会破坏该前提？
4. 6.6 把 idle worker 清算从 timer 回调挪到 work item（`idle_cull_fn`），除了"想睡"还有什么非挪不可的理由？提示：`set_worker_dying()` 之后对 worker task 做了什么操作，能在（软）中断上下文做吗？
5. 自动 CPU-intensive 检测为什么在 `wq_worker_tick()` 里用 `sum_exec_runtime - current_at` 而不用 jiffies 差值？`wq_worker_running()` 醒来时重置 `current_at` 对"睡眠频繁但每段都霸核"的 work 意味着什么？
6. 设计实验：如何用 `wq_monitor.py` 的 CMW（concurrency-management wakeup）列区分"work 天然短小"与"work 睡得太频繁导致池反复换人"？

---

> 下一章：[work 生命周期与 flush/cancel](02-work生命周期与flush-cancel.md) —— 从 `queue_work()` 的 PENDING 抢占到 `flush_workqueue()` 的颜色级联，一条 work 的完整旅程。
