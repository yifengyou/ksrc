# perf 与 BPF 观测（Linux 6.6）

> 阅读目标：补齐观测版图的两翼——perf_event 子系统的内部构造（`struct perf_event`/`struct pmu` 抽象、x86 PMU 与 PEBS、软件事件、采样如何经 PMI/hrtimer 写入 mmap ring）；perf 如何"吃"tracepoint（tracepoint 并入 perf_event 的 `perf_tracepoint` pmu）；eBPF 的三种观测挂载与 map 生态；bpftrace 一行流；最后给出观测技术的生产选型表与开销预算方法论。

---

## 一、perf_event 子系统内幕

### 1.1 两个核心抽象

perf 的世界由两个结构撑起（include/linux/perf_event.h）：

```c
struct perf_event {          // 一个"计数器/事件"实例（节选，line 671 起）
	struct list_head	event_entry;    // 挂入 perf_event_context
	struct list_head	sibling_list;   // 同组事件（group leader 概念）
	struct rb_node		group_node;    // pinned/flexible 红黑树（PMU 调度）
	struct perf_event	*group_leader;
	struct pmu		*pmu;           // ★归属的 PMU
	enum perf_event_state	state;          // INACTIVE/ACTIVE/OFF...
	local64_t		count;           // ★计数本体（per-CPU 累加）
	u64			total_time_enabled, total_time_running;  // 多路复用记账
	...
	struct perf_output_handle mmap 布局相关;
	perf_overflow_handler_t	overflow_handler;  // ★溢出处理（默认 perf_event_output_forward）
	...
};

struct pmu {                 // "计数器供给方"抽象（line 302 起）
	const char		*name;       // "cpu"/"software"/"tracepoint"/...
	int			type;        // PERF_TYPE_* 对应编号
	int			capabilities;  // PERF_PMU_CAP_NO_NMI/...
	int __percpu		*pmu_disable_count;
	int			task_ctx_nr; // per-task vs per-cpu 上下文
	void (*pmu_enable)  (struct pmu *pmu);
	int  (*event_init)  (struct perf_event *event);  // 识别/校验事件
	int  (*add)         (struct perf_event *event, int flags); // 上 PMU
	void (*del)         (struct perf_event *event, int flags); // 下 PMU
	void (*start)       (struct perf_event *event, int flags);
	void (*stop)        (struct perf_event *event, int flags);
	void (*read)        (struct perf_event *event);
	int  (*event_idx)   (struct perf_event *event);   /*optional*/
	...
};
```

每种事件源实现一个 pmu 并 `perf_pmu_register`：x86 硬件 PMU（`perf_event_pmu`，名字 "cpu"）、软件事件 `perf_swevent`（"software"）、tracepoint 桥 `perf_tracepoint`、kprobe/uprobe 的 pmu（BPF 挂载的基础，见 §3.1）。`perf_event_open` 系统调用按 `attr.type` 找 pmu → `pmu->event_init` 认领 → 建上下文（`perf_event_context`，分 cpu/task 两类）→ `add/start` 上计数器。

### 1.2 x86 硬件 PMU 与 PEBS

* **通用计数器**：每个核若干（Skylake+ 4 个 GP + 3 个 fixed：`inst_retired`/`cpu_clk_unhalted`/`topdown slots`）。事件冲突时 `perf_assign_events` 做"每 tick 硬件分时多路复用"（rotate，读数按 `time_enabled/time_running` 换算）；
* **PMI（Performance Monitoring Interrupt）**：计数器写 `sample_period` 到 MSRs（`IA32_PERFEVTSELx` + counter），溢出触 APIC 的 `LOCAL_PERF_VECTOR`（NMI 或普通中断，x86 默认 `PEBS_UNCALIBRATED` 时代多配 NMI 以防中断风暴下丢样）；`intel_pmu_handle_irq`（arch/x86/events/intel/core.c:3074）逐 active 事件 `perf_event_overflow`；
* **PEBS（Processor Event-Based Sampling）**：溢出时由**硬件**把寄存器快照写进 DS（Data Save）区的一条记录（含 RIP/RSP/GPR/标志），`intel_pmu_drain_pebs_buffer` 批量收割——采样成本从"全量上下文保存"降到"硬件写 ~200 字节"，`perf record -e cycles:pp`（`pp` 即 PEBS 前缀）在高频采样下显著降噪。PRECISE_STORE/PEBS-LL 还能带内存地址与 latency（`perf mem`）。

