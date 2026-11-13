# sync_core：指令流水线的"清空"原语（Linux 6.6）

> 阅读目标：吃透 text_poke_bp 第三步之后为什么需要 `sync_core()`（[19 章](19-text_poke_bp家族.md)的三步协议每步间都调它）——x86 CPU 的**指令预取/译码流水线**可能缓存了旧指令字节，必须用**序列化指令**强制丢弃；三种实现（SERIALIZE 指令 / IRET-to-self / CPUID）的取舍（"IRET-to-self is nice because it works on every CPU, at any CPL...but it's a bit slow...and it unmasks NMIs" 注释原话）；以及 `sync_core_before_usermode` 的 **PTI 联动**（sysret/sysysexit 非序列化——用户态切换前的兜底）。全部对照 v6.6（arch/x86/include/asm/sync_core.h 111 行 + include/linux/sync_core.h）。前置：[19-text_poke_bp](19-text_poke_bp家族.md)。

---

## 一、问题：流水线里还存着旧指令

```
text_poke_bp 三步协议完成后：
  新指令字节已写入内存
  但 CPU 的预取队列/μop cache/译码缓冲区
  可能仍然持有**旧字节**的预取副本

如果此时执行流恰好走到那个位置——
  CPU 可能执行**流水线里的旧版本**而非内存中的新版本！
```

头注释原话（sync_core.h:42-56）：

```
"This function forces the icache and prefetched instruction
 stream to catch up with reality in two very specific cases:

 a) Text was modified using one virtual address and is about to
    be executed from the same physical page at a different
    virtual address.

 b) Text was modified on a different CPU, may subsequently be
    executed on this CPU, and you want to make sure the new
    version gets executed. This generally means you're calling
    this in an IPI.

 If you're calling this for a different reason, you're probably
 doing it wrong."
```

## 二、三种实现的取舍（sync_core.h:58-89）

```c
static inline void sync_core(void)
{
	/* 首选：SERIALIZE 指令（Intel SDSI，较新 CPU 才有）*/
	if (static_cpu_has(X86_FEATURE_SERIALIZE)) {
		serialize();
		return;
	}

	/* ★兜底：IRET-to-self——见下方注释 */
	iret_to_self();
}
```

★头注释原话（:71-89）——IRET-to-self 的全评价：

```
"IRET-to-self is nice because it works on every CPU, at any CPL
 (so it's compatible with paravirtualization), and it never exits
 to a hypervisor. The only downsides are that it's a bit slow
 (it seems to be a bit more than 2x slower than the fastest
 options) and that it unmasks NMIs. The 'push %cs' is needed,
 because in paravirtual environments __KERNEL_CS may not be a
 valid CS value when we do IRET directly.

 In case NMI unmasking or performance ever becomes a problem,
 the next best option appears to be MOV-to-CR2 and an
 unconditional jump. That sequence also works on all CPUs,
 but it will fault at CPL3 (i.e. Xen PV).

 CPUID is the conventional way, but it's nasty: it doesn't
 exist on some 486-like CPUs, and it usually exits to a
 hypervisor."
```

| 实现 | 序列化保证 | 速度 | 缺点 |
| :--- | :--- | :--- | :--- |
| SERIALIZE | ✓（专用指令） | 最快 | 仅新 CPU（Intel SDSI） |
| **IRET-to-self** | ✓（IRET 天然序列化） | ~2x 慢 | **unmask NMI** + CPL 限制 |
| MOV-to-CR2 + jmp | ✓ | 较快 | CPL3 下 fault（Xen PV 不可用） |
| CPUID | ✓ | 中等 | 老 486 无此指令 + **退出到 hypervisor** |

★IRET 为什么是序列化的：x86 手册规定 IRET 会清空流水线（所有在途指令完成后才执行返回目标）——利用这个特性，`iret_to_self` 手工构造一个"中断返回到下一条指令"的栈帧，强迫 CPU 清流水线。

## 三、IRET-to-self 的汇编（:13-40）

```asm
/* x86_64 版 */
mov  %ss, %tmp          /* 保存 SS——paravirt 环境的 __KERNEL_CS 不可靠 */
push %tmp               /* SS（新栈帧的段寄存器）*/
push %rsp
addq $8, (%rsp)         /* 跳过 dummy SS 字段 */
pushfq                  /* RFLAGS */
mov  %cs, %tmp
push %tmp               /* CS */
push $1f                /* RIP = 下一条标签 */
iretq                   /* ★中断返回——流水线清空 */
1:                       /* 从这里继续执行 */
```

★**stack frame 的手工构造**——push SS/RFLAGS/CS/RIP 模拟一个"刚被中断的上下文"然后 iret 返回到自己。效果：CPU 认为"刚从中断返回"→全部预取缓冲清零。

## 四、消费者

