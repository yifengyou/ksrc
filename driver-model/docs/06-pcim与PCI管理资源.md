# pcim：PCI 的托管资源方言（Linux 6.6）

> 阅读目标：剖析 devres 在 PCI 世界的化身——pcim 不是"逐 API 的 devm_ 包装"，而是**基础函数自带托管副作用**的设计（`find_pci_dr` 的 NULL 语义让 `pci_request_region/pci_intx` 在 managed 设备上自动记账）；吃透总账本 `struct pci_devres`（五位一 mask）、`pcim_release` 的**逆序清单**、`pcim_pin_device` 的逃生门语义（"won't be disabled on driver detach"）、`pcim_iomap` 家族的**表模式**（一张 devres 记全部 BAR）、以及 `pcim_iomap_regions_request_all` 的掩码补集技巧。全部对照 v6.6（drivers/pci/pci.c、lib/devres.c）。前置：[05-devres](05-devres设备资源管理.md)（母机制）。

---

## 一、两种托管设计：devm_ 包装 vs 基础函数接管

```
通用世界（lib/devres.c）：                PCI 世界（drivers/pci/pci.c）：
  devm_kmalloc = kmalloc + 挂账            pcim_enable_device 挂一个总账本
  devm_ioremap = ioremap + 挂账            此后**普通** pci_request_region /
                                             pci_intx / pci_set_mwi 在
                                             managed 设备上自动记账
                                           （find_pci_dr 返回 NULL 时零副作用）
```

头注释（pci.c:2089-2094 原话）：*"Managed PCI resources. This manages device on/off, INTx/MSI/MSI-X on/off and BAR regions. pci_dev itself records MSI/MSI-X status, so there's no need to track it separately."*——**总账本只记基类不自记的状态**。

## 二、总账本 pci_devres 与逆序清单（pci.c:2095-2122）

```c
struct pci_devres {
	unsigned int enabled:1;        /* 被 pcim_enable 打开过 */
	unsigned int pinned:1;         /* pin 逃生门 */
	unsigned int orig_intx:1;      /* intx 首改前的原值 */
	unsigned int restore_intx:1;   /* 需要还原 intx */
	unsigned int mwi:1;            /* 开过 MWI */
	u32 region_mask;               /* 请求过的 BAR 位图 */
};

static void pcim_release(struct device *gendev, void *res)
{
	for (i = 0; i < DEVICE_COUNT_RESOURCE; i++)
		if (this->region_mask & (1 << i))
			pci_release_region(dev, i);      /* ① BAR 逐个归还 */
	if (this->mwi)       pci_clear_mwi(dev);         /* ② MWI 清除 */
	if (this->restore_intx) pci_intx(dev, this->orig_intx);  /* ③ INTx 还原 */
	if (this->enabled && !this->pinned)
		pci_disable_device(dev);                       /* ④ 关设备
		                                                  （pinned 则跳过）*/
}
```

★一个 devres 条目、一个 release 回调、**四类资源的统一清单**——与 [05 章](05-devres设备资源管理.md) 的"每资源一 devres"对照：PCI 选择了聚合（因为这四类资源天然同生命周期：设备使能的伴生状态）。

## 三、接管机制：三处自动记账

### 3.1 BAR：pci_request_region / pci_release_region（:4019-4024 / :3978-3982）

```c
/* __pci_request_region 成功路径（:4019-4024） */
	dr = find_pci_dr(pdev);
	if (dr)
		dr->region_mask |= 1 << bar;      /* ★managed 才记账 */
/* pci_release_region 对称地 &= ~(1 << bar)（:3978-3980） */
```

### 3.2 INTx：pci_intx（:4643-4663）

```c
	if (new != pci_command) {            /* 配置字真变了才动账本 */
		pci_write_config_word(pdev, PCI_COMMAND, new);
		dr = find_pci_dr(pdev);
		if (dr && !dr->restore_intx) {   /* 只记首次 */
			dr->restore_intx = 1;
			dr->orig_intx = !enable;
		}
	}
```

### 3.3 MWI：pcim_set_mwi（:4567-4578）

`find_pci_dr` 后置 `dr->mwi = 1` 再调 `pci_set_mwi`——mwi 是唯一有 pcim_ 前缀的记账者（因为 `pci_set_mwi` 本身不需要托管版语义，仅标记）。

**find_pci_dr 的双形态**（:2138-2144）：`pci_is_managed(pdev)` 才查 devres，否则返回 NULL——所有"副作用"对非托管设备透明。**接管的原地性**：驱动代码从 `pci_enable_device` 换成 `pcim_enable_device` 一行，其余 `pci_request_region` 调用**原样保留**即获得托管——迁移成本最小化。

## 四、pcim_enable_device 与 pin 逃生门

```
pcim_enable_device（:2151-2169）：
  get_pci_dr（:2124-2136）——devres_find + 候补 alloc + devres_get
    （★[05 章] §三的"惰性注册"模式：未注册则造一个再原子占位）
  pci_enable_device(pdev);
  pdev->is_managed = 1;   /* ★旗标挂在 pci_dev 上——后续所有
                             find_pci_dr 走快路径 */

pcim_pin_device（:2179-2195）：
  dr = find_pci_dr(pdev);
  WARN_ON(!dr || !dr->enabled);       /* 没启用就 pin 是编程错误 */
  dr->pinned = 1;                     /* 注释原话："Pinned device won't
                                         be disabled on driver detach." */
```

