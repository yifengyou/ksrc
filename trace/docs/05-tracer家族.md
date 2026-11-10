# tracer 家族（Linux 6.6）

> 阅读目标：遍历 `available_tracers` 里的每个成员——从 `struct tracer` 插件框架的调度逻辑开始，逐个拆解 function（dyn ftrace 补丁机制）、function_graph（返回栈与调用时长）、irqsoff/preemptoff/preemptirqsoff（关中断临界区）、wakeup 三兄弟（唤醒延迟）、hwlat（硬件噪声采样）、osnoise/timerlat（6.x 新一代噪声分解）、stack（栈深直方图）；最后用"wakeup + sched_switch + irq 关联"的实战案例收束。

---

## 一、tracer 框架回顾

第 1 章已给出 `struct tracer`（trace.h:556）全貌。框架侧的调度发生在 `current_tracer` 写入时：

```
echo irqsoff > current_tracer
  └─ tracing_set_tracer(tr, type)
       ├─ mutex_lock(&trace_types_lock)
       ├─ 若 tracer->use_max_tr：分配 max_buffer（快照 buffer）
       ├─ old = tr->current_trace
       ├─ old->reset(tr)          —— 卸载旧 tracer 的钩子
       ├─ tr->current_trace = t; t->init(tr)   —— ★新 tracer 挂钩子
       │     （function: ftrace ops; 事件类: tracepoint probe; hwlat: 内核线程）
       ├─ tracing_reset_online_cpus（可选 clear）
       └─ tracing_start/tracing_stop 联动 t->start/stop
```

两个全局约束贯穿所有 tracer：
* **独占性**：同一 trace_array 同时只有一个 current_tracer（buffer 的 `print_line` 只有一套）；但 `events/` 事件开关与 tracer **并行不悖**——function tracer 跑着照样能开 sched_switch 事件；
* **instances 可用性**：`allow_instances` 标志决定能否在子实例使用（wakeup/irqsoff 等依赖全局 ftrace 钩子的早期不行，6.6 大多已允许）。

---

## 二、function tracer：动态补丁的教科书

### 2.1 编译期：mcount/fentry 占位

`CONFIG_FUNCTION_TRACER=y` 时，gcc 的 `-pg`（或 clang 的 `-mfentry`）给**每个函数入口**插入一条 `call __fentry__`。链接期/启动期（`ftrace_process_locs`）把它们全部替换为 `ftrace_make_nop` 的 5 字节 NOP 组（x86：`0x0f 0x1f 0x44 0x00 0x00`）——**不启用时每个函数入口只有 5 字节 nop 的空间税**。

记录谁被 nop 化的是 `struct dyn_ftrace`（ftrace.h:590）数组，按 `struct ftrace_page`（ftrace.c:1115）分页管理：

```c
struct dyn_ftrace {
	unsigned long		ip;      // 调用点地址
	unsigned long		flags;   // 低 19 位引用计数 + FTRACE_FL_* 状态位
	struct dyn_arch_ftrace	arch;
};
struct ftrace_page {
	struct ftrace_page	*next;
	struct dyn_ftrace	*records;
	int			index, order;
};
```

`available_filter_functions` 文件即这份记录经 kallsyms 反查符号名的输出。

### 2.2 启用：ops 链与文本补丁

回调的注册单位是 `struct ftrace_ops`（ftrace.h:325）：

```c
struct ftrace_ops {
	ftrace_func_t			func;        // 回调本体
	struct ftrace_ops __rcu		*next;       // ops 链
	unsigned long			flags;       // FTRACE_OPS_FL_*
	void				*private;    // tracer 侧数据（多指向 trace_array）
	ftrace_func_t			saved_func;
	struct ftrace_ops_hash		local_hash;  // filter/notrace 哈希
	struct ftrace_ops_hash		*func_hash;
	unsigned long			trampoline;  // 动态生成的跳板
	...
#ifdef CONFIG_DYNAMIC_FTRACE_WITH_DIRECT_CALLS
	unsigned long			direct_call; // BPF trampoline 直连
#endif
};
```

关键 `FTRACE_OPS_FL_*` 位（ftrace.h:233 起）：`ENABLED/DYNAMIC/SAVE_REGS/RECURSION/PID/RCU/TRACE_ARRAY/PERMANENT/DIRECT/IPMODIFY...`。启用流程（`ftrace_update_pid_func` / `ftrace_startup` 一族）：

