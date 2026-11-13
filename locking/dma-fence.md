# dma_fence：异步完成原语（Linux 6.6）

> 阅读目标：吃透 dma-resv 的 fence 列表之元素——`struct dma_fence`（include/linux/dma-fence.h:65-105）：**三元组身份**（context/timeline/seqno）、**三态标志**（SIGNALED/TIMESTAMP/ENABLE_SIGNAL 的原子位操作）、`cb_list ↔ timestamp ↔ rcu` 的**联合体生命周期**（注释原话 "cb_list is *only* valid until the signal bit is set"）、`dma_fence_signal` 的**回调遍历**（test_and_set_bit → list_replace → 逐 cb 调用）、以及 6.6 的**死锁约束清单**（头注释 "drivers should annotate all code required to reach dma_fence_signal()"）。全部对照 v6.6（include/linux/dma-fence.h 669 行、drivers/dma-buf/dma-fence.c 1024 行）。前置：[dma-resv](dma-resv.md)（fence 列表的宿主）。

---

## 一、问题：GPU 异步完成的信号量

```
CPU 提交 GPU 命令（异步——命令入 ring buffer 即返回）
GPU 何时真正执行完？——dma_fence

fence 的语义：
  未 signaled = 命令在途（GPU 正在/将要处理）
  signaled = 命令完成（GPU 已写入完成信号）
```

## 二、结构：三元组身份 + 联合体生命周期（df.h:65-105）

```c
struct dma_fence {
	spinlock_t *lock;                 /* 外部提供的锁 */
	const struct dma_fence_ops *ops;  /* 驱动回调 */

	/* ★联合体：cb_list→timestamp→rcu 的三阶段 */
	union {
		struct list_head cb_list;    /* 阶段1：等待回调链 */
		ktime_t timestamp;           /* 阶段2：signal 时的时间戳 */
		struct rcu_head rcu;         /* 阶段3：释放的 RCU 头 */
	};
	u64 context;                      /* timeline ID——dma_fence_context_alloc 分配 */
	u64 seqno;                        /* 序号——同 context 内单调递增 */
	unsigned long flags;              /* 原子位操作（enum dma_fence_flag_bits） */
	struct kref refcount;
	int error;                        /* signal 前设置——可选错误码 */
};
```

★头注释原话（:76-83）：*"We clear the callback list on kref_put so that by the time we release the fence it is unused...The cb_list is *only* valid until the signal bit is set, and to read either you *must* hold a reference to the fence, and not just the rcu_read_lock."*

**联合体的三阶段**：`cb_list`（活跃期——回调链）→ signal 时被 `list_replace` 抢出、`timestamp` 填入同一内存 → release 时 `rcu` 占用同一内存。

## 三、三元组身份：context / timeline / seqno

头注释原话（df.h:45-47）：*"A fence is initialized using `dma_fence_init()` and completed using `dma_fence_signal()`. Fences are associated with a context, allocated through `dma_fence_context_alloc()`, and all fences on the same context are **completed in order**."*

```
context（u64）：timeline 的 ID——一个 GPU 引擎/队列一个
  dma_fence_context_alloc(num_contexts) 返回不重叠的 ID 区间
seqno（u64）：同 context 内单调递增——按提交顺序编号
  → (context, seqno) 二元组唯一标识一个 fence
  → 同 context 内 seqno 小的一定先完成（GPU 保证顺序完成）
```

## 四、三态标志与原子操作（df.h:49-63）

```c
enum dma_fence_flag_bits {
	DMA_FENCE_FLAG_SIGNALED_BIT,       /* 已完成 */
	DMA_FENCE_FLAG_TIMESTAMP_BIT,      /* 时间戳已记录 */
	DMA_FENCE_FLAG_ENABLE_SIGNAL_BIT,  /* enable_signaling 已被调过 */
	DMA_FENCE_FLAG_USER_BITS,          /* 驱动自定义位——必须放最后 */
};
```

头注释（:53-63）：*"The flags member must be manipulated and read using the appropriate atomic ops (bit_*), so taking the spinlock will not be needed most of the time."* + 关键竞态：*"Adding a check for DMA_FENCE_FLAG_SIGNALED_BIT after setting DMA_FENCE_FLAG_ENABLE_SIGNAL_BIT closes this race."*

## 五、signal 的回调遍历（df.c:367-400）

```c
int dma_fence_signal_timestamp_locked(struct dma_fence *fence,
				      ktime_t timestamp)
{
	struct dma_fence_cb *cur, *tmp;
	struct list_head cb_list;

	lockdep_assert_held(fence->lock);

	/* ★原子置位——如已 signaled 则 -EINVAL（防双 signal）*/
	if (unlikely(test_and_set_bit(DMA_FENCE_FLAG_SIGNALED_BIT,
				      &fence->flags)))
		return -EINVAL;

	/* 把 cb_list 抢出来——联合体切换到 timestamp 阶段 */
	list_replace(&fence->cb_list, &cb_list);

	fence->timestamp = timestamp;
	set_bit(DMA_FENCE_FLAG_TIMESTAMP_BIT, &fence->flags);
	trace_dma_fence_signaled(fence);

	/* 逐个调用注册的回调 */
	list_for_each_entry_safe(cur, tmp, &cb_list, node) {
		INIT_LIST_HEAD(&cur->node);
		cur->func(fence, cur);       /* ★驱动回调 */
	}
	...
}
```

