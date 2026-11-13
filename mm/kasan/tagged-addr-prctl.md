# tagged_addr prctl：用户态标签 ABI 的实现层（Linux 6.6）

> 阅读目标：兑现 [MTE 与 LAM 章](mte-lam.md)的"只覆盖了 uapi 层"——吃透 **PR_SET/GET_TAGGED_ADDR_CTRL 的完整实现**：★ABI 的 **opt-in 本质**（`access_ok` 里 `TIF_TAGGED_ADDR` 的门——uaccess.h 注释原文；未 opt-in 的进程带标签指针**不做 untag**，直接被拒）；`set_tagged_addr_ctrl` 的护栏（compat 拒/`abi.tagged_addr_disabled` 全局闸的"不追缴"语义）；`set_mte_ctrl` 的算术（排除掩码反转成 GCR exclude/MTE_CTRL 的 16+3 位布局/★asymm 的隐式启用注释原文）；每线程状态（`task->thread.mte_ctrl`）与 exec 重置、切换路径四件事；ptrace 的标签读写消费者。全部对照 v6.6 当轮核对。前置：[MTE 与 LAM](mte-lam.md)（uapi 常量与硬件背景）。

---

## 一、ABI 的 opt-in 本质：access_ok 里的那道门

```c
/* arch/arm64/include/asm/uaccess.h:34-48 */
static inline int access_ok(const void __user *addr, unsigned long size)
{
	/*
	 * Asynchronous I/O running in a kernel thread does not have the
	 * TIF_TAGGED_ADDR flag of the process owning the mm, so always untag
	 * the user address before checking.
	 */
	if (IS_ENABLED(CONFIG_ARM64_TAGGED_ADDR_ABI) &&
	    (current->flags & PF_KTHREAD || test_thread_flag(TIF_TAGGED_ADDR)))
		addr = untagged_addr(addr);

	return likely(__access_ok(addr, size));
}
```

★两段语义：
- **普通任务**：只有 `prctl(PR_SET_TAGGED_ADDR_CTRL, PR_TAGGED_ADDR_ENABLE)` 过的进程（`TIF_TAGGED_ADDR` 置位，thread_info.h:80 "Allow tagged user addresses"）才享受 untag——**ABI 是申请制的**；
- **内核线程**（`PF_KTHREAD`）：AIO 场景替别人做事拿不到 mm 属主的旗标——**无条件 untag**（宁可多剥不可漏剥）。

★补全 [MTE 与 LAM 章](mte-lam.md) §四的表述："`__untagged_addr` 原语与进程无关（恒剥 byte 7）"成立——但**原语无条件 ≠ 处处应用**；`access_ok` 这道门才是"宽松 ABI"的执法点（未 opt-in 进程的 tagged 指针被 `__access_ok` 拒成 EFAULT）。

## 二、PR_SET_TAGGED_ADDR_CTRL：护栏四道（process.c:665-692）

```c
long set_tagged_addr_ctrl(struct task_struct *task, unsigned long arg)
{
	unsigned long valid_mask = PR_TAGGED_ADDR_ENABLE;
	...
	if (is_compat_thread(ti))                    /* ① compat（AArch32）拒 */
		return -EINVAL;

	if (system_supports_mte())                   /* ② 无 MTE 硬件则 MTE 位全无效 */
		valid_mask |= PR_MTE_TCF_SYNC | PR_MTE_TCF_ASYNC \
			| PR_MTE_TAG_MASK;

	if (arg & ~valid_mask)                       /* ③ 越权位拒 */
		return -EINVAL;

	/* ④ 全局闸：sysctl abi.tagged_addr_disabled */
	if (arg & PR_TAGGED_ADDR_ENABLE && tagged_addr_disabled)
		return -EINVAL;

	if (set_mte_ctrl(task, arg) != 0)            /* 委托 MTE 编程 */
		return -EINVAL;

	update_ti_thread_flag(ti, TIF_TAGGED_ADDR, arg & PR_TAGGED_ADDR_ENABLE);
	return 0;
}
```

★**全局闸的"不追缴"语义**（:713-716 sysctl 表注释原文）："This control only prevents the tagged address ABI enabling via prctl() and **does not disable it for tasks that already opted in** to the relaxed ABI."——运行中系统的 sysctl 只封新开户，不没收存量。