```
register_ftrace_function(ops)（ops 入链，全 CPU stop_machine 无需——
  └─ ftrace_hash_rec_update：按 ops 的 filter_hash/notrace_hash 重算每个
     dyn_ftrace 记录的引用计数（哪些调用点需要启用）
  └─ ftrace_update_code → ftrace_make_call(rec, ftrace_caller)：
       text_poke_bp 把 5 字节 nop 换成 call ftrace_caller（x86）
  └─ ftrace_update_ftrace_func(func)：把 ftrace_caller 内部的
       call ftrace_call（ftrace.c:186 附近）也用 text_poke_bp 改成
       调用"当前生效的分发函数"
```

**多 ops 分发**：`ftrace_caller`（汇编）push 伪 regs 后调用 `ftrace_call`——其目标是 `ftrace_ops_list_func`（遍历 ops 链逐个调用，检查各自 hash）或"单 ops 优化"直接跳该 ops 的回调（`ftrace_ops_get_func`）。`FTRACE_OPS_FL_DIRECT` 时（BPF trampoline/livepatch）甚至旁路 ops 链，`direct_call` 地址直接写进调用点——零分发开销。

### 2.3 回调与记录

function tracer 的 ops 回调（trace_functions.c:171）：

```c
// kernel/trace/trace_functions.c (v6.6, 节选)
static void
function_trace_call(unsigned long ip, unsigned long parent_ip,
		    struct ftrace_ops *op, struct ftrace_regs *fregs)
{
	struct trace_array *tr = op->private;
	int bit;

	if (unlikely(!tr->function_enabled))
		return;

	bit = ftrace_test_recursion_trylock(ip, parent_ip);   // 递归防护（位图）
	if (bit < 0)
		return;

	trace_ctx = tracing_gen_ctx();          // preempt_count+irq 状态打包
	cpu = smp_processor_id();
	data = per_cpu_ptr(tr->array_buffer.data, cpu);
	if (!atomic_read(&data->disabled))
		trace_function(tr, ip, parent_ip, trace_ctx);  // → ring buffer

	ftrace_test_recursion_unlock(bit);
}
```

6.6 的选项矩阵（`select_trace_function`）：`stacktrace`（每事件抓栈）、`no-repeats`（连续同名调用折叠为 `TRACE_FUNC_REPEATS` 记录——`trace_func_repeats` 结构存次数与最后时间戳，大幅省 buffer）、二者组合——四个预编译回调变体按 `trace_options` 里 `func_stack_trace`/`func_no_repeats` 选择。

### 2.4 set_ftrace_filter：哈希过滤

`echo 'schedule*' > set_ftrace_filter` 的解析链：`ftrace_regex_write → ftrace_process_regex` 把 glob 展开成具体符号 → `ftrace_set_filter_ip` 逐 IP 塞进 `ops->func_hash->filter_hash`（`struct ftrace_hash`：`buckets + size_bits` 的开放链哈希，ftrace.c:1128 一带）。写完后 `ftrace_hash_move` + `__ftrace_hash_rec_update` 重算 dyn_ftrace 引用计数并按需补丁/回退文本。**两个哈希的语义**：`filter_hash` 非空 = 只跟踪命中者（白名单）；`notrace_hash` = 黑名单；全空 = 跟踪一切。`function_enabled` 置位前 ops 不会真正接管调用点。

---

## 三、function_graph：带返回时间的函数树

function_graph tracer 需要"函数返回时再记录一次"，这靠的是**修改被跟踪函数的返回地址**——比 kretprobe 更早使用该技巧的正是 fgraph：

```c
// kernel/trace/fgraph.c (v6.6, 节选)
int function_graph_enter(unsigned long ret, unsigned long func,
			 unsigned long frame_pointer, unsigned long *retp)
{
	struct ftrace_graph_ent trace;
	trace.func = func;
	trace.depth = ++current->curr_ret_depth;      // 调用深度

	if (ftrace_push_return_trace(ret, func, frame_pointer, retp))
		goto out;
	if (!ftrace_graph_entry(&trace))               // 过滤 + 写 entry 事件
		goto out_ret;
	return 0;
	...
}

static int ftrace_push_return_trace(unsigned long ret, unsigned long func, ...)
{
	if (current->curr_ret_stack == FTRACE_RETFUNC_DEPTH - 1) {   // 池满（50 层）
		atomic_inc(&current->trace_overrun);
		return -EBUSY;
	}
	calltime = trace_clock_local();
	index = ++current->curr_ret_stack;
	current->ret_stack[index].ret = ret;          // 真实返回地址
	current->ret_stack[index].func = func;
	current->ret_stack[index].calltime = calltime;  // ★入口时刻
	...
}
```

