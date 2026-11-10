# ring buffer 设计（Linux 6.6）

> 阅读目标：解剖 ftrace 的 per-CPU ring buffer（kernel/trace/ring_buffer.c，v6.6 约 6200 行）——页面链表 + head/tail/commit/reader 四指针模型；无锁留位（reserve→write→commit）如何扛住 NMI/IRQ/SoftIRQ/进程四层嵌套；overwrite 与 producer-consumer 两种模式的语义差异；时间戳的 27 位 delta 压缩与 before_stamp/write_stamp 协议；读取侧的 swap-reader-page 魔术；以及与 eBPF ringbuf 的设计对照。

---

## 一、总体结构：页面循环链 + 四指针

每个 CPU 一个独立 buffer，页面组织成**循环链表**，四个指针在链上滑动：

```c
// kernel/trace/ring_buffer.c (v6.6, 节选)
struct buffer_page {              // 链表节点（描述页）
	struct list_head list;      // 循环链
	local_t		 write;      // 写位置索引（20 位计数 + 12 位中断计数，见 §4.3）
	unsigned	 read;       // 读位置（仅 reader_page 使用）
	local_t		 entries;    // 本页事件数
	unsigned long	 real_end;
	struct buffer_data_page *page;   // 实际数据页（PAGE_SIZE - 页头）
};

struct ring_buffer_per_cpu {
	int				cpu;
	atomic_t			record_disabled;
	raw_spinlock_t			reader_lock;   /* 串行化读者 */
	arch_spinlock_t			lock;           /* swap head 时用 */
	struct buffer_page		*head_page;   /* 读者追到哪（最老数据）*/
	struct buffer_page		*tail_page;   /* 写者写哪 */
	struct buffer_page		*commit_page; /* 已提交到哪 */
	struct buffer_page		*reader_page; /* 读者私页（swap 用）*/
	unsigned long			lost_events;   /* 交给读者的页上丢的事件 */
	unsigned long			nest;
	local_t				entries_bytes;
	local_t				entries;       /* 总事件数 */
	local_t				overrun;       /* overwrite 覆盖数 */
	local_t				commit_overrun;
	local_t				dropped_events;/* 非 overwrite 丢弃数 */
	local_t				committing;    /* ★嵌套提交计数 */
	local_t				commits;
	rb_time_t			write_stamp;   /* 最近留位的时间戳 */
	rb_time_t			before_stamp;  /* 留位前的（写协议用）*/
	u64				event_stamp[MAX_NEST];
	u64				read_stamp;
	...
	struct rb_irq_work		irq_work;       /* 唤醒等待读者 */
};

struct trace_buffer {                // 全局 buffer（ring_buffer_alloc 的返回）
	unsigned			flags;      // RB_FL_OVERWRITE
	int				cpus;
	atomic_t			record_disabled;
	cpumask_var_t			cpumask;
	struct mutex			mutex;
	struct ring_buffer_per_cpu	**buffers;  // ★per-CPU 数组
	u64				(*clock)(void);  // trace_clock 注入
	bool				time_stamp_abs;
};
```

```
       （写方向 ──────────►）  pages 循环链（示意，实际为闭环）

      ┌────────┐   ┌────────┐   ┌────────┐   ┌────────┐
  ┌──►│ head   │◄──│        │◄──│        │◄──│ tail   │◄──┐
  │   │(最老)  │──►│        │──►│        │──►│(最新)  │──┐│
  │   └────────┘   └────────┘   └────────┘   └────────┘  ││
  │        ▲            （中间页：已提交数据）    ▲        ││
  │   reader_page                              commit───┘│
  │   （读者私页，                                page      │
  └──从 head 前方 swap 进来）                              │
      ^_______________整个环闭合__________________________─┘
```

核心不变式：
* `commit_page` **落后于或等于** `tail_page`——写者预留了空间但可能还没填完/提交（被 NMI 打断时，NMI 的事件嵌在中间先提交）；
* `head_page` 永不越过 `commit_page`（overwrite 模式下写者推 head 时必须确认 commit 已离开该页）；
* `reader_page` 不在环上，是读者的"私有兑换页"（见 §5）。

### 1.1 为什么是 per-CPU

