# initcall 与模块初始化机制：链接器排序的智慧

内核里有几千个"初始化函数"：IOMMU 要在大多数驱动之前就绪，文件系统要先于根挂载，调试设施最好最后再动。C 语言没有全局构造顺序，Linux 的解法是把"何时调用"编码进**链接段名**，启动时按段顺序遍历调用——这就是 initcall 体系。本章解剖它的宏展开、段布局、执行引擎、内置/模块双形态，以及调试启动耗时的实战方法。

### 🎯 本章主线

* `__define_initcall` 一族宏的完整展开：函数指针如何落到 `.initcallN.init` 段；
* `do_initcalls() -> do_one_initcall()` 执行引擎与 `initcall_debug`/blacklist；
* `module_init()` 的双重人格：内建时变身 initcall，模块时变身 `init_module()` 入口；
* `__init` 段的生命周期与 `free_initmem()`；
* driver core 的 deferred probe（`-EPROBE_DEFER`）。

---

## 1. 问题：为什么需要 initcall 分级

静态链接的内核没有"先构造谁"的语言级答案。initcall 用三个约定解决：

1. **级别**：宏名即级别（`core_initcall` 早于 `device_initcall`），映射到不同段名；
2. **同级按链接顺序**：同一 `obj-y` 顺序内，`Makefile` 排前者先执行——所以同级别内仍有确定的偏序；
3. **启动期一次性**：全部落在 `.init` 相关段，`free_initmem()` 之后整体回收。

---

## 2. 宏展开全景：从 `device_initcall(fn)` 到段里的一条记录

`include/linux/init.h` 的宏链条（自顶向下逐层剥开）：

```c
/* include/linux/init.h（节选） */

/* ① 使用层：device_initcall(e1000_init_module)
      → __define_initcall(fn, 6) → ___define_initcall(fn, 6, .initcall6) */
#define __define_initcall(fn, id) ___define_initcall(fn, id, .initcall##id)

#define ___define_initcall(fn, id, __sec)				\
	__unique_initcall(fn, id, __sec, __initcall_id(fn))

/* ② 唯一化：拼出全内核不重名的符号（多调用点/同名函数会撞符号） */
/* Format: <modname>__<counter>_<line>_<fn> */
#define __initcall_id(fn)					\
	__PASTE(__KBUILD_MODNAME,				\
	__PASTE(__, __PASTE(__COUNTER__,				\
	__PASTE(_, __PASTE(__LINE__, __PASTE(_, fn))))))

#define __unique_initcall(fn, id, __sec, __iid)			\
	____define_initcall(fn,					\
		__initcall_stub(fn, __iid, id),			\
		__initcall_name(initcall, __iid, id),		\
		__initcall_section(__sec, __iid))

/* ③ 落段：两种实现——PREL32 相对偏移（x86/arm64 等）或裸函数指针 */
#ifdef CONFIG_HAVE_ARCH_PREL32_RELOCATIONS
#define ____define_initcall(fn, __stub, __name, __sec)		\
	__define_initcall_stub(__stub, fn)			\
	asm(".section	\"" __sec "\", \"a\"		\n"	\
	    __stringify(__name) ":			\n"	\
	    ".long	" __stringify(__stub) " - .	\n"	\
	    ".previous					\n");	\
	static_assert(__same_type(initcall_t, &fn));
#else
#define ____define_initcall(fn, __unused, __name, __sec)	\
	static initcall_t __name __used				\
		__attribute__((__section__(__sec))) = fn;
#endif
```

深挖要点：

* **PREL32 相对偏移**：段里存的不是 64 位指针而是 4 字节的 `目标 - 自身`。好处：段内容天然位置无关（KASLR/重定位无需修 initcall 表）、省一半内存。读取端 `initcall_from_entry()` 用 `offset_to_ptr()` 现场还原。
* **LTO 特殊处理**：`CONFIG_LTO_CLANG` 下链接顺序不再可信，`__initcall_section` 会给每个 initcall 生成 `.initcall6.init..<iid>` 子段，由 `scripts/link-vmlinux.sh` 生成的顺序脚本拼接——分级机制对 LTO 依然成立。
* **`static_assert(__same_type(initcall_t, &fn))`**：把"initcall 必须是 `int (*)(void)`"做成了编译期硬约束，返回值无处安放会在编译期报错。

**段布局**（`include/asm-generic/vmlinux.lds.h`）：

