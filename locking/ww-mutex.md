# ww_mutex：事务序让步的第四形态（Linux 6.6）

> 阅读目标：吃透 wound/wait 与 wait/die 两种**死锁回避策略**——基于 acquire context 的 stamp 序（类数据库事务序）仲裁锁竞争：**Wound-Wait**（老者抢新者的锁→新者让出全部已持有）与 **Wait-Die**（新者碰老者的锁→新者自杀）。理解 `ww_acquire_ctx` 的 stamp 分配、`__ww_mutex_add_waiter` 的**按 stamp 排序入队**（"waking up waiters that must kill themselves" 注释原话）、`__ww_mutex_check_kill` 的判定链，以及与 [concession](concession.md) 三形态的关系——**让步者身份由时间戳而非等待时长决定**。全部对照 v6.6（include/linux/ww_mutex.h 369 行、kernel/locking/ww_mutex.h、kernel/locking/mutex.c:552-894）。前置：[mutex](mutex.md)｜[concession](concession.md)。

---

## 一、问题：多锁获取的死锁地狱

```
任务 A 顺序拿锁 L1→L2→L3
任务 B 顺序拿锁 L3→L2→L1
→ 经典环死锁

传统解法：
  锁序约定（所有任务按同一顺序拿）——对 DRM/GPU 的
  buffer object 动态图不适用（顺序运行时才知道）

ww_mutex 的答案：数据库事务序
  每个任务进入时领一个 stamp（时间戳序号）
  死锁检测变为：比较 stamp——谁让谁由"事务年龄"决定
```

## 二、两种算法（kernel/locking/ww_mutex.h:160-174 注释原话）

```
Wait-Die:
  "The newer transactions are killed when:
   It (the new transaction) makes a request for a lock
   being held by an older transaction."

Wound-Wait:
  "The newer transactions are wounded when:
   An older transaction makes a request for a lock
   being held by the newer transaction."
```

★**对称反转**：Wait-Die 中**新者是主动方**（碰老者的锁→自杀）；Wound-Wait 中**老者是主动方**（要新者的锁→wound 新者→新者释放全部已持有）。

## 三、数据结构（include/linux/ww_mutex.h）

```c
struct ww_class {                    /* :38-44 */
	atomic_long_t stamp;             /* 全局戳分配器 */
	...
	unsigned int is_wait_die;        /* 算法选择 */
};

struct ww_mutex {                    /* :46-51 */
	struct WW_MUTEX_BASE base;       /* mutex 或 rt_mutex */
	struct ww_acquire_ctx *ctx;      /* ★持有者的 acquire ctx */
};

struct ww_acquire_ctx {             /* :53-58 */
	struct task_struct *task;
	unsigned long stamp;             /* ★事务时间戳 */
	unsigned int acquired;           /* 已持有多少个 ww_mutex */
	unsigned short wounded;          /* 被 wound 标记 */
	unsigned short is_wait_die;
};
```

`ww_acquire_init`（:112-130）分配 stamp（`atomic_long_inc_return_relaxed(&ww_class->stamp)`）——头注释原话 *"Context-based w/w mutex acquiring can be done in **any order whatsoever** within a given lock class. Deadlocks will be detected and handled with the wait/die logic."*

## 四、入队的排序协议（mutex.c:637-648）

```c
	/* 注释原话："Add in stamp order, waking up waiters
	   that must kill themselves." */
	ret = __ww_mutex_add_waiter(&waiter, lock, ww_ctx);
	if (ret)
		goto err_early_kill;
```

`__ww_mutex_add_waiter`（ww_mutex.h:270-310）把 waiter 按 **stamp 升序**插入等待链——stamp 更老的排前面。插入过程中发现"我的 stamp 比前面的某个 waiter 新"——在 Wait-Die 模式下**立即自杀**（`-EDEADLK`）。这保证：

1. **链头永远是最老的事务**——它一定不会自杀（没有比它更老的竞争者）
2. 新加入者一到就做判定——**错误检测前置到入队时刻**（而非等到轮到自己时才发现）

## 五、check_kill 的判定链（ww_mutex.h:432-500）