| 动机 | 说明 |
| :--- | :--- |
| 消除锁竞争 | 留位路径只碰本 CPU 的 `tail_page->write` 等局部数据，**零原子 xchg、零自旋锁**（连 atomic 的缓存行弹跳都没有，用的是 local_t） |
| 缓存局部性 | 四指针 + 写计数全部驻留在同一 CPU 的热缓存行 |
| 乱序容忍 | 跨 CPU 事件时间戳允许乱序（`local` 时钟下天然乱序），把排序问题推迟到读取端 `__find_next_entry` 统一做——用"读时归并"换"写时无锁" |
| 嵌套安全 | NMI→IRQ→SoftIRQ→进程的嵌套事件都落在本 CPU buffer，`trace_recursive_lock` 用 per-CPU 位图防递归（§4.2） |

---

## 二、事件编码：8 字节头的极限压缩

```c
// include/linux/ring_buffer.h (v6.6)
struct ring_buffer_event {
	u32		type_len:5,     // ①长度/类型复用字段
		time_delta:27;  // ②距上一事件的时间差（ns）
	u32		array[];        // 负载（type_len==0 时 array[0] 存真实长度）
};

enum ring_buffer_type {
	RINGBUF_TYPE_DATA_TYPE_LEN_MAX = 28,  // ≤28：数据事件，len = type_len*4
	RINGBUF_TYPE_PADDING,                 // 29：padding/丢弃占位
	RINGBUF_TYPE_TIME_EXTEND,             // 30：时间差扩位（delta > 27bit）
	RINGBUF_TYPE_TIME_STAMP,              // 31：绝对时间戳
};
```

* **小事件**（≤ 112 字节负载）：`type_len = len/4` 直接编码进头部，仅 8 字节头（4 字节头 + array）；
* **大事件**：`type_len=0`，`array[0]` 存长度；
* **TIME_EXTEND**：`time_delta` 只装得下 2^27≈134ms 的差；超了就插入 8 字节的 extend 记录（`event->time_delta` 存低 27 位、`array[0]` 存高位）；
* **time_delta 的复用**：`RINGBUF_TYPE_PADDING` 且 `time_delta==0` 表示"页尾 padding"；非 0 表示"被 discard 的事件"（长度在 array[0]）——filter 不匹配的记录不回滚留位（会破坏嵌套），而是改类型为 PADDING 让读者跳过。

> 💡 这套编码让"每事件固定开销 = 4 字节头"成为可能。对比 perf ring buffer（`perf_event_header` 8 字节 + 数据）与 eBPF ringbuf（8 字节 header），ftrace 的 delta 压缩在同页事件密集时最省。

---

## 三、写路径全解：reserve → write → commit

### 3.1 顶层入口

```c
// kernel/trace/ring_buffer.c (v6.6, 节选自 ring_buffer_lock_reserve, line 3794)
ring_buffer_lock_reserve(struct trace_buffer *buffer, unsigned long length)
{
	preempt_disable_notrace();                 // ① 本 CPU 期间不迁移
	if (atomic_read(&buffer->record_disabled))
		goto out;
	cpu = raw_smp_processor_id();
	if (!cpumask_test_cpu(cpu, buffer->cpumask))
		goto out;
	cpu_buffer = buffer->buffers[cpu];
	if (atomic_read(&cpu_buffer->record_disabled))
		goto out;
	if (length > BUF_MAX_DATA_SIZE)            // ② 单事件上限（页减 8）
		goto out;
	if (trace_recursive_lock(cpu_buffer))       // ③ 嵌套检测（见 §4.2）
		goto out;
	event = rb_reserve_next_event(buffer, cpu_buffer, length);
	if (!event)
		goto out_unlock;
	return event;                               // ★返回的地址可直接写负载
 out_unlock:
	trace_recursive_unlock(cpu_buffer);
 out:
	preempt_enable_notrace();
	return NULL;
}
```

返回 NULL 是**常态而非异常**：tracing_on=0、buffer 满（非覆盖模式）、嵌套超限都会安静地丢——tracepoint 调用者（stage6 probe）直接 return，绝不能因此打扰被观测路径。

### 3.2 __rb_reserve_next：无锁留位的五步协议

留位唯一的"锁"是 `local_add_return` 对 `tail_page->write` 的原子加——但语义上它是**乐观的**：中断/NMI 可能同时也在加，协议靠 A→E 屏障与双时间戳收尾（line 3558，v6.6 节选）：

