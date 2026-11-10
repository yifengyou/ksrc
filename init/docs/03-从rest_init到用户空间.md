# 从 rest_init 到用户空间：0/1/2 号进程的诞生

`start_kernel()` 的最后一行 `arch_call_rest_init()` 引出了启动下半场。从这里开始，"单线程的安装程序"蜕变为"多线程的操作系统"：pid 0 变成 idle、pid 1 走向用户态 init、pid 2 成为所有内核线程的母体，多核被逐一点亮，initcall 长跑，rootfs 落成，最终第一个用户程序被 exec。

### 🎯 本章主线

* `rest_init()`：三个进程的一次性分娩；
* idle 循环与 kthreadd 的运行模型；
* `kernel_init() -> kernel_init_freeable()`：smp_init、do_basic_setup、rootfs 决策全链；
* initramfs 的解包与 `switch_root`；
* `run_init_process() -> kernel_execve()`：内核态到用户态的最后一跃。

---

## 1. 总流程图

```c
start_kernel() 最后一行
└─ arch_call_rest_init() → rest_init()            /* init/main.c */
    ├─ rcu_scheduler_starting()      /* RCU 宽限期机制正式启动 */
    ├─ user_mode_thread(kernel_init) /* pid 1：未来 exec 用户 init */
    ├─ [pin init 到 boot CPU, PF_NO_SETAFFINITY]
    ├─ numa_default_policy()
    ├─ kernel_thread(kthreadd)       /* pid 2：内核线程总母体 */
    ├─ system_state = SYSTEM_SCHEDULING
    ├─ complete(&kthreadd_done)      /* 放行 kernel_init */
    ├─ schedule_preempt_disabled()   /* 0 号进程让出 CPU 一次 */
    └─ cpu_startup_entry(CPUHP_ONLINE)  /* pid 0 → do_idle() 永不返回 */

kernel_init(pid 1)                        /* init/main.c */
└─ wait_for_completion(kthreadd_done)
   → kernel_init_freeable()
      ├─ smp_prepare_cpus()
      ├─ workqueue_init() / init_mm_internals()
      ├─ do_pre_smp_initcalls() / lockup_detector_init()
      ├─ smp_init()        /* ★ 点亮 secondary CPU */
      ├─ sched_init_smp()
      ├─ workqueue_init_topology() / padata_init() / page_alloc_init_late()
      ├─ do_basic_setup()  /* ★ driver_init + do_initcalls(第4章) */
      ├─ wait_for_initramfs() / console_on_rootfs()
      ├─ [无 /init] prepare_namespace()   /* 真实根设备路径 */
      └─ integrity_load_keys()
   → (回到 kernel_init 非 __init 部分)
      async_synchronize_full()
      SYSTEM_FREEING_INITMEM → free_initmem() → mark_readonly()
      SYSTEM_RUNNING → rcu_end_inkernel_boot() → do_sysctl_args()
      → run_init_process():
          /init (ramdisk_execute_command)
          init= 参数指定者 → CONFIG_DEFAULT_INIT
          /sbin/init → /etc/init → /bin/init → /bin/sh
      → 全部失败: panic("No working init found...")
```

---

## 2. rest_init() 深挖

```c
/* init/main.c（节选，v6.6） */
static __initdata DECLARE_COMPLETION(kthreadd_done);

noinline void __ref __noreturn rest_init(void)
{
	struct task_struct *tsk;
	int pid;

	rcu_scheduler_starting();
	/*
	 * We need to spawn init first so that it obtains pid 1, however
	 * the init task will end up wanting to create kthreads, which, if
	 * we schedule it before we create kthreadd, will OOPS.
	 */
	pid = user_mode_thread(kernel_init, NULL, CLONE_FS);
	...
	numa_default_policy();
	pid = kernel_thread(kthreadd, NULL, NULL, CLONE_FS | CLONE_FILES);
	...
	system_state = SYSTEM_SCHEDULING;

	complete(&kthreadd_done);

	/* The boot idle thread must execute schedule() at least once ... */
	schedule_preempt_disabled();
	/* Call into cpu_idle with preempt disabled */
	cpu_startup_entry(CPUHP_ONLINE);
}
```

四个深挖点：