启用方式：`echo function_graph > current_tracer` 时，`ftrace_graph_entry/return` 被 ftrace 侧的特殊机制接管——被过滤命中的函数入口处，ftrace_caller 检测到 `FTRACE_TYPE_RETURN` 模式，调用 `ftrace_graph_func` 把栈上的返回地址替换为 `return_to_handler`。返回时刻 `return_to_handler` → `ftrace_return_to_handler` pop 栈：算 `duration = now - calltime`，写 `TRACE_GRAPH_RET` 事件（若开了 `CONFIG_FUNCTION_GRAPH_RETVAL` 还记录返回值）。

输出形态（`print_graph_*` 家族，trace_functions_graph.c）：

```
 # CPU  DURATION                  FUNCTION CALLS
 #  |     |   |                     |   |   |   |
  2)   0.120 us    |  mutex_unlock();
  2) ! 100.300 us  |  schedule();           ← 惊叹号 = 超过 10us（overhead 标记）
  2)   2.100 us    |  _raw_spin_lock_irqsave() {
  2)   0.350 us    |    do_raw_spin_lock();
  2)   3.010 us    |  }                        ← 大括号层级 = curr_ret_depth
  2)   #    =====  |  }                        ← #/===== 填充 = 跨越了 IRQ 边界
                                                  （FLAGS_FILL_START/END）
```

* `set_graph_function`/`set_graph_notrace`：只跟踪指定函数子树（notrace：命中即"以下不再记录直到返回"——用负深度标记实现，trace_functions_graph.c:142 一带）；
* `sleep-time` 选项：duration 是否把"被调度出去"的时间算进去（默认算；关掉后可测纯执行时间）；
* `graph-time`（CONFIG_FUNCTION_PROFILER）：嵌套函数的时间是否从父函数扣除（"自身时间"vs"包含子调用时间"）；
* 深度上限 `fgraph_max_depth`（`echo 5 > max_graph_depth`）防爆栈。

> 📌 **FTRACE_RETFUNC_DEPTH = 50**（include/linux/ftrace.h:1089）：`ret_stack` 是 task_struct 里的固定数组，深递归（>50 层）的函数子树会被截断并 `trace_overrun++`——这是 function_graph 在深栈场景失真的第一原因。irqsoff 的 `display_graph` 模式（§4）复用同一输出引擎。

---

## 四、irqsoff / preemptoff / preemptirqsoff：临界区计时

三个 tracer 共用一个文件（trace_irqsoff.c），差别只在"计什么"：

```c
// kernel/trace/trace_irqsoff.c (v6.6)
static inline int preempt_trace(int pc)   // preemptoff：preempt_count 非 0 即计时
{
	return ((trace_type & TRACER_PREEMPT_OFF) && pc);
}
static inline int irq_trace(void)          // irqsoff：irqs_disabled() 即计时
{
	return ((trace_type & TRACER_IRQS_OFF) && irqs_disabled());
}
```

**计时的起止钩子**是全局函数 `start_critical_timings()` / `stop_critical_timings()`（line 438/446，`EXPORT_SYMBOL_GPL`）——它们被插桩在 `local_irq_disable()/enable` 的 trace 路径（`trace_hardirqs_off/on`）与 preempt 计数路径上。状态机：

