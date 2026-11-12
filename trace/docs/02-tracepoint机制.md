# tracepoint 机制（Linux 6.6）

> 阅读目标：吃透内核静态插桩的全部实现——`TRACE_EVENT` 宏如何一次定义出 tracepoint、结构体、格式化输出三样东西；`struct tracepoint` 的 `funcs` 数组 + `static_key` + `static_call` 三件套如何把"未启用零成本、启用一个间接跳转"做到极致；注册/去注册为什么需要 RCU + SRCU 双重宽限期；事件 filter 的"谓词小程序"如何编译执行；trigger 与 BPF 如何复用同一条管线。

---

## 一、概述：一个 tracepoint 的静态面貌

内核里的一行 `trace_sched_switch(preempt, prev, next)` 展开后是一个**静态 key 保护的调用**。先看定义侧的两个核心结构（include/linux/tracepoint-defs.h）：

```c
// include/linux/tracepoint-defs.h (v6.6)
struct tracepoint_func {
	void *func;              // 回调函数
	void *data;              // 回调私有数据（__data）
	int  prio;               // 注册优先级（决定执行顺序）
};

struct tracepoint {
	const char *name;                     // "sched_switch"
	struct static_key key;                // ★jump label：未启用 = nop
	struct static_call_key *static_call_key;   // ★static_call 热补丁目标
	void *static_call_tramp;              // static_call trampoline 地址
	void *iterator;                       // 多消费者时的迭代器 __traceiter_##name
	void *probestub;                      // 空探针（去注册过渡态占位）
	int  (*regfunc)(void);                // 首个探针注册前回调（如 syscall refcount）
	void (*unregfunc)(void);              // 最后一个探针移除后回调
	struct tracepoint_func __rcu *funcs;  // ★回调数组（NULL 结尾，RCU 保护）
};
```

三个性能开关逐级启用：

```
关闭态（funcs == NULL）:
   trace_##name():
        if (static_key_false(&tp->key))   ← jump label: x86 上是 5 字节 NOP
            __DO_TRACE(...)                      （永远不执行）
   成本 ≈ 1 条顺序执行的 jmp（5 字节 nop 被跳过预测，实测 ~0.3ns）

单消费者态（funcs[1].func == NULL）:
   static_key 打开 → nop 被 int3/文本补丁为 jmp
   __DO_TRACE_CALL 展开为 static_call(tp_func_##name)(...)
   static_call 已被补丁为"直接跳转 funcs[0].func"，零间接寻址

多消费者态:
   static_call 跳到 iterator（__traceiter_##name）
   iterator 用 rcu_dereference_raw 读 funcs 数组逐个调用
```

---

## 二、__DECLARE_TRACE 与 __DO_TRACE：调用点展开

`DECLARE_TRACE` 系列宏在 include/linux/tracepoint.h 中展开为调用点的全部"装备"（v6.6 节选）：

```c
// include/linux/tracepoint.h (v6.6, 节选整理)
#define __DECLARE_TRACE(name, proto, args, cond, data_proto)		\
	extern int __traceiter_##name(data_proto);			\
	DECLARE_STATIC_CALL(tp_func_##name, __traceiter_##name);	\
	extern struct tracepoint __tracepoint_##name;			\
	static inline void trace_##name(proto)				\
	{								\
		if (static_key_false(&__tracepoint_##name.key))		\
			__DO_TRACE(name, TP_ARGS(args),			\
				TP_CONDITION(cond), 0);			\
		if (IS_ENABLED(CONFIG_LOCKDEP) && (cond)) {		\
			WARN_ON_ONCE(!rcu_is_watching());		\
		}							\
	}								\
	/* register_trace_##name / unregister_trace_##name 等 API 同处展开 */

#define __DO_TRACE(name, args, cond, rcuidle)				\
	do {								\
		int __maybe_unused __idx = 0;				\
		if (!(cond))			/* 如 cpu_online 检查 */	\
			return;						\
		if (WARN_ON_ONCE(RCUIDLE_COND(rcuidle)))			\
			return;						\
		preempt_disable_notrace();	/* 回调期间禁止调度 */	\
		if (rcuidle) {						\
			/* idle 路径 RCU 不 watching，改用 SRCU */	\
			__idx = srcu_read_lock_notrace(&tracepoint_srcu);\
			ct_irq_enter_irqson();				\
		}							\
		__DO_TRACE_CALL(name, TP_ARGS(args));			\
		if (rcuidle) {						\
			ct_irq_exit_irqson();				\
			srcu_read_unlock_notrace(&tracepoint_srcu, __idx);\
		}							\
		preempt_enable_notrace();				\
	} while (0)

#ifdef CONFIG_HAVE_STATIC_CALL
#define __DO_TRACE_CALL(name, args)					\
	do {								\
		struct tracepoint_func *it_func_ptr;			\
		void *__data;						\
		it_func_ptr = rcu_dereference_raw(			\
				(&__tracepoint_##name)->funcs);		\
		if (it_func_ptr) {					\
			__data = (it_func_ptr)->data;			\
			static_call(tp_func_##name)(__data, args);	\
		}							\
	} while (0)
#endif
```