* **为什么 init 必须先创建、却必须后运行**：注释说得很直白——init 要抢 pid 1，但若它先跑起来去创建内核线程而 kthreadd 尚未出生，就会 OOPS。解法：init 创建后立刻被 `wait_for_completion(&kthreadd_done)` 卡住（`kernel_init()` 第一行），等 kthreadd 就位、`complete()` 发车后才开始真正干活。
* **`user_mode_thread()` vs `kernel_thread()`**（`kernel/fork.c`）：两者最终都进 `kernel_clone()`，唯一差别是 `kernel_thread()` 置 `.kthread = 1`（子任务带 `PF_KTHREAD`）。**kernel_init 不能带 PF_KTHREAD**——它将来要 `kernel_execve()` 变成货真价实的用户进程（systemd），kthread 标志会污染这条路径；而 kthreadd 及其所有后代都是纯内核线程。
* **`__ref` 与 `noinline` 的苦衷**：`rest_init` 本体不是 `__init`，因为它要在 `free_initmem()` 之后仍活着（idle 循环）；`__ref` 告诉 modpost "我引用 .init.text 的 kernel_init 是合法的"。`noinline` 防止 gcc 把它内联进 `start_kernel()`（老编译器 bug，注释保留至今）——若被内联，`free_initmem` 可能在 boot 线程进入 idle 之前就回收了正在执行的代码。
* **`schedule_preempt_disabled()`**：boot CPU 此前从未调度过。这一次 `schedule()` 让 1/2 号进程真正上 CPU 跑起来，之后控制权再回到 0 号进程时，它就以 idle 身份进入 `cpu_startup_entry()`——从此这个上下文永远不再返回 `rest_init`。

---

## 3. pid 0：idle 任务的一生

`cpu_startup_entry()`（`kernel/sched/idle.c`）在当前 CPU 的状态机上循环，最终落入 `do_idle()`：

```c
/* kernel/sched/idle.c: do_idle()（节选） */
	while (1) {
		...
		tick_nohz_idle_enter();        /* nohz：停 tick（若配置） */
		while (!need_resched()) {
			local_irq_disable();
			if (cpu_is_offline(cpu))    /* 热插拔下线路径 */
				arch_cpu_idle_dead();

			arch_cpu_idle_enter();
			rcu_nocb_flush_deferred_wakeup();

			if (cpu_idle_force_poll || tick_check_broadcast_expired()) {
				tick_nohz_idle_restart_tick();
				cpu_idle_poll();  /* 轮询：ct_cpuidle_enter() 后
				                     cpu_relax() 空转再退出 */
			} else {
				cpuidle_idle_call();
				/* cpuidle_idle_call() → 挑一个 idle state（menu/ladder）
				   → cpuidle_enter() → 默认 default_idle_call()
				   → ct_idle_enter() → arch_cpu_idle()：
				       x86: mwait/poll（safe_halt）
				       arm64: wfi（等待中断）
				   → ct_idle_exit() */
			}
			arch_cpu_idle_exit();
		}
		tick_nohz_idle_exit();
		...
		flush_smp_call_function_queue();  /* 处理 idle 期间积压的 IPI */
		schedule_idle();               /* 有任务就绪 → 让出 */
	}
```

深挖要点：

* **"每 CPU 一个 idle"**：boot CPU 的 idle 是 `init_task`（pid 0）本体；其余 CPU 的 idle 线程由 `smp_init() -> idle_threads_init()` 提前为每个 possible CPU fork 出来（`fork_idle()`），等待 secondary CPU 入场时当作初始 current——这就是"每 CPU 的 idle thread"。idle 任务永不退出、不可被换出、`PF_IDLE` 标记让调度器对它特殊处理。
* **idle 即 RCU 静默期（QS）**：进入实际空闲前，`cpu_idle_poll()`/`cpuidle_idle_call()` 会经 `ct_cpuidle_enter()`/`ct_idle_enter()`（`kernel/context_tracking.c`，上下文追踪层）声明本 CPU 进入扩展静默区（RCU dyntick-idle），宽限期的推进很大程度依赖这些报告；醒来后对应的 `*_exit()` 恢复计数。这也是 `need_resched()` 检查必须高频出现的原因——QS 报告不能让 RCU 读者饿死。
* **轮询 vs 深睡的权衡**：`cpu_idle_force_poll`（`cpuidle.poll=1`）或 nohz 停表时用 `cpu_idle_poll()` 保延迟；正常路径经 cpuidle 框架按预测空闲时长选 C-state。x86 `arch_cpu_idle()` 最终落到 `mwait`（带 hint 的轻睡眠）或 `hlt`；arm64 是 `wfi`。
* idle 中醒来的原因只有两个：**中断**（包括定时器）或 IPI 唤醒任务（`need_resched`）。