```c
#define INIT_CALLS_LEVEL(level)					\
		__initcall##level##_start = .;			\
		KEEP(*(.initcall##level##.init))		\
		KEEP(*(.initcall##level##s.init))		\

#define INIT_CALLS						\
		__initcall_start = .;				\
		KEEP(*(.initcallearly.init))			\
		INIT_CALLS_LEVEL(0)				\
		...							\
		INIT_CALLS_LEVEL(5)				\
		INIT_CALLS_LEVEL(rootfs)				\
		INIT_CALLS_LEVEL(6)				\
		INIT_CALLS_LEVEL(7)				\
		__initcall_end = .;
```

注意两点：每个级别是 `N` 与 `Ns`（sync）两个相邻段；`rootfs` 段物理上插在 5 与 6 之间。`KEEP()` 防止 `--gc-sections` 把"看起来没人引用"的表项删掉。

---

## 3. 八级 initcall 与典型使用者

| 级别 | 宏 | 段 | 运行时机 | 典型使用者 |
| :--- | :--- | :--- | :--- | :--- |
| early | `early_initcall(fn)` | `.initcallearly.init` | `kernel_init_freeable(): do_pre_smp_initcalls()`，**SMP 拉起前** | `rcu_spawn_gp_kthread`、timing 早期校准、SMP 依赖项 |
| pure(0) | `pure_initcall(fn)` | `.initcall0.init` | do_initcalls 起点 | 纯静态变量初始化者（如 `init_srcu_*`） |
| core(1) | `core_initcall(fn)` | `.initcall1.init` | 最早一批 | 中断子系统、`rcu` 核心、debug 子系统骨架 |
| postcore(2) | `postcore_initcall(fn)` | `.initcall2.init` | | 架构后处理、kmemleak |
| arch(3) | `arch_initcall(fn)` | `.initcall3.init` | | 架构驱动（老式）、ACPI 深度初始化 |
| subsys(4) | `subsys_initcall(fn)` | `.initcall4.init` | | 总线/子系统：PCI(e)、net 核心骨架 |
| fs(5) | `fs_initcall(fn)` | `.initcall5.init` | | 文件系统注册、network fs |
| rootfs | `rootfs_initcall(fn)` | `.initcallrootfs.init` | 5 与 6 之间 | `populate_rootfs()`——rootfs 必须在 device 级驱动探测前备好，因为后者可能要用它 |
| device(6) | `device_initcall(fn)`（= `__initcall` = `module_init` 内建形态） | `.initcall6.init` | 绝大多数驱动 | e1000、usb、 drm 注册等 |
| late(7) | `late_initcall(fn)` | `.initcall7.init` | 最后 | `deferred_probe_initcall`、`printk_late_init`（退场 boot console）、`checkpoint/restore` |
| （专属） | `console_initcall(fn)` | `.con_initcall.init` | `console_init()`（start_kernel 阶段，**远早于 device 级**） | tty/serial/vga console 探测 |
| （专属） | `security_initcall(fn)` | `.security_initcall.init` | `security_init()`（start_kernel 内） | LSM 初始化 |

> 每个 `Ns` 同步段（`core_initcall_sync` 等）紧跟对应异步段之后——异步 initcall（`async_schedule`）的汇合点。

---

## 4. 执行引擎：do_initcalls() 一族

```c
/* init/main.c */
static initcall_entry_t *initcall_levels[] __initdata = {
	__initcall0_start, __initcall1_start, __initcall2_start,
	__initcall3_start, __initcall4_start, __initcall5_start,
	__initcall6_start, __initcall7_start, __initcall_end,
};

static const char *initcall_level_names[] __initdata = {
	"pure", "core", "postcore", "arch", "subsys", "fs",
	"device", "late",
};

static void __init do_initcall_level(int level, char *command_line)
{
	initcall_entry_t *fn;

	parse_args(initcall_level_names[level],
		   command_line, __start___param,
		   __stop___param - __start___param,
		   level, level,                    /* ★ level=min=max */
		   NULL, ignore_unknown_bootoption);

	trace_initcall_level(initcall_level_names[level]);

	for (fn = initcall_levels[level]; fn < initcall_levels[level+1]; fn++)
		do_one_initcall(initcall_from_entry(fn));
}

static void __init do_initcalls(void)
{
	...
	for (level = 0; level < ARRAY_SIZE(initcall_levels) - 1; level++) {
		strcpy(command_line, saved_command_line);  /* 每级恢复原串 */
		do_initcall_level(level, command_line);
	}
	...
}
```

两个精妙设计：