```c
__rb_reserve_next(struct ring_buffer_per_cpu *cpu_buffer, struct rb_event_info *info)
{
	tail_page = info->tail_page = READ_ONCE(cpu_buffer->tail_page);

/*A*/	w = local_read(&tail_page->write) & RB_WRITE_MASK;      // 当前写位
	barrier();
	b_ok = rb_time_read(&cpu_buffer->before_stamp, &info->before);
	a_ok = rb_time_read(&cpu_buffer->write_stamp, &info->after);
	barrier();
	info->ts = rb_time_stamp(cpu_buffer->buffer);               // 取当前时间

	// delta 计算：若读取时间戳被嵌套写打断（ok=false 或 before!=after），
	// 则本事件必须携带绝对时间戳（FORCE|EXTEND）
	if (unlikely(!a_ok || !b_ok || (info->before != info->after && w))) {
		info->add_timestamp |= RB_ADD_STAMP_FORCE | RB_ADD_STAMP_EXTEND;
		info->length += RB_LEN_TIME_EXTEND;
	} else {
		info->delta = info->ts - info->after;
		if (unlikely(test_time_stamp(info->delta)))    // delta 超 27 位
			info->add_timestamp |= RB_ADD_STAMP_EXTEND;
	}

/*B*/	rb_time_set(&cpu_buffer->before_stamp, info->ts);      // 宣布"我要写了"

/*C*/	write = local_add_return(info->length, &tail_page->write);  // ★原子留位
	write &= RB_WRITE_MASK;
	tail = write - info->length;

	if (unlikely(write > BUF_PAGE_SIZE))                  // 越过页尾
		return rb_move_tail(cpu_buffer, tail, info);  //   → 换页（§5）

	if (likely(tail == w)) {
		/* 快路径：A→C 之间没有嵌套写打断我 */
/*D*/		rb_time_set(&cpu_buffer->write_stamp, info->ts);
		barrier();
/*E*/		s_ok = rb_time_read(&cpu_buffer->before_stamp, &save_before);
		if (likely(!(info->add_timestamp & (RB_ADD_STAMP_FORCE|ABSOLUTE))))
			info->delta = info->ts - info->after;   // 正常 delta
		if (unlikely(info->ts != save_before)) {
			/* 慢路径：C→E 之间被嵌套写插入——修 write_stamp 只许前进 */
			...
			(void)rb_time_cmpxchg(&cpu_buffer->write_stamp, info->after, save_before);
		}
	} else {
		/* 慢路径：A→C 之间被嵌套写打断（嵌套者已完成时间戳更新）
		   再取一次 write_stamp，尝试 cmpxchg 推进；失败则 delta=0
		   （与打断我的事件同时间戳，时间轴仍单调） */
		...
	}

	event = __rb_page_index(tail_page, tail);       // 算出事件地址
	rb_update_event(cpu_buffer, event, info);       // 写 type_len/time_delta 头
	local_inc(&tail_page->entries);
	local_add(info->length, &cpu_buffer->entries_bytes);
	return event;                                    // 调用者开始填负载
}
```

**before_stamp/write_stamp 双时间戳协议**是整个文件最精妙的部分：
* `write_stamp` = "最后一个已留位事件的时间"（delta 的基准）；
* `before_stamp` = "我已宣布要写、但还没完成"的时间；
* 嵌套写者通过检查 `before != after` 发现"我打断了一次尚未完成的留位"→ 改用绝对时间戳；被打断的外层通过 `tail != w` 发现"有人插队"→ 重新校准；
* 不变量：**事件的时间戳（页时间 + 逐事件 delta）永远单调不减**，即使写入乱序发生。

### 3.3 commit：谁最后走谁收尾

```c
// ring_buffer_unlock_commit → rb_commit → rb_end_commit (v6.6)
static __always_inline void rb_end_commit(struct ring_buffer_per_cpu *cpu_buffer)
{
	...
 again:
	commits = local_read(&cpu_buffer->commits);
	barrier();
	if (local_read(&cpu_buffer->committing) == 1)       // 我是栈底的提交者
		rb_set_commit_to_write(cpu_buffer);         // ★推进 commit_page
	local_dec(&cpu_buffer->committing);
	barrier();
	/* 提交窗口期间又来了新 commit？重走一遍防止悬挂 */
	if (unlikely(local_read(&cpu_buffer->commits) != commits) &&
	    !local_read(&cpu_buffer->committing)) {
		local_inc(&cpu_buffer->committing);
		goto again;
	}
}
```