## 六、等待：default_wait 的回调自旋（df.c:749-800+）

```
dma_fence_wait(fence, intr, timeout)
  └─ ops->wait 或 dma_fence_default_wait
       ① 先测 SIGNALED 位（快路径——已完成的直接返回）
       ② 未完成 → add_callback 注册自旋等待的回调
       ③ schedule_timeout 睡眠
       ④ 回调触发（signal 时被调用）→ 醒来重测
       ⑤ 循环直到 signaled 或 timeout
```

## 七、死锁约束清单（头注释 df.c:89-111 原话）

```
"* To ensure there's no deadlocks of dma_fence_wait() against
   other locks drivers should annotate all code required to
   reach dma_fence_signal() with lockdep's dma_fence_begin/
   end_annotation."

"* Drivers are allowed to call dma_fence_wait() while holding
   dma_resv_lock()."     ← ★resv 持有中允许等 fence

"* Drivers are allowed to call dma_fence_wait() from their
   &shrinker callbacks..."  ← shrinker 中允许（GPU 换出等完成）

"* Drivers are allowed to call dma_fence_wait() from their
   &mmu_notifier callbacks..."

"* ...But no drivers/gpu should ever call dma_fence_wait() in
   such contexts."       ← 某些上下文仍然禁止（详注 :111）
```

## 八、消费者

| 消费者 | fence 类型 | 用途 |
| :--- | :--- | :--- |
| **dma-resv**（[dma-resv](dma-resv.md)） | 任意 | GPU 命令的隐式/显式同步 |
| **sync_file** | 任意 | fence → FD 的用户态导出（dma_buf_import_sync_file） |
| **drm/scheduler** | drm_sched_fence | GPU 调度器的 scheduled/finished 双 fence |
| **virtio-gpu** | virtio_fence | 虚拟化 GPU 的完成通知 |
| **软件 fence** | dma_fence_stub / sw_fence | 内核软件信号量（timeline 框架） |

## 九、观测实战

```bash
# 1) fence 的 debugfs
cat /sys/kernel/debug/dma_buf/bufinfo 2>/dev/null | grep -A 3 fence

# 2) fence 的 tracepoint
perf record -e dma_fence:dma_fence_signaled -a sleep 5
perf record -e dma_fence:dma_fence_wait -a sleep 5

# 3) 死锁检测（lockdep 注解）
#   dma_fence_begin/end_annotation 是 lockdep 的 cross-release
#   集成——运行时自动检测 wait vs signal 的锁序违规

# 4) fence 的引用计数泄漏排查
echo 1 > /sys/kernel/debug/dma_buf/bufinfo
#   每个 dma_buf 的 resv 里的 fence 引用计数
```

## 十、思考题

1. `cb_list ↔ timestamp ↔ rcu` 的联合体为什么能安全复用？（提示：三阶段互斥——活跃期只有 cb_list、signal 后只有 timestamp、release 后只有 rcu；注释 "only valid until the signal bit is set" 界定了边界。）
2. `context` 内 "completed in order" 的保证由谁提供——fence 框架还是驱动？（提示：框架不保证——驱动（GPU 硬件或调度器）保证同 context 内按 seqno 顺序 signal。）
3. `dma_fence_signal` 的 `test_and_set_bit` 保证只 signal 一次——如果驱动 bug 导致双 signal 会怎样？（提示：第二次返回 -EINVAL 而非崩溃——但已注册的回调可能已在第一次被消费完。）
4. 头注释允许 "call dma_fence_wait() while holding dma_resv_lock()"——这不会死锁吗？（提示：等的是 fence 完成（GPU 侧事件），不是另一 CPU 上的 resv 锁——fence signal 不需要持有同一 resv。）
5. `DMA_FENCE_FLAG_USER_BITS` 必须放最后——为什么？（提示：枚举值被用作位号——用户位的起点是框架保留位之后的第一个值。）
6. lockdep 的 `dma_fence_begin/end_annotation` 是 cross-release 的应用——它检测的具体是什么循环？（提示：fence 的 wait→signal 是跨上下文的"锁"——wait 是 acquire，signal 是 release——cross-release 检测 acquire/release 不在同一上下文的死锁环。）

---

> 回看：[dma-resv](dma-resv.md)（fence 列表的宿主——本章为其元素）｜[ww-mutex](ww-mutex.md)（dma-resv 的锁基座）｜[concession](concession.md)（让步家族）｜[locking/README](README.md)（锁族谱）