* **每级重跑一遍 `parse_args`**（min=max=level）：这服务于**显式分级的参数**。`include/linux/moduleparam.h` 里 `struct kernel_param` 带一个 `s8 level` 字段：普通 `module_param()` / `core_param()` 写 -1（在 start_kernel 主解析轮 `min=max=-1` 时一次性生效）；而 `core_param_cb(1) / postcore_param_cb(2) / arch_param_cb(3) / subsys_param_cb(4) / fs_param_cb(5) / device_param_cb(6) / late_param_cb(7)` 写对应级别——这些参数被 `kernel/params.c: parse_one()` 的 `level < min_level || level > max_level` 过滤器拦下，**延迟到同级别 initcall 开始前**才解析（典型用途：某子系统参数依赖该子系统先建好基础设施）。
* **`do_pre_smp_initcalls()`**（见第 3 章）单独遍历 `__initcall_start..__initcall0_start`（early 段），在 SMP 拉起前执行。

**`do_one_initcall()`**——单个 initcall 的全生命周期包装：

```c
int __init_or_module do_one_initcall(initcall_t fn)
{
	int count = preempt_count();
	char msgbuf[64];
	int ret;

	if (initcall_blacklisted(fn))
		return -EPERM;

	do_trace_initcall_start(fn);
	ret = fn();                              /* ★ 执行 */
	do_trace_initcall_finish(fn, ret);

	msgbuf[0] = 0;
	if (preempt_count() != count) {          /* preempt 计数泄漏 → 复位并告警 */
		sprintf(msgbuf, "preemption imbalance ");
		preempt_count_set(count);
	}
	if (irqs_disabled()) {                   /* 忘开中断 → 代开并告警 */
		strlcat(msgbuf, "disabled interrupts ", sizeof(msgbuf));
		local_irq_enable();
	}
	WARN(msgbuf[0], "initcall %pS returned with %s\n", fn, msgbuf);

	add_latent_entropy();                    /* 混入启动熵池 */
	return ret;
}
```

返回值语义：0 成功；负 errno 失败（仅打日志，**不会中止启动**——rootfs/init 级别的关键失败除外）；正值"可疑"（模块路径会 `pr_warn` 并继续）。

---

## 5. initcall_debug 与启动耗时观测

**打开方式**：命令行 `initcall_debug`（`core_param`，也可 `/sys/module/kernel/parameters/initcall_debug` 运行时改）。

机制：`initcall_debug_enable()`（start_kernel 中段）向 `initcall_init` 域的两个 tracepoint 挂回调：

```c
/* init/main.c（节选） */
static void __init trace_initcall_start_cb(void *data, initcall_t fn)
{
	printk(KERN_DEBUG "calling  %pS @ %i\n", fn, task_pid_nr(current));
	*calltime = ktime_get();
}

static void __init trace_initcall_finish_cb(void *data, initcall_t fn, int ret)
{
	rettime = ktime_get();
	printk(KERN_DEBUG "initcall %pS returned %d after %lld usecs\n",
		 fn, ret, ktime_us_delta(rettime, *calltime));
}
```

于是 dmesg 里出现成对的：

```text
calling  clk_disable_unused+0x0/0x30 @ 1
initcall clk_disable_unused returned 0 after 12 usecs
```

**排查三板斧**：

```bash
# 1) 找出 top 耗时 initcall
dmesg | grep "returned.*after" | awk '{print $(NF-1), $0}' | sort -rn | head -30

# 2) 生成启动时间线 SVG（内核自带脚本）
dmesg | perl scripts/bootgraph.pl > boot.svg

# 3) 拉黑可疑驱动再对比
#   内核命令行: initcall_blacklist=e1000_init_module,foo_probe
```

黑名单实现在 `init/main.c`：`__setup("initcall_blacklist=", initcall_blacklist)`，按 `kallsyms` 反解函数名比对，命中者 `do_one_initcall` 直接返回 `-EPERM`。注意：被拉黑的驱动若是别人依赖的（如总线控制器），链式后果自负。

**ftrace function_graph 精确定位**：

```bash
cd /sys/kernel/tracing
echo function_graph > current_tracer
echo funcgraph-proc > trace_options
echo 'e1000*' > set_ftrace_filter      # 只追该驱动函数族
echo 1 > tracing_on                     # 开机后回看 trace
```

initcall 前后还有专门的 trace 事件（`initcall:initcall_start`/`initcall_initcall_finish` 与 `initcall_level`），配合 `trace-cmd record -e initcall` 开机采集更省事。