四个深挖点：

1. **`cond` 的免费检查**：`DECLARE_TRACE` 把 `cond` 默认填成 `cpu_online(raw_smp_processor_id())`——CPU 热插拔下线瞬间的事件不会打到已死的 per-CPU buffer。`DECLARE_TRACE_CONDITION` 还能追加自定义条件。
2. **`preempt_disable_notrace`**：回调执行期间关调度，这是"读 `funcs` 指针 → 调用"与"去注册释放数组"之间用 sched-RCU 兑换安全性的前提（探针回调不能睡、不能被迁移到别的 CPU 后仍持有旧数组指针）。注意用的是 `_notrace` 版本——避免 preempt 计数路径自身又被 function tracer 跟踪造成递归。
3. **`rcuidle` 变体**：idle 循环里 sched-RCU 不 watching，普通 RCU 引用会触发警告。`trace_##name##_rcuidle()` 变体切到 **SRCU（tracepoint_srcu）**——SRCU 允许在 RCU 静默区使用。NMI 里连 SRCU 都不行，故 `RCUIDLE_COND` 在非 `ARCH_WANTS_NO_INSTR` 架构上退化为 `rcuidle && in_nmi()`。
4. **lockdep 自检**：即使 tracepoint 关闭，lockdep 构建也会 `WARN_ON_ONCE(!rcu_is_watching())`——提前暴露"在不安全上下文埋点"的 bug。

---

## 三、DEFINE_TRACE_FN：定义侧的完整展开

`DEFINE_TRACE(name)`（tracepoint.h:301）把 `struct tracepoint` 实例放进 `__tracepoints` 段，并生成迭代器与空探针：

```c
// include/linux/tracepoint.h (v6.6, 节选)
#define DEFINE_TRACE_FN(_name, _reg, _unreg, proto, args)		\
	static const char __tpstrtab_##_name[]				\
	__section("__tracepoints_strings") = #_name;			\
	int __traceiter_##_name(void *__data, proto);			\
	void __probestub_##_name(void *__data, proto);			\
	struct tracepoint __tracepoint_##_name	__used			\
	__section("__tracepoints") = {					\
		.name = __tpstrtab_##_name,				\
		.key = STATIC_KEY_INIT_FALSE,				\
		.static_call_key = &STATIC_CALL_KEY(tp_func_##_name),	\
		.static_call_tramp = STATIC_CALL_TRAMP_ADDR(tp_func_##_name), \
		.iterator = &__traceiter_##_name,			\
		.probestub = &__probestub_##_name,			\
		.regfunc = _reg,  .unregfunc = _unreg,			\
		.funcs = NULL };					\
	__TRACEPOINT_ENTRY(_name);   /* 指针放入 __tracepoints_ptrs 段 */	\
	int __traceiter_##_name(void *__data, proto)			\
	{								\
		struct tracepoint_func *it_func_ptr;			\
		it_func_ptr = rcu_dereference_raw(			\
				(&__tracepoint_##_name)->funcs);		\
		if (it_func_ptr) {					\
			do {						\
				void *it_func = READ_ONCE((it_func_ptr)->func); \
				__data = (it_func_ptr)->data;		\
				((void(*)(void *, proto))(it_func))(__data, args); \
			} while ((++it_func_ptr)->func);		\
		}							\
		return 0;						\
	}								\
	void __probestub_##_name(void *__data, proto) { }		\
	DEFINE_STATIC_CALL(tp_func_##_name, __traceiter_##_name);
```