```
local_irq_disable()（或 preempt_disable）
  └─ start_critical_timing(ip, parent_ip) (line 372)
       ├─ per_cpu(tracing_cpu, cpu) 已置？→ 嵌套关中断，忽略（记最深一层）
       ├─ 置 tracing_cpu=1（per-CPU int，零原子开销）
       └─ data->preempt_timestamp = ftrace_now(cpu)   ★T0

[ 关中断区间内执行的每个函数 ]（经 function tracer 钩子）
irqsoff_tracer_call (line 139)
  └─ func_prolog_dec（tracing_cpu 未置/嵌套/disabled → 直接返回）
       └─ trace_function(tr, ip, parent_ip, ...)   → 逐函数记录到主 buffer

local_irq_enable()
  └─ stop_critical_timing → check_critical_timing (line 321)
       ├─ delta = T1 - T0
       ├─ report_latency()？tracing_thresh 阈值模式 or 新最大值模式
       ├─ raw_spin_lock(&max_trace_lock)（arch_spinlock，避开 lockdep/中断敏感）
       ├─ __trace_function(tr, CALLER_ADDR0, parent_ip)  + __trace_stack(5 层)
       ├─ data->critical_sequence != max_sequence？→ 有别的 CPU 刚写过 max，放弃
       └─ tr->max_latency = delta
          update_max_tr_single(tr, current, cpu)  ★把主 buffer 快照进 max_buffer
```

要点：
* **max_sequence 序列号**：多 CPU 同时刷新最大值时，慢的一方发现序列已变就放弃——保证 buffer 里的内容与 max_latency 一致（避免"数字是 A 的、栈是 B 的"的缝合怪）；
* **`update_max_tr_single` 的 swap**：把整个 `array_buffer` 与 `max_buffer` 逐 CPU 交换指针（第 1 章）。读 `trace` 文件时（`print_max` 置位）看到的是最坏那次临界区的完整函数轨迹；
* **`tracing_thresh`**：写 `tracing_thresh`（μs）后 tracer 变为"阈值模式"——只记录超阈值的区间，`tr->max_latency` 不再是唯一过滤器（`report_latency` 的双分支，line 309）；
* **preemptirqsoff**：两者任一成立即计时（`trace_type` 两位同置），观察"谁在关中断区间里又关了抢占"；
* **`display_graph` 选项**：把函数轨迹切到 function_graph 的缩进显示（`is_graph`，`irqsoff_graph_entry/return`）。

> 💡 irqsoff 的读法：`latency-format` 输出的 `# =>` 行标出"延迟结束点"，向上翻栈即凶手。classic 组合：`echo irqsoff > current_tracer; echo 1 > options/latency-format; cat trace`。

---

## 五、wakeup / wakeup_rt / wakeup_dl：唤醒链计时

wakeup 家族测的是"最高优先级任务从被唤醒到真正上 CPU 的延迟"，与 irqsoff 完全同构但计时窗口不同（trace_sched_wakeup.c）：

```
被测窗口：wakeup 时刻 ──► 该任务调度上 CPU 时刻
```

工作机制（v6.6 源码逐点）：

```c
// probe_wakeup：挂在 sched:sched_wakeup tracepoint 上（start_wakeup_tracer 注册）
static void probe_wakeup(void *ignore, struct task_struct *p)
{
	...
	/* 只有"比我当前追踪的更高优先级"才接管：prio 比较链 */
	if (tracing_dl || (wakeup_dl && !dl_task(p)) ||
	    (wakeup_rt && !dl_task(p) && !rt_task(p)) ||
	    (!dl_task(p) && (p->prio >= wakeup_prio || p->prio >= current->prio)))
		return;
	arch_spin_lock(&wakeup_lock);
	__wakeup_reset(wakeup_trace);          // 换目标：清旧任务
	wakeup_cpu = task_cpu(p);
	wakeup_prio = p->prio;
	if (dl_task(p))  tracing_dl = true;    // deadline 任务：接管直到它上线
	wakeup_task = get_task_struct(p);      // 引用计数防任务消失
	data->preempt_timestamp = ftrace_now(cpu);   ★T0（记在目标任务宿主 CPU）
	tracing_sched_wakeup_trace(...);       // 记一条 TRACE_WAKE 事件
	...
}

// probe_wakeup_sched_switch：挂在 sched:sched_switch 上
static void probe_wakeup_sched_switch(void *ignore, bool preempt,
				      struct task_struct *prev,
				      struct task_struct *next, ...)
{
	if (next != wakeup_task)
		return;                            // 只关心目标任务上线的那一刻
	...
	T0 = data->preempt_timestamp; T1 = ftrace_now(cpu);
	delta = T1 - T0;
	if (report_latency(wakeup_trace, delta)) {
		wakeup_trace->max_latency = delta;
		update_max_tr(wakeup_trace, wakeup_task, wakeup_cpu, NULL);   ★swap
	}
	__wakeup_reset(wakeup_trace);
	...
}
```

