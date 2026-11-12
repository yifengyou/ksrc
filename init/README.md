# init：内核启动流程学习笔记（基于 Linux 6.6）

启动流程是内核中"最不像内核"的一段代码：它没有并发、没有调度、没有内存分配器，却要亲手把这些东西全部造出来。本目录按一条时间线组织四篇笔记，从上电一路走到用户态 init，全部内容以 **Linux 6.6** 源码为唯一基准（函数名、调用顺序、文件路径均已对照 v6.6 核验），x86_64 与 arm64 双架构并行讲解。

## 🗺️ 学习路线图

```text
第 1 章 内核启动总览            第 2 章 start_kernel 详解
上电 → 固件 → bootloader        C 世界奠基：setup_arch/percpu/mm/
→ 自解压 → 内核入口             sched/irq/time/console
        │                              │
        └──────────┬───────────────────┘
                   ▼
        第 3 章 从 rest_init 到用户空间
        pid 0/1/2、smp_init、initcall 长跑、
        initramfs、run_init_process
                   │
                   ▼
        第 4 章 initcall 与模块初始化机制
        __define_initcall 宏、8 级段布局、
        module_init 双形态、__init 段生命周期、deferred probe
```

建议顺序通读一遍，之后把第 2 章当"字典"用：每个子系统初始化函数的职责、依赖与观测手段都单独成节，便于在调试具体问题时反查。

## 📖 各章导读

### [内核启动总览](docs/01-内核启动总览.md)

* 全景分层时序图：固件 / bootloader / 自解压层 / 内核入口 / start_kernel / init；
* x86_64 逐段展开：MZ 双头 bzImage → 实模式 setup（`arch/x86/boot/main.c`）→ `startup_32/64` 解压器（`arch/x86/boot/compressed/head_64.S`、`extract_kernel()`、KASLR `choose_random_location()`）→ 内核 `.head.text` `startup_64` 与 `__startup_64()` 的 RIP 相对寻址修正 → `x86_64_start_kernel()`；
* arm64 逐段展开：Image 头与 boot protocol → `primary_entry` → `create_idmap` 恒等映射（VA_BITS/附加级别深挖）→ `__primary_switch`（开 MMU → KASLR → 正式内核映射）→ `__primary_switched` → `start_kernel`；
* 引导协议数据交接：boot_params/zero page、DTB 与 ACPI 双轨、earlycon 的最早输出；
* 两架构对照表（入口地址、页表建立者、固件接口、微码、secondary 唤醒）。

### [start_kernel 详解](docs/02-start_kernel详解.md)

* `init/main.c: start_kernel()` 按 9 个阶段全序列拆解（顺序与 v6.6 原文一致）；
* 重点深挖：`setup_arch()` 的 x86/arm64 双版本、`setup_per_cpu_areas()` 与 `.data..percpu`、`mm_core_init()`（`build_all_zonelists` → `mem_init` 的 memblock→buddy 交接 → `kmem_cache_init` 自举）、`sched_init()`（per-CPU rq、调度类链接期排序）、`early_irq_init()/init_IRQ()`、`timekeeping_init()/time_init()/tick_init()`（clocksource 注册链、jiffies 兜底、tsc/cntvct）、`rcu_init()`、workqueue 的"先登记后拉起"；
* `early_param/__setup` 宏的 `.init.setup` 段机制与两轮解析；
* printk 分阶段演进（earlycon → `__log_buf` → `setup_log_buf` → `console_init` → 回放）；
* 为什么在 console 前开中断、lockdep 依赖；
* 关键全局状态标志速查表（`early_boot_irqs_disabled`、`system_state` 等）。

### [从 rest_init 到用户空间](docs/03-从rest_init到用户空间.md)

* `rest_init()`：`user_mode_thread(kernel_init)`（pid 1，为何无 PF_KTHREAD）与 `kernel_thread(kthreadd)`（pid 2）的接棒顺序学；
* idle 任务深挖：每 CPU idle 线程、`do_idle()` 循环、cpuidle（poll/mwait/wfi）、RCU 静默期报告；
* `kernel_init_freeable()` 全程：smp_init 拉起 secondary CPU（x86 SIPI 与 arm64 PSCI/spin-table、cpuhp 状态机关联）、`do_basic_setup()`、console flush、rootfs 决策；
* initramfs：`populate_rootfs()` 异步解包、`unpack_to_rootfs`、`wait_for_initramfs`；传统 initrd 与 `prepare_namespace()` 两条根路径；switch_root 的本质（pid 1 换身体不换人）；
* `run_init_process() -> kernel_execve()` 尝试顺序与 `panic("No working init found")`；
* pid 0/1/2 角色定格表与用户态（systemd）视角。

### [initcall 与模块初始化机制](docs/04-initcall与模块初始化机制.md)

