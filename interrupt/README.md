# 中断子系统（Interrupt Subsystem）

基于 **Linux 6.6** 的中断子系统深度笔记。覆盖从硬件中断控制器（x86 APIC 体系 / arm64 GIC 体系）到设备驱动 handler 的完整链路：genirq 通用层、irq_domain 翻译层、highlevel flow handlers、softirq/tasklet 下半部、线程化中断与多核亲和性工程。

## 🗺️ 学习路线图

```
第 1 章 总体架构          ← 先读: 建立分层世界观, 认识两大家族 API
   │
第 2 章 核心数据结构      ← 五个结构体逐字段精读 (对照内核源码)
   │
   ├────────────┐
第 3 章 处理流程          第 4 章 irq_domain
(动态: 中断来了怎么跑)    (静态: 中断号从哪来)
   │                        │
   └────────────┬───────────┘
第 5 章 softirq/tasklet   ← 下半部机制谱系
   │
第 6 章 线程化与亲和性    ← 现代驱动的正确姿势 + 多核调优实战
```

建议顺序：1 → 2 → 3/4（可互换，一个沿"运行时"走，一个沿"注册时"走）→ 5 → 6。每章结尾的思考题是深度检验，建议至少自答一半再进入下一章。

## 📚 各章导读

### [01 - 中断子系统总体架构](docs/01-中断子系统总体架构.md)

分层全景：硬件中断控制器 → 架构入口 → genirq core → 驱动 handler。理解 genirq 最重要的设计决策——**flow handler（流控编排）与 chip callback（芯片操作）的正交分离**；irq_desc 的演进史（静态数组 → SPARSE_IRQ → 6.6 的 maple tree）；中断上下文的四条铁律；`request_irq` 家族 API 的选型决策树。适合反复回看的一章——后面所有细节都挂在这张骨架上。

### [02 - 核心数据结构](docs/02-核心数据结构.md)

`irq_desc` / `irq_common_data` / `irq_data` / `irq_chip` / `irqaction` / `irq_domain` 的 6.6 真实定义与逐字段解读。重点搞清三个"贯穿性设计"：irq_data 内嵌与 parent_data 层级链、四层状态位体系（IRQ_* / IRQD_* / IRQS_* / IRQCHIP_*）、`kstat_irqs` per-CPU 统计如何变成 `/proc/interrupts`。

### [03 - 中断处理流程](docs/03-中断处理流程.md)

一条中断的端到端调用链（x86 与 arm64 双路径，带文件路径）。三大 flow handler 的对决——电平的"先闷住"、边沿的"ack 后循环收割"、fasteoi 的"结尾一次 EOI"；`handle_irq_event` 的 action 链遍历与 `IRQ_WAKE_THREAD` 交接；中断退出与软中断派发的时机判定；spurious 检测的 100000/99900 算法与 misrouted 轮询兜底；各阶段开销的数量级分析（cache miss、EOI 广播、IPI）。

### [04 - irq_domain 与中断号映射](docs/04-irq_domain与中断号映射.md)

hwirq → virq 翻译层。`irq_domain_ops` 两代接口（map/xlate 与 translate/alloc/activate）；revmap 三模式（linear/radix/legacy）；三条固件路径——DT 的 `of_irq_get` → `irq_create_fwspec_mapping`、ACPI 的 `acpi_register_gsi`/`acpi_gsi_to_irq`、PCI MSI 的 msi domain → GIC-ITS 链；层级 domain 的**两步走**：分配沿叶→根（`irq_domain_alloc_irqs_parent`）、激活沿根→叶（`irq_domain_activate_irq`）；从 `request_threaded_irq` 一路回溯到 `__irq_alloc_descs` 的完整注册路径。

### [05 - softirq 与 tasklet](docs/05-softirq与tasklet.md)

`__softirq_pending` per-CPU 位图与 10 个软中断编号；`__do_softirq` 的双重预算机制（2ms + 10 轮）与 ksoftirqd 移交协议；ksoftirqd 为什么必须是 SCHED_OTHER；`local_bh_disable` 与 preempt_count 的奇偶语义（SOFTIRQ_DISABLE_OFFSET = 2×SOFTIRQ_OFFSET）；tasklet 的 state/count 机制与它被淘汰的五个理由；softirq vs tasklet vs workqueue vs threaded irq 四方对比表。

### [06 - 线程化中断与中断亲和性](docs/06-线程化中断与中断亲和性.md)