### 1.3 软件事件

`perf_swevent`（kernel/events/core.c:10018）处理 `PERF_TYPE_SOFTWARE`：

| 事件 | 实现钩子 |
| :--- | :--- |
| cpu-clock / task-clock | **hrtimer**：`hrtimer_init(&hwc->hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD)`（core.c:11061），到期回调 `perf_swevent_hrtimer` 用当前 regs 生成样本 |
| page-faults / faults | `perf_sw_event(PERF_COUNT_SW_PAGE_FAULTS)` 插桩在缺页路径（`do_user_addr_fault`） |
| context-switches | `perf_event_task_sched_out/in`（调度器内联钩子） |
| cpu-migrations / task migrations | 同上 + `perf_event_comm` |
| minor/major faults | 同 page-faults 细分 |

软件计数的累加走 `perf_swevent_event`（core.c:9630 一带）：`local64_add(nr, &event->count)`；采样模式用 `period_left` 的符号翻转判断溢出（cmpxchg 循环），批量溢出用 `perf_swevent_overflow` 逐次触发。

### 1.4 采样管线：从溢出到 mmap ring

```
PMU 计数器溢出（NMI）or hrtimer 到期
  └─ intel_pmu_handle_irq / perf_swevent_hrtimer
       └─ perf_event_overflow → __perf_event_overflow (core.c:9500)
            ├─ __perf_event_account_interrupt   // throttle（默认 1s 内 ~10 万次上限）
            └─ event->overflow_handler(event, data, regs)
                 = perf_event_output_forward (core.c:7835)
                     └─ __perf_event_output
                          ├─ perf_prepare_sample(data, event, regs)  (core.c:7596)
                          │     按 attr.sample_type 填充：IP/CALLCHAIN/RAW/
                          │     REGS_USER/STACK_USER/BRANCH_STACK/WEIGHT...
                          │     （struct perf_sample_data，header.h:1143）
                          ├─ perf_output_begin_forward(handle, ...)  ★mmap ring 留位
                          └─ perf_output_sample(&handle, &header, data, event)
                               // 逐字段写入用户态 mmap 的 data 页

用户态（perf record）:
  mmap 布局: [meta 页: perf_event_mmap_page{data_head, data_tail}] [data 环]
  perf 自旋读 data_head 前进量 → 解析事件 → 写 perf.data
  （data_head 由内核 store_release，用户态 consumer 自己推 data_tail）
```

`perf_output_begin` 满 ring 时丢事件（`ring_buffer` 模块的 `rb->lost` 计数，输出里 `PERF_RECORD_LOST`）——perf 的 ring 是**覆盖式**但可配 watermark 唤醒。callchain 由 `perf_callchain`（内核侧用栈回溯器：x86 `ORC unwinder`）在 PMI 上下文采集，`fp`/`dwarf`/`lbr` 三种模式。

### 1.4.1 用户态 mmap 布局细看

```c
// include/uapi/linux/perf_event.h (要点)
struct perf_event_mmap_page {
	__u32	version;
	...
	__u64	data_head;     // ★内核写（事件尾部）；smp_store_release 发布
	__u64	data_tail;     // ★用户写（消费进度）；读完后推进
	__u64	data_offset;   // data 区起始
	__u64	data_size;
	...  // capabilities、aux_head/aux_tail（Intel PT / ARM SPE 的 AUX 区）
};
```