---

## 6. module_init() 的双重人格

`include/linux/module.h` 按编译形态二选一：

```c
/* 内建形态（!MODULE）：直接降级为 device 级 initcall */
#define module_init(x)	__initcall(x);        /* = device_initcall(x) */
#define module_exit(x)	__exitcall(x);

/* 模块形态（MODULE）：级别宏全部塌缩成 module_init（模块无分级概念）*/
#define core_initcall(fn)   module_init(fn)    /* early..late 同理 */
#define device_initcall(fn) module_init(fn)

#define module_init(initfn)					\
	static inline initcall_t __maybe_unused __inittest(void)	\
	{ return initfn; }					\
	int init_module(void) __copy(initfn)			\
		__attribute__((alias(#initfn)));		\
	___ADDRESSABLE(init_module, __initdata);

#define module_exit(exitfn)					\
	void cleanup_module(void) __copy(exitfn)		\
		__attribute__((alias(#exitfn)));
```

深挖要点：

* **内建时**你的 `foo_init()` 变成 `.initcall6.init` 表项 + `__init` 生命周期的受益者（可回收）；**模块时**它被赋予固定入口名 `init_module`（alias 到你的函数），供 `finit_module(2)` 系统调用按符号寻址。模块的"级别"信息丢失——**模块没有插入时机的级别承诺，谁先 insmod 谁先跑**，模块间依赖靠模块符号引用解决。
* **为什么模块不分级**：模块加载发生在运行期（SYSTEM_RUNNING），届时所有内建级别早已跑完，级别没有意义；真正需要次序的场景应拆成多模块用符号依赖表达。
* 加载路径：`insmod` → `finit_module(fd, args)`（`kernel/module/main.c`）→ `load_module()`：校验签名 → 布置 `mod->mem[MOD_TEXT/MOD_INIT_TEXT/...]` 各段 → 重定位 → `do_init_module()`：

```c
/* kernel/module/main.c: do_init_module()（节选） */
	do_mod_ctors(mod);
	if (mod->init != NULL)
		ret = do_one_initcall(mod->init);   /* ★ 复用同一执行引擎！
		                                       preempt/irq 自检、trace、
		                                       latent entropy 全都适用 */
	if (ret < 0) goto fail;
	...
	/* 模块转正 MODULE_STATE_LIVE、发 uevent 后，立即归还 init 段 */
	if (llist_add(&freeinit->node, &init_free_list))
		schedule_work(&init_free_wq);   /* do_free_init() 经 RCU+工作队列
		                                  异步 module_memfree() */
```

---

## 7. `__init` 段的生命周期与 free_initmem()

```c
/* include/linux/init.h */
#define __init		__section(".init.text") __cold __latent_entropy
#define __initdata	__section(".init.data")
#define __initconst	__section(".init.rodata")
#define __exit		__section(".exit.text") ...
```

链接脚本把所有 init 段夹在 `__init_begin..__init_end` 之间，与 `.init.setup`（命令行）、`.initcall*`、`.con_initcall` 等表段同居。释放发生在 `kernel_init()`：

```text
free_initmem() → free_initmem_default(POISON_FREE_INITMEM)
              → free_reserved_area(__init_begin..__init_end, ...)
                 逐页 poison 后 __free_pages_core 交还 buddy
```

三个深挖点：

* **为什么模块不能等"启动后统一释放"**：模块的加载/卸载发生在任意时刻，其 `.init` 段是 `module_alloc()` 动态分配的私有内存，无法并入全局 `__init_begin..__init_end` 区间。所以模块在 `do_init_module()` 里**各自**释放（经 `init_free_wq` 工作队列 + `synchronize_rcu`，因为 kallsyms 可能正拿着指针遍历）。这也是 `module_init` 的函数在模块形态下要不要标 `__init` 的答案：**标了有效且推荐**——它把函数放进 `MOD_INIT_TEXT` 段，init 完即回收。
* **`__exit` 的不对称命运**：内建驱动的清理函数永远不会被调用（没有"卸载内核"这回事），但 `__exit` 段并不急于消失——x86/arm64 因 `.altinstructions` 可能引用退出代码而定义 `RUNTIME_DISCARD_EXIT`（各自的 `vmlinux.lds.S`），`.exit.text/.exit.data` 被放进 `__init_begin..__init_end` 区间**随 initmem 一起在运行期释放**；不支持该机制的小架构则在链接期经 `EXIT_DISCARDS` 直接丢弃。`__exitcall`（`module_exit` 的内建形态）注册的回调同样无人调用，段同归于尽。
* **modpost 的 section mismatch 检查**：编译期 `scripts/mod/modpost` 扫描"非 init 引用 init"的跨段引用——运行时 init 段消失后这类指针全部变野指针。合法引用（如 `kernel_init` 调 `kernel_init_freeable`）必须以 `__ref` 显式豁免并注释理由，这正是第 3 章 `rest_init` 标 `__ref` 的原因。