| 消费者 | 调用方式 | 场景 |
| :--- | :--- | :--- |
| **text_poke_bp**（[19 章](19-text_poke_bp家族.md)） | `text_poke_sync()` → `on_each_cpu(do_sync_core, NULL, 1)` | 每步补丁后的 IPI 全核 sync |
| **stop_machine** | 间接（stop_machine 内含 sync_core 语义） | 全核停止后的代码修改 |
| **membarrier** | `MEMBARRIER_CMD_SYNC_CORE` → `ipi_sync_core` | 用户态请求的全核序列化 |
| **sched/core.c** | `switch_mm` 的 SYNC_CORE 标志 | membarrier 的调度器联动 |
| **jump_label**（static_key） | 经 text_poke_bp | NOP↔jmp 翻转后 |

## 五、sync_core_before_usermode 与 PTI（:96-109）

```c
static inline void sync_core_before_usermode(void)
{
	/* With PTI, we unconditionally serialize before running user code. */
	if (static_cpu_has(X86_FEATURE_PTI))
		return;    /* PTI 的 CR3 切换本身就是序列化的 */

	sync_core();   /* 非 PTI：sysret/sysexit 不序列化——手动 sync */
}
```

头注释原话（:92-94）：*"x86 implements return to user-space through sysexit, sysretl, and sysretq, which are **not core serializing**."*

★**PTI 联动**：PTI（Meltdown 缓解）在每次内核↔用户态切换时**切换 CR3**（页表隔离）——CR3 写入本身是序列化操作——PTI 开启时 `sync_core_before_usermode` 免做。

## 六、在 text_poke_bp 中的角色（回扣 [19 章](19-text_poke_bp家族.md)）

```
text_poke_bp_batch 的三步中每步后调用 text_poke_sync()：
  text_poke_sync() → on_each_cpu(do_sync_core, NULL, 1)
                     → 每 CPU 执行一次 sync_core()
                     → 流水线清空——保证下一步开始前
                        所有 CPU 都不会执行旧的预取副本

成本：on_each_cpu = 一次全核 IPI（~1-5μs）
三步 = 3 次全核 IPI
★text_poke_bp 总延迟 ~3-15μs vs stop_machine 的 ~ms 级
  （仍然好两个数量级）
```

## 七、观测实战

```bash
# 1) CPU 是否支持 SERIALIZE
grep serialize /proc/cpuinfo

# 2) text_poke 的 sync_core 频率
perf record -e text_poke -a sleep 5
#   每次 text_poke 事件对应 3 次 sync_core IPI

# 3) membarrier 的 SYNC_CORE 命令
#   用户态可主动请求全核序列化（用于 JIT 代码修改）
membarrier(MEMBARRIER_CMD_SYNC_CORE, 0, 0)

# 4) PTI 状态
cat /sys/kernel/debug/x86/pti_enabled 2>/dev/null || \
  grep pti /proc/cmdline
```

## 八、思考题

1. IRET 是序列化的——CALL/RET 为什么不是？（提示：x86 手册——RET 仅保证栈上的返回地址正确，不清空流水线；IRET 涉及权限/段/标志的全量恢复，因此被定义为序列化。）
2. SERIALIZE 指令是 Intel SDSI（Software De-serialization Instruction）——为什么 AMD 不叫这个名字？（提示：SDSI 是 Intel 文档名称；AMD 的 SERIALIZE 实现同一功能但手册叫法可能不同。）
3. `on_each_cpu(do_sync_core, NULL, 1)` 的 `1` 参数意味着什么？（提示：wait=1——同步等待所有 CPU 完成 IPI 处理——保证下一步开始前全部 sync。）
4. PTI 开启时 `sync_core_before_usermode` 变空——这是 PTI 的**额外好处**还是设计目标？（提示：PTI 的目标是 Meltdown 防护；CR3 切换的序列化是**副作用**——白赚的性能优化。）
5. membarrier 的 SYNC_CORE 命令让用户态 JIT 也能安全修改代码——它和内核的 text_poke_bp 有何关系？（提示：两者共享 sync_core 原语；JIT 修改自己的代码页而非内核 .text。）
6. 486 无 CPUID → text_poke_bp 在真 486 上用什么？（提示：486 没有 SERIALIZE 也没有 → IRET-to-self 是终极兜底——"works on every CPU, at any CPL"。）

---

> 回看：[19-text_poke_bp 家族](19-text_poke_bp家族.md)（三步协议的主体——本章为其 sync 步骤的深挖）｜[20-static_call](20-static_call.md)（另一消费者）｜[05-tracer 家族](05-tracer家族.md)（ftrace 的批量 text_poke → 3 次 sync_core IPI）｜[18-ftrace 汇编层](18-ftrace汇编层DIRECT分派.md)（RSB 重平衡与序列化的关系）