mmap 的长度 = meta 页 + `2^n` 页 data 环。内核写侧 `perf_output_begin` 用 `local_cmpxchg` 推进 `rb->head` 留位、写事件、`perf_output_end` 时若越过 `attr.wakeup_watermark` 就唤醒（`ring_buffer_wake`）。用户态读侧（perf 库/自研消费者）的经典循环：读 `data_head` → rmb → 拷贝 [tail, head) → 处理 → `store_release(data_tail, head)`。**反向（overwrite）模式**（`perf_event_mlock` + `PERF_FORMAT` 的 backward 标志）下用户读的是"最新的窗口"，尾追 head——自研 always-on 剖析器的常用形态。

### 1.5 上下文与组调度：counter 的"时分复用"

硬件计数器数量有限（4~8 个 GP），事件多于计数器时怎么办？`perf_event_context`（per-cpu 或 per-task）把事件挂两棵红黑树：

```
perf_event_context
├── pinned_tree：要求独占的（attr.pinned=1，如 NMI watchdog 的周期事件）
└── flexible_tree：可分时的（perf record 的采样事件）
      └── 每 tick（ctx->time）触发 perf_event_task_tick → rotate_context：
          flexible 树上的事件轮换上下 PMU（按 group_index 顺序）
          下 PMU 的事件 stop 时 PERF_EF_UPDATE 把计数折回 count，
          time_running 累计但不含离线时间
```

读数语义因此分两种：`perf stat` 的计数是 `count × (time_enabled/time_running)` 的换算（scale），`perf record` 的采样率在 rotation 下自然降频——这是"每核只能数 4 件事"的物理约束下的优雅折衷。`perf_event_open` 的 group（`group_fd`）把一组事件绑成原子调度单元：leader 上 PMU 时整组一起上（cgroup 事件、混合 PMU 的 group 走 `perf_pmu_ctx` 的跨 PMU 编排）。

---

## 二、perf 与 ftrace 的联姻：tracepoint 并入 perf_event

tracepoint 不止服务 ftrace——同一静态点可以喂 perf（`perf record -e sched:sched_switch`）。桥是第七个 pmu：

```c
// kernel/events/core.c (v6.6, line 10060)
static int perf_tp_event_init(struct perf_event *event)
{
	if (event->attr.type != PERF_TYPE_TRACEPOINT)
		return -ENOENT;
	err = perf_trace_init(event);       // 按 event->attr.config(事件 id) 找 call
	...
}

static struct pmu perf_tracepoint = {
	.task_ctx_nr	= perf_sw_context,
	.event_init	= perf_tp_event_init,
	.add		= perf_trace_add,       // 把 event 挂进 call->perf_events（per-cpu hlist）
	...
};
```

而 tracepoint 侧为 perf 生成的探针（include/trace/perf.h，与 ftrace 的 stage6 平行的模板）：

```c
// include/trace/perf.h (v6.6, 展开效果)
static notrace void
perf_trace_sched_switch(void *__data, struct rq *rq,
			struct task_struct *prev, struct task_struct *next)
{
	struct trace_event_call *event_call = __data;      // __data = call（非 file！）
	struct hlist_head *head = this_cpu_ptr(event_call->perf_events);

	if (!bpf_prog_array_valid(event_call) && hlist_empty(head))
		return;                                    // 该 CPU 无 perf 消费者 → 零成本退出

	entry = perf_trace_buf_alloc(__entry_size, &__regs, &rctx);  // per-cpu 临时缓冲
	perf_fetch_caller_regs(__regs);                   // 合成 regs（给 perf 的 IP 字段）
	{ /* TP_fast_assign */ }

	perf_trace_run_bpf_submit(entry, __entry_size, rctx,
				  event_call, __count, __regs, head, __task);
	  ├─ 遍历 head 上的 perf_event → perf_tp_event(event, entry, ...)
	  │     （含 perf 侧 filter：perf_tp_filter_match → filter_match_preds，
	  │       与 ftrace filter 同一套谓词引擎！）
	  └─ BPF：call->prog_array 上挂的程序逐个执行（把 entry 作为 ctx）
}
```