链接器段的三张表由 `for_each_kernel_tracepoint()`（tracepoint.c:745）遍历 `__start___tracepoints_ptrs..__stop___tracepoints_ptrs` 收集——ftrace 的事件子系统初始化正是靠它给每个 tracepoint 建 `trace_event_file`。模块加载走 `MODULE_STATE_COMING` 通知链（tracepoint.c:640），tainted 模块（非 GPL/forced 等）被拒之门外以防结构不匹配导致崩溃。

### 3.1 static_key（jump label）与 text patching 的联动

`static_key_false` 未启用时在 x86 上编译为一条 5 字节 `jmp .+5`（跳过整个 `__DO_TRACE`）。`static_key_enable()`（tracepoint.c:361 在注册路径中调用）通过 stop_machine/`text_poke_bp` 把该跳转原地改成 2 字节 nop（配合 int3 断点逐字节打补丁 + 同步执行流）。关键性质：

* **单一写点**：`static_key_enable` 只把 key 计数从 0 变 1 那次才 patch 文本；
* **与 static_call 的顺序**：`tracepoint_add_func` 先 `tracepoint_update_call()`（补 static_call 目标），再 `rcu_assign_pointer(tp->funcs, ...)`，最后 `static_key_enable()`——保证 key 打开时，static_call 的目标已就绪；
* **x86 的 patch 原语是 `text_poke_bp()`**（arch/x86/include/asm/text-patching.h:51）：int3 占位 → 写入新指令 → int3 陷阱处理器在"旧指令模拟执行"（emulate 参数）与"跳过"之间切换 → 移除 int3。所有观测基础设施（static_key/static_call/ftrace/optprobe）共用这一套安全文本补丁。

### 3.2 static_call 的角色

`DECLARE_STATIC_CALL(tp_func_##name, ...)` 声明一个可运行时改目标的直接调用点。`tracepoint_update_call()`（tracepoint.c:308）决定跳哪：

```c
// kernel/tracepoint.c (v6.6)
static void tracepoint_update_call(struct tracepoint *tp,
				   struct tracepoint_func *tp_funcs)
{
	void *func = tp->iterator;          // 默认：多消费者迭代器
	if (!tp->static_call_key)
		return;                     // 合成事件无 static_call 站点
	if (nr_func_state(tp_funcs) == TP_FUNC_1)
		func = tp_funcs[0].func;    // 单消费者：直跳回调，去掉一层循环
	__static_call_update(tp->static_call_key, tp->static_call_tramp, func);
}
```

> 💡 这是 5.10 引入的优化（commit d25e37d89dd2 "tracepoint: Optimize using static_call()"，2020-08；本仓 git log -S + tag --contains 实证；此前是清一色 indirect call 走数组）。单消费者（绝大多数生产场景：一个 ftrace 实例或一个 BPF 程序）从"读指针数组+循环"降为"一条 call 指令"。

---

## 四、TRACE_EVENT 宏：七阶段工厂

`TRACE_EVENT(name, TP_PROTO, TP_ARGS, TP_STRUCT__entry, TP_fast_assign, TP_printk)` 一个宏定义一个事件。v6.6 把展开重构成 `include/trace/trace_events.h` + `stages/*.h` 的**七遍包含**模式——每个 stage 重定义字段宏（`__field`/`__array`/`__dynamic_array`/`__string`...），再 `#include TRACE_INCLUDE(TRACE_INCLUDE_FILE)` 把同一个头文件再编一遍：

| Stage | 生成物 | 用途 |
| :--- | :--- | :--- |
| 1 struct_define | `struct trace_event_raw_##name { struct trace_entry ent; <字段>; char __data[]; }` | ring buffer 里的真实记录布局 |
| 2 data_offsets | `struct trace_event_data_offsets_##name { u32 <动态字段>; }` | 动态字段的偏移（高 16 位存长度） |
| 3 trace_output | `trace_raw_output_##name()` + `trace_event_type_funcs_##name` | TP_printk 文本格式化 |
| 4 event_fields | `trace_event_fields_##name[]`（名字/类型/偏移/长度反射表） | format 文件、filter 编译的元数据 |
| 5 get_offsets | `trace_event_get_offsets_##call()` | 运行时算 __dynamic_array/__string 实际尺寸 |
| 6 event_callback | `trace_event_raw_event_##call(void *__data, proto)` ★ | 注册到 tracepoint 的 probe（见下） |
| 7 class_define | `event_class_##call` + `event_##call`（trace_event_call）→ `_ftrace_events` 段 | 事件注册与 perf/BPF 挂载点 |