`irq/NN-name` 线程的完整交接协议（IRQTF_RUNTHREAD、6.6 起的 SCHED_FIFO 50）；IRQF_ONESHOT 的 `threads_oneshot` 位图与 `irq_finalize_oneshot` 锁序；PREEMPT_RT 的 force-threading 与硬中断最小化哲学；亲和性设置链路、意图/实效掩码分离、pending move 的安全迁移窗口；managed IRQ 与 CPU hotplug 的 `migrate_one_irq`；多核实战（RSS 配对、irqbalance、/proc/interrupts 诊断三板斧）与观测工具箱（ftrace/irqsoff/debugfs/lockdep）。

## 🖥️ 与硬件架构的对应

| 概念 | x86 | arm64 |
| :--- | :--- | :--- |
| 根控制器 | Local APIC（每 CPU）+ IO-APIC | GICv3 Distributor + Redistributor |
| 消息中断 | MSI/MSI-X（写 APIC 地址） | LPI（ITS 硬件表遍历） |
| 架构入口 | IDT → `DEFINE_IDTENTRY_IRQ(common_interrupt)` → `vector_irq[]` | `handle_arch_irq` → `gic_handle_irq` → `generic_handle_domain_irq` |
| ack/EOI | IO-APIC mask+ack / x2APIC MSR 写 EOI | 读 `ICC_IAR1_EL1` / 写 `ICC_EOIR1_EL1` |
| 翻译层 | vector 管理（`irq_matrix`）+ irq_domain | GIC domain + ITS domain（层级） |
| per-CPU 中断 | LOC timer、RES/CAL/TLB IPI | PPI（arch timer）、SGI |
| 特殊 | NMI（`DEFINE_IDTENTRY_NMI`）、posted interrupt | pseudo-NMI（FIQ + PMR 优先级） |

## 🔭 快速观测速查（学习时随手验证）

```bash
cat /proc/interrupts            # 每 IRQ per-CPU 计数 + 架构行
cat /proc/softirqs              # 软中断 per-CPU 计数
ls /proc/irq/                   # 每 IRQ 目录: smp_affinity(_list), spurious, node
ls /sys/kernel/debug/irq/       # CONFIG_GENERIC_IRQ_DEBUGFS: domains/ + irqs/
ps -e | grep 'irq/'             # 线程化中断线程 (irq/NN-name)
perf record -e irq:irq_handler_entry -a sleep 5   # tracepoint
echo irqsoff > /sys/kernel/tracing/current_tracer # 最长关中断追踪
```

## 📖 延伸阅读（本仓库内 .rst）

- [core-api/genericirq.rst](../core-api/genericirq.rst) —— 内核官方的 Generic IRQ Guide，本笔记第 1–3 章的权威对照（flow handler 章节尤其值得并读）
- [PCI/msi-howto.rst](../PCI/msi-howto.rst) —— MSI/MSI-X 驱动接口，配合第 4 章 msi domain 一节
- [PCI/boot-interrupts.rst](../PCI/boot-interrupts.rst) —— legacy 8259/SCI boot interrupt 的历史包袱
- [power/suspend-and-interrupts.rst](../power/suspend-and-interrupts.rst) —— IRQ 与 suspend/resume/wakeup（对应 `IRQD_WAKEUP_STATE`、`wake_depth`）
- [RCU/NMI-RCU.rst](../RCU/NMI-RCU.rst) —— NMI 上下文的特殊性（对应第 6 章 `IRQS_NMI`）
- [driver-api/gpio/](../driver-api/gpio/) —— gpio irqchip 的层级 domain 实战样本（第 4 章 gpiochip→GIC 一节的具体化）
- [admin-guide/kernel-parameters.rst](../admin-guide/kernel-parameters.rst) —— `threadirqs`、`irqpoll`、`noirqdebug` 等参数官方说明
- [trace/events.rst](../trace/events.rst) —— tracepoint 使用（`irq:irq_handler_entry` 等）

## 🧭 阅读源码的入口

```
include/linux/irq.h             irq_chip / irq_data / IRQD_*
include/linux/interrupt.h       irqaction / IRQF_* / softirq / tasklet
include/linux/irqdesc.h         irq_desc
include/linux/irqdomain.h       irq_domain / irq_domain_ops
kernel/irq/chip.c               highlevel flow handlers ★
kernel/irq/handle.c             handle_irq_event / __irq_wake_thread
kernel/irq/manage.c             request_* 家族 / irq_thread / affinity ★
kernel/irq/irqdomain.c          翻译/分配/激活 ★
kernel/irq/irqdesc.c            desc 分配 (maple tree sparse_irqs)
kernel/irq/spurious.c           spurious 检测与轮询
kernel/irq/cpuhotplug.c         migrate_one_irq / managed IRQ
kernel/softirq.c                __do_softirq / ksoftirqd / tasklet 引擎
arch/x86/kernel/irq.c           common_interrupt
drivers/irqchip/irq-gic-v3.c    gic_handle_irq / ITS
```
