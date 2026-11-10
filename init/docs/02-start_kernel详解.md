# start_kernel 详解：C 世界的奠基仪式

`start_kernel()`（`init/main.c`）是所有架构殊途同归的汇合点。它从架构汇编代码手里接过一台"MMU 已开、中断全关、单 CPU、没有内存分配器、没有调度器"的裸机，用几百行顺序调用把它变成一个"有内存、有调度、有中断、有时钟、能打印、可再生长"的真正内核，最后把接力棒交给 `rest_init()`。

### 🎯 本章视角

* 按主题分阶段（阶段划分是本笔记的组织方式，源码中它们首尾相连）过一遍 `start_kernel()` 的全序列；
* 每个重点子系统（setup_arch / percpu / mm / sched / irq / time / rcu / printk）单独深挖；
* 附 `early_param`/`__setup` 宏的链接期展开机制与"关键全局状态标志"速查表。

---

## 1. 输入状态：start_kernel 看到的世界

| 维度 | 状态 | 由谁保证 |
| :--- | :--- | :--- |
| 地址空间 | MMU 开启，运行于内核虚拟地址，仅覆盖内核镜像 + 早期直接映射 | x86: `__startup_64()`；arm64: `__primary_switch()` |
| 中断 | **全部关闭**（`local_irq_disable` 即将执行；x86 侧从实模式起就 cli） | 解压器 / head.S |
| current | 永远是静态 `init_task`（pid 0，"swapper"） | 链接脚本里的 `init_thread_union` 栈 |
| 内存分配 | memblock 可用（x86 在 setup_arch 里灌入 E820；arm64 在 setup_arch 里灌入 DTB/EFI） | setup_arch() |
| CPU | 仅 boot CPU；`cpu_possible/present/online/active` 四张掩码即将初始化 | `boot_cpu_init()` |
| 输出 | earlycon（若配置）可用；printk 仅写缓冲区不输出 | early_param("earlycon") |

---

## 2. 全景：start_kernel 分阶段流程图

```c
start_kernel()                                        /* init/main.c */
│
├── ① 门槛与防御   set_task_stack_end_magic → smp_setup_processor_id
│                   → debug_objects_early_init → init_vmlinux_build_id
│                   → cgroup_init_early → local_irq_disable
│                   → early_boot_irqs_disabled = true
│                   → boot_cpu_init → page_address_init
│                   → pr_notice(linux_banner) → early_security_init
│
├── ② 架构早期     setup_arch(&command_line)      ★ x86/arm64 各一节深挖
│                   → setup_boot_config → setup_command_line
│                   → setup_nr_cpu_ids → setup_per_cpu_areas
│                   → smp_prepare_boot_cpu → boot_cpu_hotplug_init
│
├── ③ 参数解析     jump_label_init → parse_early_param     ★ early_param 专题
│                   → parse_args("Booting kernel", __start___param ...)
│                   → print_unknown_bootoptions → set_init_arg ...
│                   → random_init_early
│
├── ④ 内存子系统   setup_log_buf(0) → vfs_caches_init_early
│                   → sort_main_extable → trap_init
│                   → mm_core_init()             ★ memblock→buddy→slab 专题
│                   → poking_init → ftrace_init → early_trace_init
│
├── ⑤ 调度器       sched_init()                   ★ rq/调度类专题
│
├── ⑥ RCU/WQ/trace radix_tree_init → maple_tree_init → housekeeping_init
│                   → workqueue_init_early → rcu_init → trace_init
│                   → initcall_debug_enable → context_tracking_init
│
├── ⑦ 中断与时间   early_irq_init → init_IRQ      ★ irq_desc 专题
│                   → tick_init → rcu_init_nohz → init_timers → srcu_init
│                   → hrtimers_init → softirq_init → timekeeping_init
│                   → time_init                     ★ clocksource 专题
│                   → random_init → kfence_init → boot_init_stack_canary
│                   → perf_event_init → profile_init → call_function_init
│
├── ⑧ 开闸         early_boot_irqs_disabled = false → local_irq_enable()
│                   → kmem_cache_init_late
│                   → console_init()             ★ printk 演进专题
│                   → lockdep_init → locking_selftest
│
├── ⑨ 收尾长跑     setup_per_cpu_pageset → numa_policy_init → acpi_early_init
│                   → late_time_init → sched_clock_init → calibrate_delay
│                   → arch_cpu_finalize_init → pid_idr_init → anon_vma_init
│                   → cred_init → fork_init → proc_caches_init → uts_ns_init
│                   → key_init → security_init → net_ns_init
│                   → vfs_caches_init → pagecache_init → signals_init
│                   → proc_root_init → cpuset_init → cgroup_init
│                   → acpi_subsystem_init → arch_post_acpi_subsys_init
│                   → kcsan_init
│
└── arch_call_rest_init()   → rest_init()  （第 3 章）
```

下面按阶段展开，重点子系统用 ★ 标记并各设专节。

---

## 3. 阶段 ①：门槛与防御（几乎不消耗时间，但都是保命符）

```c
/* init/main.c（节选） */
	set_task_stack_end_magic(&init_task);  /* 栈底写入 STACK_END_MAGIC */
	smp_setup_processor_id();              /* 弱实现：记录/校验 boot CPU id */
	debug_objects_early_init();            /* debugobjects 的静态池先就绪 */
	init_vmlinux_build_id();               /* 供 dmesg 打印 build id */
	cgroup_init_early();                   /* cgroup 根层级/子系统骨架 */

	local_irq_disable();                   /* 从这里到阶段⑧，全局关中断 */
	early_boot_irqs_disabled = true;

	boot_cpu_init();                       /* cpu 0 置位 possible/present/
	                                             online/active 四张掩码 */
	page_address_init();                   /* 高端内存 hash（32 位遗产，64 位空） */
	pr_notice("%s", linux_banner);         /* "Linux version 6.6.x ..." */
	early_security_init();                 /* LSM 的 early 钩子 */
```