`get_tagged_addr_ctrl`（:695-709）为镜像回读：`TIF_TAGGED_ADDR → PR_TAGGED_ADDR_ENABLE` + `get_mte_ctrl`。

## 三、set_mte_ctrl：两段算术与一个惊喜（mte.c:333-366）

```c
long set_mte_ctrl(struct task_struct *task, unsigned long arg)
{
	u64 mte_ctrl = (~((arg & PR_MTE_TAG_MASK) >> PR_MTE_TAG_SHIFT) &
			SYS_GCR_EL1_EXCL_MASK) << MTE_CTRL_GCR_USER_EXCL_SHIFT;
	...
	if (arg & PR_MTE_TCF_ASYNC)
		mte_ctrl |= MTE_CTRL_TCF_ASYNC;
	if (arg & PR_MTE_TCF_SYNC)
		mte_ctrl |= MTE_CTRL_TCF_SYNC;

	/*
	 * If the system supports it and both sync and async modes are
	 * specified then implicitly enable asymmetric mode.
	 * Userspace could see a mix of both sync and async anyway due
	 * to differing or changing defaults on CPUs.
	 */
	if (cpus_have_cap(ARM64_MTE_ASYMM) &&
	    (arg & PR_MTE_TCF_ASYNC) && (arg & PR_MTE_TCF_SYNC))
		mte_ctrl |= MTE_CTRL_TCF_ASYMM;

	task->thread.mte_ctrl = mte_ctrl;
	if (task == current) {
		preempt_disable();
		mte_update_sctlr_user(task);     /* TCF0 编入 thread.sctlr_user */
		mte_update_gcr_excl(task);       /* exclude 掩码写 GCR_EL1 */
		update_sctlr_el1(task->thread.sctlr_user);
		preempt_enable();
	}
	return 0;
}
```

- **排除掩码的反转**：用户态 `PR_MTE_TAG_MASK`（uapi prctl.h:243-244，`0xffff<<3`）给的是"**允许**的标签"集合——取反成 **exclude** 进 `GCR_EL1`（`irg` 随机标签生成器不再产出被排除者，KASAN 的 `kasan_random_tag` 同受约束）
- **MTE_CTRL 每线程格式**（processor.h:19-24）：bits 0-15 = GCR user exclude（`MTE_CTRL_GCR_USER_EXCL_MASK 0xffff`）；bit 16/17/18 = TCF SYNC/ASYNC/AS YMM
- ★**asymm 的隐式启用**：同时要求 sync+async 且硬件支持 asymm → 直接给 asymm（读同步写异步恰好是两者的合成；注释的兜底理由——"Userspace could see a mix of both sync and async anyway due to differing or changing defaults on CPUs"）
- **对非 current 任务**只存 `thread.mte_ctrl`（切到它时再生效）——prctl(操作别的进程) 的延迟生效路径

## 四、每线程状态：exec 重置与切换四件事

```
arch_setup_new_exec（process.c:633）→ mte_thread_init_user（mte.c:231-242）
  ★每个 exec 全部重来：清 TFSRE0_EL1 的 pending 异步故障 +
  TIF_MTE_ASYNC_FAULT + set_mte_ctrl(current, 0)（TCF 归零/全标签允许）

mte_thread_switch（mte.c:244-260，每次 __switch_to）：
  ① mte_update_sctlr_user(next)    /* 从 thread.mte_ctrl 恢复 TCF0 */
  ② mte_update_gcr_excl(next)      /* 恢复本线程的排除掩码 */
  ③ mte_disable_tco_entry(next)    /* 注释原文："TCO may not have been
                                      disabled on exception entry for the
                                      current task." */
  ④ 检查 EL1 的异步标签故障（TFSR_EL1——依赖 __switch_to 的 dsb 保证
     间接写已同步，见 mte_suspend_enter 的同款 dsb(nsh)+isb 理由）
```

## 五、ptrace 消费者：读别人的标签（mte.c:390+ / ptrace.c:2134-2135）

```
PTRACE_PEEKMTETAGS / PTRACE_POKEMTETAGS（arch/arm64/kernel/ptrace.c:2134）
  → __access_remote_tags(mm, addr, kiov, gup_flags)
     函数族头注释："Access MTE tags in another process' address space
     as given in mm... Inspired by __access_remote_vm()."
  调试器（gdb/valgrind-mte 类工具）可逐字节读/写目标进程的内存标签——
  MTE 生态的"外部观察窗"
```