> 💡 **一份埋点、三种消费者**至此闭环：`funcs` 数组里可能同时有 ftrace 的 `trace_event_raw_event_*`（写 ftrace ring buffer，`__data` 是 file）和 perf 的 `perf_trace_*`（写 perf mmap ring，`__data` 是 call）——互不干扰，iterator 逐个调用。BPF 程序借 perf 事件通道（`bpf_perf_event_output`）把样本送进 `BPF_MAP_TYPE_PERF_EVENT_ARRAY`。

---

## 三、BPF 观测

### 3.1 三种挂载点

| prog type | 挂载机制 | ctx 内容 | 典型取参 |
| :--- | :--- | :--- | :--- |
| `BPF_PROG_TYPE_TRACEPOINT` | attach 到 `tp->funcs`（bpf_raw_tracepoint 或 perf 事件） | tracepoint 参数数组 | `bpftrace` 的 `args->xxx` |
| `BPF_PROG_TYPE_KPROBE` | perf_event_open(type=6, "kprobe:func") + `PERF_EVENT_IOC_SET_BPF`；内核侧 `kprobe_dispatcher` → `bpf_prog_run` | `struct pt_regs *` | `PT_REGS_RC/PARM1`... |
| `BPF_PROG_TYPE_KPROBE`+kretprobe | 同上（`retprobe` 位） | 同上（返回时刻的 regs） | `PT_REGS_RC` 取返回值 |
| `BPF_PROG_TYPE_TRACEPOINT（raw_tp）` | `bpf_raw_tracepoint_open`：bpf_func 打包原始参数 | 裸参数指针数组 | `bpf_probe_read_kernel` 手工取 |
| `BPF_PROG_TYPE_TRACING`（fentry/fexit/fmod_ret/lsm） | **BTF+直接跳转**：ftrace IPMODIFY/DIRECT 通道把程序 trampoline 织进函数入口/出口 | 强类型 BTF ctx（无需 probe_read！） | 直接 `ctx->arg1` |

**kprobe 挂载的内核路径**（`perf_event_open` 的 type=6 `PERF_TYPE_KPROBE`，trace_event_perf.c:247）：attr 里带 `kprobe_func`/`kprobe_addr`/`probe_offset` 字符串与地址——

```c
// kernel/trace/trace_event_perf.c (v6.6, 节选)
int perf_kprobe_init(struct perf_event *p_event, bool is_retprobe)
{
	tp_event = create_local_trace_kprobe(      // 动态造一个"私有 trace_event_call"
		func, (void *)(unsigned long)(p_event->attr.kprobe_addr),
		p_event->attr.probe_offset, is_retprobe);
	...
	ret = perf_trace_event_init(tp_event, p_event);   // 挂进 perf 事件上下文
}

/* 随后 ioctl(fd, PERF_EVENT_IOC_SET_BPF, prog_fd)：
     perf_event_set_bpf_prog → tp_event->prog_array 追加程序 */
```

注意这**复用了第 4 章的 trace_kprobe 基础设施**：`create_local_trace_kprobe` 造的"local"探针与 `kprobe_events` 创建的共享同一个 dispatcher（`kprobe_dispatcher` → `trace_probe_dispatch`，再到 BPF 的 `bpf_prog_run_array`）；uprobe 同构（`perf_uprobe_init`，`attr.uprobe_path` + `probe_offset`）。**内核里没有"BPF 专用探针"**——BPF 是 kprobe/uprobe/tracepoint 三种探针的第四类消费者（ftrace、perf、eBPF）。

`fentry/fexit`（`CONFIG_DEBUG_INFO_BTF` + `CONFIG_DYNAMIC_FTRACE_WITH_DIRECT_CALLS`）是新时代答案：没有 int3、没有 pt_regs 拷贝，验证器按 BTF 直接生成参数访问——开销接近 function tracer 的 ops 回调。