```c
static inline int
__ww_mutex_check_kill(struct MUTEX *lock, struct MUTEX_WAITER *waiter,
		      struct ww_acquire_ctx *ctx)
{
	if (ctx->acquired == 0)
		return 0;              /* 没持有任何锁 → 让步无意义 */

	if (!ctx->is_wait_die) {
		/* Wound-Wait 模式 */
		if (ctx->wounded)
			return __ww_mutex_kill(lock, ctx);
			/* ★被 wound 了 → 放弃全部已持有 + -EDEADLK */
		return 0;              /* 没被 wound → 正常等待 */
	}

	/* Wait-Die 模式 */
	hold_ctx = READ_ONCE(ww->ctx);
	if (hold_ctx && __ww_ctx_less(ctx, hold_ctx))
		return __ww_mutex_kill(lock, ctx);
		/* ★持有者比我老（stamp 小）→ 新者自杀 */

	/* 再检查前方 waiter：比 waiter 更老的排队者 */
	for (cur = __ww_waiter_prev(lock, waiter); cur;
	     cur = __ww_waiter_prev(lock, cur)) {
		if (!cur->ww_ctx) continue;
		if (__ww_ctx_less(ctx, cur->ww_ctx))
			return __ww_mutex_kill(lock, ctx);
	}
	return 0;
}
```

## 六、让步的代价：-EDEADLK 与回退协议

```
__ww_mutex_kill 的效果：
  当前任务释放全部已持有的 ww_mutex（ww_mutex_unlock 循环）
  返回 -EDEADLK
调用方的标准模式（DRM/GPU 驱动）：
  retry:
    ctx = ww_acquire_init(...)
    for each buffer_object:
        ret = ww_mutex_lock(&bo->lock, &ctx)
        if (ret == -EDEADLK) {
            释放全部已拿的锁
            ww_acquire_fini(&ctx)
            goto retry;      /* 重新领 stamp 再来 */
        }
```

★**让步的代价**不是一次调度——而是**整个事务的回退重做**。这比 [concession](concession.md) 的 handoff（只等一个释放窗口）重得多，但换来的是：**任何锁序都能存活**（无需全局锁序约定）。

## 七、与 concession 三形态的关系

| 维度 | mutex handoff | seqlock 重试 | kmod_dup 一跳 | **ww_mutex（第四形态）** |
| :--- | :--- | :--- | :--- | :--- |
| 让步者 | 新来者/非队首 | 读者 | 首战者 | **事务序较新的一方** |
| 先行者 | 队首等待者 | 写者 | 首提交者 | **stamp 较老的事务** |
| 判定依据 | 等待时长 | 结构性（读写角色） | 提交次序 | **进入时刻（stamp）** |
| 让步成本 | 一次唤醒等待 | 一次重读循环 | 一个 work 调度 | **全部已持有锁回退 + 重做** |
| 适用 | 单锁公平性 | 读多写少 | 状态一致性 | **多锁动态序（无全局锁序）** |

## 八、观测实战

```bash
# 1) ww_mutex 在 DRM 中的用例
grep -r 'ww_mutex_lock' drivers/gpu/drm/ | head -5
#   典型：drm_gem_object 的 reserve（dma-resv 内 ww_ctx）

# 2) -EDEADLK 的重试统计（无直接接口；用 BPF）
bpftrace -e 'kprobe:__ww_mutex_kill { @[comm] = count(); }'

# 3) stamp 序的验证
cat /proc/lockdep_stats 2>/dev/null | grep ww

# 4) 压力测试（内核树自带）
make M=kernel/locking test-ww_mutex.ko
#   CONFIG_DEBUG_WW_MUTEX_SLOWPATH=y 后运行
```

## 九、思考题

1. Wait-Die 中"新者自杀"意味着**每次新事务碰老者**都回退——高并发下会不会活锁？（提示：自杀释放全部锁→老者一定能完成→新者重试时领到更新的 stamp——**越活越新越容易死**。这是 Wait-Die 的已知弱点，Wound-Wait 对此更好。）
2. `__ww_mutex_add_waiter` 按 stamp 排序入队——为什么不按 FIFO？（提示：stamp 序保证链头永远是最老的事务——它不需要自杀；FIFO 下新者可能排在前面。）
3. Wound-Wait 中被 wound 的一方如何得知？（提示：`ctx->wounded = 1` 标记 + 下次 `check_kill` 时生效——**不是立即中断**而是在下一个锁操作时检查。）
4. ww_mutex 要求同 class 不混用 ctx 和非 ctx 模式——为什么？（头注释 *"Mixing...can result in undetected deadlocks"*。）
5. `ww_acquire_done` 的语义是什么——它与"事务开始"有何区别？（提示：`done` 之后不允许再 acquire，只允许 unlock——**事务的提交点**。）
6. 把 ww_mutex 的"stamp 序让步"与数据库 MVCC 的快照隔离对比——共同点是什么？（提示：都是"时间戳定义优先级"+ 竞争时年轻方让步/回滚。）

---

> 回看：[mutex](mutex.md)（owner 位域与 optimistic spin 的全部机制）｜[concession](concession.md)（三形态家族——本章为第四形态）｜[rwsem](rwsem.md)（rwsem handoff 的对照）｜[locking/README](README.md)（锁族谱）