---

## 4. pid 2：kthreadd 与内核线程的生产线

```c
/* kernel/kthread.c（节选） */
int kthreadd(void *unused)
{
	struct task_struct *tsk = current;

	/* Setup a clean context for our children to inherit. */
	set_task_comm(tsk, "kthreadd");
	ignore_signals(tsk);
	set_cpus_allowed_ptr(tsk, housekeeping_cpumask(HK_TYPE_KTHREAD));
	set_mems_allowed(node_states[N_MEMORY]);

	current->flags |= PF_NOFREEZE;      /* 不参与 freeze */
	cgroup_init_kthreadd();

	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (list_empty(&kthread_create_list))
			schedule();            /* 没活干就睡 */
		__set_current_state(TASK_RUNNING);

		spin_lock(&kthread_create_lock);
		while (!list_empty(&kthread_create_list)) {
			struct kthread_create_info *create;
			create = list_first_entry(&kthread_create_list, ...);
			list_del_init(&create->list);
			spin_unlock(&kthread_create_lock);
			create_kthread(create);  /* kernel_thread(kthread) */
			spin_lock(&kthread_create_lock);
		}
		spin_unlock(&kthread_create_lock);
	}
}
```

内核线程不许"私自 fork"：任何代码调用 `kthread_create()`（或 `kthread_run()`）只是把请求挂上 `kthread_create_list` 并唤醒 kthreadd；由它统一 `create_kthread() -> kernel_thread(kthread)` 诞生新线程——这保证了新内核线程拥有一致干净的信号/亲和性/cgroup 初始上下文（都继承自 kthreadd 的精心布置）。在 `ps` 里所有 `[xxx]` 内核线程的 ppid 都是 2，原因即在此。

---

## 5. kernel_init()：启动下半场的总指挥

### 5.1 kernel_init_freeable() 逐段精读（`__init` 部分）

```c
/* init/main.c: kernel_init_freeable()（节选，顺序保持原样） */
	/* Now the scheduler is fully set up and can do blocking allocations */
	gfp_allowed_mask = __GFP_BITS_MASK;   /* 解除 GFP 约束（早期禁
	                                       __GFP_RECLAIM 以防休眠） */
	set_mems_allowed(node_states[N_MEMORY]);

	cad_pid = get_pid(task_pid(current)); /* Ctrl-Alt-Del 接收者 = pid 1 */

	smp_prepare_cpus(setup_max_cpus);     /* 架构准备：x86 侧预计算拓扑、
	                                       arm64 侧设置 secondary 入口 */

	workqueue_init();                     /* ★ 正式创建 worker pool 的
	                                       kthread 并唤醒（此时 kthreadd 已就绪） */
	init_mm_internals();                  /* mm 统计/proc 接口 */
	rcu_init_tasks_generic();             /* RCU tasks（文件级宽限期） */
	do_pre_smp_initcalls();               /* early 级 initcall
	                                       （__initcall_start..__initcall0_start）*/
	lockup_detector_init();               /* NMI watchdog（hardlockup） */

	smp_init();                           /* ★ 点亮多核，见下节 */
	sched_init_smp();                     /* 调度域/拓扑正式构建 */

	workqueue_init_topology();            /* unbound pool 按新拓扑重算 cpumask */
	padata_init();
	page_alloc_init_late();               /* 大页/延迟初始化收尾 */

	do_basic_setup();                     /* ★ initcall 大长征（第 4 章） */

	kunit_run_all_tests();
	wait_for_initramfs();                 /* 等待异步 initramfs 解包完成 */
	console_on_rootfs();                  /* open /dev/console → 复制为
	                                       fd 0/1/2（init 的标准三件套） */

	/* check if there is an early userspace init.  If yes, let it do all
	 * the work */
	if (init_eaccess(ramdisk_execute_command) != 0) {
		ramdisk_execute_command = NULL;
		prepare_namespace();          /* 没有 /init → 真实根设备路径 */
	}

	integrity_load_keys();                /* 加载平台密钥到 keyring */
```

