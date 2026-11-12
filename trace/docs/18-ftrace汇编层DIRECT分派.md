# ftrace 汇编层：DIRECT 分派与 trampoline 生成（Linux 6.6）

> 阅读目标：吃透 ftrace 的 x86 汇编级实现——`ftrace_caller`（无 regs 快路径）与 `ftrace_regs_caller`（全 regs 慢路径）的栈布局、`ftrace_call` 标签的**运行时 text_poke**（call ftrace_stub → 改成 call 实际 handler）、`create_trampoline` 的**模板拷贝法**（把 ftrace_caller 的机器码复制到分配页并打 ops 指针补丁）、以及 DIRECT 分派的 `orig_ax` 机制（x86 用一个寄存器实现零 text_poke 的函数替换）。全部对照 v6.6（arch/x86/kernel/ftrace_64.S 380 行、arch/x86/kernel/ftrace.c、arch/x86/include/asm/ftrace.h）。前置：[05-tracer 家族](05-tracer家族.md)（ftrace ops 链）｜[14-BPF trampoline](14-BPFtrampoline.md)（DIRECT 消费者）｜[17-ftrace direct](17-ftrace_direct共享地基.md)（协调协议）。

---

## 一、两条汇编路径（ftrace_64.S:154-312）

```
被跟踪函数入口（gcc -pg / clang -mfentry 插入的 call __fentry__）
  → 5 字节 NOP（未启用）/ call ftrace_caller（启用后）
      │
      ├── ftrace_caller（:154-196）── 快路径：只保存 ip/parent_ip
      │   ├── save_mcount_regs（压 RDI/RSI/RDX/RCX/R8/R9 + 栈帧）
      │   ├── leaq MCOUNT_REG_SIZE+8(%rsp), %rcx → RSP 字段
      │   ├── movq function_trace_op(%rip), %rdx   ← ★第 3 参数：当前 ops
      │   ├── leaq (%rsp), %rcx                     ← 第 4 参数：栈帧（非 pt_regs）
      │   │
      │   SYM_INNER_LABEL(ftrace_call, ...)  ← ★text_poke 的目标
      │   call ftrace_stub                ← 原地 5 字节 call（初始为 stub）
      │   │
      │   ├── movq RIP(%rsp), %rax        ← ★handler 可改 RIP
      │   ├── movq %rax, MCOUNT_REG_SIZE(%rsp)  ← 改写返回地址
      │   └── restore_mcount_regs + RET
      │
      │   /* :194 注释原话："The code up to this label is copied into
      │      trampolines so think twice before adding any new code
      │      or changing the layout here." */
      │   SYM_INNER_LABEL(ftrace_caller_end, ...)
      │
      └── ftrace_regs_caller（:199-312）── 慢路径：完整 pt_regs
          ├── pushfq（先存 flags）
          ├── save_mcount_regs 8（+8 字节 flags 偏移）
          ├── 逐寄存器保存 R15/R14/R13/R12/R11/R10/RBX
          ├── SS/CS 段选择子（__KERNEL_DS/__KERNEL_CS）
          ├── ftrace_regs_call 标签（同款 text_poke 点）
          ├── 恢复时："Handlers can change the RIP" 的注释原话
          ├── popfq + RET
          └── ★CALL .Ldo_rebalance; int3 ← RSB 重平衡
              （注释原话："This PUSH;RET pattern unbalances the RSB,
                inject a pointless CALL to rebalance."——RSB 填充防
                Spectre-RSB）
```

## 二、ftrace_call 标签：text_poke 的锚点

```
ftrace_caller 汇编中：
    SYM_INNER_LABEL(ftrace_call, ...)
    call ftrace_stub             ← 5 字节 call 指令

启用 tracer 时（ftrace.c 的 ftrace_update_ftrace_func）：
    text_poke_bp(&ftrace_call 的 call 目标, 新 handler)
    → 原地改写 call 指令的 rel32 偏移

★ftrace_stub 的身份：
    它不是真正的"什么都不做"——它是 text_poke 的**初始占位**
    （5 字节 call 需要一个合法目标才能被 int3 陷阱安全替换）
```