`boot_cpu_init()`（`kernel/cpu.c`）值得单独一提：它把 boot CPU 同时写进 `__cpu_possible_mask / __cpu_present_mask / __cpu_online_mask / __cpu_active_mask` 四张掩码并记录 `__boot_cpu_id`——这是 SMP 世界的第一块基石。

---

## 4. 阶段 ②-1：setup_arch() —— x86_64 版

`setup_arch()` 是 `start_kernel()` 中最大的一块架构代码（x86 约 450 行，`arch/x86/kernel/setup.c`），核心使命：**摸清内存、填好 memblock、建立正式直接映射**。删减后的主干：

```c
/* arch/x86/kernel/setup.c: setup_arch()（节选） */
	printk(KERN_INFO "Command line: %s\n", boot_command_line);

	idt_setup_early_traps();       /* #DB/#N 提前可处理 */
	early_cpu_init();              /* arch/x86/kernel/cpu/topology.c 等：
	                                 CPUID 识别、厂商/型号、cap 掩码 */
	jump_label_init();
	static_call_init();
	early_ioremap_init();          /* 早期 ioremap：fixmap 上的 slot 栈 */

	ROOT_DEV = old_decode_dev(boot_params.hdr.root_dev);
	...                            /* 从 zero page 摘行李：screen_info、
	                                 bootloader 版本、EFI 签名标志等 */
	x86_init.oem.arch_setup();

	early_reserve_memory();        /* 先于一切 memblock 分配：boot_params 本身、
	                                 setup_data、EFI 表等占用的区间 */
	e820__memory_setup();          /* 融合 bootloader E820 与固件表 → e820_table */
	parse_setup_data();            /* 遍历 setup_data 链表 */

	strscpy(command_line, boot_command_line, COMMAND_LINE_SIZE);
	*cmdline_p = command_line;     /* 命令行交给 start_kernel 保存 */
	x86_configure_nx();
	parse_early_param();           /* ！early_param 在这里第一次执行
	                                 （memmap=、efi=、noexec=...） */

	if (efi_enabled(EFI_BOOT)) {   /* EFI：内存图 reserve → 后续 efi_init() */
		efi_memblock_x86_reserve_range();
		efi_init();               /* 解析 EFI 配置表、SMBIOS、esrt... */
	}
	e820__reserve_setup_data();
	e820__finish_early_params();   /* 应用 memmap= 等对 E820 的修改 */

	dmi_setup();
	init_hypervisor_platform();    /* VMware/KVM/Xen 检测 */
	tsc_early_init();              /* 早期 TSC 校准，供 memblock 打印等计时 */

	max_pfn = e820__end_of_ram_pfn();          /* 物理内存上界 */
	find_smp_config();             /* 找 MP 表（老式 SMP 配置） */
	early_alloc_pgt_buf();         /* .brk 里预留页表缓冲 */
	reserve_brk();                 /* 封账：brk 区间 memblock_reserve */
	cleanup_highmap();             /* 清理 _end 之外的直接映射残留 */

	memblock_set_current_limit(ISA_END_ADDRESS);
	e820__memblock_setup();        /* ★ E820 → memblock.memory/reserved 的灌入点：
	                                 E820_TYPE_RAM → memblock_add()，
	                                 reserved 类型 → memblock_reserve() */

	...                            /* efi_fake_memmap / esrt / mptable 等 */
	x86_platform.realmode_reserve();  /* trampoline 分配在低 1M */
	init_mem_mapping();            /* ★ 直接映射区正式页表：从 0 到 max_pfn，
	                                 优先 1G 大页，不足用 2M；early ioremap
	                                 之后的 page table 也在此扩容 */
	idt_setup_early_pf();          /* #PF → do_early_exception 处理器挂上 */
	memblock_set_current_limit(get_max_mapped());

	setup_log_buf(1);              /* 用 memblock 把 printk 缓冲区扩容 */

	reserve_initrd();              /* initrd 区间 memblock_reserve */
	acpi_table_upgrade();
	acpi_boot_table_init();        /* 找 RSDT/XSDT、APIC 表 */
	early_acpi_boot_init();
	initmem_init();                /* NUMA/SRAT 或平坦模式 → 节点划分 */
	x86_init.paging.pagetable_init();  /* = paging_init()：zone 划分、
	                                     sparse vmemmap、fixmap 收尾 */
	zone_sizes_init();             /* 计算 DMA/DMA32/DMA32 之上各 zone 边界 */
	acpi_boot_init();              /* MADT：APIC/IOAPIC 拓扑 */
	e820__reserve_resources();     /* /proc/iomem 的资源树 */
	x86_init.timers.wallclock_init();   /* EFI/RTC/CMOS wall clock 选择 */
	...                            /* apic_intr_mode_init()、efi 虚拟模式
	                                 准备等收尾 */
```

**读法提示**：x86 setup_arch 的叙事线是"先预约（early_reserve_memory），再摸底（e820__memory_setup），再开账（e820__memblock_setup），再铺路（init_mem_mapping / paging_init）"。memblock 在"开账"之前任何分配都不可靠，这就是 `early_reserve_memory()` 必须先行的原因。