`do_basic_setup()` 本体（v6.6）：

```c
static void __init do_basic_setup(void)
{
	cpuset_init_smp();
	driver_init();       /* drivers/base/init.c：设备模型地基——
	                        /sys、bus/class/设备体系、devtmpfs 等 */
	init_irq_proc();     /* /proc/irq */
	do_ctors();          /* 运行 C++ 风格全局构造器（.ctors 段） */
	do_initcalls();      /* 全部 0..7 级 initcall（第 4 章专题） */
}
```

> 常见误解澄清：老内核 `do_basic_setup` 里的 `init_tmpfs` 已不存在——tmpfs 初始化如今走 `rootfs_initcall`；`usermodehelper_enable()` 也不再在这里，而是移到了 `populate_rootfs()`（见第 7 节）。

### 5.2 smp_init()：secondary CPU 拉起

```c
/* kernel/smp.c */
void __init smp_init(void)
{
	idle_threads_init();      /* 为每个 possible CPU fork idle 线程 */
	cpuhp_threads_init();     /* 创建 cpuhp/0..N 线程：状态机执行者 */

	pr_info("Bringing up secondary CPUs ...\n");
	bringup_nonboot_cpus(setup_max_cpus);   /* → cpuhp_bringup_mask
	                                           （支持 parallel 优化路径） */
	num_cpus = num_online_cpus();
	pr_info("Brought up %d node%s, %d CPU%s\n", ...);
	smp_cpus_done(setup_max_cpus);
}
```

**secondary CPU 的两条架构路线**（都被 `__cpu_up(cpu, idle)` 统一抽象）：

* **x86**（`arch/x86/kernel/smpboot.c`）：`native_cpu_up() -> common_cpu_up()`（把该 CPU 的 idle 线程栈地址写入其 `initial_gs`/`current_task`）`-> do_boot_cpu()`：设 `initial_code = (unsigned long)start_secondary`，构造 real-mode trampoline，向 AP 发 INIT-SIPI-SIPI。AP 从实模式蹦床苏醒 → `secondary_startup_64`（head_64.S，与 boot CPU 共享后半段）→ `start_secondary()` → `cpu_startup_entry(CPUHP_AP_ONLINE_IDLE)`。
* **arm64**（`arch/arm64/kernel/smp.c` + `cpu_ops`）：`__cpu_up()` 经 `cpu_ops[cpu]->cpu_boot(cpu)` 走两条路：`spin-table`（`arch/arm64/kernel/smp_spin_table.c`：prepare 时把 `__pa(secondary_holding_pen)` 写入 DTB 的 `cpu-release-addr`，boot 时置 pen_release 并 `sev` 唤醒——AP 在 holding pen 自旋等到自己的 hwid 才放行）或 **PSCI**（SMC `CPU_ON`，入口 `secondary_entry`）。AP 走 `secondary_entry → secondary_startup → __secondary_switched → secondary_start_kernel() → cpu_startup_entry(...)`。

**与 cpuhp 状态机的关联**：AP 侧 `notify_cpu_starting()` 从 `CPUHP_AP_OFFLINE` 起逐级回调至 `CPUHP_AP_ONLINE`；BSP 侧 `_cpu_up()` 经 `cpuhp_threads` 线程把状态推到 `CPUHP_ONLINE`。启动拉起与日后热插拔复用同一台状态机（`kernel/cpu.c`），这正是 `boot_cpu_hotplug_init()` 提前把 boot CPU 置为 `CPUHP_ONLINE` 的原因——全员走同一条路。

---

## 6. kernel_init()：非 `__init` 部分（initcall 之后）

```c
/* init/main.c: kernel_init()（节选） */
	kernel_init_freeable();
	/* need to finish all async __init code before freeing the memory */
	async_synchronize_full();

	system_state = SYSTEM_FREEING_INITMEM;
	kprobe_free_init_mem();
	ftrace_free_init_mem();
	kgdb_free_init_mem();
	exit_boot_config();
	free_initmem();          /* ★ 释放 .init.text/.init.data 等（第 4 章） */
	mark_readonly();         /* 内核 text/rodata 只读化（CONFIG_STRICT_KERNEL_RWX） */
	pti_finalize();          /* KPTI 用户页表定稿 */

	system_state = SYSTEM_RUNNING;
	numa_default_policy();
	rcu_end_inkernel_boot(); /* RCU 退出"内核启动期"宽松模式 */
	do_sysctl_args();        /* 处理 sysctl.= 命令行参数 */

	/* ---- 下面开始寻找 init ---- */
```