**取栈**：`bpf_get_stack(ctx, buf, size, flags)` / `bpf_get_stackid(ctx, map, flags)`（kernel/trace/bpf_trace.c:81 声明）把内核/用户栈回溯进 map（`BPF_MAP_TYPE_STACK_TRACE`），`flags` 区分 KERNEL/USER 栈；火焰图的后端。

### 3.2 map 生态（观测视角）

| map | 用途 | 特点 |
| :--- | :--- | :--- |
| `BPF_MAP_TYPE_HASH` | 键值聚合（按 pid/tgid/addr） | 内核哈希，读侧 `map_lookup_elem` |
| `BPF_MAP_TYPE_PERCPU_HASH/LRU_HASH` | 高并发聚合 / 自动淘汰 | |
| `BPF_MAP_TYPE_STACK_TRACE` | `get_stackid` 去重存栈 | id 复用（LRU 淘汰） |
| `BPF_MAP_TYPE_RINGBUF` | ★事件流（替代 perf array） | 单共享 ring、busy 位留位、记录粒度唤醒（第 3 章对比） |
| `BPF_MAP_TYPE_PERF_EVENT_ARRAY` | 兼容路径（perf ring per-CPU） | 与 perf 工具互通 |
| `BPF_MAP_TYPE_HISTOGRAM`（内建） | 无——直方图由 bpftrace 用 HASH 语法糖实现 | `hist()`/`lhist()` 编译成 log2 桶 |

### 3.3 程序执行的安全性边界（生产视角）

验证器（verifier）保证：无界循环拒绝（6.6 已支持有界循环）、指针访问必须 probe_read 系、栈深 512 字节、程序总量限额（`RLIMIT_MEMLOCK` → 6.6 的 memcg 计费）。`bpf_prog_active` per-CPU 计数防 NMI 嵌套执行程序（bpf_trace.c:113）。这些都意味着 **BPF 观测的尾延迟可控**——这是相对 kprobe C 模块（一处 BUG 全机 panic）的根本优势。

---

## 四、bpftrace 一行流大全

bpftrace 把"probe + 过滤 + map 聚合"压缩成一行；下面是生产最常用的套件（每个都可 `--dry-run` 看 BPF 生成）：

```bash
# ── runqlat：运行队列延迟分布（wakeup → 上CPU）──
bpftrace -e '
tracepoint:sched:sched_wakeup  { @q[args->pid] = nsecs; }
tracepoint:sched:sched_switch
 /args->next_pid != 0 && @q[args->next_pid]/
 { @usecs = hist((nsecs - @q[args->next_pid]) / 1000);
   delete(@q[args->next_pid]); }'

# ── offcputime：off-CPU 时间（谁在睡、睡了多久、栈）──
bpftrace -e '
kprobe:finish_task_isolated { } # (示意) 实际用 kprobe:try_to_wake_up 反向记账
tracepoint:sched:sched_switch
 /args->prev_state == 2/        # TASK_UNINTERRUPTIBLE (D)
 { @[ksym(kstack)] = sum(1); }'  # 完整版见 bpftrace 官方工具

# ── funcslower：函数耗时（kprobe+kretprobe 对）──
bpftrace -e '
kprobe:vfs_read { @start[tid] = nsecs; }
kretprobe:vfs_read /@start[tid]/
 { @ns = hist(nsecs - @start[tid]); delete(@start[tid]); }'

# ── opensnoop：谁在开文件 ──
bpftrace -e '
tracepoint:syscalls:sys_enter_openat
 { printf("%s %s %s\n", comm, pid, str(args->filename)); }'

# ── biolatency：块 IO 延迟直方图 ──
bpftrace -e '
kprobe:blk_account_io_start  { @start[args->dev] = nsecs; }   # 或 tracepoint:block:block_rq_issue
kprobe:blk_account_io_done /@start[args->dev]/
 { @usecs = hist((nsecs - @start[args->dev])/1000);
   delete(@start[args->dev]); }'

# ── 自写：SYN 洪峰时的 CPU 归因（TCP 事件 + 栈直方图）──
bpftrace -e '
tracepoint:tcp:tcp_retransmit_skb
 { @retrans_stacks[kstack] = count(); }'

# ── 自写：内存分配大小 × 调用方 ──
bpftrace -e '
kprobe:__kmalloc { @size = hist(args->size); @caller[ksym(kstack(1))] = count(); }'
# 注意：kprobe 的 args->size 依赖 kallsyms/BTF；fentry 版本更稳：
# fentry:__kmalloc { @size = hist(ctx->size); }
```

