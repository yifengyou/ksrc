# static_call：运行时改目标的直接调用（Linux 6.6）

> 阅读目标：吃透 static_call 的**双层机制**——trampoline 模式（调用点跳到一个 trampoline，trampoline 的 jmp 目标可修改）与 inline 模式（CONFIG_HAVE_STATIC_CALL_INLINE：objtool 找出所有调用点，**调用点本身**被 text_poke_bp 改写为直接 call）；`struct static_call_key` 的 key/site/tramp 三角；x86 的五种指令变换（CALL/NOP/JMP/RET/JCC）；`__static_call_return0` 的 xor5rax 技巧（5 字节清寄存器替代一次 call+ret）；以及 tracepoint 的 `__DO_TRACE_CALL` 消费端回扣。全部对照 v6.6（include/linux/static_call.h 346 行、arch/x86/kernel/static_call.c 214 行）。前置：[02-tracepoint 机制](02-tracepoint机制.md)（static_call 的头号消费者）｜[19-text_poke_bp](19-text_poke_bp家族.md)（打补丁原语）。

---

## 一、问题：间接调用的分支预测税

```
传统函数指针调用：call *(%rax)——间接跳转
  开销：分支预测器可能预测错（~15-20 cycles 的 pipeline flush）
  spectre v2 时代再加 retpoline → 开销更重

static_call 的答案：调用点的目标在运行时可改的**直接调用**
  call 0x<直接地址>——无间接跳转、无预测、无 retpoline
  修改目标 = text_poke_bp 改写 call 的 rel32 偏移
```

## 二、两种模式（static_call.h:52-64 注释原话）

```
头注释："Each static_call() site calls into a trampoline
        associated with the name. The trampoline has a direct
        branch to the default function. Updates to a name will
        modify the trampoline's branch destination.

        If the arch has CONFIG_HAVE_STATIC_CALL_INLINE, then
        the call sites themselves will be patched at runtime
        to call the functions directly, rather than calling
        through the trampoline."
```

| 模式 | CONFIG | 调用路径 | 修改时改什么 |
| :--- | :--- | :--- | :--- |
| **trampoline** | HAVE_STATIC_CALL | 调用点 → trampoline → 默认函数 | trampoline 的 jmp 目标（**一处**补丁，全体生效） |
| **inline** | HAVE_STATIC_CALL_INLINE | 调用点 → **直接** call 目标函数 | **每个调用点**逐个补丁（objtool 扫描 `.static_call_sites` 段） |

★inline 模式是终极形态——**零间接层**——但需要 objtool 配合（发现所有调用点并记录到段中）。x86 默认开 inline。

## 三、三角结构：key / site / tramp

```c
struct static_call_key {          /* 全局的"名字"——控制目标 */
	void *func;               /* 当前生效的函数指针 */
#ifdef CONFIG_HAVE_STATIC_CALL_INLINE
	/* 由 objtool 填充：本 key 的所有调用点 */
	struct static_call_site *sites;
	int nr_sites;             /* count in .static_call_sites */
#endif
};

/* 每个 static_call(name)(...) 调用点： */
struct static_call_site {         /* 链接期由 objtool 写入段 */
	s32 insn;                 /* 相对 __static_call_sites 的偏移 */
	s32 key;                  /* 相对 key 的偏移 */
};

/* 每个 DEFINE_STATIC_CALL(name, func) 还生成： */
/* trampoline（汇编/机器码）——初始化为 jmp func 的 5 字节跳板 */
```

## 四、五种指令变换（arch/x86/kernel/static_call.c:11-17/:54-111）

```c
enum insn_type {
	CALL = 0,  /* site call——5 字节 call rel32 */
	NOP  = 1,  /* site cond-call——5 字节 NOP（static_call_cond 的"关"态）*/
	JMP  = 2,  /* tramp / site tail-call——5 字节 jmp rel32 */
	RET  = 3,  /* tramp / site cond-tail-call——ret+int3 */
	JCC  = 4,  /* 条件跳转 Jcc.d32——6 字节（0x0f 8x rel32）*/
};

static void __static_call_transform(void *insn, enum insn_type type,
				    void *func, bool modinit)
{
	...
	case CALL:
		code = text_gen_insn(CALL_INSN_OPCODE, insn, func);
		if (func == &__static_call_return0) {
			emulate = code;        /* 仿真 call+ret */
			code = &xor5rax;       /* ★替代为 5 字节 xor %eax,%eax */
		}
		break;
	...
	if (system_state == SYSTEM_BOOTING || modinit)
		return text_poke_early(insn, code, size);  /* boot 期不打补丁 */
	text_poke_bp(insn, code, size, emulate);         /* 运行期三步协议 */
}
```

★**xor5rax 技巧**：`__static_call_return0`（返回 0 的"空函数"）的调用可替换为 `xor %eax,%eax`（5 字节、清返回值寄存器）——省去整个 call+ret 往返。emulate 参数让 text_poke_bp 的 INT3 handler 能正确仿真。