`rb_set_commit_to_write`（line 3085）把 `commit_page` 沿链表追到 `tail_page`，并逐页 `page->commit = write`——读者以 `page->commit` 为可见性边界（`smp_wmb()` 保证内容先于 commit 可见）。**只有嵌套栈最外层的提交者才推进 commit**，内层提交是空操作——这就是"中断的事件先提交、被中断的事件后提交，但中间不留洞"的机制：`committing` 计数像引用计数，1→0 的那次负责整体收账。

之后 `rb_wakeups()`（line 3192）检查 `waiters_pending` 标记，通过 `irq_work_queue()` 唤醒阻塞在 `trace_pipe`/`poll()` 上的读者（NMI 里不能直接 wake_up，irq_work 是安全的中转）。

---

## 四、嵌套与递归保护

### 4.1 四级上下文

```c
// kernel/trace/ring_buffer.c (v6.6)
enum {
	RB_CTX_TRANSITION,
	RB_CTX_NMI,        // 1：最内层
	RB_CTX_IRQ,        // 2
	RB_CTX_SOFTIRQ,    // 3
	RB_CTX_NORMAL,     // 4：最外层
	RB_CTX_MAX
};
#define MAX_NEST 5
```

`event_stamp[MAX_NEST]` 按嵌套层级保存时间戳，`rb_update_event` 里 `nest = local_read(&cpu_buffer->committing) - 1` 记账。

### 4.2 trace_recursive_lock：位图防死循环

function tracer 跟踪的函数可能自身触发事件→再进 buffer→无限递归。`trace_recursive_lock`（line 3271 附近的 `current_context` 位图）：

```
current_context 位图（per-CPU）:  bit0=NMI bit1=IRQ bit2=SoftIRQ bit3=Normal
进入：算出当前上下文的位，若已置位 → 返回 1（拒绝写入，事件被丢）
      否则置位。
退出：巧妙之处——清除最低已置位只需 (val - 1) & val：
      0b101 - 1 = 0b100; 0b100 & 0b101 = 0b100  → 清了 bit0
      0b1010 - 1 = 0b1001; & 0b1010 = 0b1000    → 清了 bit1
```

同一上下文第二次进入（递归）被直接拒绝。ftrace 侧还有并行的 `ftrace_test_recursion_trylock`（function tracer 用），两套独立但思路一致。

### 4.3 write 计数器的 32 位戏法

`buffer_page.write` 是 local_t 但被 `RB_WRITE_MASK(0xfffff)` 掩码使用——高 12 位是**中断计数**：跨页边界时要同时重置 write 与 entries 两个计数，任何中断者通过高位计数感知"有人正在跨页"从而退避（`rb_move_tail` 的竞争处理）。这是老设计（2008）的一部分，v6.6 仍在沿用。

### 4.4 32 位平台的 rb_time

64 位时间戳在 32 位机上无法原子读写。`rb_time_struct`（line 475）把 60 位时间拆 top/bottom/msb 三个 30 位 local_t，各带 2 位计数器；读侧循环校验计数器一致性（读到一半被打断则重读），写侧允许覆盖（嵌套写者覆盖外层的部分写入，由 §3.2 的协议兜底）——用"可失败的读 + 可覆盖的写"替代真正的 64 位原子。

---

## 五、两种模式：overwrite vs producer/consumer

`struct trace_buffer.flags` 的 `RB_FL_OVERWRITE` 位决定"写满了怎么办"。

### 5.1 非 overwrite（默认，"丢弃模式"）

`rb_move_tail`（line 2722）发现 tail 前进会碰到 head：

```c
if (rb_is_head_page(next_page, &tail_page->list)) {
	if (!rb_is_reader_page(cpu_buffer->commit_page)) {
		if (!(buffer->flags & RB_FL_OVERWRITE)) {
			local_inc(&cpu_buffer->dropped_events);   // 记账后放弃
			goto out_reset;
		}
		ret = rb_handle_head_page(...);               // 否则推 head
		...
	}
}
```