---

## 5. 阶段 ②-1：setup_arch() —— arm64 版

arm64 版（`arch/arm64/kernel/setup.c`）短得多，因为"摸内存"的活被 head.S 的早期页表和 DTB/EFI 结构化了：

```c
/* arch/arm64/kernel/setup.c: setup_arch()（节选） */
	setup_initial_init_mm(_stext, _etext, _edata, _end);
	*cmdline_p = boot_command_line;

	kaslr_init();                          /* 依据 kaslr-seed 决定后续策略 */
	arm64_use_ng_mappings = kaslr_requires_kpti();

	early_fixmap_init();                   /* fixmap PGD/PMD 就位 */
	early_ioremap_init();                  /* 同 x86：slot 化的早期映射 */

	setup_machine_fdt(__fdt_pointer);      /* ★ 校验 DTB，fdt 早期映射，
	                                         解析 /chosen、memory 节点
	                                         （memblock_add 到 memory 列表），
	                                         记录 boot_command_line */
	jump_label_init();
	parse_early_param();                   /* earlycon= 在此激活 */

	local_daif_restore(DAIF_PROCCTX_NOIRQ);/* 放开异步异常/fiq 报告 */
	cpu_uninstall_idmap();                 /* TTBR0 指向空表，防推测取指 */

	xen_early_init();
	efi_init();                            /* EFI 运行时服务、内存图 */

	arm64_memblock_init();                 /* ★ memblock 灌入：裁掉超物理位宽的
	                                         内存、memstart_addr 对齐、内核镜像/
	                                         DTB/initrd reserve、crashkernel/CMA */
	paging_init();                         /* ★ 正式页表：直接映射（含 pgprot
	                                         修饰）、vmemmap、fixmap、idmap */

	acpi_table_upgrade();
	acpi_boot_table_init();
	if (acpi_disabled)
		unflatten_device_tree();           /* DTB → struct device_node 树 */

	bootmem_init();                        /* zone/pfn 初始化入口：
	                                         zone_sizes_init + free_area_init
	                                         （arm64 移植层 mm/init.c），
	                                         memblock 内存交给 buddy 的准备 */
	kasan_init();
	request_standard_resources();

	early_ioremap_reset();                 /* 早期 ioremap 区间交接给 vmalloc */
	psci_dt_init() / psci_acpi_init();     /* 按 DTB/ACPI 提取 PSCI 调用约定 */
	init_bootcpu_ops();
	smp_init_cpus();                       /* /cpus 或 MADT → cpu_possible
	                                         掩码 + cpu_ops（psci/spin-table） */
	smp_build_mpidr_hash();
	...
	if (boot_args[1] || boot_args[2] || boot_args[3])
		pr_err("WARNING: x1-x3 nonzero in violation of boot protocol...\n");
```

**与 x86 的本质差异**：arm64 的"内存拓扑"来自 DTB `/memory` 或 EFI 内存图，格式规整，不需要 E820 那样的融合与仲裁；而页表结构（VA 位数、52 位 PA、ROX 权限）复杂得多，所以 `paging_init()`（`arch/arm64/mm/mmu.c`）承担了 x86 里 `init_mem_mapping()` + `paging_init()` 的全部职责。

---

## 6. 阶段 ②-2：setup_per_cpu_areas() 与 `.data..percpu`

```c
/* init/main.c（节选） */
	setup_nr_cpu_ids();         /* nr_cpu_ids = possible 掩码最高位+1 */
	setup_per_cpu_areas();      /* 每个静态 per-CPU 变量的"分身"安家 */
	smp_prepare_boot_cpu();     /* 架构钩子：x86 侧重置 GSBASE 等 */
	boot_cpu_hotplug_init();    /* cpuhp_state.state = CPUHP_ONLINE */
```

链接期内核只有**一份** per-CPU 变量模板（`.data..percpu` 段，`__per_cpu_start..__per_cpu_end`）；`setup_per_cpu_areas()` 为每个 possible CPU 复制一份并登记偏移：

```c
/* mm/percpu.c —— 通用 SMP 实现（非 CONFIG_HAVE_SETUP_PER_CPU_AREA 架构） */
void __init setup_per_cpu_areas(void)
{
	rc = pcpu_embed_first_chunk(PERCPU_MODULE_RESERVE,
				    PERCPU_DYNAMIC_RESERVE, PAGE_SIZE, NULL, NULL);
	if (rc < 0)
		panic("Failed to initialize percpu areas.");
	delta = (unsigned long)pcpu_base_addr - (unsigned long)__per_cpu_start;
	for_each_possible_cpu(cpu)
		__per_cpu_offset[cpu] = delta + pcpu_unit_offsets[cpu];
}
```

深挖要点：

* **第一块"镜像"**：`pcpu_embed_first_chunk()` 用 memblock 从内核镜像附近分配一整块（`ai->unit_size`），把 `.data..percpu` 模板逐 CPU 复制进去。x86 自己的实现（`arch/x86/kernel/setup_percpu.c`）优先 embed；`CONFIG_NEED_PER_CPU_PAGE_FIRST_CHUNK` 时退化为 `pcpu_page_first_chunk()`（每 CPU 按页散布，配合 NUMA 大页策略）。
* **per-CPU 寻址**：此后 `this_cpu_ptr(p)` = `p + __per_cpu_offset[smp_processor_id()]`。x86 用 `%gs` 段基址、arm64 用 `TPIDR_EL1` 持有该偏移；boot CPU 在 `__startup_64`/head.S 里就已经用 `fixed_percpu_data`/`init_per_cpu` 提前埋好了这套机制（见第 1 章）。
* **`PERCPU_MODULE_RESERVE`**：为模块的 per-CPU 变量预留的头寸，模块加载时从这里切分。
* 调用顺序约束：`setup_per_cpu_areas()` 必须在所有 per-CPU 使用者（`sched_init` 的 per-CPU rq、`early_irq_init` 的 kstat 等）之前——这也是它紧跟 setup_arch 的原因。

