# Introduction — 设备模型分册（driver-model）

基于 **Linux 6.6** 的设备模型深度笔记（3 章，第 4 章原计划的 ACPI 基础已并入第 3 章统一讲述）。

## 章节导读

| 章节 | 主题 | 一句话 |
| :--- | :--- | :--- |
| [01-kobject 与 sysfs](docs/01-kobject与sysfs.md) | 对象地基 | kobject/kset/kobj_type 生命周期、kernfs 共引擎、uevent 热插拔广播 |
| [02-总线-设备-驱动匹配](docs/02-总线-设备-驱动匹配.md) | 绑定机器 | bus/device/driver 三层、match→probe 全链（`really_probe` 已核对）、-EPROBE_DEFER、device link |
| [03-设备树与 ACPI](docs/03-设备树与ACPI.md) | 固件描述 | DTB 布局与 unflatten、of_ API 与 phandle、AML 与 _CRS、两条世界线汇入总线模型 |

## 与其他分册的联系

* **cgroup 分册**：kernfs 同引擎（sysfs 与 cgroupfs 的对比）
* **power 分册**：`dev_pm_ops` 挂在 bus/driver 上、device link 的电源序
* **interrupt 分册**：GIC/IO-APIC 的拓扑来源（MADT/DT 的 interrupt 节点）
* **module 分册**：modalias→modprobe 的冷启动闭环

## 源码索引（Linux 6.6）

```
drivers/base/{kobject.c,bus.c,dd.c,core.c}    对象/绑定/延迟探测
include/linux/{kobject.h,device.h}            三层结构定义
fs/sysfs + kernel/kernfs                      目录树引擎
drivers/of/{base.c,fdt.c}                     DT 解析与 API
drivers/acpi/                                 AML 解释器与设备桥
```
