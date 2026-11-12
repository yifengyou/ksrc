# dma-resv：GPU buffer object 的预约协议（Linux 6.6）

> 阅读目标：吃透 ww_mutex 的最大树内消费者——`struct dma_resv`（drivers/dma-buf/dma-resv.c 806 行）：**预约 = ww_mutex + fence 列表**的双层结构、`dma_resv_lock` 对 ww_mutex 的薄封装（头注释原话"only against other writers, readers will run concurrently with a writer under RCU"）、四档 USAGE 偏序（KERNEL<WRITE<READ<BOOKKEEP 的 fence 查询语义）、`-EDEADLK` 的"unlock all + lock_slow"标准模式（ww-mutex 的回退协议在 DRM 中的实践）、fence 的 add/reserve/iterate 生命周期。全部对照 v6.6（include/linux/dma-resv.h 487 行、drivers/dma-buf/dma-resv.c 806 行）。前置：[ww-mutex](ww-mutex.md)（wound/wait 的机制基础）。

---

## 一、问题：跨驱动共享 buffer 的同步

```
GPU 渲染管线：多个 buffer object（BO）→ 命令提交时需要
  原子地预约一组 BO（防止另一进程同时移动/释放它们）
  + 等/发布 fence（异步 GPU 完成的信号量）

"预约"= 写侧互斥（ww_mutex）+ 异步完成通知（fence 列表）
"跨驱动"= dma_buf 框架让不同驱动的 BO 共享同一个 resv
```

头注释原话（drv.h:151-158）*"This is a container for dma_fence objects which needs to handle multiple use cases. One use is to synchronize cross-driver access to a struct dma_buf...The other major use is to manage access and locking within a driver in a buffer based memory manager."*

## 二、结构：ww_mutex + fence 列表（drv.h:155-172）

```c
struct dma_resv {
	/*
	 * @lock: Update side lock. Don't use directly, instead use
	 * the wrapper functions like dma_resv_lock() and
	 * dma_resv_unlock().
	 *
	 * 头注释原话（:326-328）："Note, that the lock is only against
	 * other writers, readers will run concurrently with a writer
	 * under RCU. The seqlock is used to notify readers if they
	 * overlap with a writer."
	 */
	struct ww_mutex lock;            /* ★写侧互斥——wound/wait 仲裁 */

	/*
	 * @fences: Array of fences which where added to the dma_resv
	 * object. A new fence is added by calling dma_resv_add_fence().
	 * Since this often needs to be done past the point of no return
	 * in command submission it cannot fail, and therefore sufficient
	 * slots need to be reserved by calling dma_resv_reserve_fences().
	 */
	struct dma_resv_list __rcu *fences;  /* ★RCU 保护的 fence 数组 */
};
```

## 三、lock 封装与 -EDEADLK 标准模式（drv.h:321-344）

```c
static inline int dma_resv_lock(struct dma_resv *obj,
				struct ww_acquire_ctx *ctx)
{
	return ww_mutex_lock(&obj->lock, ctx);
}
```

★头注释原话（:333-337）：*"As the reservation object may be locked by multiple parties in an undefined order, a `ww_acquire_ctx` is passed to unwind if a cycle is detected. See ww_mutex_lock() and ww_acquire_init(). A reservation object may be locked by itself by passing NULL as @ctx. When a die situation is indicated by returning `-EDEADLK` all locks held by @ctx must be unlocked and then `dma_resv_lock_slow()` called on @obj."*

DRM 驱动的标准模式（ww-mutex 回退协议的实践）：

```
retry:
	ww_acquire_ctx ctx;
	ww_acquire_init(&ctx, &resv_class);

	for each bo in command_buffer_list:
		ret = dma_resv_lock(bo->resv, &ctx);
		if (ret == -EDEADLK) {
			/* 释放全部已拿的 */
			for each held_bo: dma_resv_unlock(held_bo->resv);
			ww_acquire_fini(&ctx);
			/* 慢路径重拿 */
			dma_resv_lock_slow(bo->resv, &ctx);
			goto retry;    /* 或继续拿下一个 */
		}

	/* 全部锁到手——原子地检查/修改这组 BO */
	... 命令提交 ...
	for each bo: dma_resv_unlock(bo->resv);
	ww_acquire_fini(&ctx);
```

★**为什么 GPU 需要 ww_mutex 而非传统锁序**：BO 的依赖图是**运行时构造的**（用户态提交决定哪些 BO 参与同一命令）——编译期不知道锁序，无法用静态锁序约定。

## 四、四档 USAGE 偏序（drv.h:71-133）

```
enum dma_resv_usage {
	DMA_RESV_USAGE_KERNEL,    /* 内核内存管理（页迁移/TTM）*/
	DMA_RESV_USAGE_WRITE,     /* 用户态隐式写依赖 */
	DMA_RESV_USAGE_READ,      /* 用户态隐式读依赖 */
	DMA_RESV_USAGE_BOOKKEEP,  /* 不参与隐式同步（preemption/TLB flush）*/
};
```

