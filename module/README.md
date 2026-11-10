# Introduction — 模块机制分册（module）

基于 **Linux 6.6** 的可加载模块子系统深度笔记（3 章）。

## 章节导读

| 章节 | 主题 | 一句话 |
| :--- | :--- | :--- |
| [01-模块布局与加载器](docs/01-模块布局与加载器.md) | .ko 与加载 | ELF 专用段、`load_module` 十四段流水线、签名校验、W^X 执行域、init 段一次性生命周期 |
| [02-符号解析与 kallsyms](docs/02-符号解析与kallsyms.md) | 符号世界 | EXPORT_SYMBOL 落盘、GPL-only 判定链、symbol namespace、kallsyms 压缩与 kptr_restrict |
| [03-KO 构建与 BTF](docs/03-KO构建与BTF.md) | 工具链 | modpost/sign-file 流水线、vermagic 与 MODVERSIONS、BTF 与 CO-RE、模块参数 |

## 与其他分册的联系

* **init 分册**：模块 initcall 与内建 initcall 的同构（`do_initcalls` 的 module 路径）
* **trace/bpf 分册**：kallsyms 的按名注册、BTF 与 CO-RE 的消费端
* **ras 分册**：oops 的符号还原依赖 kallsyms
* **locking 分册**：模块参数与 cmdline 参数的同源机制

## 源码索引（Linux 6.6）

```
kernel/module/main.c（6.6 已拆分：main/elf/kallsyms/strict_rwx/tree_lookup）
kernel/module/kallsyms.c        模块符号域
kernel/kallsyms.c               内核符号表（构建期 scripts/kallsyms 生成）
include/linux/{module.h,export.h,moduleparam.h}
scripts/mod/{modpost.c,sign-file.c?（scripts/sign-file.c）}
```