## 五、消费者一：tracepoint 的 __DO_TRACE_CALL

[02 章](02-tracepoint机制.md) 已核对的 static_call 展开（tracepoint.h）：

```c
#define __DECLARE_TRACE(name, proto, args, cond, data_proto)		\
	DECLARE_STATIC_CALL(tp_func_##name, __traceiter_##name);	\
	static inline void trace_##name(proto)				\
	{								\
		if (static_key_false(&__tracepoint_##name.key))		\
			__DO_TRACE(name, ...);				\
	}								\

#ifdef CONFIG_HAVE_STATIC_CALL
#define __DO_TRACE_CALL(name, args)					\
	do {								\
		struct tracepoint_func *it_func_ptr;			\
		it_func_ptr = rcu_dereference_raw(			\
				(&__tracepoint_##name)->funcs);		\
		if (it_func_ptr) {					\
			__data = (it_func_ptr)->data;			\
			static_call(tp_func_##name)(__data, args);	\
		}							\
	} while (0)
#endif
```

★**tracepoint 的 static_call 化**（5.10 引入）：单消费者时 `static_call(tp_func_##name)(...)` 是**一条直接 call**（目标 = funcs[0].func）——替代旧的间接 `(*it_func)(__data, args)`。多消费者时 target 切到 iterator。

## 六、消费者二：perf 的 PMU 调用

```
DEFINE_STATIC_CALL(perf_guest_info, __weak perf_guest_info);
/* x86 的 PMU 回调、KVM 的 guest info——高频路径上的间接调用消除 */
```

其他消费者：`static_call(cpu_key)`（crash 上下文的 CPU 获取）、`x86_return_thunk` 的 spectre 缓解路径等。

## 七、观测实战

```bash
# 1) 消费者清点
grep -r 'DEFINE_STATIC_CALL\|DECLARE_STATIC_CALL' kernel/ arch/x86/ | wc -l
grep -r 'static_call(' kernel/trace/tracepoint.h | head -3  # tracepoint

# 2) objtool 的段
readelf -S vmlinux | grep static_call
#   .static_call_sites 段：所有 inline 模式调用点

# 3) tracepoint 的 static_call 目标切换（第 02 章 §3.2）
cd /sys/kernel/tracing
echo 1 > events/sched/sched_switch/enable
#   → tracepoint_add_func → tracepoint_update_call → __static_call_update
#   → text_poke_bp 改写调用点的 call 目标

# 4) 性能对比（间接 vs static_call）
perf stat -e branches,branch-misses -a -- bpftrace -e 'kfunc:X { @=count(); }'
```

## 八、思考题

1. trampoline 模式下**一次** tramp 补丁就全体生效——inline 模式逐 site 补丁反而贵，为什么 x86 默认选 inline？（提示：trampoline 多一层 jmp → 尾延迟 + I-cache 额外一行；inline 零间接层。）
2. `static_call_cond` 的 NOP 态——text_poke_bp 写 5 字节 NOP 替代 call，INT3 handler 需要仿真什么？（提示：NOP 不需要仿真——旧 call 的副作用（栈上 push 返回地址）必须"撤销"——emulate 回调把 rsp 恢复。）
3. `DEFINE_STATIC_CALL_RET0` 的 `xor5rax` 优化——如果函数非 NULL 但返回值恰为 0，static_call 也能做这个替换吗？（提示：不能——RET0 是**语义约定**（调用者知道返回 0 可用 xor 替代），不是运行时推断。）
4. objtool 扫描 `.static_call_sites` 段——为什么不在运行时用 text_poke_bp 的批量模式改？（提示：**在运行时改**——objtool 只在**链接期**找到所有 site 并写入段；运行时 `__static_call_update` 遍历段做批量补丁。）
5. tracepoint 的 static_call 在 5.10 引入——[trace/02] 引用的 commit d25e37d89dd2 已核证。static_call 本身也是 5.10 进的——这是巧合还是协同设计？（提示：同一 commit 系列——tracepoint 的 static_call 优化**依赖** static_call 基础设施，两者一起落地。）
6. `__static_call_return0` 的 emulate 参数让 INT3 handler 仿真 `call+ret`——仿真与实际执行的副作用差异？（提示：栈上多 push 的返回地址需要 handler 恢复——emulate 回调里 `regs->ip` 和 `regs->sp` 同时修改。）

---

> 回看：[02-tracepoint 机制](02-tracepoint机制.md)（头号消费者的完整展开）｜[19-text_poke_bp](19-text_poke_bp家族.md)（打补丁的三步协议）｜[18-ftrace 汇编层](18-ftrace汇编层DIRECT分派.md)（ftrace 的另一套直接调用通道）｜[05-tracer 家族](05-tracer家族.md)（ftrace_make_call 也用 text_poke_bp）