（`bpftrace -l 'tracepoint:sched:*'` 可列全部挂载点；`kstack`/`ustack`、`ustacks` 依赖 frame pointer 或 DWARF。）

---

## 五、选型表：tracepoint vs kprobe vs BPF（vs perf）

| 维度 | 静态 tracepoint | kprobe（trace_kprobe/kprobe_events） | eBPF 程序 |
| :--- | :--- | :--- | :--- |
| 覆盖范围 | 仅已埋点的位置 | 任意内核符号/地址（黑名单除外） | 同 kprobe + fentry + USDT |
| 参数获取 | 编译期定死、语义稳定 | regs/内存 fetch（可失效：编译器改布局） | fentry 强类型；kprobe 同左 |
| 每事件开销 | 最低（~百 ns） | int3 高（~μs）；fentry/optimized 低 | 宿主开销 + 程序 ~50-500ns |
| 聚合能力 | 无（配 hist trigger 有限） | 无 | ★map 任意聚合，事件可不落盘 |
| 过滤位置 | 事后 filter（仍先写 buffer） | 同左 | ★谓词前置，不匹配零成本 |
| 安全性 | 编译期保证 | 探针 BUG 可 panic | 验证器保证 |
| 生态 | ftrace/trace-cmd/perf 全兼容 | 同左 | bpftrace/BCC/libbpf |
| 适用 | 通用事件记录、低开销长期监控 | 无埋点函数、取内部状态、临时诊断 | 定制指标、直方图、生产常驻 |

perf 的位置：**需要硬件计数器（cycles/instructions/cache-miss）或微架构事件（PEBS/内存延迟）时唯一选择**；软件事件与 tracepoint 采集上与 ftrace/BPF 重叠，选生态顺手即可。

---

## 六、生产环境注意事项

### 6.1 开销预算

* **事件频率 × 每事件成本** 先算再开：100 万事件/秒 × 300ns ≈ 30% 的一个核——高频事件（中断、锁、内存分配）只做**聚合**（hist）不落盘；
* **kprobe 热点陷阱**：给每秒百万次的函数（如 `__memcpy`）上 int3 kprobe = 单核陷进异常处理；确认 `/sys/kernel/debug/kprobes/list` 里是 `[OPTIMIZED]`/`[FTRACE]` 前缀；
* **function tracer 全开**在生产上相当于全局减速 2~10 倍——永远配 `set_ftrace_filter` 缩小面；
* perf 采样率（`-F`）与 throttle：默认上限内安全，PEBS 前缀 `:pp` 降抖动。

### 6.2 爆栈与递归限制

* function_graph 深度 50（`FTRACE_RETFUNC_DEPTH`），深栈截断不报错（`trace_overrun`）；
* ring buffer 嵌套 5 层（`MAX_NEST`），NMI storm 下事件静默丢弃；
* BPF 栈 512 字节 + 无递归（`bpf_prog_active`）——写复杂逻辑用尾调用/循环展开。

### 6.3 巨事件与 buffer 言论

* 单事件上限 = `PAGE_SIZE - 头`（`BUF_MAX_DATA_SIZE`）；`__string`/`__dynamic_array` 大负载前先估算频率；
* overwrite 模式的"黑匣子"语义 vs 非覆盖模式的"完整性"语义——取证场景（要事后分析）选后者并预置足够 buffer_size_kb；
* `trace_marker` 写入也走同一 buffer：用户态打点风暴同样能吃满。