Stage 6 生成的 probe 是事件写入 ring buffer 的"总入口"（v6.6 精简）：

```c
// include/trace/stages/stage6_event_callback.h 展开效果
static notrace void
trace_event_raw_event_sched_switch(void *__data,
				   struct rq *rq, struct task_struct *prev,
				   struct task_struct *next)
{
	struct trace_event_file *trace_file = __data;      // __data 是实例侧文件!
	struct trace_event_data_offsets_sched_switch __maybe_unused __data_offsets;
	struct trace_event_buffer fbuffer;
	struct trace_event_raw_sched_switch *entry;
	int __data_size;

	if (trace_trigger_soft_disabled(trace_file))       // soft-disable/pid 过滤短路
		return;

	__data_size = trace_event_get_offsets_sched_switch(&__data_offsets, rq, prev, next);

	entry = trace_event_buffer_reserve(&fbuffer, trace_file,
					   sizeof(*entry) + __data_size);
	if (!entry)
		return;

	{ /* TP_fast_assign 展开体，填充 entry-> 各字段 */ }

	trace_event_buffer_commit(&fbuffer);   // filter/trigger/commit 都在这里
}
```

> 📌 注意 `__data` 的角色：tracepoint 的 `funcs[0].data` 是 **`trace_event_file`**（实例级开关），所以 probe 一进来就能读该实例的 enable/filter/trigger 状态——"事件 per-instance 控制"就是这么实现的。同一 tracepoint 挂两个实例时 funcs 数组有两条（各带自己的 file），走 iterator 路径。

`TRACE_EVENT_FLAGS(event, flag)` 是补充宏：通过 `__TRACE_EVENT_FLAGS` 生成 early_initcall，把 `TRACE_EVENT_FL_CAP_ANY`（允许非 root perf）之类的标志 OR 进 `event_##name.flags`。`TRACE_EVENT_FN` 变体允许传入 reg/unreg 函数（在 tracepoint 首个消费者注册/注销时调用，例如 syscall tracepoint 借此打开全任务的 `SYSCALL_TRACEPOINT` 工作位，tracepoint.c:758 的 `syscall_regfunc` 遍历 `for_each_process_thread`）。

---

## 五、tracepoint_probe_register：回调安装全流程

注册一个探针（内核 API `register_trace_##name(probe, data)`，用户态则通过 `echo 1 > events/.../enable` 间接触发 `trace_event_reg` → `tracepoint_probe_register`）的完整协议（kernel/tracepoint.c）：

```c
// kernel/tracepoint.c (v6.6, 节选)
static int tracepoint_add_func(struct tracepoint *tp,
			       struct tracepoint_func *func, int prio, bool warn)
{
	if (tp->regfunc && !static_key_enabled(&tp->key))
		tp->regfunc();                       // ① 首注册通知（如 syscall 位图）

	tp_funcs = rcu_dereference_protected(tp->funcs, ...);
	old = func_add(&tp_funcs, func, prio);   // ② 新数组：按 prio 插入排序

	switch (nr_func_state(tp_funcs)) {
	case TP_FUNC_1:      /* 0 → 1 */
		tp_rcu_cond_sync(TP_TRANSITION_SYNC_1_0_1);  // ③ 见下：防 1→0→1 竞态
		tracepoint_update_call(tp, tp_funcs);        // ④ static_call → 单回调
		rcu_assign_pointer(tp->funcs, tp_funcs);     // ⑤ 发布数组
		static_key_enable(&tp->key);                 // ⑥ jump label 打开
		break;
	case TP_FUNC_2:      /* 1 → 2 */
		tracepoint_update_call(tp, tp_funcs);        // static_call → iterator
		fallthrough;
	case TP_FUNC_N:      /* N → N+1 */
		rcu_assign_pointer(tp->funcs, tp_funcs);
		if (tp_funcs[0].data != old[0].data)
			tp_rcu_get_state(TP_TRANSITION_SYNC_N_2_1);
		break;
	}
	release_probes(old);                   // ⑦ 旧数组 RCU+SRCU 延迟释放
	return 0;
}
```