注意顺序语义：`free_initmem()` 必须等 `async_synchronize_full()` 收齐所有异步 initcall；`SYSTEM_RUNNING` 在 initmem 释放**之后**才置位——从此 `MODULE` 加载、热插拔等一切"运行时"行为合法化。

---

## 7. rootfs 决策：initramfs、initrd 与 prepare_namespace

### 7.1 populate_rootfs：rootfs 的填充

rootfs（rootfs_fs_type，ramfs/tmpfs 实现）在 `vfs_caches_init()` 时挂为 `/`，但内容为空。填充发生在 `rootfs_initcall` 级的 `populate_rootfs()`（`init/initramfs.c`）：

```c
/* init/initramfs.c（节选） */
static int __init populate_rootfs(void)
{
	initramfs_cookie = async_schedule_domain(do_populate_rootfs, NULL,
						 &initramfs_domain);  /* 异步解包！ */
	usermodehelper_enable();               /* umh 从此刻起放行 */
	if (!initramfs_async)
		wait_for_initramfs();          /* 默认：同步等完 */
	return 0;
}
rootfs_initcall(populate_rootfs);
```

`do_populate_rootfs()` 的顺序：

1. `unpack_to_rootfs(__initramfs_start, __initramfs_size)`——**内建 initramfs**（构建时 `usr/` 目录打包进内核镜像的 cpio，通常只有基本 /dev 节点）；
2. 若外部 initrd 存在（`initrd_start`）且未设 `CONFIG_INITRAMFS_FORCE`：再次 `unpack_to_rootfs(initrd_start, size)`——把 bootloader 传入的 cpio 解开到 rootfs；
3. 解包失败且配了 `CONFIG_BLK_DEV_RAM`：`populate_initrd_image()` 退路——把 initrd 当**块设备 initrd**（/dev/ram0）处理，走 7.3 的老路径；
4. 成功后 `free_initrd_mem()` 归还 initrd 占用的物理页（`retain_initrd` 可保留调试）。

`unpack_to_rootfs()` 是一个手写的流式 cpio（newc 格式）解释器，边解边创建文件/目录/设备节点/符号链接，可选 gzip/xz/lz4/zstd 解压。解包是**异步**的（`async_schedule_domain`），为的是让后续 device 级 initcall 与解包并行；但任何想访问 rootfs 的代码（含挂真实根设备前）必须先 `wait_for_initramfs()`——kernel_init_freeable 里那次调用就是全系统的汇合点。

### 7.2 没有 initramfs 时：prepare_namespace()

`kernel_init_freeable` 末尾用 `init_eaccess(ramdisk_execute_command)` 探测 `/init` 是否可执行：不可用则清空它并走**传统根挂载**：

```c
/* init/do_mounts.c: prepare_namespace()（节选） */
	if (root_delay) { ...ssleep... }        /* rootdelay= 参数 */

	/* wait for the known devices to complete their probing
	 * Note: this is a potential source of long boot delays. */
	wait_for_device_probe();                 /* ★ 常见开机慢点 */

	md_run_setup();                          /* md= 软阵列 */

	if (saved_root_name[0])
		ROOT_DEV = parse_root_device(saved_root_name);  /* root= 参数
	                                                   → dev_t（含 PARTUUID） */

	if (initrd_load(saved_root_name))        /* 见 7.3 */
		goto out;

	if (root_wait)
		wait_for_root(saved_root_name); /* rootwait：等设备出现 */

	mount_root(saved_root_name);             /* 挂 root= 设备到 /root */
out:
	devtmpfs_mount();                        /* /dev 挂 devtmpfs */
	init_mount(".", "/", NULL, MS_MOVE, NULL);  /* 把新根 move 到 / */
	init_chroot(".");
```

`mount_root()` 内部还兼容 NFS/CIFS 网络根（`root=/dev/nfs`）与多文件系统探测（`rootfstype=`）。

