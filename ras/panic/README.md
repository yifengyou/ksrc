# panic

## 入口函数

```c

/**
 *	panic - halt the system
 *	@fmt: The text string to print
 *
 *	Display a message, then perform cleanups.
 *
 *	This function never returns.
 */
void panic(const char *fmt, ...)
{
	static char buf[1024];
	va_list args;
	long i, i_next = 0, len;
	int state = 0;
	int old_cpu, this_cpu;
	bool _crash_kexec_post_notifiers = crash_kexec_post_notifiers;

	if (panic_on_warn) {
		/*
		 * This thread may hit another WARN() in the panic path.
		 * Resetting this prevents additional WARN() from panicking the
		 * system on this thread.  Other threads are blocked by the
		 * panic_mutex in panic().
		 */
		panic_on_warn = 0;
	}

	/*
	 * Disable local interrupts. This will prevent panic_smp_self_stop
	 * from deadlocking the first cpu that invokes the panic, since
	 * there is nothing to prevent an interrupt handler (that runs
	 * after setting panic_cpu) from invoking panic() again.
	 */
	local_irq_disable();
	preempt_disable_notrace();

	/*
	 * It's possible to come here directly from a panic-assertion and
	 * not have preempt disabled. Some functions called from here want
	 * preempt to be disabled. No point enabling it later though...
	 *
	 * Only one CPU is allowed to execute the panic code from here. For
	 * multiple parallel invocations of panic, all other CPUs either
	 * stop themself or will wait until they are stopped by the 1st CPU
	 * with smp_send_stop().
	 *
	 * `old_cpu == PANIC_CPU_INVALID' means this is the 1st CPU which
	 * comes here, so go ahead.
	 * `old_cpu == this_cpu' means we came from nmi_panic() which sets
	 * panic_cpu to this CPU.  In this case, this is also the 1st CPU.
	 */
	this_cpu = raw_smp_processor_id();
	old_cpu  = atomic_cmpxchg(&panic_cpu, PANIC_CPU_INVALID, this_cpu);

	if (old_cpu != PANIC_CPU_INVALID && old_cpu != this_cpu)
		panic_smp_self_stop();

	console_verbose();
	bust_spinlocks(1);
	va_start(args, fmt);
	len = vscnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	if (len && buf[len - 1] == '\n')
		buf[len - 1] = '\0';

	pr_emerg("Kernel panic - not syncing: %s\n", buf);
#ifdef CONFIG_DEBUG_BUGVERBOSE
	/*
	 * Avoid nested stack-dumping if a panic occurs during oops processing
	 */
	if (!test_taint(TAINT_DIE) && oops_in_progress <= 1)
		dump_stack();
#endif

	/*
	 * If kgdb is enabled, give it a chance to run before we stop all
	 * the other CPUs or else we won't be able to debug processes left
	 * running on them.
	 */
	kgdb_panic(buf);

	/* Run any panic handlers before cpu shutdown. */
	atomic_notifier_call_chain(&panic_early_notifier_list, 0, buf);

	/*
	 * If we have crashed and we have a crash kernel loaded let it handle
	 * everything else.
	 * If we want to run this after calling panic_notifiers, pass
	 * the "crash_kexec_post_notifiers" option to the kernel.
	 *
	 * Bypass the panic_cpu check and call __crash_kexec directly.
	 */
	if (!_crash_kexec_post_notifiers)
		__crash_kexec(NULL);

	panic_other_cpus_shutdown(_crash_kexec_post_notifiers);

	/*
	 * Run any panic handlers, including those that might need to
	 * add information to the kmsg dump output.
	 */
	atomic_notifier_call_chain(&panic_notifier_list, 0, buf);

	panic_print_sys_info(false);

	kmsg_dump(KMSG_DUMP_PANIC);

	/*
	 * If you doubt kdump always works fine in any situation,
	 * "crash_kexec_post_notifiers" offers you a chance to run
	 * panic_notifiers and dumping kmsg before kdump.
	 * Note: since some panic_notifiers can make crashed kernel
	 * more unstable, it can increase risks of the kdump failure too.
	 *
	 * Bypass the panic_cpu check and call __crash_kexec directly.
	 */
	if (_crash_kexec_post_notifiers)
		__crash_kexec(NULL);

	console_unblank();

	/*
	 * We may have ended up stopping the CPU holding the lock (in
	 * smp_send_stop()) while still having some valuable data in the console
	 * buffer.  Try to acquire the lock then release it regardless of the
	 * result.  The release will also print the buffers out.  Locks debug
	 * should be disabled to avoid reporting bad unlock balance when
	 * panic() is not being callled from OOPS.
	 */
	debug_locks_off();
	console_flush_on_panic(CONSOLE_FLUSH_PENDING);

	panic_print_sys_info(true);

	if (!panic_blink)
		panic_blink = no_blink;

	if (panic_timeout > 0) {
		/*
		 * Delay timeout seconds before rebooting the machine.
		 * We can't use the "normal" timers since we just panicked.
		 */
		pr_emerg("Rebooting in %d seconds..\n", panic_timeout);

		for (i = 0; i < panic_timeout * 1000; i += PANIC_TIMER_STEP) {
			touch_nmi_watchdog();
			if (i >= i_next) {
				i += panic_blink(state ^= 1);
				i_next = i + 3600 / PANIC_BLINK_SPD;
			}
			mdelay(PANIC_TIMER_STEP);
		}
	}
	if (panic_timeout != 0) {
		/*
		 * This will not be a clean reboot, with everything
		 * shutting down.  But if there is a chance of
		 * rebooting the system it will be rebooted.
		 */
		if (panic_reboot_mode != REBOOT_UNDEFINED)
			reboot_mode = panic_reboot_mode;
		emergency_restart();
	}
#ifdef __sparc__
	{
		extern int stop_a_enabled;
		/* Make sure the user can actually press Stop-A (L1-A) */
		stop_a_enabled = 1;
		pr_emerg("Press Stop-A (L1-A) from sun keyboard or send break\n"
			 "twice on console to return to the boot prom\n");
	}
#endif
#if defined(CONFIG_S390)
	disabled_wait();
#endif
	pr_emerg("---[ end Kernel panic - not syncing: %s ]---\n", buf);

	/* Do not scroll important messages printed above */
	suppress_printk = 1;

	/*
	 * The final messages may not have been printed if in a context that
	 * defers printing (such as NMI) and irq_work is not available.
	 * Explicitly flush the kernel log buffer one last time.
	 */
	console_flush_on_panic(CONSOLE_FLUSH_PENDING);

	local_irq_enable();
	for (i = 0; ; i += PANIC_TIMER_STEP) {
		touch_softlockup_watchdog();
		if (i >= i_next) {
			i += panic_blink(state ^= 1);
			i_next = i + 3600 / PANIC_BLINK_SPD;
		}
		mdelay(PANIC_TIMER_STEP);
	}
}

```


