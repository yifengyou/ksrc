# oops

## 入口函数

```c
unsigned long oops_begin(void)
{
	int cpu;
	unsigned long flags;

	oops_enter();

	/* racy, but better than risking deadlock. */
	raw_local_irq_save(flags);
	cpu = smp_processor_id();
	if (!arch_spin_trylock(&die_lock)) {
		if (cpu == die_owner)
			/* nested oops. should stop eventually */;
		else
			arch_spin_lock(&die_lock);
	}
	die_nest_count++;
	die_owner = cpu;
	console_verbose();
	bust_spinlocks(1);
	return flags;
}
NOKPROBE_SYMBOL(oops_begin);
```

在 Linux 内核中，**Oops** 和 **Panic** 是两类不同严重程度的内核错误处理机制，它们有明确的入口函数、调用路径和触发条件。以下是详细分析（基于主流内核版本，包括即将发布的 6.18）：

---

### 一、Panic 的入口函数

#### ✅ 入口函数：
```c
void panic(const char *fmt, ...)
```
- **定义位置**：`kernel/panic.c`
- **作用**：处理**不可恢复的致命错误**，终止系统运行。

#### 🔁 谁调用 `panic()`？
`panic()` 通常由以下几类代码主动调用：

| 调用者 | 示例场景 |
|--------|---------|
| **内核核心子系统** | - `BUG()` 宏（`include/asm-generic/bug.h`）- `assert_spin_locked()` 失败- 文件系统元数据损坏（如 ext4 检测到超级块异常） |
| **架构相关代码** | - ARM64/x86 在异常处理中遇到无法恢复的错误（如 double fault、stack overflow）- `die()` 函数（x86）或 `__do_kernel_fault()`（ARM64）在特定条件下调用 |
| **配置驱动行为** | - `panic_on_oops=1` 时，Oops 会升级为 Panic- `panic_on_warn=1` 时，`WARN()` 触发 Panic |
| **看门狗超时** | - soft/hard lockup watchdog 超时且配置为 `nmi_watchdog=panic` |

> 📌 注意：`panic()` 是**显式调用**的，不是自动触发的异常处理入口。

---

### 二、Oops 的入口函数

#### ✅ Oops 并没有一个单一的“入口函数”，而是一个**错误报告流程**，其核心是：

```c
void oops_begin(void)
void oops_end(unsigned long flags, struct pt_regs *regs, int signr)
```
- **定义位置**：`kernel/printk/printk.c`（旧版在 `kernel/panic.c`）
- **但真正的起点是架构相关的异常处理函数**。

#### 🔁 Oops 的典型触发路径（以 ARM64 为例）：

1. **硬件异常发生**（如非法指令、页错误、对齐错误等）
2. **进入异常向量表** → 调用 `el1_sync`（内核态同步异常）
3. **分发到具体处理函数**，例如：
   ```c
   // arch/arm64/mm/fault.c
   asmlinkage void __do_kernel_fault(unsigned long addr, unsigned int esr,
                                     struct pt_regs *regs)
   ```
4. **在 `__do_kernel_fault()` 中**：
    - 若判断为“可报告但非致命”错误（如用户态访问非法地址已正确杀死进程），则**不触发 Oops**。
    - 若发生在**内核态且无法处理**（如内核访问空指针），则调用：
      ```c
      die("Oops", regs, esr);  // ← Oops 的实际入口点！
      ```

#### ✅ 因此，**Oops 的真正入口是架构相关的 `die()` 函数**：

- **ARM64**：`arch/arm64/kernel/traps.c`
  ```c
  void die(const char *str, struct pt_regs *regs, int err)
  ```
- **x86**：`arch/x86/kernel/dumpstack.c`
  ```c
  void die(const char *str, struct pt_regs *regs, long err)
  ```

#### `die()` 做了什么？
1. 调用 `oops_begin()` 获取控制台锁、禁用本地中断。
2. 打印寄存器状态、堆栈回溯（通过 `show_regs()`）。
3. 调用 `oops_end()`：
    - 如果 `panic_on_oops == 1` → 调用 `panic("Fatal exception")`
    - 否则 → 尝试杀死当前进程（若在用户上下文）或继续（危险！）

---

### 三、关键区别总结

| 特性 | Oops | Panic |
|------|------|-------|
| **入口函数** | 架构相关 `die()`（如 `arch/arm64/kernel/traps.c`） | `kernel/panic.c: panic()` |
| **触发方式** | 内核态异常（页错误、未定义指令等）→ `die()` | 显式调用 `panic()` 或 Oops 升级 |
| **是否致命** | 默认**非致命**（系统可能继续运行，但状态不确定） | **致命**，系统必须终止 |
| **谁调用它** | 异常处理路径（如 `__do_kernel_fault`） | 内核任意子系统（BUG、断言失败、MCE 等） |
| **后续行为** | 打印日志，可能 kill 进程；若 `panic_on_oops=1` 则转 panic | 停止 CPU、kdump、重启或挂起 |

---

### 四、典型调用链示例（ARM64 内核空指针解引用）

```text
[异常] 
  → el1_sync 
    → bad_el1_sync 
      → __do_kernel_fault(addr=0x0, ...) 
        → die("Oops", regs, esr) 
          → oops_begin() 
          → show_regs() 
          → oops_end()
            ├─ if (panic_on_oops) → panic("Fatal exception in Oops")
            └─ else → try to continue (dangerous!)
```

而如果是 `BUG()` 触发：
```c
BUG(); 
  → __builtin_unreachable() + ud2 (x86) or brk (ARM64)
    → illegal instruction exception
      → die("BUG") 
        → ... → panic() （因为 BUG 默认设置 `panic_on_oops` 语义）
```

> 实际上，`BUG()` 通常直接展开为 `unreachable()` + 编译器提示，但也会触发异常并进入 `die()`，最终因 `report_bug()` 返回 `BUG_PANIC` 而调用 `panic()`。

---

### 结论

- **Panic 入口**：`kernel/panic.c: panic()` —— **被显式调用**。
- **Oops 入口**：架构相关 `die()` 函数（如 `arch/arm64/kernel/traps.c`）—— **由异常处理路径调用**。
- **调用者**：
    - `panic()` ← 内核各子系统、`die()`（当 `panic_on_oops=1`）、看门狗等。
    - `die()` ← 异常处理函数（如 `__do_kernel_fault`, `undef_hook` 等）。

这种设计使得 Linux 能在“尽力恢复”（Oops）和“安全终止”（Panic）之间灵活切换，是 RAS 机制的重要体现。


---

