# Introduction — Rust for Linux（rust）

基于 **Linux 6.6** 的 Rust-in-kernel 现状导读（1 章）。与目录内 `.rst` 官方镜像互补：rst 讲"如何配置工具链与上手"，本导读讲"6.6 树里实际有什么、边界在哪"。

## 章节导读

| 章 | 文件 | 一句话 |
| :--- | :--- | :--- |
| 6.6 现状导读 | [01-RustForLinux现状导读](docs/01-RustForLinux现状导读.md) | 基座完备生态为零（drivers/ 零 Rust 目录核对）、rust/ 树布局（alloc/bindings/macros/kernel）、sync 原语与 pin-init 宏系统、CONFIG_RUST 依赖表（!MODVERSIONS 等）、module! 样例、与 C 的边界 |

## 与其他分册的联系

- **module 分册**：KO 加载的两栖对照（C 与 Rust 模块同一基础设施）
- **locking 分册**：sync/lock 的 lockdep 双侧打通
- **driver-model 分册**：pin-init vs probe 清理链

## 源码索引（Linux 6.6）

```
rust/kernel/{lib,error,init,task,types,str,print}.rs   抽象层
rust/kernel/sync/{arc,condvar,lock/{mutex,spinlock},locked_by}.rs
rust/{macros,bindings,uapi,helpers.c,exports.c}
samples/rust/{rust_minimal,rust_print,hostprogs}
init/Kconfig (CONFIG_RUST)
```