头注释原话（:60-66）：*"An important fact is that there is the order KERNEL < WRITE < READ < BOOKKEEP and when the dma_resv object is asked for fences for one use case the fences for the lower use case are returned as well. For example when asking for WRITE fences then the KERNEL fences are returned as well."*

| 查询档位 | 返回的 fence |
| :--- | :--- |
| KERNEL | 仅 KERNEL |
| WRITE | KERNEL + WRITE |
| READ | KERNEL + WRITE + READ |
| BOOKKEEP | 全部 |

★**fence 的升格**：一个 BOOKKEEP fence 可以**后来**升格为 READ/WRITE（`dma_buf_import_sync_file()`）——但**不可降格**。头注释原话：*"Already used fences can be promoted...But fences can never be degraded."*

## 五、fence 生命周期

```
dma_resv_reserve_fences(obj, num_fences)   ← 持锁预留槽位
  /* 头注释原话（:170-172）："Since this often needs to be done past
     the point of no return in command submission it cannot fail" */
dma_resv_add_fence(obj, fence, usage)      ← 持锁添加（不可失败）
dma_resv_for_each_fence(obj, usage, fence)  ← ★RCU 迭代器
  /* 无锁版：dma_resv_for_each_fence_unlocked
     头注释（:184-186）："beware that the iterator can be restarted.
     Code which accumulates statistics needs to check for this with
     dma_resv_iter_is_restarted()." */
```

## 六、与 ww-mutex 的对照消费链

| ww_mutex 层 | dma_resv 层 | DRM/GPU 层 |
| :--- | :--- | :--- |
| `ww_class` | `reservation_ww_class` | 全局唯一（DRM 所有 resv 共享） |
| `ww_acquire_init` | 同名（DRM 直接用） | 命令提交时构造 |
| `ww_mutex_lock(lock, ctx)` | `dma_resv_lock(obj, ctx)` | bo 预约 |
| `-EDEADLK` | 同值 | unlock_all + lock_slow + retry |
| `ww_mutex_unlock` | `dma_resv_unlock` | 提交完释放 |
| —— | `dma_resv_add_fence` | GPU 命令的完成 fence |
| —— | `dma_resv_usage` | fence 的语义分类 |

## 七、观测实战

```bash
# 1) fence 的等待观测（debugfs）
ls /sys/kernel/debug/dma_buf/bufinfo 2>/dev/null
#   每个 dma_buf 的 resv/fence 状态

# 2) -EDEADLK 的发生率（BPF 侧）
bpftrace -e 'kprobe:ww_mutex_lock /arg2 != 0/ { @ctx = count(); }'
bpftrace -e 'kprobe:__ww_mutex_kill { @deadlock++; }'

# 3) fence 的 dump
cat /sys/kernel/debug/dma_buf/bufinfo | grep -A 5 "fence"

# 4) GPU 侧的预约等待
#   drm-debug 开启后 dmesg 可见 resv 的 lock/unlock 痕迹
echo 0x02 > /sys/module/drm/parameters/debug
```

## 八、思考题

1. `dma_resv` 的 lock "only against other writers"——读者走 RCU 并发。这与 seqlock 的"写者优先"有何异同？（提示：dma_resv 的"读"是 fence 查询（走 RCU + iter_is_restarted），不是数据读取——数据由 fence 的语义保证时序。）
2. `dma_resv_reserve_fences` 为什么不能失败？（头注释 "past the point of no return"——GPU 命令已提交到 ring buffer，fence 必须能记录，否则完成信号丢失。）
3. 四档 USAGE 中 BOOKKEEP 的 fence 不参与隐式同步——为什么还要存进 resv？（提示：调试/审计需要知道"GPU 曾经触碰过这个 BO"；且可后续升格为 READ/WRITE。）
4. `-EDEADLK` 的"unlock all + lock_slow + retry"模式与数据库事务的 rollback+retry 有何对应？（提示：ww_mutex 的 stamp 即事务号——回退=释放全部=rollback；retry=新事务号——完全同构。）
5. fence 升格（BOOKKEEP→READ）为什么不能降格？（提示：降格意味着"我不再需要等这个 fence"——但等待者可能已经看到它并开始等待；升格则只会增加等待者。）
6. `dma_resv_iter_is_restarted()` 的重启检测——什么场景下迭代器会重启？（提示：无锁迭代期间写者 add 了新 fence——RCU 切换到新数组，迭代从头开始。）

---

> 回看：[ww-mutex](ww-mutex.md)（wound/wait 的机制基础）｜[ww-rt-mutex](ww-rt-mutex.md)（PREEMPT_RT 变体）｜[concession](concession.md)（让步家族——ww 为第四形态）｜[modular-arithmetic](modular-arithmetic.md)（stamp 的模算术）