---

## 7. 阶段 ③：命令行解析与 `.init.setup` 段机制

命令行有三份拷贝、两个消费阶段：

| 拷贝 | 生成处 | 用途 |
| :--- | :--- | :--- |
| `boot_command_line[]` | 架构代码从 boot_params/DTB 拷入 | 原始命令行，`/proc/cmdline` 源头 |
| `saved_command_line` | `setup_command_line()` memblock 分配 | 保存副本（含 extra_command_line），最终展示 |
| `static_command_line` | 同上 | 被就地解析的工作副本 |

解析分两轮：

```c
/* init/main.c（节选） */
	jump_label_init();          /* static key 先冻结，参数可能翻转它们 */
	parse_early_param();        /* 第一轮：early_param 声明的参数 */
	after_dashes = parse_args("Booting kernel", static_command_line,
				  __start___param, __stop___param - __start___param,
				  -1, -1, NULL, &unknown_bootoption);
	print_unknown_bootoptions();
```

**`early_param` / `__setup` 的链接期展开**（`include/linux/init.h`）：

```c
struct obs_kernel_param {
	const char *str;              /* 参数名 */
	int (*setup_func)(char *);    /* 值处理器（返回 1=已消费） */
	int early;                    /* 1 = early_param，参与第一轮 */
};

#define __setup_param(str, unique_id, fn, early)			\
	static const char __setup_str_##unique_id[] __initconst	\
		__aligned(1) = str;				\
	static struct obs_kernel_param __setup_##unique_id	\
		__used __section(".init.setup")			\
		__aligned(__alignof__(struct obs_kernel_param))	\
		= { __setup_str_##unique_id, fn, early }

#define __setup(str, fn)      __setup_param(str, fn, fn, 0)
#define early_param(str, fn)  __setup_param(str, fn, fn, 1)
```

两轮解析的执行路径：

```text
第一轮（setup_arch 内部即调用，start_kernel 再调是幂等的）:
  parse_early_param() → parse_early_options() → parse_args(..., do_early_param)
  do_early_param(): 遍历 __setup_start..__setup_end，
      只处理 p->early == 1 且名字匹配的项；
      另有特例："console=ttyS0" 也放行给 earlycon 匹配

第二轮:
  parse_args("Booting kernel", ...) 先扫 __start___param..__stop___param
      （module_param 风格参数），未匹配者交给 unknown_bootoption
  unknown_bootoption() → obsolete_checksetup()
      遍历 .init.setup 段：
        - p->early == 1 → 跳过（第一轮已处理）
        - setup_func(line+n) 返回 1 → 消费成功
        - 纯名字匹配但无 setup_func → "Parameter %s is obsolete"
```

深挖要点：

* **`__initconst`/`__initdata` 到处出现**：命令行解析结构体全部落在 `.init.*` 段，`free_initmem()` 后连同名段一起释放（第 4 章详述段生命周期）。
* **返回值语义陷阱**：`__setup` 处理器返回 1 表示"已处理"；`early_param`/module_param 风格返回 0 表示成功。`do_early_param` 对返回值只打 warn，**永不拒绝参数**（"We accept everything at this stage"）——启动早期没有回退余地。
* `parse_args` 还识别 `--` 分隔符，其后内容经 `set_init_arg` 成为 init 进程的参数（`extra_init_args`）。

---

## 8. 阶段 ④：mm_core_init() —— memblock → buddy → slab 的交接典礼

```c
/* mm/mm_init.c（节选） */
void __init mm_core_init(void)
{
	/* Initializations relying on SMP setup */
	build_all_zonelists(NULL);          /* ★ zone 备援链 */
	page_alloc_init_cpuhp();
	...
	mem_init();                          /* ★ memblock → buddy */
	mem_init_print_info();
	kmem_cache_init();                   /* ★ slab 起步 */
	...
	vmalloc_init();
	/* x86: init_espfix_bsp(); pti_init() 等架构加固也在此 */
}
```

**（a）build_all_zonelists()**（`mm/page_alloc.c`）：每个 node/zone 都要有一份"本 zone 不够时找谁借"的备援顺序（zonelist）。`build_all_zonelists()` 对每个 online node 调 `build_zonelists()`，把本节点各 zone 按 ZONE_HIGHMEM→NORMAL→DMA32→DMA 降序、再按距离追加其他节点，构建 `pglist_data.node_zonelists[]`。它要求 `nr_cpu_ids`、zone 边界（zone_sizes_init）已就绪——这正是它位于 setup_arch 之后的原因。同时 `set_zonelist_order()` 决定节点内/节点间优先级。

**（b）mem_init()：memblock 的谢幕**。x86_64（`arch/x86/mm/init_64.c`）：

```c
void __init mem_init(void)
{
	pci_iommu_alloc();
	/* this will put all memory onto the freelists */
	memblock_free_all();          /* 见下 */
	after_bootmem = 1;            /* 全局标志：从此 buddy 当家 */
	...
}
```