## 三、create_trampoline：模板拷贝法（arch/x86/kernel/ftrace.c:313-395）

```c
static unsigned long __init
create_trampoline(struct ftrace_ops *ops, unsigned int *tramp_size)
{
	/* 模板选择：SAVE_REGS → regs 版 / 否则 → 普通 ftrace_caller */
	if (ops->flags & FTRACE_OPS_FL_SAVE_REGS) {
		start_offset = (unsigned long)ftrace_regs_caller;
		end_offset = (unsigned long)ftrace_regs_caller_end;
		op_offset = (unsigned long)ftrace_regs_caller_op_ptr;
		call_offset = (unsigned long)ftrace_regs_call;
	} else {
		start_offset = (unsigned long)ftrace_caller;
		end_offset = (unsigned long)ftrace_caller_end;
		op_offset = (unsigned long)ftrace_caller_op_ptr;
		call_offset = (unsigned long)ftrace_call;
	}

	size = end_offset - start_offset;
	trampoline = alloc_tramp(size + RET_SIZE + sizeof(void *));

	/* ① 把 ftrace_caller 的机器码原样拷贝到 trampoline 页 */
	memcpy(trampoline, (void *)start_offset, size);

	/* ② 修改 op_ptr 处的 RIP 相对寻址——指向 trampoline 尾部的 ops 指针 */
	/* ③ 修改 call 偏移——指向 ops->func 或 __ftrace_regs_caller */
	/* ④ 尾部加 ret + int3 */
	...
	set_vm_flush_reset_perms(trampoline);   /* W^X：只执行 */
}
```

★**模板拷贝法**：trampoline 不是手写汇编——而是把 `ftrace_caller` 的**现成机器码**拷贝到新页并打三个补丁（ops 指针/call 目标/返回指令）。这就是 ftrace_64.S 注释 "think twice before adding any new code" 的原因——**模板的布局就是 trampoline 的 ABI**。

## 四、DIRECT 分派：orig_ax 的妙用

```c
/* arch/x86/include/asm/ftrace.h:87-93 */
static inline notrace void
__arch_ftrace_set_direct_caller(struct pt_regs *regs, unsigned long addr)
{
	/* Emulate a call */
	regs->orig_ax = addr;
}
#define arch_ftrace_set_direct_caller(fregs, addr) \
	__arch_ftrace_set_direct_caller(&(fregs)->regs, addr)
```

```
★x86 的 DIRECT 实现：
  ftrace_regs_caller 的返回路径中：
    movq RIP(%rsp), %rax       ← handler 可改的 RIP
    movq %rax, MCOUNT_REG_SIZE+8(%rsp)
    /* orig_ax 作为"附加返回地址"参与最终的跳转决策 */

  IPMODIFY handler（如 klp_ftrace_handler）通过
  arch_ftrace_set_direct_caller(fregs, addr)
  设置 regs->orig_ax = addr
  → 汇编返回路径检测到 orig_ax 非零 → 跳转到 orig_ax 而非原 RIP

★orig_ax 是系统调用号保存槽——在 ftrace 上下文中被征用为
  "direct call 目标地址"——一次寄存器复用，零额外栈空间
```

## 五、RSB 重平衡：Spectre 的回响

```
ftrace_regs_caller 返回路径（ftrace_64.S:295-304）：
    popfq
    /* :295-298 注释原话："The above left an extra return value on
       the stack; effectively doing a tail-call without using a
       register. This PUSH;RET pattern unbalances the RSB, inject
       a pointless CALL to rebalance." */
    CALL .Ldo_rebalance
    int3
.Ldo_rebalance:
    add $8, %rsp
    RET
```

