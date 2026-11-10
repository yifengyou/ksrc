# Introduction — 热插拔分册（hotplug）

基于 **Linux 6.6** 的 CPU/内存热插拔深度笔记（2 章）。

## 章节导读

| 章 | 文件 | 一句话 |
| :--- | :--- | :--- |
| cpuhp 状态机 | [01-cpuhp状态机](docs/01-cpuhp状态机.md) | 状态机模型（OFFLINE→ONLINE 三段）、cpuhp_step 单/多实例回调、控制 CPU 与 `cpuhp/%u` 线程分工、失败回滚、takedown_cpu 的 stop_machine 终局、6.6 AP 同步新机制 |
| 内存热插拔 memhp | [02-内存热插拔memhp](docs/02-内存热插拔memhp.md) | resource→block→zone 三层、add_memory_resource 与 sparse section、memmap_on_memory 自托管 vmemmap、offline_pages 事务性协议（隔离/迁移/收缩/回滚） |

## 与其他分册的联系

- **workqueue 分册**：CPUHP_AP_WORKQUEUE_ONLINE 的消费方
- **timers 分册**：tick 广播交接（hotplug_cpu__broadcast_tick_pull）
- **interrupt 分册**：下线时的 IRQ 迁移
- **mm 分册**：buddy/pcp 与 ZONE_MOVABLE 的热插拔联动
- **kdump 分册**：6.6 crash hotplug 的另一半语境

## 源码索引（Linux 6.6）

```
kernel/cpu.c                cpuhp 框架与状态机执行
include/linux/cpuhotplug.h  状态枚举与注册 API
mm/memory_hotplug.c         上/下线与 block 设备
include/linux/memory_hotplug.h  mhp_t 旗标与接口
```
