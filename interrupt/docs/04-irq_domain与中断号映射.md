# irq_domain 与中断号映射

irq_domain 是 genirq 里"命名空间翻译层"：它把**控制器本地的硬件中断号（hwirq）**翻译成**全局唯一的 Linux IRQ number（virq）**。没有它，gpio 的 hwirq 5 和 GIC 的 hwirq 5 就无法区分；有了它，DT/ACPI 里的中断描述、PCIe 的 MSI 消息、软件模拟的 irq，全部统一到同一套 virq 语义下。

### 🎯 为什么需要 irq_domain

三个硬需求：

1. **级联（cascading）**：SoC 里 gpio 控制器自己是个设备，挂在 GIC 的某个 SPI 上；它的 32 个引脚号（hwirq 0–31）与 GIC 的 INTID 完全是两套编号。gpio domain 负责把"gpio hwirq"翻译成 virq，再通过父 domain 的翻译落到 GIC 的物理线上。
2. **大而稀疏的编号空间**：GICv3 ITS 的 LPI 从 8192 起可到上百万；MSI-X 每设备可要 2048 个中断。静态预留 irq_desc 数组不可想象，必须"翻译 + 按需分配"。
3. **固件中立**：DT 的 `interrupts` 属性、ACPI 的 `_CRS`/IORT、PCI 的 MSI Capability，都要找到"哪个控制器、哪个 hwirq、什么触发类型"。domain 用 `fwnode_handle`（DT 的 device_node 与 ACPI 节点的统一封装）做匹配键，翻译逻辑由各控制器的 `irq_domain_ops` 自定义。

两条核心查找路径（一正一反）：

```
正向(注册设备时):  固件描述(fwspec) → domain 匹配 → translate → alloc → virq
反向(中断到来时):  hwirq → irq_find_mapping(domain, hwirq) → virq → desc
```

### 🧾 struct irq_domain_ops 逐回调解读

```c
// include/linux/irqdomain.h (v6.6, 原文)
struct irq_domain_ops {
	int (*match)(struct irq_domain *d, struct device_node *node,
		     enum irq_domain_bus_token bus_token);
	int (*select)(struct irq_domain *d, struct irq_fwspec *fwspec,
		      enum irq_domain_bus_token bus_token);
	int (*map)(struct irq_domain *d, unsigned int virq, irq_hw_number_t hw);
	void (*unmap)(struct irq_domain *d, unsigned int virq);
	int (*xlate)(struct irq_domain *d, struct device_node *node,
		     const u32 *intspec, unsigned int intsize,
		     unsigned long *out_hwirq, unsigned int *out_type);
#ifdef	CONFIG_IRQ_DOMAIN_HIERARCHY
	/* extended V2 interfaces to support hierarchy irq_domains */
	int (*alloc)(struct irq_domain *d, unsigned int virq,
		     unsigned int nr_irqs, void *arg);
	void (*free)(struct irq_domain *d, unsigned int virq,
		     unsigned int nr_irqs);
	int (*activate)(struct irq_domain *d, struct irq_data *irqd, bool reserve);
	void (*deactivate)(struct irq_domain *d, struct irq_data *irq_data);
	int (*translate)(struct irq_domain *d, struct irq_fwspec *fwspec,
			 unsigned long *out_hwirq, unsigned int *out_type);
#endif
#ifdef CONFIG_GENERIC_IRQ_DEBUGFS
	void (*debug_show)(struct seq_file *m, struct irq_domain *d,
			   struct irq_data *irqd, int ind);
#endif
};
```

两代接口并存：

**总线令牌（bus_token）小字典**——`select`/匹配的第二把钥匙，同一 fwnode 下区分不同用途的 domain：

| token | 用途 |
| :--- | :--- |
| `DOMAIN_BUS_ANY` | 万能兜底（查询顺序：先精确后 ANY） |
| `DOMAIN_BUS_WIRED` | 传统引脚中断（`irq_create_fwspec_mapping` 的首选查询） |
| `DOMAIN_BUS_PCI_MSI` / `_MSIX` | PCI MSI 分配 |
| `DOMAIN_BUS_DEVICE_MSI` | 平台设备 MSI（device msi） |
| `DOMAIN_BUS_FSL_MC_MSI` 等 | 总线特化变体 |

这就是 MSI 场景一个 fwnode 能同时服务" wired 引脚"与"MSI 消息"两套 domain 的机制基础。