语义：**保证不覆盖任何已写事件**，满了就丢新事件。适合"抓现场"式诊断（trace-cmd record 后离线分析，事件必须完整）。

### 5.2 overwrite（"覆盖模式"）

`echo 1 > /sys/kernel/tracing/options/overwrite`（或 trace-cmd）后，写者通过 `rb_handle_head_page`（line 2485）**吃掉最老的页**给新事件腾地方。难点：head 可能正被另一个 CPU 的读者盯着 swap。协议用 next 指针的低位状态做信号量：

```c
/*
 * 指针低位编码（list 指针最低位，因对齐恒为 0，可盗用）：
 *   NORMAL —— 稳态
 *   HEAD   —— "head 指针指向我"标记
 *   UPDATE —— 有人正把 head 往后推（写者置位，读者见到即自旋等待）
 *   MOVED  —— 读者刚把 head 换成了 reader_page
 */
type = rb_head_page_set_update(cpu_buffer, next_page, tail_page, RB_PAGE_HEAD);
switch (type) {
case RB_PAGE_HEAD:      // 我是第一个推 head 的：记账覆盖
	local_add(entries, &cpu_buffer->overrun);        // ★lost events 计数来源
	local_sub(rb_page_commit(next_page), &cpu_buffer->entries_bytes);
	local_inc(&cpu_buffer->pages_lost);
	break;
case RB_PAGE_UPDATE:    // 我是打断别人的中断：继续推进即可
	break;
case RB_PAGE_NORMAL:    // 已有中断替我干完了
	return 1;
case RB_PAGE_MOVED:     // 读者刚 swap 走：全部重来
	return 1;
}
/* 把新 head 的 next 标 HEAD，旧的复位 NORMAL —— 全程 int3 式 cmpxchg 逐状态迁移 */
```

语义：**永远保留最新事件，代价是最老的被静默覆盖**。适合长期监控（buffer_size_kb 小、只关心最近 1 秒的"飞机黑匣子"）。

### 5.3 读取侧的 swap 魔术：rb_get_reader_page

读者（consume 模式）不直接从环上读，而是把自己的 `reader_page` **换进环里**（line 4543）：

```
读之前:  [P3] ⇄ [P4=head] ⇄ [P5] ...     reader_page(空) 游离在外
                 ▲ commit_page 可能更靠后

1. 检查 reader_page->read < page_size？ → 还有没读完的，继续读
2. commit_page == reader_page？ → 已追平写者，无新数据
3. 重置 reader_page（write/entries/commit 清零）
4. spin: 把空 reader_page 插到 head 前面（list 前后指针改接），
        smp_mb() 后读 overrun 快照，
        rb_head_page_replace() 用 cmpxchg 把 head 的 prev 指向 reader_page
        —— 与写者的 rb_handle_head_page 竞争：谁 cmpxchg 失败谁重试
5. 成功后 head_page 前移一页（原 head 变成新的 reader_page！）
   cpu_buffer->lost_events = overrun - last_overrun   ★把这一页上被覆盖的事件数报给读者
6. goto again（新 reader_page 可能整页都是数据）

读之后:  [P3] ⇄ [RP=旧head] ⇄ [P5] ...  读者独占旧 head 页慢慢读
                                       写者写 P5/P6，互不干扰
```

这个"换页"设计的收益：**读慢一点也不阻塞写**——写者永远只碰 tail/commit/head 指针，读者在换下来的私页上随便游荡。代价是 arch_spinlock（`cpu_buffer->lock`）+ local_irq_save 只保护 swap 本身。读者还要在返回前 `udelay` 等待可能正在写这一页的写者跨过页边界（"writer stuck" 检查，最多 1 秒）。

`lost_events` 的传递链：`rb_handle_head_page` 的 `local_add(entries, overrun)` → `rb_get_reader_page` 的差值 → `ring_buffer_consume(..., &lost_events)` → `trace_iterator->lost_events` → 输出里打印 `(lost events)`。

---

## 六、用户态读取协议

### 6.1 两条读取通道

