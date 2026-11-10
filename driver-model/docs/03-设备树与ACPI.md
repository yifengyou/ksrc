# 设备树与 ACPI（Linux 6.6）

> 阅读目标：掌握 DTB 的二进制布局与 unflatten 成内核对象树的过程；吃透 of_ API 的匹配-取值族；理解 ACPI 的表组织（DSDT/FADT/MADT）与 _CRS 资源翻译；画出 DT 与 ACPI 在"描述-发现-绑定"三段与上一章模型的映射。

---

## 一、设备树（DT）：数据描述一切

```
源码（.dts/.dtsi）--dtc--> DTB（扁平二进制，固件传递 boot 参数）：
  header{magic 0xd00dfeed, totalsize, off_dt_struct/off_dt_strings...}
  结构区：FDT_BEGIN_NODE ... FDT_PROP(nameoff/len/data) ... FDT_END_NODE
  strings 区：属性名去重池

内核展开（drivers/of/fdt.c）：
  unflatten_device_tree → __unflatten_device_tree
    early_init_dt_scan（boot 期先扫 chosen/memory 等根属性——init 分册启动链）
    → 构建运行时 device_node 树（property 链表，OF_DYNAMIC 可改）
```

```
运行时对象（include/linux/of.h）：
  struct device_node{ name, phandle, parent/child/sibling,
                      struct property *properties, kobject kobj（/proc/device-tree 同步）}
  struct property{ name, length, value, next }
```

### 1.1 驱动侧的匹配与取值 API

```
匹配：drv->of_match_table = of_device_id[]（compatible 优先，由
      bus->match（上一章）里的 of_driver_match_device 调用）
取值族（of_.*，返回 0 成功——EPROBE_DEFER 友好）：
  of_get_property/of_find_property           原始属性
  of_property_read_u32/u64/string/index      标量（reg/interrupts/gpios）
  of_irq_get/of_get_named_gpio? → gpiod（6.6 用 gpiod_get_index 走
      of_mdio/... 的桥接层）——本章只讲 of_ 基座，具体子系统各有包装
  of_iomap/of_address_to_resource            "reg" → 资源
  of_platform_populate                       ★把 DT 子树实例化成 platform
                                            设备（of_device，bus=platform）
  of_parse_phandle(+args)                    ★引用的解析（gpios=<&gpio 5 0>：
                                            phandle+参数元组的通用协议）
```

**phandle 是 DT 的指针**：跨节点引用（clock/gpio/iommu）全部经它——`of_parse_phandle` 解析、被引用方以 `#xxx-cells` 声明参数元数。

## 二、ACPI：可执行的字节码

```
表族（RSDP→XSDT→各表，boot 期 acpi_table_init 解析）：
  DSDT/SSDT   ★AML 字节码：设备对象（_HID/_ADR）与方法（_STA/_CRS/_DSM...）
  FADT        固定硬件寄存器（电源按钮/睡眠）
  MADT        中断控制器拓扑（interrupt 分册 GIC/IO-APIC 的来源）
  HPET/MCFG/...  时钟、PCIe ECAM 等发现表
内核侧（drivers/acpi/）：
  acpi_bus_scan → acpi_device 树（namespace 即设备树）
  acpi_dev_get_resources：_CRS（Current Resource Settings）→ struct resource
  平台桥：acpi_platform_enumerate → platform 设备（与 DT 殊途同归进总线模型）
```

与 DT 的本质差异：

| | DT | ACPI |
| :--- | :--- | :--- |
| 哲学 | 数据描述（纯声明） | 部分逻辑（AML 方法可执行） |
| 匹配键 | compatible 字符串 | _HID（PNPxxxx）/_ADR（总线地址） |
| 引用 | phandle | _DEP/资源桥接（device_link 的 ACPI 路径） |
| 修改 | 运行期 OF_DYNAMIC/overlay | 理论不改（固件契约） |
| 主场 | arm64/riscv 嵌入式 | x86/服务器 arm64 |

## 三、两条世界线汇入总线模型

```
DT：of_platform_populate → platform_device → device_add ─┐
ACPI：acpi_bus_scan → acpi_device →（platform 桥）→ ────┴→ 上一章的
                                                            match/probe 机器
驱动侧同一张双表：of_match_table + acpi_match_table
  （bus->match 的 DT/ACPI 双分支——匹配协议由总线仲裁，描述来源无关）
```

**这就是 Linux 能一份驱动吃两种固件描述的原因**：描述层差异被吸收在"发现"段，绑定段完全统一。

## 四、观测与实战

```
DT 侧：/proc/device-tree（运行时树镜像）、
        dtc -I fs -O dts /proc/device-tree    # 反编译回去
        of_mdio/ethernet 的 bindings 文档（内核 Documentation/devicetree/bindings）
ACPI 侧：/sys/firmware/acpi/（tables/ 可 raw dump）、
        acpidump/acpixtract/iasl -d（DSDT 反汇编——排障利器）
匹配现场：/proc/bus/pci/devices、/sys/bus/platform/devices/*/modalias
排障对照：设备没 probe → 看 modalias → 反查驱动 match 表的 id 有没有盖住
```

## 五、思考题
1. `of_parse_phandle` 的 args 元数由被引用方的 `#xxx-cells` 声明——这把"接口版本"编码在哪里？改 cells 数的兼容性代价？
2. ACPI 的 `_STA` 返回"设备不存在"时，驱动模型（上一章）看到的序列是什么？（对照 acpi_bus_scan 的过滤。）
3. DT overlay（OF_DYNAMIC）热改设备树——对已经 probe 的设备，上一章的哪个机制负责联动？（提示：reprobe/`of_platform_depopulate`。）
4. 一个同时有 `_HID` 与 `compatible`（ACPI + DT 双描述）的 arm64 设备：谁赢？依据哪一层的优先级？
5. 设计：给一块自定义 FPGA 载板选描述方案（DT/ACPI/PCI BAR 自动发现）——列出决策要素。

---

> 本分册 3 章完整（第 4 章«printk 子系统»在后续任务补为独立分册——见路线图）。回看：[01 kobject 与 sysfs](01-kobject与sysfs.md)｜[02 总线-设备-驱动匹配](02-总线-设备-驱动匹配.md)