**V1（非层级，`map`/`xlate` 体系）**：
- `match`/`select`：domain 匹配。`match` 拿裸 of_node 比较（老接口）；`select` 拿 `irq_fwspec`（含 fwnode + 参数数组 + bus_token）做精细匹配（新接口）。缺省实现按 fwnode 指针相等匹配；MSI domain 常按 bus_token（DOMAIN_BUS_PCI_MSI 等）区分。
- `xlate`：解码 DT cells。标准两-cell 格式 `<parent-hwirq flags>` 由 `irq_domain_xlate_twocell()` 通用实现（flags 的低 4 位即触发类型）；GIC 的三-cell 格式（GIC type + hwirq + flags）由 `gic_irq_domain_xlate` 类函数处理。
- `map`/`unmap`：virq↔hwirq 映射建立/拆除时回调，控制器驱动在此 `irq_domain_set_info()` 挂 chip、设 flow handler。**map 是纯软件操作**（填 irq_desc），不碰硬件。

**V2（层级，`translate`/`alloc`/`activate` 体系）**：
- `translate`：fwspec（不再限于 DT cells，ACPI/MSI 也构造它）→ hwirq + 触发类型。是 xlate 的泛化。
- `alloc`/`free`：分配/释放该 domain 层次上的中断资源（见层级一节）。**只要 ops 实现了 `alloc`，domain 自动被标记 `IRQ_DOMAIN_FLAG_HIERARCHY`**（`irq_domain_check_hierarchy()`）。
- `activate`/`deactivate`：**真正写硬件**（program the chip），把已分配的资源下发。GIC 的 activate 会为 hwirq 设 group/优先级/路由，gpio 的 activate 会配置引脚复用为中断输入。
- `debug_show`：debugfs 展示 domain 私有信息。

### 🗄️ revmap 三模式

domain 创建时按 hwirq 空间选 revmap（反向映射表）实现：

```c
// kernel/irq/irqdomain.c (v6.6, revmap 查找核心)
/* Check if the hwirq is in the linear revmap. */
if (hwirq < domain->revmap_size)
	data = rcu_dereference(domain->revmap[hwirq]);   /* 线性直取 */
else
	data = radix_tree_lookup(&domain->revmap_tree, hwirq);
```

| 模式 | 创建方式 | 内部结构 | 适用 |
| :--- | :--- | :--- | :--- |
| **linear** | `irq_domain_create_simple(fwnode, size, ...)` | `revmap[]` 柔性数组（附在 domain 尾部），size 通常 ≤ 256 | gpio bank、小中断控制器 |
| **tree** | `irq_domain_create_tree(fwnode, ops, host_data)` | radix tree + RCU | ITS/LPI、MSI、hwirq 巨大或稀疏 |
| **legacy** | `irq_domain_add_legacy(of_node, size, first_irq, first_hwirq, ...)` / `irq_domain_create_legacy` | 直接把 virq 与 hwirq 按 `first_irq + i`/`first_hwirq + i` 静态绑死，并**预分配** desc | x86 ISA 兼容（irq 0–15）、旧平台驱动写死中断号的兼容层 |

线性模式下中断到来时的翻译成本是一次数组下标读（cache 友好）；tree 模式是 radix 查找（log 级，但 RCU 无锁）。`irq_domain_create_linear` 本质是"tree + 全量线性表"的优化特例：`__irq_domain_add()` 里 `struct_size(domain, revmap, size)` 一次分配，`domain->revmap_size = size`。

**映射生命周期 API**：

- `irq_create_mapping(domain, hwirq)`（内部 `irq_create_mapping_affinity`）：V1 风格——查 `irq_find_mapping`，没有则 `irq_domain_alloc_descs()` 分配空闲 virq、调 `ops->map`、插入 revmap。返回 virq。
- `irq_find_mapping(domain, hwirq)`：只查不建，查不到返回 0。中断热路径（`generic_handle_domain_irq`）用它。
- `irq_dispose_mapping(virq)`：拆映射——`ops->unmap`、从 revmap 删除、释放 desc（RCU 延迟）。`free_irq()` 不自动 dispose（映射可被再次 request 复用），显式 dispose 是设备移除时的驱动责任。

### 🌳 建立映射的三条固件路径

#### 1. Device Tree：of_irq_get → irq_create_fwspec_mapping