| | `trace`（快照） | `trace_pipe`（流式） |
| :--- | :--- | :--- |
| 底层 | `ring_buffer_iter`（游标：head/head_page/read_stamp/page_stamp） | `ring_buffer_consume`（swap reader page） |
| 语义 | 不消费，可反复读；读到 buffer 实时状态 | 读走即删，配合写者像管道 |
| 并发 | 多读者 OK | **每 CPU 同时仅一个读者**（pipe_cpumask 强制） |
| 数据保真 | overwrite 下新旧混杂可能 | 同左 |
| 阻塞 | 不阻塞（EOF） | `poll()`/读阻塞，rb_wakeups→irq_work 唤醒 |

迭代器读（trace 文件）的核心是 `ring_buffer_iter_peek`/`advance`：iter 自带 head_page 与 head 索引，advance 只是推进自己的游标——**从不修改共享状态**，这就是它不需要锁的原因。跨 CPU 归并由 `__find_next_entry` 完成：对每个 CPU 的 iter peek 出候选事件，取时间戳最小者。

### 6.2 trace_seq 与 print_trace_line

`struct trace_seq`（include/linux/trace_seq.h）是 PAGE_SIZE 的临时格式化缓冲（`seq_buf` 包装）。读取主循环（trace.c:6960 一带）：

```
tracing_read_pipe:
  while (每一轮) {
     iter->ent = find_next_entry(iter)          // 跨 CPU 归并取最早
     trace_seq_init(&iter->seq)
     print_trace_line(iter)
        ├─ 先试 trace_handle_return（seq 满则返回 PARTIAL_LINE 让外层先 flush）
        └─ iter->trace->print_line ? 定制 : trace_event 的格式化回调
     copy_to_user(seq 缓冲)                     // 一页 PAGE_SIZE
     trace_consume(iter)                        // pipe 才消费；trace 不调
     处理 lost_events 提示
  }
```

### 6.3 buffer 管理文件

```
buffer_size_kb            # 每每 CPU buffer 大小（KB；写即 resize，经工作线程逐 CPU 交换）
buffer_total_size_kb      # 总量 = size × nr_cpus
per_cpu/cpu0/trace        # 单 CPU 的快照
per_cpu/cpu0/trace_pipe   # 单 CPU 流式（可绑核读取，避免归并开销）
per_cpu/cpu0/trace_pipe_raw  # 原始二进制（perf/BPF/trace-cmd 解析用）
per_cpu/cpu0/snapshot     # 单 CPU 的 max_buffer
per_cpu/cpu0/stats        # ★buffer 健康检查：
                          #   entries / overrun / commit overrun / 
                          #   dropped events / read events / oldest event ts...
buffer_percent           # 0-100：buffer 使用率达到多少才唤醒阻塞读者
                          # （等待"攒一批再读"的批量化选项）
```

resize（`ring_buffer_resize`）通过 `nr_pages_to_update` + per-CPU 工作线程完成：把新页挂 `new_pages` 链，逐 CPU 在 reader_lock 下拼进环——正在进行的写者通过 `resize_disabled` 计数被挡住。

---

## 七、深挖：为什么 v6.6 的 ring buffer 不用无锁 MPSC 队列

对比现代设计（eBPF ringbuf 的 single-producer-per-CPU + 2 个指针；LTTng 的 "slot + TSC 序号"），ftrace ring buffer 的复杂度几乎全部来自一个需求：**NMI 嵌套安全**。它必须在"NMI 打断 IRQ 正在写的半个事件"时依然正确——这逼出了：
* 留位不回滚（嵌套者的空间在外面套着，内层先完成）；
* 时间戳协议容忍覆盖；
* 唤醒只能 irq_work（NMI 里 spin_lock/wait_queue 都非法）；
* 读者用换页而不是读指针（避免写者常见读游标的缓存行）。

ring-buffer-design.rst 里 Steven Rostedt 的原话概括了设计目标："readers never block writers, writers never block readers, and the fastest path is the write path"。

---

## 八、对比：eBPF ringbuf（BPF_MAP_TYPE_RINGBUF）