三个变体的过滤差异（probe_wakeup 的 prio 检查）：

| tracer | 追踪对象 | 判定 |
| :--- | :--- | :--- |
| wakeup | 全部任务（CFS 也算） | `p->prio` 更高即接管 |
| wakeup_rt | RT + DL | `rt_task(p) \|\| dl_task(p)` |
| wakeup_dl | 仅 Deadline | `dl_task(p)`，且 `tracing_dl` 锁定直到它上线 |

期间的函数轨迹由 `wakeup_tracer_call`（function tracer ops，只认 `wakeup_task` 宿主 CPU 的 `tracing_cpu` 语义）持续写入。**唤醒期间目标任务不在 CPU 上**，所以记录的是"宿主 CPU 上挡路的东西"——这正是 wakeup 延迟的语义。

---

## 六、hwlat：硬件噪声采样器

hwlat 检测的是 **CPU 之外的延迟**（SMI/固件/虚拟化窃取）：一个内核线程在关中断+不抢占比对 TSC，若两次读数之差超阈值，说明 CPU 被"偷走"了（kernel/trace/trace_hwlat.c）。

```c
// kernel/trace/trace_hwlat.c (v6.6)
#define DEFAULT_SAMPLE_WINDOW	1000000			/* 1s 窗口 */
#define DEFAULT_SAMPLE_WIDTH	500000			/* 0.5s 采样 */
#define DEFAULT_LAT_THRESHOLD	10			/* 10us 阈值 */

struct hwlat_sample {
	u64			seqnum;
	u64			duration;        /* 内层差（两次紧邻读 TSC）*/
	u64			outer_duration;  /* 外层差（一轮循环间隔）*/
	u64			nmi_total_ts;    /* 采样期间 NMI 总时长 */
	struct timespec64	timestamp;
	int			nmi_count;       /* NMI 次数 */
	int			count;           /* 超阈值循环次数 */
};

static struct hwlat_data {
	u64	sample_window;      /* 周期（含采样+休眠）*/
	u64	sample_width;       /* 窗口内的活跃采样时长 */
	int	thread_mode;        /* NONE(单线程迁移)/ROUND_ROBIN(默认)/PER_CPU */
} hwlat_data = {
	.sample_window = DEFAULT_SAMPLE_WINDOW,
	.sample_width  = DEFAULT_SAMPLE_WIDTH,
	.thread_mode   = MODE_ROUND_ROBIN
};
```

`get_sample()`（line 201）的核心循环：

```
local_irq_disable()（在 kthread_fn 里）
trace_hwlat_callback_enabled = true       // 让 NMI 回调开始记账
start = t1 = t2 = time_get()              // trace_clock_local（TSC）
do {
	t1 = time_get();
	t2 = time_get();
	outer_diff = t1 - last_t2;         // 外层差：上一轮 t2 → 本轮 t1
	diff      = t2 - t1;               // 内层差：两条相邻读数
	if (diff > sample) sample = diff;         // 只记最大
	if (outer_diff > outer_sample) outer_sample = outer_diff;
	last_t2 = t2;
} while (total <= hwlat_data.sample_width);  // 采样满 sample_width μs

if (sample > thresh || outer_sample > thresh) {   // 超阈值 → 报事件
	... trace_hwlat_sample(&s)（写 TRACE_HWLAT 事件到 buffer）
	if (latency > tr->max_latency) { tr->max_latency = latency; ... }
}
local_irq_enable()
udelay(window - width)                    // 睡完窗口剩余部分（hwlatd 线程）
```

**双层差的意义**：内层 diff 抓"精确发生在两条指令之间的停顿"；外层 outer_diff 抓"被循环体边界吞掉的停顿"。两者取 max 作为本次延迟。

**NMI 记账**（`trace_hwlat_callback`，line 163）：NMI 里无法写 buffer（会递归），所以 NMI enter/exit 钩子只累加 `nmi_count/nmi_total_ts`，采样结束后一并补进样本——把"NMI 造成的延迟"与"硬件造成的延迟"区分开（NMI 期间的停顿不算 hwlat 的锅）。