* `__define_initcall` 宏链逐层剥开（PREL32 相对偏移、LTO 子段、`static_assert` 签名约束）；
* `INIT_CALLS` 段布局与 8 级 initcall（early/pure/core/postcore/arch/subsys/fs/rootfs/device/late + sync/console）及典型使用者表；
* `do_initcalls -> do_one_initcall` 执行引擎：每级重跑 `parse_args` 的分级参数机制、preempt/irq 自检、latent entropy；
* `initcall_debug` / `initcall_blacklist` / `bootgraph.pl` / ftrace function_graph 实战定位启动慢驱动；
* `module_init` 双形态（内建 → initcall 段；模块 → `init_module` 别名 + `finit_module` 路径）；
* `__init` 段生命周期与 `free_initmem()`；模块为何必须"各自释放"（`init_free_wq`）；
* deferred probe（`-EPROBE_DEFER`）与 `devices_deferred` 观测。

### [async 异步基础设施](docs/05-async异步基础设施.md)

* sequence cookie 协议：u64 单调计数 + 双链表 + `wait_event` 不等式实现"乱序执行、顺序退休"（async.c:10-40 设计注释原话，含乱序 CPU 类比）；
* 调度/汇合/域三件套：`async_schedule*` 家族（NUMA 就近 + GFP_ATOMIC + ★MAX_WORK=32768 过载回落同步执行）、`async_synchronize_*` 四马甲归一、`ASYNC_DOMAIN(_EXCLUSIVE)` 的 registered 位与 fw_cache_domain 实例闭环；
* initcall 层级编排：`s` 段链接序机关（vmlinux.lds.h:908-911 同层殿后）、`main.c:1439` 总闸门的深层理由（`.init` 内存回收时序约束）；
* 异步探测三层开关（`probe_type` 三值 / `driver_async_probe=` cmdline / 模块 `async_probe=1`）与设备侧·驱动侧两路分发；
* ★`current_is_async()` 死锁警戒线：async 上下文禁止同步 `request_module`（kmod.c:143-148 死锁环原注释）。

## 🖥️ 双架构视角说明

本系列所有架构相关内容都同时给出 x86_64 与 arm64 两条线，理由有三：

1. **启动是架构差异最密集的区域**：实模式 vs 异常级、E820 vs DTB、解压器 vs head.S 直启、SIPI vs PSCI——只看单架构容易把"平台习惯"误当"通用设计"；
2. **两架构互相映照能暴露本质**：例如"谁建立早期页表、何时开 MMU"这一问，x86 答在解压器、arm64 答在 head.S，对照后才能看清"恒等映射自举"这一共同内核思想；
3. **实用主义**：服务器/桌面调试多在 x86_64，嵌入式/手机学习多在 arm64，读者迟早要在两个世界间迁移。

阅读时遇到单架构不熟悉的术语（如 APIC、EL2、TTBR），各章的对照表都给出了另一侧的对应物。

## 🔭 前置与关联笔记

* 建议先读 `mm/memblock/memblock.md`：启动早期内存管理的底层细节本系列仅在第 2 章 `setup_arch`/`mem_init` 处衔接；
* `mm/buddy/buddy.md`：第 2 章 `mem_init()` 交接的"接收方"；
* 后续可衔接调度器（`sched_init` 的延伸）、中断子系统（`init_IRQ` 的延伸）笔记。

## 📐 版本与体例说明

* **源码基准**：Linux 6.6（torvalds 主线 v6.6 原始源码逐文件核对，非发行版/vendor 树）；文中所有函数名、调用顺序、段名、文件路径均以 v6.6 为准，未标注版本的差异一律不引入；
* **体例**：代码块首行标注来源文件路径；调用链统一用 `func() -> func()` 格式；`★` 标记重点深挖子系统；每章结构为"概述 → 分阶段详解 → ASCII 流程图 → 深挖难点 → 观测与调试 → 思考题（附提示）"；
* **术语**：正文中文，术语与符号名保留英文原文；中文与英文之间、行内代码两侧均以空格分隔，便于阅读；
* **架构标注**：凡架构相关内容均显式标注 `x86_64` 或 `arm64`；未标注者为两架构通用路径（`init/main.c` 层）。

## 🛠️ 快速上手实验

```bash
# 无 bootloader 直观观察两个入口（QEMU 直接进入解压入口）
qemu-system-x86_64 -kernel arch/x86/boot/bzImage -initrd rootfs.cpio.gz \
    -append "console=ttyS0 earlycon=uart8250,io,0x3f8 initcall_debug" -nographic

# arm64
qemu-system-aarch64 -M virt -cpu cortex-a57 -kernel Image \
    -initrd rootfs.cpio.gz -append "console=ttyAMA0 earlycon=pl011,mmio32,0x09000000" -nographic

# 启动耗时分析
dmesg | grep "after" | sort -t' ' -k7 -rn | head
dmesg | perl scripts/bootgraph.pl > boot.svg
```