`func_add` 的要点：**永远分配全新数组**（`kmalloc` + 拷贝 + 插入 + NULL 结尾），从不原地修改——读者（`__traceiter` 或 static_call 直达的回调）在 RCU 读临界区内看到的数组始终自洽；`prio` 小的先执行（ftrace 事件回调用 `TRACEPOINT_DEFAULT_PRIO=10`）。`func_remove` 失败路径很有意思：如果分配新数组失败，就把待删回调原地改成 `tp_stub_func`（空函数）——宁可泄漏一次调用，绝不 use-after-free。

### 5.1 为什么去注册要"双宽限期"

探针可能被三类上下文执行，需要**两种 RCU**都过完宽限期才能释放数组：

```c
// kernel/tracepoint.c (v6.6)
static void release_probes(struct tracepoint_func *old)
{
	...
	/* 链式等待：先 sched-RCU 宽限期，再到期后启动 SRCU 宽限期 */
	call_rcu(&tp_probes->rcu, rcu_free_old_probes);
}
static void rcu_free_old_probes(struct rcu_head *head)
{
	call_srcu(&tracepoint_srcu, head, srcu_free_old_probes);
}
static inline void release_probes(...) { ... }

// 模块退出等"必须等回调跑完"的场景：
static inline void tracepoint_synchronize_unregister(void)
{
	synchronize_srcu(&tracepoint_srcu);
	synchronize_rcu();
}
```

* **sched-RCU** 覆盖普通/preempt-disable 上下文的读者（`preempt_disable_notrace` 区间相当于 RCU 读临界区）；
* **SRCU** 覆盖 `*_rcuidle` 变体（idle 里 SRCU 读锁）；
* `call_rcu → call_srcu` 的**链式回调**一次覆盖两者：sched-RCU 宽限期结束时，所有普通读者已退出；随后 SRCU 宽限期收走 idle 读者；最后 `kfree`。

### 5.2 过渡态同步：TP_TRANSITION_SYNC 的两个陷阱

static_call 优化引入了两个微妙竞态，用"过渡快照"修补：

* **`1→0→1` 竞态**：static_call 目标里**嵌着 data 指针的加载时机**。时序：CPU0 正执行旧回调（拿了 old data）→ 去注册（funcs=NULL + static_call 改向 stub/probestub）→ 重新注册新回调（static_call 改向新 func）。若 CPU0 的执行流停在"已加载 static_call 目标、尚未调函数"的窗口跨过两次修改，它可能带着**旧 data 调用新 func**。对策：`TP_TRANSITION_SYNC_1_0_1` 在 0→1 时若存在未完成的 1→0 过渡（`tp_rcu_get_state` 记录的快照），先 `cond_synchronize_rcu + poll_state_synchronize_srcu` 等它落地（tracepoint.c:47-67）。
* **`N→2→1` 竞态**：iterator→单回调切换同理，且只有当 `tp_funcs[0].data` 变化时才需要等（data 没变则无 UAF 风险）。

这两个状态机是 tracepoint 子系统近十年最难的内存序代码，论文级细节在 `kernel/tracepoint.c` 的注释与 LKML 讨论 "static_call + tracepoint" 系列。

---

## 六、事件 filter：编译成"跳转小程序"

`echo 'prev_pid == 1234 && next_comm ~ "bash*"' > events/sched/sched_switch/filter` 之后发生了什么？filter **不是**字符串匹配，而是编译为谓词程序（kernel/trace/trace_events_filter.c）：

```c
// kernel/trace/trace_events_filter.c (v6.6)
struct filter_pred {                       // 单个谓词
	struct regex		*regex;         // 字符串匹配用（glob/正则前缀）
	struct cpumask          *mask;          // cpumask 谓词
	unsigned short		*ops;
	struct ftrace_event_field *field;   // 字段元数据（来自 format 反射）
	u64			val;            // 比较常量
	u64			val2;
	enum filter_pred_fn	fn_num;         // 类型化函数编号（见下）
	int			offset;         // 记录内偏移
	int			not;
	int			op;             // OP_EQ/OP_LT/.../OP_BAND
};

struct prog_entry {                        // 程序：谓词数组 + 跳转表
	int			target;         // 命中分支时的跳转目标
	int			when_to_branch; // 真/假跳
	struct filter_pred	*pred;
};

int filter_match_preds(struct event_filter *filter, void *rec)
{
	struct prog_entry *prog;
	int i;

	if (!filter) return 1;                  // 无 filter = 全匹配
	prog = rcu_dereference_raw(filter->prog);
	if (!prog) return 1;

	for (i = 0; prog[i].pred; i++) {
		struct filter_pred *pred = prog[i].pred;
		int match = filter_pred_fn_call(pred, rec);
		if (match == prog[i].when_to_branch)
			i = prog[i].target;      // ★布尔短路被编译成跳转
	}
	return prog[i].target;
}
```

