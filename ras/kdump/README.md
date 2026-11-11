# Introduction — kdump 子分册（ras/kdump）

基于 **Linux 6.6** 的 kexec/kdump 深度笔记（2 章）：从热重启机制（kexec）到崩溃转储的证据链（crashkernel/elfcorehdr/VMCOREINFO）。

## 章节导读

| 章 | 文件 | 一句话 |
| :--- | :--- | :--- |
| kexec 加载与跳转 | [01-kexec加载与跳转](docs/01-kexec加载与跳转.md) | 两阶段语义（load/execute）、struct kimage 与 IND_* 页链表、kexec_load(246) vs kexec_file_load(320) 与 KEXEC_SIG 签名、x86 relocate_kernel 恒等映射跳转、kexec jump |
| crashkernel 预留与 elfcorehdr | [02-crashkernel预留与elfcorehdr](docs/02-crashkernel预留与elfcorehdr.md) | crashkernel= 三种语法、crashk_res/crashk_low_res、elfcorehdr 双层构造（load 骨架 + panic 血肉）、VMCOREINFO 契约、6.6 crash hotplug、purgatory sha256、/proc/vmcore |
| kexec 签名与 lockdown 联动 | [03-kexec签名与lockdown](docs/03-kexec签名与lockdown.md) | 信任链交叉：老 kexec_load 封锁注释原句、KEXEC_SIG/FORCE 语义、PE 验签双密钥环回退、IMA 豁免三通道矩阵 |

## 与其他分册的联系

- **ras/panic**：panic() → __crash_kexec 的上游路径
- **printk 分册**：kmsg_dump 与 crash_kexec 的顺序（post_notifiers）
- **module 分册**：KEXEC_SIG 与 KO 签名同属"锁死引导面"
- **init 分册**：capture kernel 的启动位置（跳过固件）

## 源码索引（Linux 6.6）

```
kernel/kexec.c            sys_kexec_load 入口
kernel/kexec_file.c       file 模式 + purgatory + KEXEC_SIG
kernel/kexec_core.c       kimage 分配/panic 路径/crashk_res
kernel/crash_core.c       crashkernel 解析 + ELF 头构造 + VMCOREINFO
arch/x86/kernel/crash.c   x86 elfcorehdr 段 + NMI shootdown
arch/x86/kernel/machine_kexec_64.c  relocate_kernel 跳转
fs/proc/vmcore.c          capture kernel 侧 /proc/vmcore
```
