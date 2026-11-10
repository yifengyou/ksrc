# Introduction — printk 分册

基于 **Linux 6.6** 的 printk 子系统深度笔记（2 章）。

## 章节导读

| 章节 | 主题 | 一句话 |
| :--- | :--- | :--- |
| [01-printk-ringbuffer](docs/01-printk-ringbuffer.md) | 无锁环形缓冲 | prb 三环结构、描述符状态机（reserved→committed→finalized）、数据环隐式回收、seq 验证复制、printk_safe 与 /dev/kmsg |
| [02-console驱动模型](docs/02-console驱动模型.md) | 搬运与 console | struct console、console_unlock 搬运循环、CON_BOOT 交接、loglevel 过滤、RT 争议与 6.8 kthread 化前夜 |

## 与其他分册的联系

- **init 分册**：earlycon/boot console 在启动链中的位置
- **ras 分册**：panic 路径的 `console_flush_on_panic` 与 kmsg_dump
- **power 分册**：suspend_console 与 no_console_suspend
- **interrupt 分册**：printk_safe 的 NMI 缓冲与中断嵌套

## 源码索引（Linux 6.6）

```
kernel/printk/printk.c             主逻辑（vprintk/console_unlock/dmesg 接口）
kernel/printk/printk_ringbuffer.c  prb 实现（内存序大注释是权威）
include/linux/printk_ringbuffer.h  三环结构与状态宏
include/linux/console.h            struct console 与 CON_* flags
kernel/printk/printk_safe.c        NMI/递归安全缓冲
```