```c
// drivers/of/irq.c (v6.6, 原文节选)
int of_irq_get(struct device_node *dev, int index)
{
	struct of_phandle_args oirq;
	struct irq_domain *domain;

	rc = of_irq_parse_one(dev, index, &oirq);   /* 解析 interrupts = <&intc N flags> */
	domain = irq_find_host(oirq.np);            /* phandle 指向的控制器 → domain */
	if (!domain)
		return -EPROBE_DEFER;               /* 控制器还没 probe, 稍后再试 */

	return irq_create_of_mapping(&oirq);
}
```

`irq_create_of_mapping()` 把 `of_phandle_args` 包装成 `irq_fwspec`，交给核心：

```c
// kernel/irq/irqdomain.c (v6.6, 原文节选)
unsigned int irq_create_fwspec_mapping(struct irq_fwspec *fwspec)
{
	if (fwspec->fwnode) {
		domain = irq_find_matching_fwspec(fwspec, DOMAIN_BUS_WIRED);
		if (!domain)
			domain = irq_find_matching_fwspec(fwspec, DOMAIN_BUS_ANY);
	} else
		domain = irq_default_domain;

	if (irq_domain_translate(domain, fwspec, &hwirq, &type))  /* ops->translate */
		return 0;

	mutex_lock(&domain->root->mutex);

	virq = irq_find_mapping(domain, hwirq);       /* 幂等: 已有映射直接复用 */
	if (virq) {
		if (type == IRQ_TYPE_NONE ||
		    type == irq_get_trigger_type(virq))
			goto out;
		/* ...类型不匹配的修正/报错路径... */
	}

	if (irq_domain_is_hierarchy(domain))
		virq = irq_domain_alloc_irqs_locked(domain, -1, 1,
						    NUMA_NO_NODE, fwspec, false, NULL);
	else
		virq = irq_create_mapping_affinity_locked(domain, hwirq, NULL);

	irqd_set_trigger_type(irq_data, type);        /* 记录触发类型 */
	mutex_unlock(&domain->root->mutex);
	return virq;
}
```

平台驱动 `platform_get_irq()` → `of_irq_get()`；通用代码 `of_irq_parse_one` 处理 interrupt-map（OpenPIC 风格的硬件路由表）和 interrupt-parent 链。**注意幂等性**：同一 hwirq 重复映射会复用既有 virq，这是多驱动共享 / 重试 probe 不炸的保证；触发类型冲突则显式告警拒绝。

#### 2. ACPI：acpi_register_gsi / acpi_gsi_to_irq

```c
// drivers/acpi/irq.c (v6.6, 原文节选)
int acpi_gsi_to_irq(u32 gsi, unsigned int *irq)
{
	struct irq_domain *d;

	d = irq_find_matching_fwnode(acpi_get_gsi_domain_id(gsi),
				     DOMAIN_BUS_ANY);
	*irq = irq_find_mapping(d, gsi);
	/* 无映射则尝试 arch fallback */
	return (*irq > 0) ? 0 : -EINVAL;
}
```

ACPI 世界里 GSI（Global System Interrupt）就是根控制器的 hwirq。注册方向 `acpi_register_gsi()` 构造 `irq_fwspec`（fwnode 是 MADT/GIC build 出来的）走 `irq_create_fwspec_mapping()`；查询方向 `acpi_gsi_to_irq()`（如 GHES 错误源用它）只查 `irq_find_mapping`。IORT 表负责把 ITS group/PCI RC 映射到对应 domain——所以 ACPI 与 DT 最终在 `irq_fwspec` 这一层汇合。

#### 3. PCI MSI：msi domain 与 GIC-ITS

MSI 不走"线"，设备直接写 Memory Write（地址=doorbell，数据=vector）。它的 domain 链是：

```
per-device msi domain (irq_domain_create_simple, ops=msi_domain_ops)
    ↓ parent
ITS domain (its_init → irq_domain_create_tree, ops=its_domain_ops)
    ↓ (ITS 即根, LPI 由硬件路由)
```

分配时 `msi_domain_alloc_irqs()` → `__irq_domain_alloc_irqs(domain, -1, nvec, node, &stacked_fwspec, false, affdesc)`：先在 per-device domain 分配 virq，其 `alloc` 回调（`msi_domain_alloc`）逐层 `irq_domain_alloc_irqs_parent()` 向 ITS 申请 LPI 号，ITS 再分配 event id 并建立 Device Table→ITT 链。`irq_compose_msi_msg`/`irq_write_msi_msg` 回调把 doorbell 地址与 event id 写回设备 MSI-X Table。亲和性变更时 ITS 只改 Collection Table（目标 Redistributor），**设备无感知**——这是 MSI 天然亲和性友好、被高速设备青睐的根本原因（第 6 章）。

