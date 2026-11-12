# Introduction — 设备模型分册（driver-model）

基于 **Linux 6.6** 的设备模型深度笔记（4 章；最初计划的 ACPI 基础章已并入第 3 章统一讲述，第 4 章现为固件加载专题）。

## 章节导读

| 章节 | 主题 | 一句话 |
| :--- | :--- | :--- |
| [01-kobject 与 sysfs](docs/01-kobject与sysfs.md) | 对象地基 | kobject/kset/kobj_type 生命周期、kernfs 共引擎、uevent 热插拔广播 |
| [02-总线-设备-驱动匹配](docs/02-总线-设备-驱动匹配.md) | 绑定机器 | bus/device/driver 三层、match→probe 全链（`really_probe` 已核对）、-EPROBE_DEFER、device link |
| [03-设备树与 ACPI](docs/03-设备树与ACPI.md) | 固件描述 | DTB 布局与 unflatten、of_ API 与 phandle、AML 与 _CRS、两条世界线汇入总线模型 |
| [04-firmware 加载与 request_firmware 生命周期](docs/04-firmware加载与request_firmware生命周期.md) | 固件补给线 | 五级搜索路径、builtin/缓存/在途三级快路径、override_creds 安全设计、sysfs fallback 的 loading/data 协议与 LSM 钩子、suspend 固件缓存、fw_upload 反向通道 |
| [05-devres 设备资源管理](docs/05-devres设备资源管理.md) | probe 解药 | 三结构两把锁、FIFO 入队/LIFO 释放、★group 两遍染色算法（"wholly contained" 注释）、devm_kmalloc 的 noop release 设计、devm_action 泛型族、三消费方案例（fw magic 匹配/IRQ 双元组/pcim）、devres_log tracepoint |
| [06-pcim 与 PCI 管理资源](docs/06-pcim与PCI管理资源.md) | PCI 方言 | ★基础函数接管设计（pci_request_region/pci_intx 在 managed 设备自动记账——find_pci_dr 的 NULL 语义）、总账本 pci_devres 五位一 mask 与逆序清单、pin 逃生门（"won't be disabled on driver detach"）、pcim_iomap 表模式（一张 devres 记全部 BAR）与 "duplicate mappings not allowed"、request_all 的掩码补集 |

## 与其他分册的联系

* **cgroup 分册**：kernfs 同引擎（sysfs 与 cgroupfs 的对比）
* **interrupt 分册**：GIC/IO-APIC 的拓扑来源（MADT/DT 的 interrupt 节点）
* **module 分册**：modalias→modprobe 的冷启动闭环；第 4 章的 sysfs fallback（uevent→udev 回灌固件）与 KO 加载共用"内核请求、用户态响应"模型
* **power 分册**：`dev_pm_ops` 挂在 bus/driver 上、device link 的电源序；第 4 章 suspend 固件缓存挂在 PM notifier 链上

## 源码索引（Linux 6.6）

```
drivers/base/{kobject.c,bus.c,dd.c,core.c}    对象/绑定/延迟探测
include/linux/{kobject.h,device.h}            三层结构定义
fs/sysfs + kernel/kernfs                      目录树引擎
drivers/of/{base.c,fdt.c}                     DT 解析与 API
drivers/acpi/                                 AML 解释器与设备桥
drivers/base/firmware_loader/                 固件加载器（main/sysfs/fallback/fallback_table/sysfs_upload）
```