### 7.3 initrd（块设备形态）与 handle_initrd

`initrd_load()`（`init/do_mounts_initrd.c`）：`create_dev("/dev/ram", Root_RAM0)` → `rd_load_image("/initrd.image")` 把 initrd 拷进 ram0；若 root 不是 ram0，则 `handle_initrd()`——**老式两段式 initrd**（内核会打 warn："using deprecated initrd support"）：先把 ram0 挂到 rootfs 的 `/root`，经 `call_usermodehelper_exec()` 运行 initrd 里的 `/linuxrc`（UMH_WAIT_PROC 等其完成，用户态可用 `/proc/sys/kernel/real-root-dev` 回写真实根设备号），然后把 initrd 挪去 `/old`，最后挂真实根。现代 initramfs（cpio 模式）已基本淘汰这条路；`CONFIG_BLK_DEV_INITRD` 关闭的内核则走 `init/noinitramfs.c` 的 `default_rootfs()`（rootfs_initcall，仅创建 /dev、/dev/console、/root 并 `usermodehelper_enable()`）。`rdinit=` 参数只改 cpio 路线里 initramfs 内 `/init` 的名字（默认 "/init"，见 init/main.c）。

### 7.4 switch_root：initramfs 里的"偷梁换柱"

initramfs 方案中 `/init`（通常为 shell/systemd）接手后需要切换到真实根：

```text
用户态 switch_root 做的事（util-linux / systemd / busybox 各有实现）：
 1. mount --move /sys   /newroot/sys    （内核 MS_MOVE，搬挂载树）
 2. mount --move /proc  /newroot/proc
 3. mount --move /dev   /newroot/dev
 4. chroot /newroot && chdir /
    （PivotRoot 或 MS_MOVE+chroot 等价组合）
 5. exec /sbin/init      （原 pid 1 上下文内直接替换映像）
```

关键认知：**pid 1 没有换人，只是换了身体**——`switch_root` 全程是同一进程（pid 1）的 `mount(2)`/`chroot(2)`/`execve(2)` 调用序列；内核不需要任何"switch_root 系统调用"。`prepare_namespace()` 里 `init_mount(".", "/", MS_MOVE)` + `init_chroot(".")` 就是内核替你做的同一套动作。

---

## 8. run_init_process()：最后一跃

```c
/* init/main.c */
static int run_init_process(const char *init_filename)
{
	argv_init[0] = init_filename;
	pr_info("Run %s as init process\n", init_filename);
	return kernel_execve(init_filename, argv_init, envp_init);
}
```

尝试顺序（`kernel_init()`）：

| 序 | 候选 | 来源 | 失败处理 |
| :--- | :--- | :--- | :--- |
| 1 | `ramdisk_execute_command`（默认 `/init`） | `rdinit=` 可改 | 仅换下一个候选继续 |
| 2 | `execute_command` | `init=` 参数 | **直接 panic**（用户明确指定却失败） |
| 3 | `CONFIG_DEFAULT_INIT` | 编译期默认（如 /sbin/init） | 换下一个 |
| 4 | `/sbin/init`、`/etc/init`、`/bin/init` | 硬编码序列 | 逐个尝试（ENOENT 静默） |
| 5 | `/bin/sh` | 应急 shell | 逐个尝试 |
| — | 全部失败 | — | `panic("No working init found. Try passing init= ...")` |

**`kernel_execve()` 一跃的内核路径**（简述）：pid 1 在内核上下文里直接调用 `kernel_execve()` → `bprm_execve()` → 打开文件、分配 `linux_binprm` → `search_binary_handler()` 识别 ELF → `load_elf_binary()`：建立新 mm、映射各段、设 `elf_entry` → `start_thread()` 改写 pt_regs → 返回用户态路径上自然"返回"到 ELF 入口。**没有 ret_to_user 的异常帧**——kernel_init 从未进过用户态，这是借内核线程的壳直接降生用户进程的独门技巧。此后 pid 1 的所有新行为都来自用户映像（systemd）。

---

## 9. 三个进程的角色定格

