# Introduction — 系统调用机制分册（syscalls）

基于 **Linux 6.6** 的系统调用机制深度笔记：入口与分发、seccomp 防火墙、io_uring 异步 IO。

## 章节导读

| 章节 | 主题 | 一句话 |
| :--- | :--- | :--- |
| [01-系统调用入口与分发](docs/01-系统调用入口与分发.md) | 交接协议 | x86 syscall/sysret 与 arm64 SVC、通用入口层钩子序列、系统调用表、syscall 重启、vDSO |
| [02-seccomp](docs/02-seccomp.md) | 调用防火墙 | 两种模式、cBPF 字节码、七种处置、TSYNC/USER_NOTIF/no_new_privs |
| [03-io_uring](docs/03-io_uring.md) | 异步 IO | 三环共享内存协议、SQPOLL/IOPOLL 零系统调用形态、multishot、安全风波与部署现实 |

## 与其他分册的联系

* **process-mgmt**：入口层的信号递送/系统调用重启与第 4 章信号子系统一体两面；`exit_task_work` 收割 io_uring 请求
* **workqueue**：io-wq 线程池、task_work 机制
* **block / mm / networking**：io_uring 的 opcode 深入各自的执行层（plug/POLL 队列、pin_user_pages、multishot 网络）
* **admin-guide/LSM**：seccomp 与 LSM 的分工线

## 源码索引（Linux 6.6）

```
arch/x86/entry/{entry_64.S,common.c,syscall_64.c,vdso/}
arch/arm64/kernel/{syscall.c,entry-common.c}
kernel/entry/common.c + include/linux/entry-common.h   通用入口层
kernel/seccomp.c + include/uapi/linux/seccomp.h
io_uring/（6.6 已是独立目录，io_uring.c 等 40+ 文件）+ include/uapi/linux/io_uring.h
```