---

## 8. deferred probe：运行期的"级别补丁"

initcall 只解决了**静态**次序；设备树里两个设备谁依赖谁，编译期无从知晓。driver core 的解法（`drivers/base/dd.c`）：

```text
device_attach() → really_probe() → driver_probe()
    └─ 返回 -EPROBE_DEFER（如依赖的 GPIO/i2c 总线还没就绪）
        → driver_deferred_probe_add(dev)：挂入 deferred_probe_pending_list
        → [触发器] driver_deferred_probe_trigger()：
             每当有新设备 bound 成功，重试整张挂起表
             （__device_attach_async_helper / deferred_probe_work）
```

关键时序锚点：

```c
/* drivers/base/dd.c */
static int __init deferred_probe_initcall(void)
{
	debugfs_create_file("devices_deferred", 0444, ...);  /* 观测口 */

	driver_deferred_probe_enable = true;   /* 之前只挂表不重试，
	                                             避免与 device 级
	                                             probe 大部队互相踩踏 */
	driver_deferred_probe_trigger();
	flush_work(&deferred_probe_work);
	initcalls_done = true;

	driver_deferred_probe_trigger();       /* 再来一轮收尾 */
	flush_work(&deferred_probe_work);

	if (driver_deferred_probe_timeout > 0) /* 默认 10 秒 */
		schedule_delayed_work(&deferred_probe_timeout_work, ...);
	return 0;
}
late_initcall(deferred_probe_initcall);
```

* `deferred_probe_timeout=`（默认 10，0=禁用）：超时后剩余挂起者强制终审并打 `deferred probe timeout` 警告；
* 观测：`/sys/kernel/debug/devices_deferred` 列出仍未 probe 的设备名；
* 与 `CONFIG_DEFERRED_STRUCT_PAGE_INIT`（第 2 章 `mm_core_init` 里闪过的 `deferred_struct_pages`）**毫无关系**——那是指 struct page 描述符的延迟初始化（kworker 并行补页元数据），名字撞车而已。

---

## 9. 🧠 思考题

1. **为什么 `rootfs_initcall` 必须排在 `device_initcall` 之前？** 若把 `populate_rootfs()` 降为 device 级，哪些驱动会先于 rootfs 就绪、又会出什么错？提示：`request_module()` 依赖 rootfs 里的 kmod；固件加载（`fw_devlink`）也指向用户态。
2. **同一个驱动源码 `module_init(foo_init)`：内建时 foo_init 能否标注 `__init`？模块形态时呢？如果它想在两种形态下都保留 init 函数供运行期手动再调用，会发生什么？** 提示：内建 `__init` 段在 free_initmem 后消失；模块段在 init 完成后被 `init_free_wq` 回收。
3. **`do_initcall_level` 每级都重跑 `parse_args` 且 min=max=level——请推导：普通 `module_param` 与 `core_param_cb` 的 `struct kernel_param.level` 各是几？谁在 start_kernel 主轮生效、谁被延迟到同级 initcall 前？** 提示：`kernel/params.c: parse_one()` 的 level 过滤；主解析轮传的是 `-1, -1`；`initcall_debug` 本身用的是 `core_param`（level=-1），所以 start_kernel 里的 `initcall_debug_enable()` 来得及看到它。
4. **deferred probe 为什么把"启用重试"推迟到 `late_initcall`，而不是每次 EPROBE_DEFER 立刻全局重试？** 提示：O(n²) 重试风暴 vs "绝大多数依赖会在 device 级自然就绪"的统计事实。
5. **initcall 返回负值不会中止启动，那"init= 所需的 rootfs 挂载失败"为什么能停下来？** 提示：panic 的来源不是 initcall 返回值，而是 `kernel_init` 里 `run_init_process` 全失败或 `prepare_namespace` 内部 panic。
6. **`initcall_blacklist` 按"函数名"而不是"驱动名"工作，这带来哪些坑？** 提示：kallsyms 反解、同名静态函数、内联后符号消失。