| 维度 | ftrace ring buffer | eBPF ringbuf（kernel/bpf/ringbuf.c） |
| :--- | :--- | :--- |
| 拓扑 | N 个 per-CPU buffer，读时归并 | **单个**共享 buffer（所有 CPU 写同一个） |
| 每记录开销 | 4 字节头（type_len+delta） | 8 字节头（len + busy/discard 两个标志位） |
| 留位 | `local_add_return`（计数器） | `cons_pos/prod_pos` 的 64 位原子递增 + 对齐 |
| 嵌套 | NMI 安全（reserve 不回滚） | busy 位 + 重试/丢弃；NMI 场景依赖预留成功与否 |
| 唤醒 | irq_work + wait_queue（page 粒度） | `bpf_ringbuf_commit` 时 waitqueue + `spin_lock_irqsave`（记录粒度，延迟更低） |
| 覆盖 | overwrite/丢弃两种模式 | 满则丢弃（预留失败返回 NULL，BPF 程序自己记账） |
| 用户态消费 | trace_pipe_raw + format 手工解码，或 trace-cmd | `mmap` + data_head/tail，`libbpf` ring_buffer/resizable 采样器 |
| 归并 | 读取端跨 CPU 时间戳归并（锁 trace_types_lock 内） | 天然全序（单 buffer，prod_pos 即全序） |
| 多消费者 | 每 trace_pipe 独占 per-CPU | 单消费者设计（多消费者要 eBPF 侧 fan-out 或 perf array） |

eBPF 还保留了 `BPF_MAP_TYPE_PERF_EVENT_ARRAY`（per-CPU perf ring）作为兼容路径——它就是本章 buffer 的"perf 版亲戚"（events/ring_buffer.c），每 CPU 一页环，靠 `data_head` 无锁发布。

---

## 九、实战观测

```bash
# 1. buffer 健康：丢没丢事件？
cat per_cpu/cpu*/stats | grep -A1 cpu
#   overrun 非零 → overwrite 模式覆盖了旧事件（正常）
#   dropped events 非零 → 非覆盖模式满了（考虑加大 buffer 或上 filter）
#   commit overrun → 嵌套太深（异常，通常意味着 buffer 太小）

# 2. resize 体验
echo 16384 > buffer_size_kb        # 16MB/CPU，resize 立即生效
cat buffer_total_size_kb

# 3. 覆盖模式对比实验
echo 0 > options/overwrite; 高频事件压满 → 看 dropped events 增长
echo 1 > options/overwrite; 同样压 → 看 overrun 增长、events 总数恒定

# 4. 低开销读取：绑核读单 CPU raw 流
taskset -c 0 cat per_cpu/cpu0/trace_pipe_raw > /tmp/raw.cpu0 &

# 5. 批量唤醒：等 buffer 半满再读
echo 50 > buffer_percent
cat trace_pipe    # 现在会攒到 50% 或被唤醒超时才返回

# 6. 验证 swap-reader：读 trace_pipe 同时猛写事件，观察 stats 的 pages_read 增长
```

---

## 十、思考题

1. `rb_set_commit_to_write` 为什么可以由"嵌套栈最外层"的提交者一次性推进所有内层事件的可见性？如果每个嵌套事件自己推进 commit_page，会破坏什么不变式？
2. 非 overwrite 模式下写者发现 buffer 满会 `dropped_events++` 后放弃本次留位。但留位的 `local_add_return` 已经加过了 write——它是如何"假装没加过"的？（提示：`out_reset → rb_reset_tail`，注意 padding 记录与 write 的回退。）
3. before_stamp 协议里，如果只有 write_stamp 一个时间戳（无 before_stamp），哪个具体场景会计算出错误的 delta？（提示：A→C 之间被 NMI 打断且 NMI 事件已完成提交。）
4. 读者 swap 进的 reader_page 与写者 `rb_handle_head_page` 看到的 `RB_PAGE_MOVED` 状态：画出完整的状态迁移图，说明为什么 cmpxchg 失败的读者必须 goto spin 重来而不是继续。
5. eBPF ringbuf 用单共享 buffer 换来了天然全序，为什么 ftrace 不这么改？（提示：单 buffer 的 prod_pos 会成为所有 CPU 的缓存行争用点；ftrace 事件的写频率与 BPF 程序的典型频率差异。）
6. `buffer_percent` 机制下，阻塞读者的唤醒由 `rb_wakeups` 的 `pages_touched` 判断触发（`shortest_full`）。思考：为什么用"页数"而不是"字节数"做唤醒条件？

---

> 下一章：[kprobes 与 uprobes](04-kprobes与uprobes.md) —— int3 断点的 x86 全路径、optprobe 的 5 字节 jmp、kretprobe 的返回地址栈，以及用户态的 XOL 单步。