★**PUSH;RET 模式破坏 RSB**（Return Stack Buffer，CPU 的返回地址预测器）——攻击者可利用 RSB 不平衡训练 Spectre-RSB。注入一个"无用 CALL"让 RSB 恢复平衡——**观测基础设施的安全副作用**。

## 六、三级调用路径的成本对比

| 路径 | 指令数（估） | 使用者 |
| :--- | :--- | :--- |
| NOP（未启用） | 5 字节 nop = 1 指令 | 所有关 |
| ftrace_caller（ops 链） | ~20 指令（save/加载/call/restore/ret） | function tracer |
| ftrace_regs_caller（全 regs） | ~35 指令（额外 9 寄存器 + flags） | kprobe-on-ftrace/livepatch |
| trampoline（DIRECT） | ~20 指令（同 caller 模板）+ 0 间接 | BPF fentry/fexit |

## 七、观测实战

```bash
# 1) ftrace_call 的 text_poke 现场（需要 kprobe）
bpftrace -e 'kprobe:text_poke_bp /str(arg1) != ""/ { @[kstack] = count(); }'

# 2) 汇编级查看（VMLINUX 有符号时）
objdump -d vmlinux --start-address=$(nm vmlinux | grep ftrace_caller | awk '{print "0x"$1}') | head -30

# 3) trampoline 页的识别
grep ftrace_trampoline /proc/kallsyms
#   每个有 trampoline 的 ops 一个符号

# 4) DIRECT 调用的验证
perf record -e cycles -a -- bpftrace -e 'kfunc:__x64_sys_write { @=count(); }'
#   fentry（DIRECT）走 trampoline 模板 → 路径与 function tracer 等长
```

## 八、思考题

1. `ftrace_caller` 只保存 6 个参数寄存器（RDI/RSI/RDX/RCX/R8/R9）——不保存 callee-saved（RBX/R12-R15），为什么安全？（提示：ABI——callee-saved 由被调用者保存；ftrace_caller 本身就是"被调用者"。）
2. `ftrace_regs_caller` 保存 flags 在 `save_mcount_regs 8` 的 +8 偏移——为什么不把 flags 放在 pt_regs 的 EFLAGS 字段？（提示：pushfq 在最前面压——后续的 save_mcount_regs 布局不变；EFLAGS 字段是从栈顶偏移拷贝填充的。）
3. trampoline 模板拷贝法的一个隐患：如果 ftrace_caller 的布局在内核版本间变化会怎样？（提示："think twice" 注释——布局即 ABI，无兼容性承诺。）
4. `orig_ax` 被征用为 direct_call 目标——如果被跟踪函数恰好是系统调用入口（entry_SYSCALL_64 的路径），orig_ax 已被占用怎么办？（提示：系统调用入口不走 ftrace——`__fentry__` 在 do_syscall_64 之前。）
5. RSB 重平衡的 CALL;int3 对性能影响多大？（提示：仅在 ftrace_regs_caller 的返回路径——livepatch/kprobe 场景，本身已 30+ 指令。）
6. DIRECT 路径的 trampoline 与 [14 章](14-BPFtrampoline.md) 的 trampoline 是同一个东西吗？（提示：不是——ftrace 的 create_trampoline 拷贝 ftrace_caller 模板；BPF 的 arch_prepare_bpf_trampoline 按需生成——两者共存于 [17 章](17-ftrace_direct共享地基.md)的协调协议。）

---

> 回看：[05-tracer 家族](05-tracer家族.md)（ftrace ops 链与 function tracer 的机制）｜[14-BPF trampoline](14-BPFtrampoline.md)（DIRECT 的消费者——但 trampoline 生成器不同）｜[17-ftrace direct](17-ftrace_direct共享地基.md)（DIRECT 通道与 IPMODIFY 协调）｜[debug-adversarial](../../locking/debug-adversarial.md)（上刑家族——text_poke 是它们的共享地基）