执行模型是个**单遍跳转 VM**：解析器（自研 infix parser）把布尔表达式生成 prog_entry 数组，`&&`/`||` 的短路语义变成条件跳转——`A && B` 编译为"A 假则跳到 FAIL 行"。`fn_num` 按字段类型选择特化函数（`FILTER_PRED_FN_U64/FN_32/FN_S16/FN_STRING/FN_COMM/FN_CPUMASK...`），避免热路径上的类型分派。filter 的更新走 RCU（`filter->prog` 换指针），旧程序宽限期后释放；热路径上 `filter_match_preds` 在 `preempt_disable`/SRCU 保护下调用（注释：line 1180）。

> 💡 与用户态正则的对比：`~ "bash*"` 只支持 glob 与前缀/全等匹配，**不是**完整 regex——为了每事件纳秒级的求值开销。

---

## 七、trigger：写路径上的动作注入

trigger 依附于 `trace_event_file->triggers` 链表，在 probe 的提交阶段执行（kernel/trace/trace_events_trigger.c:57）：

```c
// kernel/trace/trace_events_trigger.c (v6.6, 节选)
enum event_trigger_type
event_triggers_call(struct trace_event_file *file,
		    struct trace_buffer *buffer, void *rec,
		    struct ring_buffer_event *event)
{
	struct event_trigger_data *data;
	enum event_trigger_type tt = ETT_NONE;

	if (list_empty(&file->triggers))
		return tt;

	list_for_each_entry_rcu(data, &file->triggers, list) {
		if (data->paused)
			continue;
		if (!rec) {                    // soft-disabled 模式：无记录也触发
			data->ops->trigger(data, buffer, rec, event);
			continue;
		}
		filter = rcu_dereference_sched(data->filter);
		if (filter && !filter_match_preds(filter, rec))
			continue;              // trigger 自身可带 if 条件
		if (event_command_post_trigger(data->cmd_ops)) {
			tt |= data->cmd_ops->trigger_type;   // post 类：登记延后执行
			continue;
		}
		data->ops->trigger(data, buffer, rec, event);
	}
	return tt;
}
```

* **pre/post 分类**：`stacktrace` 是 post trigger——必须等事件已写入 buffer 才能抓栈（保证栈上能看到这条事件）；`traceon/traceoff`、`enable_event` 是立即型。post 触发通过返回的 `tt` 位集在 `event_triggers_post_call()` 补跑。
* **soft-disable 联动**：安装了 trigger 却未 `enable` 事件时，`EVENT_FILE_FL_TRIGGER_MODE` 置位、`SOFT_DISABLED` 置位——probe 早退分支 `trace_trigger_soft_disabled()` 仍会以 `rec=NULL` 调 trigger（"只触发不记录"模式，hist 常用）。
* **hist trigger**：`hist:keys=next_pid:lat=common_timestamp.usecs` 把每条命中的记录聚合进内核哈希表（`CONFIG_HIST_TRIGGERS`），支持变量、算术、onmatch 跨事件关联——本质是"内核态的 t-shirt 版 SQL GROUP BY"，`histogram.rst` 有完整语法。

---

## 八、与 BPF 的挂载点

BPF 程序（`BPF_PROG_TYPE_TRACEPOINT`）并不"劫持"tracepoint，而是作为**又一种 tracepoint 消费者**挂进同一 `funcs` 数组，以及 perf 侧的直连通道：

1. **bpf_raw_tracepoint**（`bpf_raw_tracepoint_open`）：把 `bpf_prog` 直接 push 到 `tp->funcs`（bpf_trace.c 的 `bpf_probe_register`，走 `tracepoint_probe_register_prio_may_exist`）。`perf_trace_run_bpf_submit` 在 perf 事件侧也会把记录投递给挂着的 BPF 程序数组。
2. **prog_array 快路径**：`struct trace_event_call.prog_array`（`CONFIG_PERF_EVENTS` 时存在）保存挂在 perf 事件上的 BPF 程序，`bpf_prog_array_valid()`（linux/trace_events.h:439）用 `READ_ONCE` 做无锁快速检查。
3. **tracepoint-defs.h 的桥接结构**：