| | pid 0 (swapper/0) | pid 1 (init) | pid 2 (kthreadd) |
| :--- | :--- | :--- | :--- |
| 诞生方式 | 静态 `init_task`（链接期） | `rest_init: user_mode_thread(kernel_init)` | `rest_init: kernel_thread(kthreadd)` |
| 标志 | `PF_IDLE`，非 kthread | 初始非 kthread（可 exec） | `PF_KTHREAD`、`PF_NOFREEZE` |
| 身份演变 | boot CPU idle；其余 CPU idle 由它 fork 衍生 | `kernel_execve` 成 systemd（用户态） | 永驻内核的线程工厂 |
| 生命周期 | 到关机 | 到关机（退出会 panic） | 到关机 |
| 关键职责 | 空转省电、报告 RCU QS、flush IPI | initcall 长跑（初期）、孤儿进程收养、服务管理 | 消费 `kthread_create_list`，统一 fork 内核线程 |
| 死亡后果 | 不可能（不可调度出去后退出） | `panic("Attempted to kill init!")` | 子线程生成失败 |

**用户态视角（systemd 与内核的握手）**：init 接手后，内核与用户空间的日常通道只剩系统调用与驱动接口——`fork/clone` 走 `kernel_clone()`（kernel/fork.c）生成新任务，`execve` 走 `sys_execve -> bprm_execve`（fs/exec.c）替换映像，`mount`/`openat`/`ioctl` 构建起用户可见的世界。第 1 章 bootloader 塞进 boot_params/DTB 的信息，此时多以 `/sys/firmware/*`、`/proc/cmdline` 的形式被 systemd 重新读回。

---

## 10. 观测与调试

* **谁在拖慢启动**：`initcall_debug` 打点每一级 initcall 耗时（第 4 章实操）；`prepare_namespace` 前的 `wait_for_device_probe()` 是注释点名的重灾区（`rootdelay=` 可复现）。
* **多核拉起**：`dmesg | grep -i "smpboot\|CPU"`；x86 侧 `smpboot: CPU N: ...`、arm64 侧 `CPUx: Booted secondary processor`。
* **initramfs 体检**：`lsinitrd`、`initramfs_async` 参数（解包与 initcall 并行）；解包失败会 `Initramfs unpacking failed:` 直白报错。
* **init 失败三板斧**：`init=/bin/sh`（跳过发行版 init）、`rdinit=/bin/sh`（卡在 initramfs 内）、`panic("No working init found")` 时读引导参数是否被 grub 截断。
* **验证三进程**：`ps -o pid,ppid,comm -p 1 2`；0 号不可见（idle 不在进程列表，`cat /proc/sched_debug` 可见 swapper/0）。

---

## 11. 🧠 思考题

1. **`rest_init()` 里若把 `complete(&kthreadd_done)` 挪到 `kernel_thread(kthreadd)` 之前，会发生什么？** 提示：init 抢跑后第一个想创建的内核线程是谁？`kthread_create()` 的排队对象存在吗？
2. **boot CPU 的 idle 是 `init_task` 本体，其余 CPU 的 idle 却是它 fork 出来的副本——为什么不能让所有 CPU 共用一个 idle 任务？** 提示：`current`、内核栈、`rq->idle` 各自的 per-CPU 语义。
3. **`kernel_init` 用 `user_mode_thread` 创建（无 PF_KTHREAD），但它在 exec 之前分明只跑内核代码——这个"假用户线程"的身份带来了哪些方便、埋了哪些坑？** 提示：`kernel_execve` 对 mm/pt_regs 的假设；以及谁在 exec 前调用了大量只允许 kernel thread 语境的 `__init` 代码。
4. **initramfs 解包为什么设计成异步（`async_schedule_domain`），又为什么必须提供 `wait_for_initramfs()` 汇合点？** 提示：rootfs_initcall 级并行收益 vs `init_eaccess(ramdisk_execute_command)` 的依赖。
5. **`prepare_namespace()` 与 initramfs 的 `/init` 是互斥的两条路。如果两者都被提供（initrd 里有 /init，命令行又有 root=），内核会怎么选？/init 里怎么"降级"走第二条路？** 提示：`init_eaccess` 探测失败才走 prepare_namespace；switch_root 的退路。
6. **为什么 `system_state` 要先经历 `SYSTEM_FREEING_INITMEM` 再到 `SYSTEM_RUNNING`，而不是 free_initmem 前直接置 RUNNING？** 提示：谁在 initcall 后期仍可能访问 init 段（`__init` 调用者退出前）？modprobe 的早期加载约束。