**三种线程模式**（`thread_mode`，6.6 新增）：`MODE_NONE`（单线程全核轮转迁移 `move_to_next_cpu`）、`MODE_ROUND_ROBIN`（默认，同单线程但固定顺序）、`MODE_PER_CPU`（`kthread_run_on_cpu(cpu, "hwlatd/%u")` 每 CPU 一个线程，同时测所有核——虚拟化 host 抢核场景必备）。参数经 `hwlat_detector/` 目录：`window`、`width`、`cpus`、`thread_mode`；阈值复用全局 `tracing_thresh`。

---

## 七、osnoise 与 timerlat：噪声分解（6.x 新一代）

hwlat 只回答"有没有停顿"；osnoise（kernel/trace/trace_osnoise.c，6.6 已成熟）回答"**停顿是谁干的**"。

### 7.1 原理：不关中断的循环计时 + 噪声源挂钩

```c
struct osnoise_sample {
	u64	runtime;       /* 本轮采样总时长 */
	u64	noise;         /* 累计噪声 */
	u64	max_sample;    /* 最大单次噪声 */
	int	hw_count;      /* ★无中断解释的停顿次数（≈hwlat 的发现）*/
	int	nmi_count, irq_count, softirq_count, thread_count;  /* 各源计数 */
};
```

`run_osnoise()`（line 1414）的主循环**保持抢占开启**（与 hwlat 的本质区别）：

```
每轮（sample_period 周期，默认 1s；期间活跃 runtime_us，默认 1s=全周期）:
  save_osn_sample_stats()                    // 记录各计数器初值
  （可选 OSNOISE_IRQ_DISABLE/PREEMPT_DISABLE 选项模拟 hwlat 行为）
  do {
     int_count = set_int_safe_time(osn_var, &sample);   // NMI-safe 取时间
     noise = sample - last_sample;                       // 本轮间隔
     if (noise >= threshold) {             // 阈值（tracing_thresh 或默认 5μs）
         interference = int_count - last_int_count;      // 期间 NMI 数
         if (!interference) hw_count++;    // ★没有任何 NMI 却停了 → 硬件/虚拟化
         sum_noise += noise; max_noise = max(...);
         trace_sample_threshold(...);      // 逐次超阈事件（osnoise/ 树下）
     }
     cond_resched();                        // ★主动让出：噪声源因此可被观测
     rcu_momentary_dyntick_idle();          // PREEMPT_RCU 下主动报告 QS
  } while (total < runtime);
  diff_osn_sample_stats(&s);                // 计算各计数增量
  trace_osnoise_sample(&s);                 // 汇总事件（写 TRACE_OSNOISE）
```

**分解四源**的挂钩（`hook_*_events`，line 994-1313）：

| 噪声源 | 挂钩点 | 计数 |
| :--- | :--- | :--- |
| NMI | `trace_osnoise_callback`（NMI enter/exit 钩子，与 hwlat 共用机制） | `nmi_count` |
| IRQ | `register_trace_irq_handler_entry/exit`（irq tracepoint） | `irq_count`（+每次时长） |
| SoftIRQ | `register_trace_softirq_entry/exit` | `softirq_count` |
| 线程（调度） | `register_trace_sched_switch`（含 `sched_migrate_task`） | `thread_count` |

被这些钩子观测到的时间都**算进对应源的 noise**（`trace_irq_noise` 等函数把 `duration` 累进 per-CPU 的 `osn_var`），剩下的无主停顿即 `hw_count`。事件输出：`osnoise_sample`（汇总）+ `osnoise_noise`（逐超阈）。

### 7.2 timerlat：定时器延迟的精确定义

timerlat 是 osnoise 家族的第二个 tracer（`CONFIG_TIMERLAT_TRACER`）：用 hrtimer 主动制造可预期的到期，测**到期→真正执行**的延迟并分解在哪一层：

```
timerlat_main（每 CPU 一个内核线程，FIFO 95 = DEFAULT_TIMERLAT_PRIO）
  hrtimer_init(&tlat->timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS_PINNED_HARD);
  tlat->timer.function = timerlat_irq;      // 硬中断上下文到期回调
  循环：
    wait_next_period()                       // hrtimer 到期（默认 1ms 周期）
      └─ timerlat_irq：
           diff = now - abs_period           // ①中断上下文延迟
           trace_timerlat_sample({context: IRQ_CONTEXT})
           wake_up(tlat->kthread)            // 唤醒 FIFO95 线程
    （线程被调度上 CPU 后：）
    now = hrtimer_cb_get_time();
    diff = now - abs_period                  // ②线程上下文延迟（cyclictest 语义）
    s.context = THREAD_CONTEXT
    trace_timerlat_sample(&s)
    notify_new_max_latency(diff)             // 刷新 max_latency + fsnotify
    timerlat_dump_stack(diff > print_stack 时抓栈)
```

