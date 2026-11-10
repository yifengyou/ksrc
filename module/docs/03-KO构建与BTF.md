# KO 构建与 BTF（Linux 6.6）

> 阅读目标：掌握模块的工具链流水线（modpost/sign-file/Ordering）；理解 vermagic 与 CONFIG_MODVERSIONS 的生成；掌握 BTF（BPF Type Format）在内核与模块中的角色（.BTF 段、CO-RE 的运行时重定位依据）、`bpftool btf dump` 实战。

---

## 一、构建流水线（内核 Makefile + scripts/mod/）

```
foo.c → foo.o → 链接 foo.ko（ld -r 合并目标）：
  ① MODPOST（scripts/mod/modpost.c）：
     - 扫描未定义符号 → 与内核导出表（Module.symvers）对账
       → 生成警告（unknown symbol / GPL-only / namespace 未导入）
     - 生成 .mod.c：__versions（CRC 表）与 .modinfo 的拼装源
  ② 重链接含 .mod.c 的 foo.o → foo.ko
  ③ strip 调试信息（保留 .symtab 供加载器）
  ④ sign-file（CONFIG_MODULE_SIG）：私钥对 foo.ko 签名 → 追加 PKCS#7
     （仅签名无强制：CONFIG_MODULE_SIG_FORCE 决定拒载与否；
      内核密钥环 `.builtin_trusted_keys` 审签）
  ⑤ modules_install → /lib/modules/$(uname -r)/...（+ depmod 生成
     modules.dep/alias——modprobe 的数据库）
```

`Module.symvers` 是**树内构建的全局导出清单**（地址/CRC/namespace/导出模块）——modpost 对账与 out-of-tree 模块构建（`make -C /lib/modules/.../build M=$PWD`）都吃它。

## 二、vermagic 与 MODVERSIONS

```
include/linux/vermagic.h 自动拼装：UTS_RELEASE + SMP/PREEMPT/64BIT
 + 编译器版本 + 关键 CONFIG 串 → .modinfo 的 vermagic=
加载对账（第一章 early_mod_check）：
  完整串匹配——"同版本内核不同配置"拒绝混载
CONFIG_MODVERSIONS=y 时启用 CRC 逐符号对账（第一章⑨）：
  精细级：允许 ABI 未变的同版本跨小版本（发行版 kABI 的基石）
```

## 三、BTF：内核的"自我描述"

```
构建期（pahole，CONFIG_DEBUG_INFO_BTF=y）：
  DWARF 调试信息 → 压缩的 BTF 段（.BTF：类型图：struct/union/enum/
  func proto/函数参数的行号……）
  模块各自生成 .ko 的 .BTF 段（加载时 btf_parse_module 合入全局）
运行期消费：
  BPF CO-RE（Compile Once – Run Everywhere）：libbpf 按 BTF 现场
  重定位结构体偏移（旧方式 = 为每个内核版本编一版模块化 BPF）
  bpftool btf dump file /sys/kernel/btf/vmlinux format c
  ——把整个内核的类型图导成 C 头（BTF Hub 的日常姿势）
  kfunc/BPF 追踪的按类型编程（trace 分册的 BPF 章承接）
/sys/kernel/btf/vmlinux（只读 mmap）+ /sys/kernel/btf/<module>
```

**为什么模块生态依赖它**：BPF 程序与探针工具不再需要内核源码/头——`vmlinux.h`（`bpftool btf dump -c` 产物）一个文件走天下；发行版内核随包发布 BTF（`kernel-modules` 里的 .BTF）。

## 四、模块参数（param 机制小传）

```c
module_param(name, type, perm) → .modinfo 里 "name=type" +
  param_attrs 生成 sysfs /sys/module/foo/parameters/name（perm 控制可见性）
insmod foo.ko bar=1 → 解析进 uargs → param_set
```

与 bootloader 的内核 cmdline 参数共用一套 `param_ops` 引擎（init 分册的 `__setup/early_param` 同源）——**同一个旋钮两种装载通道**（模块参数与 `foo.bar=1` 内核 cmdline）。

## 五、观测与实战

```
modinfo -F vermagic foo.ko; uname -r   # 对账现场
modprobe -v e1000e                      # 按 modules.dep 解依赖链
bpftool btf dump file foo.ko | head     # 模块 BTF
cat /sys/module/btrfs/parameters/*      # 参数现场值
dmesg: "module verification failed: signature and/or required key
missing" → 体系：安全启动 + MOK（mokutil 导入自签公钥）路线
```

## 六、思考题

1. modpost 在**编译期**警告 GPL-only/namespace，加载器在**运行期**再拒——两道闸的成本收益各是什么？
2. `Module.symvers` 的 CRC 来自哪里？（提示：genksyms 对类型可重定义的敏感——为什么 `struct` 改字段名也算 ABI 变化？）
3. BTF 的存在使“模块必须匹配内核”的约束对 **BPF 模块**失效——这把 ABI 边界推给了谁维护？（libbpf 的 relocations 与内核的 BTF 稳定性承诺。）
4. `module_param` 的 perm=0 与 perm=0644 在 sysfs 与 `/sys/module/*/parameters` 的差异——为什么有些参数只读？
5. 安全启动链下自编译模块的三条出路（MOK/关 enforcement/重签名）各自的信任代价？

---

> 本分册 3 章完整。回看：[01 模块布局与加载器](01-模块布局与加载器.md)｜[02 符号解析与 kallsyms](02-符号解析与kallsyms.md)｜[03 KO 构建与 BTF](03-KO构建与BTF.md)