一个具体的 MSI-X 分配时序（NVMe 每队列一个中断）：

```
驱动: pci_alloc_irq_vectors(pdev, minvec, maxvec, PCI_IRQ_MSIX | PCI_IRQ_AFFINITY)
 → msi_domain_alloc_irqs(msi_domain, dev, nvec)
    per-device domain alloc: 从全局分配 nvec 个 virq(__irq_alloc_descs),
                             每个 virq 绑一个 msi_desc(写进 irq_common_data)
    → irq_domain_alloc_irqs_parent → ITS alloc:
         从 LPI bitmap (lpi_range) 挑 nvec 个 LPI 号(hwirq)
         为设备 DeviceID 建 ITT, 每个 LPI 一个 ITT entry
         event id = LPI 号 → msi msg data
    activate 阶段(irq_startup 时):
         ITS: 映 ITT entry → collection(目标 Redistributor)
         compose/write_msi_msg: doorbell 地址 + data 写设备 MSI-X Table[i]
 设备此后每次触发 = 写这个 doorbell → ITS 查表 → 目标核收 LPI
```

关键理解：**MSI 的"中断号"完全活在内存表里**。改路由 = 改 ITS 的表（软件可见），设备侧的 msg 不动；这与 IO-APIC 要改重定向寄存器、且 mask 粒度受限形成鲜明对比。LPI 的 mask 也特殊——GICv3 LPI 没有 per-LPI mask 寄存器，oneshot/禁用语义靠 ITS 的表操作或配置（这也解释了为什么 MSI 芯片多声明 `IRQCHIP_ONESHOT_SAFE` 的变体语义）。

### 🏗️ 层级 domain：分配与激活

层级化的典型：`gpiochip → GIC`。gpio 驱动通过 `irq_domain_create_hierarchy(parent, flags, size, fwnode, &girq->child_to_parent_hwirq..., ...)`（`drivers/gpio/gpiolib.c`）建立 child domain，其 `alloc` 实现是 `gpiochip_hierarchy_irq_domain_alloc()`。

**第一步：分配（叶→根，软件资源）**

```c
// kernel/irq/irqdomain.c (v6.6, 原文节选)
static int irq_domain_alloc_irqs_locked(struct irq_domain *domain, ...)
{
	virq = irq_domain_alloc_descs(irq_base, nr_irqs, 0, node, affinity);
	/* ↑ 从全局分配 virq 段 (irq_desc 侧, 见下文 __irq_alloc_descs) */

	irq_domain_alloc_irq_data(domain, virq, nr_irqs);
	/* ↑ 沿 parent 链为每层 domain 各插入一个 irq_data:
	 *   最外层(叶) irq_data 内嵌于 irq_desc,
	 *   每个父层 new 一个, 用 parent_data 串成链 */

	ret = irq_domain_alloc_irqs_hierarchy(domain, virq, nr_irqs, arg);
	/* ↑ 调【叶】domain 的 ops->alloc —— gpio 的 alloc 内部会
	 *   irq_domain_alloc_irqs_parent() 调【父】GIC 的 ops->alloc,
	 *   逐级向上。每层的 alloc 各自认领自己层次的资源:
	 *   gpio 挑一个空闲 gpio hwirq, GIC 挑父 hwirq/配置分发 */

	for (i = 0; i < nr_irqs; i++)
		irq_domain_insert_irq(virq + i);   /* 插入每层 revmap */
	return virq;
}
```

注意**分配的执行方向**：调用从叶子 domain 发起，叶的 `ops->alloc` 先跑，内部 `irq_domain_alloc_irqs_parent()` 逐级向上触发父的 alloc。资源认领沿 **child→parent** 传播（叶先根后）。

**第二步：激活（根→叶，写硬件）**

```c
// kernel/irq/irqdomain.c (v6.6, 原文节选)
static int __irq_domain_activate_irq(struct irq_data *irqd, bool reserve)
{
	if (irqd && irqd->domain) {
		struct irq_domain *domain = irqd->domain;

		if (irqd->parent_data)
			ret = __irq_domain_activate_irq(irqd->parent_data, reserve);
		                          /* ① 先递归激活【根】(父) */
		if (!ret && domain->ops->activate)
			ret = domain->ops->activate(domain, irqd, reserve);
		                          /* ② 再激活自己(子) */
		...
	}
}
```