输出里同时出现 IRQ 与 THREAD 两个 context 的样本：**THREAD 减 IRQ ≈ 调度器延迟**（唤醒到上线），IRQ 延迟本身 ≈ 中断关闭/禁抢占比 hrtimer 到期晚了多少。参数目录 `osnoise/`：`timerlat_period_us`（默认 1000μs）、`stop_tracing_us/stop_tracing_total_us`（超阈值自动停 trace 并抓现场——**OS 噪声版的"自动 trigger"**）、`print_stack`（超阈值时打印中断栈）。用户态工具 `rtla timerlat`（tools/tracing/rtla/）封装了完整的读数与报告；6.6 还支持 `timerlat_fd`——用户线程自己当 workload（`OSN_WORKLOAD` 位翻转，stop_kthread 的 kill 路径）。

> 💡 cyclictest（传统 RT 测试）= 用户态定时器 + 测唤醒延迟；timerlat = 内核态等价物且多分解一层（IRQ 延迟）。RTLA 文档明确以 timerlat 为下一代基准。

---

## 八、stack tracer：最坏栈深直方图

stack tracer（trace_stack.c）不属于 current_tracer——它常驻为**一个 ftrace_ops**，`/proc/sys/kernel/stack_tracer_enabled`（或启动参数 stacktrace）置 1 后在每个被跟踪函数入口测量当前内核栈深度，只保留最深现场的完整回溯：

```c
// kernel/trace/trace_stack.c (v6.6, 节选)
static void check_stack(unsigned long ip, unsigned long *stack)
{
	this_size = THREAD_SIZE - (((unsigned long)stack) & (THREAD_SIZE-1));
	this_size -= frame_size;                        // 扣除 tracer 自身帧
	if (this_size <= stack_trace_max_size)          // 不破纪录 → 返回（快路径）
		return;
	if (!object_is_on_stack(stack))                 // 中断栈不测
		return;
	local_irq_save(flags);
	arch_spin_lock(&stack_trace_max_lock);
	if (this_size <= stack_trace_max_size)          // 双检（他 CPU 先破纪录）
		goto out;
	stack_trace_max_size = this_size;
	stack_trace_nr_entries = stack_trace_save(stack_dump_trace, ...);
	/* 在栈上逐 long 搜索回溯地址，换算每层帧大小 → stack_trace_index[] */
	...
	arch_spin_unlock(&stack_trace_max_lock);
}
```

`stack_trace_index[]` 的算法很"考古"：把 `stack_trace_save` 得到的回溯地址序列在**实际栈内存里逐字扫描**定位，从而得到**每个函数各占多少栈**（而不只是总深）——输出里每行 `size` 列即该函数的栈帧贡献。用于追查"谁把 16K 内核栈吃到爆"。配套文件：`stack_max_size`（读写，写 0 重置）、`stack_trace`（当前最深回溯）、`stack_trace_filter`（只测指定函数）。

---

## 九、实战：wakeup + sched_switch + irq 关联定位调度延迟

场景：某业务线程偶发 10ms+ 的唤醒延迟，需要定位是中断、关抢占还是调度器本身。三步法：