```c
// include/linux/tracepoint-defs.h (v6.6, 节选)
struct bpf_raw_event_map {
	struct tracepoint *tp;       // 目标 tracepoint
	void *bpf_func;              // BPF 适配函数（把可变参数打包成 ctx）
	struct bpf_raw_tp_bitmap cache_raw_tp;  // 可 attach 参数位置位图（6.6 新）
	int  wakeup_flags;
};
```

`cache_raw_tp` 是 6.6 新增：让 `bpf_trace_vprintk` 之类的参数敏感 attach 能快速判断哪些 slot 可用。BPF 程序拿到的 `ctx` 就是 `bpf_func` 打包好的 `void *` 数组（布局由 `format` 文件的 BTF 信息决定，`bpftrace` 的 `args->prev_pid` 由此而来）。

---

## 九、实战观测

```bash
# 1. 观察注册过程：开一个 tracepoint，另一个实例观察 tracepoint 自身
mkdir instances/tp
echo 1 > instances/tp/events/kernel_trace/enable 2>/dev/null || true
# （内核没有直接的 "tracepoint 注册" 事件，改用模块加载观察）
modprobe kprobe_example   # 其 sample handler 注册到 _do_fork tracepoint
dmesg | tail -5           # sample 模块打印 __data 的值

# 2. 单/多消费者差异验证：static_call 的存在证据
grep __traceiter_sched_switch /proc/kallsyms   # iterator 符号（需 CONFIG_KALLSYMS_ALL）
grep __probestub_sched_switch /proc/kallsyms   # 空探针符号
grep ' tp_func_' /proc/kallsyms | head -3      # static_call 站点符号

# 3. filter 编译错误定位（parser 会画箭头）
echo 'prev_pid ><> 10' > events/sched/sched_switch/filter
cat events/sched/sched_switch/filter   # 输出 parse_error + ^ 指示

# 4. trigger 组合：wakeup 触发栈
echo 'stacktrace if next_prio < 20' > events/sched/sched_wakeup/trigger
cat trace | tail -40 | stacktrace 展示

# 5. hist 快速上手
echo 'hist:keys=next_pid:lat=common_timestamp.usecs' \
     > events/sched/sched_switch/trigger   # 需 CONFIG_HIST_TRIGGERS
cat events/sched/sched_switch/hist
```

---

## 十、思考题

1. `__DO_TRACE` 里 `preempt_disable_notrace` 与 sched-RCU 的关系是什么？如果去掉它（只靠 `rcu_read_lock`），`tp_stub_func` 的过渡方案还安全吗？提示：递归 tracepoint（tracepoint 回调里又触发 tracepoint）。
2. 单消费者时 static_call 直跳 `funcs[0].func`，但 `__data` 是从 `funcs[0].data` 读的（见 `__DO_TRACE_CALL`）。思考 `tracepoint_update_call` 与 `rcu_assign_pointer(tp->funcs)` 的先后顺序为什么能防止"新 func 配旧 data"——`TP_TRANSITION_SYNC_1_0_1` 到底防的是哪个窗口？
3. 为什么 filter 求值用"跳转小程序"而不是每次解析布尔树？在 `sched_switch` 每秒 100 万次的系统上，一棵 5 节点树的指针追逐开销是多少量级？
4. `trace_##name##_rcuidle` 为什么要同时 `srcu_read_lock_notrace` 和 `ct_irq_enter_irqson()`？只做其一分别会出什么问题（RCU warning vs. 时间记账错误）？
5. kprobe_events（第 4 章）创建的"动态 tracepoint"没有 `static_call_key`（`tracepoint_update_call` 的 NULL 检查）。为什么合成事件可以没有 static_call 站点？
6. 给一个"必须用 `tracepoint_synchronize_unregister` 而普通 `synchronize_rcu` 不够"的具体场景。（提示：NOHZ_FULL 的 idle CPU + rcuidle tracepoint。）

---

> 下一章：[ring buffer 设计](03-ring-buffer设计.md) —— trace_event_buffer_reserve 之后的世界：per-CPU 无锁留位、两种覆盖模式、时间戳压缩与 32 位原子的绝技。