arm64（`arch/arm64/mm/init.c`）在 `bootmem_init()` 中先完成 `zone_sizes_init()`/`free_area_init()`（sparsemem 的 `mem_map` 与 zone 结构），随后同样 `mem_init() → memblock_free_all()`。`memblock_free_all()`（`mm/memblock.c`→`mm/mm_init.c` 的 `free_low_memory_core_early()` 路径）做的事：

```text
遍历 memblock.memory 中每个 region：
  对每个与 reserved 不相交的空闲子区间：
    按区间对齐计算可用 order；
    __free_pages_core(page, order) → 直接把页挂入
        zone->free_area[order].free_list 并 set_page_order/PG_buddy，
        同时回填 zone present_pages 与 managed_pages
（ buddy 侧无需分配任何元数据——struct page 数组已由 free_area_init
  期间用 memblock 分配好，这正回答了"为什么不能跳过 memblock" ）
```

**（c）kmem_cache_init()**（以默认 SLUB 为例，`mm/slub.c`）：

```c
/* mm/slub.c: kmem_cache_init()（节选） */
	kmem_cache_node = &boot_kmem_cache_node;   /* 静态引导结构体 */
	kmem_cache = &boot_kmem_cache;

	create_boot_cache(kmem_cache_node, "kmem_cache_node", ...);
	slab_state = PARTIAL;                      /* 已能分配 kmem_cache_node */
	create_boot_cache(kmem_cache, "kmem_cache", ...);

	kmem_cache = bootstrap(&boot_kmem_cache);        /* 静态结构体的内容
	                                                    搬进新分配的正式对象 */
	kmem_cache_node = bootstrap(&boot_kmem_cache_node);
	setup_kmalloc_cache_index_table();
	create_kmalloc_caches(0);                  /* kmalloc-8..kmalloc-8k 全家桶 */
```

经典的**自举**：SLUB 需要 `kmem_cache` 结构来管理 slab，而 `kmem_cache` 自身也要从 slab 分配——解法是先用两个静态结构体（`boot_kmem_cache[_node]`）开第一批 cache，再用新生的分配器把静态内容"搬家"成正规对象。此后 `kmalloc()` 终于可用，这也是为什么阶段⑧之后的初始化代码可以放心使用普通分配器。

---

## 9. 阶段 ⑤：sched_init() —— 调度器的冷启动

```c
/* kernel/sched/core.c: sched_init()（节选） */
	/* root_task_group 及 RT 带宽（CONFIG_CGROUP_SCHED） */
	init_rt_bandwidth(&def_rt_bandwidth, global_rt_period(), global_rt_runtime());
	...
	init_defrootdomain();                 /* 默认根调度域 */

	for_each_possible_cpu(i) {
		struct rq *rq = cpu_rq(i);    /* rq 就在 per-CPU runqueues 里 */
		raw_spin_lock_init(&rq->__lock);
		rq->nr_running = 0;
		init_cfs_rq(&rq->cfs);        /* CFS：min_vruntime=0，任务树空 */
		init_rt_rq(&rq->rt);          /* RT：优先级位图 + 带宽 */
		init_dl_rq(&rq->dl);          /* DEADLINE：红黑树 + 带宽 */
		...
		rq_attach_root(rq, &def_root_domain);
		...
	}

	set_load_weight(&init_task, false);
	...
	mmgrab_lazy_tlb(&init_mm);
	enter_lazy_tlb(&init_mm, current);
	WARN_ON(!set_kthread_struct(current));

	init_idle(current, smp_processor_id());   /* ★ 把 init_task 注册为 0 号 idle */
	...
	init_sched_fair_class();       /* fair_sched_class 的初始化钩子 */
	psi_init();
	init_uclamp();
	preempt_dynamic_init();        /* preempt=none/voluntary/full 动态模式 */
	scheduler_running = 1;         /* 全局标志：调度器正式当班 */
```

深挖要点：

* **per-CPU `struct rq`**：`DEFINE_PER_CPU_SHARED_ALIGNED(struct rq, runqueues)`，`cpu_rq(i)` 即 `&per_cpu(runqueues, i)`。这也是它必须排在 `setup_per_cpu_areas()` 之后的原因。
* **三个子运行队列**：`rq->cfs/rt/dl` 分别服务 fair_sched_class / rt_sched_class / dl_sched_class；每个 `sched_entity`/`sched_rt_entity`/`sched_dl_entity` 挂在对应的树上。
* **调度类的"链接"不在运行时，而在链接期**：

```c
/* kernel/sched/sched.h */
#define DEFINE_SCHED_CLASS(name) \
const struct sched_class name##_sched_class \
	__aligned(__alignof__(struct sched_class)) \
	__section("__" #name "_sched_class")

extern struct sched_class __sched_class_highest[];
extern struct sched_class __sched_class_lowest[];
#define for_each_class(class) \
	for_class_range(class, __sched_class_highest, __sched_class_lowest)
```

  `include/asm-generic/vmlinux.lds.h` 把各 `__*_sched_class` 段按**逆序**排布，使地址升序恰好是优先级降序：`stop → dl → rt → fair → idle`。`pick_next_task()` 里 `for_each_class()` 从高往低扫，第一个有任务的类胜出——一个零运行时开销的静态分发设计。
* **init_idle()**：把 `init_task`（current）标为 idle 任务（`PF_IDLE`）、挂到本 CPU 的 rq、`rq->idle = current`。此后 boot CPU 上下文就是 pid 0 idle 线程本体——第 3 章的 `cpu_startup_entry()` 将进入它的死循环。
* **为什么 sched_init 在 IRQ 之前**：注释写明"Set up the scheduler prior starting any interrupts (such as the timer interrupt)"——一旦时钟中断来临就可能调度，那时 rq 必须已可用；而 SMP 拓扑（调度域）此时还没建，留到 `sched_init_smp()`（第 3 章）完成。