### 6.4 权限与部署

* 大多数接口需 root（或 `perf_event_paranoid <= 1`、`kernel.perf_event_max_sample_rate` 调节）；
* BPF：`unprivileged_bpf_disabled` 默认 1；容器内挂载常需 privileged 或 BPF LSM 白名单；
* tracefs 是 debugfs 的兄弟：生产镜像里挂载选项（只读/隐藏）按合规要求处理。

### 6.5 时钟与多源对齐

跨工具对时间轴是生产排障的常见痛点：

* ftrace 的 `trace_clock=mono` 与 perf 的 `CLOCK_MONOTONIC` 采样同源，可精确对表；默认 `local`（sched_clock）只能同 CPU 对比；
* BPF 里的 `bpf_ktime_get_ns()` 也是 CLOCK_MONOTONIC——`bpftrace` 事件与 ftrace 事件可混合排序；
* `trace_marker` 是对齐锚点：脚本往里写时间戳，事后的多源日志按此对齐。

---

## 七、实战观测

```bash
# 1. perf 吃 tracepoint（与 ftrace 对照）
perf record -e sched:sched_switch -a -- sleep 5
perf script | head          # 每事件一行：时间戳/CPU/comm/prev->next

# 2. CPU 剖析 + callchain（火焰图原料）
perf record -F 99 -a -g -- sleep 10
perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg

# 3. PEBS 精确 IP 与内存延迟
perf record -e cycles:pp -a -- sleep 5
perf mem record -- sleep 5 && perf mem report

# 4. BPF 常驻（libbpf skeleton 方式，生产推荐）
# bpftool prog load monitor.bpf.o /sys/fs/bpf/monitor
# （带 BTF 的 CO-RE 程序跨内核版本可重定位）

# 5. 把 ftrace buffer 与 BPF 聚合放进同一时间轴
bpftrace -e 'BEGIN { printf("marker\n"); }' &      # 对齐用
cat /sys/kernel/tracing/trace_pipe > /tmp/f.txt &
```

---

## 八、思考题

1. `perf_trace_*` 探针里 `__data` 是 `trace_event_call` 而 ftrace 的 `trace_event_raw_event_*` 里是 `trace_event_file`。同一个 tracepoint 的 funcs 数组同时挂两者时，iterator 如何避免混淆？`bpf_prog_array_valid` 的快速检查为什么放在 `hlist_empty` 之前？
2. 软件事件 cpu-clock 用 hrtimer 采样。对比硬件 PMI：`perf_swevent_hrtimer` 里 `perf_event_overflow` 的 regs 是谁的？为什么它天然"无抖动地"代表任意执行流，而 PMI 有 skid（`PERF_SAMPLE_IP` 的 `exact` 位）？
3. `fentry` 程序不需要 `bpf_probe_read` 就能取参数，而 kprobe 程序需要。从验证器的视角解释这个差异（提示：BTF 的存在让验证器知道了什么？kprobe 的 ctx 里参数在哪里？）。
4. 设计一个实验：用 `perf stat -e cycles` 圈定区间，分别测量 (a) 开启 `sched_switch` tracepoint、(b) `kprobe:__schedule`、(c) `fentry:__schedule` 三种方式各 100 万命中的每事件开销，并解释三者比例。
5. `BPF_MAP_TYPE_RINGBUF` 单 buffer 带来天然全序，但在 NUMA 多 socket 机器上 `prod_pos` 原子操作的争用会怎样？什么负载下应该退回 `PERF_EVENT_ARRAY`？（提示：记录大小 vs 更新频率。）
6. 生产环境"观测预算"如何量化？给出一个把 CPU 开销控制在 5% 以内的多工具组合（ftrace 事件 + BPF 聚合 + perf 低频采样）并论证各项的预算。

---

> 上一章：[tracer 家族](05-tracer家族.md) ／ 返回：[内核观测体系总览](01-内核观测体系总览.md)