`irq_domain_activate_irq(irq_data, reserve)` 从**最外层（叶）irq_data** 进入，但递归保证**根 domain 的 `ops->activate` 先执行、叶最后执行**——方向与分配相反。为什么？父（GIC）必须先把物理路由配好，子（gpio）的 activate（引脚复用成中断输入、edge/level 配置）才有意义。`reserve=true` 是 x86 vector 的"预约"模式（`irqd_can_reserve`）：启动阶段只占坑不写死，等 activate 落实。

这个两步设计（分配=纯软件记账，激活=写硬件）的价值在**回滚**：分配失败只需逐层 free 软件资源；激活失败只 deactivate，不泄漏硬件状态。同时也是 CPU hotplug 与 managed IRQ 的基石（shutdown=deactivate，online=activate）。

### 🔗 request_threaded_irq 到 domain alloc 的完整路径

把第 1、6 章的注册路径与本章的翻译路径拼起来（含 x86 的 vector 分配）：

```
request_threaded_irq(irq, ...)                    kernel/irq/manage.c
  → irq_to_desc(irq)                              (virq 已存在——映射早已建好)
  → __setup_irq(irq, desc, action)
      → (若 IRQ 没启动) irq_startup(desc, ...)
          → __irq_startup(desc)
              → irq_domain_activate_irq(&desc->irq_data, false)
                  → __irq_domain_activate_irq → 各层 ops->activate
                    x86:  assign_irq_vector_irq()   (APIC vector 分配, arch)
                    arm64: gic_irq_domain_alloc 已在 alloc 阶段完成路由,
                          activate 检查/编程        (GIC 的 activate 实现)

而 virq 本身从哪来? 平台设备 probe 时:
platform_get_irq() → of_irq_get() → irq_create_of_mapping()
  → irq_create_fwspec_mapping() → irq_domain_alloc_irqs_locked()
      → irq_domain_alloc_descs(desc 域的分配器):
          → __irq_alloc_descs(-1, from, cnt, node, NULL, of_node)  kernel/irq/irqdesc.c
              → alloc_descs() → alloc_desc(irq, node, ...)
                    kzalloc_node(sizeof(irq_desc))          (maple tree 挂载)
                    desc->kstat_irqs = alloc_percpu(...)     (percpu 统计)
      → irq_domain_alloc_irq_data()   (建 per-层 irq_data 链)
      → irq_domain_alloc_irqs_hierarchy() (叶→根 alloc)
      → irq_domain_insert_irq()        (各层 revmap 插入)

MSI 设备则由 pci_alloc_irq_vectors() → msi_domain_alloc_irqs() 发起,
同样落进 __irq_domain_alloc_irqs(),只是 domain 换成 per-device msi domain。
```

`__irq_alloc_descs()`（irqdesc.c）是**全局 virq 号码分配器**：从 `from` 起找 `cnt` 个连续空闲号（managed 分配用 `alloc_masks` 按 NUMA/亲和约束挑），经 `bitmap` 分配器（`allocated_irqs`）记账，然后 `alloc_descs()` 真正造 desc 并插入全局 maple tree `sparse_irqs`。**virq 的分配策略（哪个号）与 hwirq 的翻译（哪条线）完全解耦**——这就是第 1 章"分离动机"的代码落点。

补充一个容易忽视的细节：`__irq_alloc_descs()` 的调用者分两类——irq_domain 路径传 `irq_base=-1`（让分配器自由挑号，现代默认）；legacy 路径传固定 base（绑定老号码段）。而 `struct irq_affinity_desc` 参数（`affinity`）只在 managed 场景传入：为每个待分配的 virq 指定一个目标 CPU 掩码——网卡 RSS 的 per-queue 亲和在**分配时**就已定型（`IRQD_AFFINITY_MANAGED`），而不是事后设置。这也解释了第 6 章 managed IRQ "不能手改亲和性"的来源：掩码在 desc 诞生那一刻就写进了基因。

### 🐛 观测与调试