## 六、uapi → 实现映射总表（与 [MTE 与 LAM 章](mte-lam.md) 对齐）

| uapi 位 | 实现落点 | 效果 |
| :--- | :--- | :--- |
| `PR_TAGGED_ADDR_ENABLE`（bit 0） | `TIF_TAGGED_ADDR` + `access_ok` 门 | 宽松 ABI 开启（带标签指针被 untag 后检查） |
| `PR_MTE_TCF_SYNC/ASYNC`（bits 1-2） | `thread.mte_ctrl` bits 16/17 → `sctlr_user` 的 TCF0 → 上下文切换写入 | EL0 访问的标签检查模式 |
| （sync+async 且硬件支持） | `MTE_CTRL_TCF_ASYMM` bit 18 | ★隐式 asymm |
| `PR_MTE_TAG_MASK`（bits 3-18） | 反转 → `thread.mte_ctrl` bits 0-15 → `GCR_EL1.EXCLUDE` | 标签生成器的允许集合 |

## 七、观测实战

```bash
# 1) 全局闸现状
cat /proc/sys/abi/tagged_addr_disabled     # 0=可开新户（默认）

# 2) 某进程是否 opt-in（TIF_TAGGED_ADDR）
cat /proc/<pid>/status | grep -i 'tagged'  # 无直接导出——间接法：
#   看其 uapi 调用（strace -e prctl -p <pid> | grep TAGGED_ADDR）

# 3) 运行时验证 ABI 生效（自测程序）
#   prctl(PR_GET_TAGGED_ADDR_CTRL) 返回值含 PR_TAGGED_ADDR_ENABLE？
#   带 tag 指针做 write() 成功与否（未 opt-in 应 EFAULT）

# 4) 硬件前提
grep -o mte /proc/cpuinfo | head -1         # HWCAP_MTE
dmesg | grep -i mte                         # "MTE: enabled in ... mode"
```

## 八、思考题

1. `access_ok` 对 `PF_KTHREAD` 无条件 untag——为什么不等价于"全局恒 untag"？（提示：普通任务的 untag 门是 ABI 契约的一部分——未 opt-in 进程**应该**被拒（它的指针高位是脏数据而非标签）；内核线程的 mm 属主不可知，宁可多剥。两种"宽容"理由完全不同。）
2. `PR_MTE_TAG_MASK` 传"允许集"而 GCR 存"排除集"——反转为什么发生在内核侧？（提示：用户态语义是白名单（我只打算用这些 tag）；硬件语义是黑名单（irg 别生成这些）；一处反转把两个世界接上。）
3. `set_mte_ctrl` 对非 current 任务只写 `thread.mte_ctrl` 不碰寄存器——何时补写？（提示：`mte_thread_switch` 的 ①②——被控进程下次被调度时统一恢复；prctl(PTRACE 式远控)的生效点在切换。）
4. asymm 的"隐式启用"为什么不算破坏 uapi 契约？（提示：注释的兜底——用户态本来就"could see a mix of both sync and async"；asymm=读侧 sync+写侧 async，恰是同时请求两者的最严格合成。）
5. exec 全量重置（`set_mte_ctrl(current, 0)`）为什么必要？（提示：exec 换了程序但可能没换 mm 归属语义；旧映像的标签检查模式/排除集对新映像是不可预测的先验状态——归零即安全默认。）
6. `mte_thread_switch` 的第④步为何强调"依赖 __switch_to 的 dsb"？（提示：TFSR_EL1 是**间接写**寄存器——写后同步有延迟；在切换点读它必须先有屏障保证读到的是最终值——suspend 路径的 dsb(nsh)+isb 是同款纪律。）

---

> 回看：[MTE 与 LAM](mte-lam.md)（uapi 常量与硬件层——本章补全其 §五的实现侧）｜[KASAN boot 初始化](kasan-boot-init.md)（kasan_init_tags 与排除集的联动）｜[KASAN_VMALLOC](kasan-vmalloc.md)（HW_TAGS 的 vmalloc 侧）｜[mm/README](../README.md)（分册导读）