---

## 10. 阶段 ⑥：workqueue 与 RCU 的时机学

此阶段依次执行 `housekeeping_init()`（isolcpus 的 housekeeping 掩码）→ `workqueue_init_early()` → `rcu_init()` → `trace_init()`。两个深挖点：

* **workqueue_init_early()**（`kernel/workqueue.c`）只做"登记"：创建 `system_wq / system_highpri_wq / system_long_wq / system_unbound_wq / system_freezable_wq` 等 workqueue 结构与 pool（unbound pool 按 NUMA 节点/优先级/CPUMASK 组合），**但不创建任何 worker**——worker 是 kthread，而 kthreadd 尚未出生（它要到 `rest_init()` 才被创建）。真正的 worker 拉起在 `workqueue_init()`（第 3 章，smp_prepare_cpus 之后）。这解释了注释"Work item execution depends on kthreads and starts after workqueue_init()"。
* **rcu_init()**（`kernel/rcu/tree.c`，节选）：

```c
void __init rcu_init(void)                  /* kernel/rcu/tree.c（节选） */
{
	rcu_early_boot_tests();                 /* 自检/announce（TREE01..）*/
	kfree_rcu_batch_init();
	rcu_init_geometry();        /* rcu_node 树几何（叶子数量/层级） */
	rcu_init_one();             /* 建 rcu_state 结构 + node 树 */
	if (use_softirq)
		open_softirq(RCU_SOFTIRQ, rcu_core_si);

	rcutree_prepare_cpu(cpu);   /* 本 CPU 回调队列；cpu_starting 报告 QS */
	rcu_cpu_starting(cpu);
	rcutree_online_cpu(cpu);

	rcu_gp_wq = alloc_workqueue("rcu_gp", WQ_MEM_RECLAIM, 0);  /* expedited GP */
	...
}
```

  注意它必须在 `sched_init()` 之后（会创建 workqueue）、在 `tick/IRQ` 之前（需要软中断注册）。RCU 宽限期正式起跑要等 `rcu_scheduler_starting()`（`rest_init()` 里），`rcu_spawn_gp_kthread` 则是 early_initcall。

---

## 11. 阶段 ⑦：中断与时钟

### 11.1 early_irq_init() / init_IRQ()

```c
/* kernel/irq/irqdesc.c —— CONFIG_SPARSE_IRQ 路径（节选） */
int __init early_irq_init(void)
{
	init_irq_default_affinity();

	/* Let arch update nr_irqs and return the nr of preallocated irqs */
	initcnt = arch_probe_nr_irqs();        /* x86: NR_IRQS 估算；
	                                          arm64: nr_irqs 上限 */
	printk(KERN_INFO "NR_IRQS: %d, nr_irqs: %d, preallocated irqs: %d\n", ...);

	for (i = 0; i < initcnt; i++) {
		desc = alloc_desc(i, node, 0, NULL, NULL);  /* 每个预分配 IRQ 一个
	                                                    irq_desc + per-CPU
	                                                    kstat_irqs */
		irq_insert_desc(i, desc);          /* radix tree: irq → desc */
	}
	return arch_early_irq_init();          /* x86: 挂 interrupt attribution... */
}
```

`init_IRQ()` 则是架构函数：x86（`arch/x86/kernel/irqinit.c`）调用 `init_ISA_irqs()`（`init_bsp_APIC()` 虚拟线模式 + `legacy_pic->init()` + 0..15 号 ISA IRQ 挂 chip/handler）；真正的 APIC/vector 体系由后续 `arch_early_irq_init`/lapic initcall 完成。arm64（`arch/arm64/kernel/irq.c`）：

```c
void __init init_IRQ(void)
{
	init_irq_stacks();       /* 每 CPU 中断栈（IRQ_STACK_SIZE，CONFIG_VMAP_STACK） */
	init_irq_scs();          /* 影子调用栈（SCS）*/
	irqchip_init();          /* 扫描 DTB/ACPI 的中断控制器声明
	                            （IRQCHIP_DECLARE → of_irq_init）：
	                            GIC/GICv3 驱动在这里 probe */
	...
}
```

### 11.2 时间子系统三件套

```c
	tick_init();            /* tick_broadcast_init + tick_nohz_init */
	...
	timekeeping_init();     /* 时间戳"墙上时钟"的种子 */
	time_init();            /* 架构时钟设备 */
```

**timekeeping_init()**（`kernel/time/timekeeping.c`）：`clocksource_default_clock()` 此时几乎必然返回 **jiffies**（`kernel/time/jiffies.c` 的 `clocksource_jiffies`，rating 最低）——因为高性能 clocksource（TSC/arch_timer）驱动要到设备初始化阶段才注册，注册后 `clocksource_register()` 会按 rating 换马并经 `clocksource_switch()` 重算 `tk_setup_internals()`。它从 RTC/EFI（`read_persistent_wall_and_boot_offset()`）读出墙上时间，确立 `wall_to_mono`（保证 `wall + mono = boot time` 的不变式）。

**time_init()**：

```c
/* arch/x86/kernel/time.c */
void __init time_init(void)
{
	late_time_init = x86_late_time_init;   /* 挂钩！真正的初始化被推迟 */
}
/* start_kernel 后段（阶段⑨）执行：
   late_time_init() → hpet_time_init() + x86_init.timers.timer_init()
                    （lapic 时钟/校准）+ tsc_init() */
```