pin 的场景：probe 成功后设备要跨 detach 存活（驱动热重载、devtmpfs 常驻的 console 设备）——release 清单的第 ④ 步被跳过，其余照常。

## 五、pcim_iomap 家族：表模式（lib/devres.c:318-505）

```c
#define PCIM_IOMAP_MAX	PCI_STD_NUM_BARS        /* :318 六根标准 BAR */
struct pcim_iomap_devres {                       /* :320-322 */
	void __iomem *table[PCIM_IOMAP_MAX];    /* ★一张表装全部 BAR */
};
static void pcim_iomap_release(...) { 遍历 table 逐 iounmap; }  /* :324 */
```

| API | 行为 | 行 |
| :--- | :--- | :--- |
| `pcim_iomap_table`（:348） | 惰性取得/创建那张表（devres_get 模式） | 返回 `void __iomem * const *` |
| `pcim_iomap(pdev, bar, len)`（:374） | `tbl[bar] = pci_iomap(...)`——**先查表防重复映射**（:380-382 注释原话 *"duplicate mappings not allowed"*，重复直接返回 NULL） | :374-386 |
| `pcim_iounmap`（:396） | 清表位 + 真正 iounmap | |
| `pcim_iomap_regions(pdev, mask, name)`（:423） | 逐 mask 位：`pci_request_region`（★普通函数——接管机制自动记账 §3.1）+ `pcim_iomap`；**自带手写回滚**（err_region/err_inval 逆序拆解） | :423-462 |
| `pcim_iomap_regions_request_all`（:476） | ★掩码补集：`request_mask = ((1 << 6) - 1) & ~mask`——把**没映射的** BAR 也请求掉（防其他驱动误碰），映射指定的 | :476-489 |
| `pcim_iounmap_regions`（:500） | 表位 + region 双清 | |

★表模式与 [05 章](05-devres设备资源管理.md) "每资源一 devres" 的对照：六个 BAR 共享一个 devres 条目——链表更短、释放一遍循环搞定、`pcim_iomap_table` 给驱动一个可索引的"我的映射都在这"句柄（很多老驱动直接拿着表用）。

## 六、观测实战

```bash
# 1) 设备是否 managed（pcim_enable 过）
#    无直接 sysfs 位；间接：驱动源码 grep pcim_enable_device
grep -r 'pcim_enable_device' drivers/net/ethernet/intel/

# 2) 谁在用表模式 / request_all
grep -r 'pcim_iomap_table\|pcim_iomap_regions_request_all' drivers/ | head

# 3) devres 全景（含 pcim 条目）——[05 章] 的观测法同样适用
echo 1 > /sys/module/devres/parameters/log   # CONFIG_DEBUG_DEVRES
dmesg | grep DEVRES | grep -i pcim

# 4) pin 语义的实证场景：console/早期设备驱动
grep -r 'pcim_pin_device' drivers/ | head    # 树内用户清点
```

## 七、思考题

1. `pcim_release` 的清单顺序是 BAR → MWI → INTx → disable——为什么 disable 必须最后？（提示：配置字访问与 disable 的次序；region 归还需要设备活着吗？）
2. `pci_request_region` 的记账副作用依赖 `pci_is_managed`——一个驱动混用 `pci_enable_device`（非托管）+ `pci_release_region` 会怎样？（提示：find_pci_dr 返回 NULL 的语义；泄漏还是正常？）
3. `pci_intx` 只记**首次**变更（`!dr->restore_intx`）——驱动多次切换 INTx 后 detach，还原到哪个状态？（对照 orig_intx 的一次性捕获。）
4. 表模式里 `pcim_iomap` 先查 `tbl[bar]` 防重复——如果两个驱动（或同驱动两处）对同一 BAR 两次 pcim_iomap，第二次会怎样？（提示：返回旧映射还是新映射？谁负责 iounmap？）
5. `pcim_iomap_regions_request_all` 的 `((1 << 6) - 1) & ~mask` 硬编码 6——这与 `PCIM_IOMAP_MAX/DEVICE_COUNT_RESOURCE` 的关系？（提示：PCI_STD_NUM_BARS=6；扩展 BAR（res[6..]）为何不在托管内？）
6. pin 过的设备在驱动 detach 后仍 enabled——下一个驱动 probe 它时 `pcim_enable_device` 的 `if (dr->enabled) return 0` 快路径会发生什么？（提示：账本跨驱动生命周期的正确性。）

---

> 回看：[05-devres 设备资源管理](05-devres设备资源管理.md)（母机制：FIFO/LIFO/分组染色）｜[02-总线-设备-驱动匹配](02-总线-设备-驱动匹配.md)（probe/detach 的时序宿主）｜[05-firmware 加载](04-firmware加载与request_firmware生命周期.md)（另一类 PCI 设备资源：固件）｜[trace/13-内核代码生成惯用法](../../trace/docs/13-内核代码生成惯用法.md)（对照：一份清单多种产物的"反向"——一份产物多份清单）