- **debugfs**（`kernel/irq/irqdomain.c` + `kernel/irq/debugfs.c`）：
  ```
  /sys/kernel/debug/irq/domains/          # 所有 domain + 层级 + revmap 状态
  /sys/kernel/debug/irq/domains/default
  /sys/kernel/debug/irq/irqs/             # 每 IRQ: handler/chip/状态位/affinity
  ```
  历史上曾有独立的 `CONFIG_IRQ_DOMAIN_DEBUGFS`，**6.6 中已合并**：两处都由 `CONFIG_GENERIC_IRQ_DEBUGFS` 控制（`irq_debugfs_init()` 是 `__initcall`，同时调用 `irq_domain_debugfs_init`）。

  `irqs/N` 的典型输出（arm64 GIC-ITS 上的 NVMe 队列中断）：

  ```
  handler:  handle_fasteoi_irq
  chip:     ITS
  Status:   0x00004108                    ← 位图逐项展开(IRQD_*)
            IRQD_LEVEL
            IRQD_AFFINITY_MANAGED
            IRQD_IRQ_STARTED
  Affinity: 0-3                           ← 请求掩码
  Effectiv: 2                             ← 实际投递核
  Pending:  (empty)
  Action:   nvme0q2                       ← action 链(共享时多行)
  ```

  排障时把 `domains/` 的 domain 层级树和 `irqs/N` 的状态对照看：irq_data 链每层一个 chip，`/sys/kernel/debug/irq/irqs/N` 展示的是**最外层**；层级中间层的状态要靠各控制器驱动的 debug_show。

- `/proc/interrupts` 第一列即 virq，"irq domain 名"出现在 chip 列；`/proc/irq/N/` 提供每 IRQ 视角。
- 常见排障：`"no irq domain found for /soc/xxx"` → 控制器驱动没 probe（EPROBE_DEFER 竞态）；`"type mismatch, failed to map hwirq"` → DT cells 与已有映射的触发类型打架；`"error: hwirq 0x%x is already allocated"` → 双重映射 bug。

---

### 💭 思考题

1. 为什么 `irq_create_fwspec_mapping()` 要先 `irq_find_mapping()` 查一遍再分配？如果直接分配会发生什么？
   <details><summary>提示</summary>同一 hwirq 可能被多个设备节点引用（共享）或 probe 重试触发多次 of_irq_get；重复分配 virq 会让 revmap 与 irq_desc 出现两个"真身"。</details>
2. 层级 domain 中"分配沿叶→根、激活沿根→叶"的顺序若颠倒（激活时叶先根后），举一个具体的坏结果。
   <details><summary>提示</summary>gpio 的 activate 若先把引脚配成中断输入，而 GIC 的父 hwirq 路由还没配好：引脚边沿触发时中断无处投递或投到错误核——"半激活"窗口丢中断。</details>
3. MSI 的亲和性切换为什么可以完全不碰设备寄存器？哪个结构让"改路由"变成了纯控制器侧操作？
   <details><summary>提示</summary>MSI 消息内容不变，doorbell 地址映射到 ITS 的 event id；改的只是 ITS Collection Table 中 event→Redistributor 的指向。</details>
4. legacy domain 为什么在创建时就要"预分配"整段 desc，而 linear/tree 是懒分配？
   <details><summary>提示</summary>legacy 的存在意义就是兼容写死 virq 的老驱动/老 ISA 语义（如 irq 0 定时器），virq 空间必须在 domain 创建时即已就位。</details>
5. `irq_dispose_mapping()` 之后 RCU 宽限期前，正被另一个 CPU 处理的同号中断会发生什么？哪条代码路径保证了它不踩空？
   <details><summary>提示</summary>中断入口先 `irq_find_mapping`（RCU 读临界区内）拿 irq_data 指针；desc 释放走 call_rcu；flow handler 持有的 desc 在 grace period 内仍然有效。</details>
6. `irq_domain_alloc_irqs_locked()` 的注释把 setup 拆成 alloc/activate 两步的理由是"更容易回滚"。举一个 activate 失败需要精确回滚的场景。
   <details><summary>提示</summary>x86 vector 紧张时：alloc 成功（软件记账完成），activate 里 assign vector 失败——若没有两步设计，desc 里已挂的 chip/irq_data 状态与硬件不一致；现在只需 deactivate 逐层撤销，desc 可整体释放重用。</details>

---

*上一章：[中断处理流程](03-中断处理流程.md)　|　下一章：[softirq 与 tasklet](05-softirq与tasklet.md)*
