# Introduction — livepatch 分册

基于 **Linux 6.6** 的内核热补丁（livepatch）深度笔记（2 章）。与目录内 `.rst` 官方镜像互补：rst 讲"如何写补丁与接口约定"，本笔记讲"内部机制与数据流"。

## 章节导读

| 章 | 文件 | 一句话 |
| :--- | :--- | :--- |
| klp 核心结构与打补丁流 | [01-klp核心结构与打补丁流](docs/01-klp核心结构与打补丁流.md) | klp_patch/object/func 三层结构、klp_enable_patch 注册流、klp_write_relocation、ftrace 集成（SAVE_REGS+IPMODIFY、klp_ops func_stack 栈式派发、transition 窗口） |
| 一致性模型与 transition | [02-一致性模型与transition](docs/02-一致性模型与transition.md) | 逐任务一致性（klp_target_state/patch_state/TIF_PATCH_PENDING）、klp_try_switch_task 与 klp_check_stack 栈检查、replace/nop 原子替换、klp_state 系统状态、影子变量 |

## 与其他分册的联系

- **module 分册**：补丁自身是 KO（隔离加载）；kallsyms 是"老世界地图"
- **trace 分册**：ftrace IPMODIFY 特权、可靠栈回溯（kprobes/ORC）
- **printk 分册**：transition 进度的 dmesg 观测

## 源码索引（Linux 6.6）

```
include/linux/livepatch.h   三层结构 + klp_callbacks/klp_state
kernel/livepatch/core.c     注册流/sysfs/module 通知
kernel/livepatch/patch.c    ftrace 集成/klp_ftrace_handler
kernel/livepatch/transition.c 一致性模型/栈检查
kernel/livepatch/shadow.c   影子变量
```