### **1. 触发 Panic：调用 `panic()` 函数**

当内核检测到无法恢复的致命错误（如空指针解引用、死锁、硬件异常等），会调用 `panic(const char *fmt, ...)`（定义于 `kernel/panic.c`）：

```c
void panic(const char *fmt, ...)
{
    // ... 格式化错误信息到 buf

    pr_emerg("Kernel panic - not syncing: %s\n", buf);

    // 停止所有 CPU（SMP）
    smp_send_stop();

    // 执行崩溃转储（如果配置了 kdump）
    crash_kexec(regs);

    // 调用通知链（如 pstore、ramoops）
    kmsg_dump(KMSG_DUMP_PANIC);

    // 调用 panic_notifier_list 回调
    atomic_notifier_call_chain(&panic_notifier_list, 0, buf);

    // 如果启用了控制台锁，则释放它以确保日志输出
    console_flush_on_panic();

    // 根据 panic_timeout 决定后续行为
    if (panic_timeout > 0) {
        // 启动自动重启定时器
        schedule_timeout_uninterruptible(panic_timeout * HZ);
        emergency_restart(); // 强制重启
    }

    // 默认：无限循环挂起
    for (;;)
        cpu_relax();
}
```

---

### **2. 关键步骤详解**

#### **(1) 禁用中断并加锁**
- 在进入 `panic()` 之前，触发点通常已禁用本地中断（如通过 `die()` 或 `__do_kernel_fault()`）。
- `panic()` 自身不显式加锁，但通过 `smp_send_stop()` 确保其他 CPU 停止运行，避免并发问题。

#### **(2) 停止其他 CPU：`smp_send_stop()`**
- ARM64 实现位于 `arch/arm64/kernel/smp.c`：
  ```c
  void smp_send_stop(void)
  {
      // 发送核间中断（IPI）给所有在线 CPU
      smp_cross_call(cpumask_of_each_possible_cpu(), IPI_CPU_STOP);
  }
  ```
- 其他 CPU 收到 `IPI_CPU_STOP` 后执行 `ipi_cpu_stop()`，进入忙等待或 WFI 状态。

#### **(3) 触发 Kdump：`crash_kexec()`**
- 若系统配置了 `kdump`（通过 `kexec_load()` 预加载 crash kernel），`crash_kexec()` 会跳转到备用内核。
- ARM64 的实现依赖 `machine_kexec()`（`arch/arm64/kernel/machine_kexec.c`），保存寄存器状态后跳转。

#### **(4) 日志转储：`kmsg_dump()`**
- 调用所有注册的 `kmsg_dump` 处理程序（如 `pstore`、`ramoops`），将最后的日志写入持久存储。

#### **(5) 自动重启或挂起**
- `panic_timeout` 由内核参数 `panic=` 设置（默认为 0）。
    - 若 `> 0`：等待指定秒数后调用 `emergency_restart()`。
    - ARM64 的 `emergency_restart()`（`arch/arm64/kernel/process.c`）：
      ```c
      void machine_restart(char *cmd)
      {
          local_irq_disable(); // 再次确保中断关闭
          smp_send_stop();     // 确认其他 CPU 已停
          // 调用平台特定的重启函数（如 EFI 或 PSCI）
          if (efi_enabled(EFI_RUNTIME))
              efi_reboot(reboot_mode, cmd);
          else
              __psci_sys_reset(); // 使用 ARM PSCI 接口重启
      }
      ```
    - 若 `= 0`：进入无限循环（`cpu_relax()`），系统挂起。

---

### **3. ARM64 架构特殊处理**

- **异常向量表**：早期 panic（如 MMU 启用前）由 `head.S` 中的异常向量处理，直接打印寄存器并死循环。
- **PSCI 支持**：现代 ARM64 平台通过 PSCI（Power State Coordination Interface）实现 `smp_send_stop` 和 `machine_restart`。
- **EFI 运行时服务**：若启用 EFI，重启可能通过 `efi_reboot()` 完成。

---

### **总结**

Linux 6.18 在 ARM64 下的 panic 流程严格遵循“稳定现场 → 输出诊断 → 转储内存 → 终止或重启”的策略。其代码结构清晰分离了通用逻辑（`panic.c`）和架构细节（`arch/arm64/`），确保在多核、复杂硬件环境下可靠执行崩溃处理。