```bash
cd /sys/kernel/tracing

# ── 第 1 步：用 wakeup tracer 拿到最坏现场 ──
echo 0 > tracing_on
echo wakeup > current_tracer
echo 1000 > tracing_thresh            # 只关心 > 1ms 的
echo 1 > options/latency-format
echo 1 > options/sym-addr
echo 1 > tracing_on
# ...复现负载...
cat trace | tail -80
# 输出特征（latency format）:
#   任务: my_thread-12345 (运行在 cpu 2)
#   延迟: 10234 us, 时间戳/序列号
#   <断点1: 被唤醒处> sched_wakeup → __wake_up_q → ...
#   <断点2: 上CPU处> __schedule → context_switch
#   中间的函数链 = 挡路者（例如一长串 NVMe 中断处理 + softirq）

# ── 第 2 步：换成事件流，量化"挡路窗口"里的具体事件 ──
echo 0 > tracing_on
echo nop > current_tracer
echo 1 > events/sched/sched_wakeup/enable
echo 1 > events/sched/sched_switch/enable
echo 1 > events/irq/irq_handler_entry/enable
echo 1 > events/irq/irq_handler_exit/enable
echo 1 > events/timer/hrtimer_expire_entry/enable
echo 1 > events/timer/hrtimer_expire_exit/enable
echo 1 > tracing_on
# 复现后：
trace-cmd report trace.dat | trace-cmd profile   # 每个函数/中断的持续时间聚合
# 或手工：找 wakeup(next_pid=X) → sched_switch(next_pid==X) 之间的事件

# ── 第 3 步：分解归属（IRQ off / preempt off / 调度慢）──
echo 0 > tracing_on
echo 1 > events/preemptirq/irq_disable/enable    # CONFIG_PREEMPTIRQ_EVENTS
echo 1 > events/preemptirq/irq_enable/enable
echo preemptirqsoff > current_tracer             # 若怀疑关中断区间
echo 1 > tracing_on
# irqsoff 的 max buffer 直接给出最坏关中断区间 + 触发栈

# ── 替代法：rtla 一把梭（6.6 自带）──
rtla timerlat top -a 300 -P f:95                # 300s，FIFO95 模拟 RT 任务
#   输出按 CPU 分解：IRQ delay / Thread delay / 前台噪声源
rtla osnoise top -a 60                           # 60 秒噪声普查（NMI/IRQ/softirq/thread/hw 占比）
```

定位决策树：

```
唤醒延迟 > 1ms
├─ irqsoff 最大值也 > 1ms？ → 关中断凶手（看 irqsoff 栈：常是驱动 spin_lock_irq）
├─ preemptoff 大 > 1ms？     → 关抢占凶手（softirq 长任务、驱动 preempt_disable）
├─ 两者都不大？              → 调度器/cgroup 限流（看 wakeup 的 trace：
│                              目标 CPU 的 runqueue 状态、rt throttling、
│                              sched_rt_runtime_us 耗尽？）
└─ timerlat 的 IRQ 层大？     → 硬件/虚拟化（对照 osnoise hw_count）
```

---

## 十、思考题

1. function tracer 的 `ftrace_ops` 链在多消费者（function tracer + kprobe-on-ftrace + BPF）同时注册时如何分发？`FTRACE_OPS_FL_DIRECT` 为什么可以旁路整条链——它和 ops 链的互斥由谁保证（提示：`direct_mutex` 与 IPMODIFY 共享函数的协商命令 `FTRACE_OPS_CMD_ENABLE_SHARE_IPMODIFY_*`）？
2. function_graph 的 `ret_stack` 挂在 task_struct 且深度上限 50。深递归场景（如文件系统的深层调用链）会发生什么？`trace_overrun` 计数在哪里暴露给用户？
3. irqsoff 的 `max_sequence` 防的是"两个 CPU 同时刷新最大值导致 buffer 与数字不一致"。为什么不用一把普通 spinlock 保护整个 check_critical_timing？（提示：这段代码在关中断上下文里执行，lockdep 呢？缓存行弹跳呢？）
4. wakeup tracer 在目标任务"尚未上线"期间持续记录宿主 CPU 的函数轨迹。如果目标任务迁移了（`probe_wakeup_migrate_task` 挂钩存在），`wakeup_cpu` 的语义如何维持正确？
5. osnoise 的主循环 `cond_resched()` 主动让出 CPU——这看似"放走噪声"，实则是设计必需。为什么？（提示：若不让出，thread_count 将恒为 0，且长时间采样会触发 softlockup。）
6. timerlat 同时输出 IRQ_CONTEXT 与 THREAD_CONTEXT 两组延迟。设计一个由"500μs 关中断 + 正常调度"构成的故障，预测两种 context 的读数各是多少量级，再用 rtla 验证。

---

> 下一章：[perf 与 BPF 观测](06-perf与BPF观测.md) —— perf_event 的 pmu 抽象与采样管线、tracepoint 并入 perf 的通道、BPF 的三挂载点与 map 生态、bpftrace 一行流与生产选型。
