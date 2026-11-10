# Rust for Linux：6.6 现状导读（Linux 6.6）

> 阅读目标：建立 Rust-in-kernel 的准确坐标系——6.1 合入的"基座"在 6.6 长成了什么样（**答案：抽象层完备、驱动生态为零**，`drivers/` 下无任何 Rust 目录，samples 仅 3 个——均 ls-tree 核对）；代码布局、同步原语与 pin-init 系统、构建依赖链、与 C 的边界。目录内 `.rst`（quick-start/coding-guidelines 等）是官方镜像，本导读讲"树里实际有什么"。

---

## 一、6.6 的坐标：基座完备、生态起步

```
6.1  基座合入（构建系统 + 基础抽象，无驱动）
6.6  本导读基线：
  rust/kernel 抽象层：error/init/ioctl/kunit/print/str/sync/task/types
  ★注意：无 workqueue.rs、无 device/driver.rs（后续版本才长出，
    对照 [workqueue 分册](../../workqueue/README.md) 的 C 世界）
  drivers/：0 个 Rust 目录（ls-tree 核对）——"第一个内核驱动"
    尚未发生；samples/ 仅 3 个示例
```

## 二、代码布局（rust/ 树，已核对 6.6）

```
rust/
├── alloc/            引入的 Rust alloc 库副本（内核化改造）
├── bindings/         bindgen 从 C 头生成的 extern 声明（自动）
├── uapi/             uapi 头的绑定（同样生成）
├── macros/           proc-macro crate（module!/devres 等宏）
├── helpers.c         ★手写 C 胶水（bindgen 表达不了的 inline/宏）
├── exports.c         符号导出桥
├── compiler_builtins.rs / build_error.rs
└── kernel/           ★抽象层（"kernel" crate，驱动开发的 API 面）
    lib.rs 的 pub mod 全表（已核对）：error/init/ioctl/kunit/
    prelude/print/static_assert/std_vendor/str/sync/task/types
    + pub use bindings/macros/uapi
```

## 三、同步原语（rust/kernel/sync/，已核对）

```
sync/
├── arc.rs（+ arc/std_vendor.rs）  Arc/UniqueArc——引用计数
├── condvar.rs                     条件变量
├── lock.rs + lock/{mutex.rs, spinlock.rs}
│     基类 + 两实现：与 C 侧 lockdep/锁验证器打通
├── locked_by.rs                   "被 X 锁保护"的编译期标记
语义红利：Guard 的 RAII 析构——忘记解锁是编译错误而非死锁
```

## 四、pin-init：安全初始化的宏系统（rust/kernel/init/）

```
init.rs + init/{__internal.rs, macros.rs}
问题：内核对象常需"先分配后半初始化"（两阶段构造）——裸 Rust
  的未初始化引用是 UB 温床
方案：#[pin_data]/#[pin_init] 宏族把"初始化槽位"类型化——
  init!() 宏展开成状态机，任何早期 return 路径都安全消解
这是 Rust 内核代码比 C 显著安全的第一现场（对照 C 的 goto err
  清理链——[driver-model/02](../../driver-model/docs/02-总线-设备-驱动匹配.md) 的 probe 失败路径）
```

## 五、构建链与 CONFIG_RUST 依赖（init/Kconfig:1884，已核对）

```
config RUST 依赖：
  HAVE_RUST + RUST_IS_AVAILABLE（工具链探测）
  !MODVERSIONS        ← 符号版本化与 Rust 的 CRC 语义不兼容
  !GCC_PLUGINS / !RANDSTRUCT
  !DEBUG_INFO_BTF || PAHOLE_HAS_LANG_EXCLUDE  ← pahole 需能跳过
    Rust 对象（BTF 的 C-only 假设）
工具链：rustc + bindgen（脚本可探测 RUSTC_VERSION_TEXT 的暴露
  与 Makefile 的版本门）
```

## 六、样例（samples/rust/，已核对）

```
rust_minimal.rs —— module! { type/name/author/description/license }
  字段全核对；最小生命周期（init/exit 打印）
rust_print.rs    —— pr_info!/pr_err! 族
hostprogs/       —— 用户态宿主程序（构建工具演示）
构建：make LLVM=1 ... CONFIG_RUST=y 后 samples/rust/*
```

## 七、与 C 的边界（协作模型）

```
C 世界 → Rust：bindings（bindgen 自动）+ helpers.c（手写包装）
Rust 世界 → C：exports.c 导出符号；ABI 稳定性依赖"抽象层手工对齐"
uapi：头文件共享（Rust 侧用生成绑定读同一份 uapi）
语义桥：错误码（kernel::Error ↔ errno）、锁定（lockdep 双侧打通）
```

## 八、观测与上手

```
cat /proc/kallsyms | grep rust_      # 加载的 Rust 模块符号
make LLVM=1 menuconfig → Kernel hacking → Rust support
快速通道：仓库内镜像 rust/quick-start.rst（官方权威步骤）
排障：RUST_IS_AVAILABLE 的探测日志（make 输出的 rustavailable
  目标）；bindgen 版本不匹配是第一高频故障
```

## 九、思考题

1. 6.6 无 workqueue/device 抽象——这如何约束"能在 6.6 里写什么驱动"？（提示：无设备模型锚点 = 只能写自包含字符设备类示例。）
2. `helpers.c` 为什么必须存在？（bindgen 对 static inline/宏的翻译盲区。）
3. `!MODVERSIONS` 依赖的技术根源？（CRC 校验的 C ABI 假设 vs Rust 泛型单态化。）
4. `locked_by` 用类型系统表达锁保护关系——与 C 侧的 `__acquires` 注解相比差异是什么？（编译期 vs Sparse 的静态检查。）
5. pin-init 解决的"两阶段构造"在 C 世界对应哪段代码？（对照 probe 失败清理链与 kzalloc 的隐式零初始化。）

---

> 回看：本目录 rst 镜像（quick-start/coding-guidelines/general-information/arch-support）｜[module 分册](../../module/README.md)（KO 加载的两栖对照）