x86 把真家伙（HPET/LAPIC/TSC）推迟到 `late_time_init` 是因为它们需要 ioremap 与中断——此刻才具备。arm64 的 `time_init()`（`arch/arm64/kernel/time.c`）直接完成：校验 `/timer` 或 ACPI GTDT → 初始化 Generic Timer（`arch_timer`），注册 `cntvct` clocksource（rating 400）与 `cntv_ctl` clock_event，同时 `sched_clock_register()` 把 arm64 的 `sched_clock` 绑到 `cntvct`（64 位、GHz 级，天然免于 jiffies 抖动）。

**sched_clock() 的两幅面孔**：通用层（`kernel/time/sched_clock.c`）维护 `struct clock_data`（`cyc_to_ns_offset`、epoch 结构），未注册架构实现前用 jiffies 兜底（`jiffy_sched_clock_read`）；x86 在极早期就由 `tsc_early_init()` 临时注册 TSC 版本，`sched_clock_init()`（阶段⑨）再固化。所以 dmesg 前几行的时间戳精度会随后续重校准"跳变"——这是正常现象，不是 bug。

---

## 12. 阶段 ⑧：开闸、console_init 与 lockdep

```c
	WARN(!irqs_disabled(), "Interrupts were enabled early\n");

	early_boot_irqs_disabled = false;
	local_irq_enable();           /* ★ 启动过程首次开中断 */

	kmem_cache_init_late();       /* slub_flushwq 等 */
	console_init();               /* ★ */
	if (panic_later)
		panic("Too many boot %s vars at `%s'", ...);  /* 命令行变量超限 */

	lockdep_init();               /* 锁依赖哈希表/类分配 */
	locking_selftest();           /* 需要中断已开启，见下 */
```

**为什么在这里开中断？**三重原因：`call_function_init()`（IPI 基础设施）刚就绪，SMP IPI 理论上已可接收；`console_init()` 里大量 console 驱动探测（serial、tty、VGA）会做耗时 I/O 且部分依赖中断可用；`locking_selftest()` 需要真实开/关中断来自检原子性假设——注释原话："Need to run this when irqs are enabled, because it wants to self-test [hard/soft]-irqs on/off lock inversion bugs too"。

**lockdep 的启动时序**：`lockdep_init()`（哈希表、lock_class 缓冲）在 console 之后；而 `lockdep_off/on` 的最早使用者（printk 的 logbuf_lock 等）从第一个 printk 起就依赖其桩实现——内核编译了 CONFIG_PROVE_LOCKING 时，早期 `early_boot_irqs_disabled` 阶段的任何意外开中断都会被 `TRACE_IRQS_OFF` 自检捕获。

---

## 13. 专题：printk 的分阶段演进

printk 是"先攒着、再放音"的典范，四个阶段能力递进：

```text
阶段 A  earlycon            setup_arch/parse_early_param 期间
        drivers/tty/serial/earlycon.c：轮询写、无缓冲、无 loglevel、
        不经过 printk 体系（直接写字符）
阶段 B  printk 缓冲区       第一个 printk() 起就可写：
            static char __log_buf[__LOG_BUF_LEN] __aligned(LOG_ALIGN);
            /* kernel/printk/printk.c：静态预留，printk ringbuffer(prb)
               组织，记录 timestamp/cpu/level；但没有 console → 只进不出 */
阶段 C  setup_log_buf()     start_kernel 阶段③（early=0）/setup_arch 内（early=1）
        用 memblock 分配 LOG_BUF_LEN（log_buf_len= 可调）的大缓冲区，
        把 __log_buf 旧内容搬到新家
阶段 D  console_init()      阶段⑧
        遍历 __con_initcall_start..__con_initcall_end（console_initcall
        注册的探测函数）：tty/serial/vga/efi console 依次 register_console
        注册成功即回放（flush）积压的全部 log → 屏幕"突然"喷出大量输出
```

几个深挖点：

* **为什么屏幕输出是"一坨"涌出的**：`register_console()` 会把新 console 挂入 `console_list` 并（若 `console_enable` 且 loglevel 允许）触发对积压日志的回放；此前所有 printk 都只是在 ringbuffer 里排队。`console_init()` 与真正的"实时输出"之间还有一层 `console_unlock()` 的输出节流（legacy 路径在 printk 调用者上下文同步打印）。
* **earlycon → 正式 console 的交棒**：boot console（`CON_BOOT`）在最后一个真 console 注册后被 `unregister_console`（`late_initcall(printk_late_init)` 兜底）；`keep_bootcon` 可强留。
* **loglevel 过滤**：`console_loglevel` 控制"哪些级别值得输出到 console"，缓冲区则**全量**记录，这就是 `dmesg` 永远比屏幕全的原因。

---

## 14. 阶段 ⑨：收尾长跑（速览 + 表格）

这一长段是"多而不险"的各子系统初始化，按依赖顺序排布。挑几个有故事的：

| 调用 | 文件 | 作用与依赖 |
| :--- | :--- | :--- |
| `setup_per_cpu_pageset()` | mm/page_alloc.c | 每 CPU pageset（PCP 高速缓存），依赖 buddy |
| `acpi_early_init()` / `acpi_subsystem_init()` | drivers/acpi/ | ACPI 表深度解析、命名空间初始化 |
| `late_time_init()` | 架构提供 | x86: HPET/LAPIC/TSC 真正初始化 |
| `sched_clock_init()` | kernel/time/sched_clock.c | 固化 sched_clock（可重入安全） |
| `calibrate_delay()` | init/calibrate.c | BogoMIPS：忙等延时循环校准（jiffies 走表） |
| `arch_cpu_finalize_init()` | arch/x86/kernel/cpu/*.c 等 | 替代老 `check_bugs`：mitigations、NX/SMAP 决策 |
| `pid_idr_init()` → `fork_init()` → `proc_caches_init()` | kernel/pid.c、fork.c | pid IDR、task_struct 与配套 cache，fork 的前提 |
| `security_init()` | security/security.c | LSM 串行初始化（apparmor/selinux...） |
| `vfs_caches_init()` | fs/dcache.c | dentry/inode 哈希，挂载 rootfs 的前提 |
| `cgroup_init()` | kernel/cgroup/cgroup.c | cgroup 文件系统与子系统（与早期骨架会师） |

最后：

```c
	/* Do the rest non-__init'ed, we're now alive */
	arch_call_rest_init();
```

`arch_call_rest_init()` 是个弱符号包装（默认调 `rest_init()`），给架构留最后一个收尾点。注意它**不在 `__init` 段内被调用**——第 3 章会看到 `rest_init` 被声明为 `noinline __ref` 的深意。

---

## 15. 关键全局状态标志速查表

| 标志/变量 | 定义处 | 何时置位/翻转 | 消费者 |
| :--- | :--- | :--- | :--- |
| `early_boot_irqs_disabled` | init/main.c | start_kernel 开头 = true；阶段⑧ = false | WARN 检查、early console 调试输出 |
| `system_state` | include/linux/kernel.h | SYSTEM_BOOTING（0，静态）→ SYSTEM_SCHEDULING（rest_init 中）→ SYSTEM_FREEING_INITMEM（kernel_init 中）→ SYSTEM_RUNNING | LSM、debug 代码、`__might_sleep` 豁免早期告警 |
| `scheduler_running` | kernel/sched/core.c | sched_init 末尾 = 1 | 调度器内部路径 |
| `after_bootmem` | arch/x86/mm/init_64.c | mem_init() 中 = 1 | x86 早期页表代码选择分配方式 |
| `panic_later/panic_param` | init/main.c | `parse_args` 阶段超限（init args 过多） | console_init 后 panic("Too many boot vars") |
| `nr_cpu_ids / __cpu_possible_mask` | kernel/cpu.c | setup_nr_cpu_ids / 架构 smp_init_cpus | SMP 全体系 |
| `__per_cpu_offset[]` | mm/percpu.c | setup_per_cpu_areas | 所有 this_cpu 操作 |

---

## 16. 观测与调试

* **dmesg 时间戳质量**：前几行的时间戳来自 jiffies/未校准 TSC，`sched_clock_init()` 前后可能有跳变；分析启动耗时请用 `initcall_debug` 的相对时间而非绝对时间戳。
* **锁定启动早期问题**：`earlycon=` + `keep_bootcon`（防 earlycon 提前退场）+ `initcall_debug`；QEMU 下 `-d int,guest_errors` 可看早期异常。
* **memblock 视角**：`memblock=debug` 打印每次分配；`dmesg | grep -E "Memory:|Initmem|On node"` 分别对应 `mem_init_print_info()`、NUMA 初始化输出。
* **参数没生效？**：先区分它是不是 `early_param`（`early` 标志），第一轮在 setup_arch 内（x86 甚至更早），第二轮在 start_kernel 中段；`print_unknown_bootoptions` 的 "Unknown kernel command line parameters" 提示会指出被透传给 init 的参数。
* **追踪 start_kernel 本身**：ftrace 需到 `ftrace_init()` 才可用；此前只有 earlycon 与 panic。`CONFIG_BOOT_CONFIG` 可以把大量参数移出命令行。

---

## 17. 🧠 思考题

1. **为什么 `parse_early_param()` 被设计成幂等（`done` 静态标志），而架构代码在 `setup_arch()` 里已经调用过一次？** 提示：谁可能在其后修改 `boot_command_line`（EFI kexec、CMDLINE_FORCE）？
2. **`sched_init()` 为什么必须在 `early_irq_init()` 之前、而 `workqueue_init_early()` 却在 `rcu_init()` 之前？** 提示：中断来了会调度；RCU 需要 workqueue 结构但不执行 worker。
3. **`memblock_free_all()` 把页挂入 buddy 时，`struct page` 数组本身是谁分配的？如果 4G 物理内存的 `mem_map` 需要 2G 虚拟地址，memblock 够不够用？** 提示：`free_area_init()`（sparsemem `vmemmap_populate`）与 memblock 的分工。
4. **x86 的 `time_init()` 只挂了个 `late_time_init` 钩子，为什么不等 `late_time_init()` 真正初始化完成后再注册 jiffies clocksource？** 提示：timekeeping 需要一个"永远可用"的底线时钟；谁在设备模型起来之前就需要时间？
5. **`console_init()` 在 PCI/设备探测之前执行，那些依赖 PCI 的 console（如 DRM）是怎么"后来居上"的？** 提示：console_initcall 只是"探测"，注册次序与 flush 语义；late_initcall(printk_late_init)。
6. **如果某驱动在 `subsys_initcall` 里调用 `kmalloc(64)`，它会成功吗？走的哪条分配路径？** 提示：subsys 级 initcall 运行于 kernel_init 上下文（第 3 章），早于它的是 `kmem_cache_init()`。
