# 符号解析与 kallsyms（Linux 6.6）

> 阅读目标：掌握 EXPORT_SYMBOL 家族的落盘机制（__ksymtab 段与 CRC 表）；吃透 GPL-only 的判定链与 license taint 的语义；理解模块符号解析（resolve_unresolved）、symbol namespace、以及 kallsyms 的压缩与查找（`/proc/kallsyms` 的 kptr_restrict 语义）。

---

## 一、EXPORT_SYMBOL：导出的落盘形态

```c
EXPORT_SYMBOL(sym)      → __ksymtab 段追加 struct kernel_symbol{value,name}
EXPORT_SYMBOL_GPL(sym)  → __ksymtab_gpl 段（另有 _NS 变体：带 namespace）
EXPORT_SYMBOL_NS(sym, ns)（6.6 主流写法）
                          CRC 落 __kcrctab / __kcrctab_gpl
```

宏的展开（include/linux/export.h）把符号与名字（`__stringify`）成对入段——**内核自身是一张巨大的导出表**（`nm vmlinux | grep __ksymtab` 数万条），模块的每个未定义符号都要在这张表里找到条目并且 CRC 相符（第一章流水线第 ⑨⑩ 步）。

## 二、GPL-only：判定链

```
解析时命中 __ksymtab_gpl：
  simplify_symbols → resolve_symbol_wait
    resolve_symbol（kernel/module/main.c）
      ├─ 查 GPL 表成功 → check_version（CRC）
      │   └─ 失败/仅非 GPL 表命中 → -ENODEV：GPL-only symbol 引用
      └─ 命中即置 mod->using_gplonly_symbols = true（main.c:1132，
          已核对——后面 license 判定的证据链一环）
license 审判（module_license_taint_check，main.c:1577）：
  MODULE_LICENSE("GPL"/"GPL v2"/Dual BSD/GPL"...) → 洁净
  "Proprietary" 等 → TAINT_PROPRIETARY_MODULE（dmesg 加 P、oops 加标记）
  非 GPL 的 license 引用了 GPL-only 符号 → 加载被拒（不是 taint，是失败）
```

**动机**：内核 GPL 边界的执行机制——闭源驱动只能用非 GPL 表里的符号（大量接口被有意“GPL-only 化”，如调试/跟踪基础设施）。

## 三、symbol namespace（5.0+）

`EXPORT_SYMBOL_NS(sym, USB_CORE)` + 模块侧 `MODULE_IMPORT_NS(USB_CORE)`——命名空间不匹配的解析失败（`-ENOSYS`）。工程意义：把“能用”与“该用”分离（子系统内部 API 显式圈起来），modpost 在**编译期**就警告未声明的导入。

## 四、kallsyms：符号的运行时地图

```
构建期（scripts/kallsyms）：vmlinux 全符号 → 压缩表嵌入 .rodata
  （相对偏移 + 变长编码：名称查四级表——kallsyms_names/token_table...）
运行期（kernel/kallsyms.c + kernel/module/kallsyms.c）：
  kallsyms_lookup_name(name) → 地址（含已加载模块）
  模块自己的符号经 each_symbol_section 合并进查找域
/proc/kallsyms 输出：地址 类型 名字 [模块名]
  kptr_restrict=2（默认）对无 CAP_SYSLOG 者显示 0000000000000000
  ——地址泄露面收敛（perf/kprobe 走 CAP 或注册制旁路）
```

`kallsyms_lookup_name` 从 5.7 起不再 EXPORT（防模块滥用绕锁）——BPF/kprobe 生态改走 `kallsyms_on_each_symbol` 注册制或 BTF。

## 五、oops 的符号还原（与 ras 分册的衔接）

oops 文本里的 `fffffff0+symbol+0x18/0x40` 即 kallsyms 的三件套（符号基址+偏移/符号大小）——`decodecode`/`faddr2line`（addr2line 需调试信息）把现场还原到源码行。模块的符号在卸载后从地图消失——**事后分析要靠 dmesg 当时的还原**（crash 工具用嵌入模块的 mod 段做 vmcore 侧还原）。

## 六、观测与实战

```
cat /proc/kallsyms | grep ' btrfs_'        # 模块符号的标注形态
cat /proc/modules                          # 模块清单与引用
nm foo.ko | grep ' U '                     # 待解析符号清单
modprobe --show-depends foo                # 依赖闭包
# 排障：
"Unknown symbol in module"   → nm 对照 kallsyms（导出表没有/CRC 不符/GPL 拒）
"Key was rejected by service" → 签名策略（第一章 dmesg 细读）
```

## 七、思考题

1. CRC 表为什么按符号（而非整内核）对账？内核加了一个未导出函数、没动导出符号——模块必须重编吗？（对照 vermagic 的粗粒度。）
2. `using_gplonly_symbols` 在 license 审判前就置位——这个证据链顺序为什么重要？（提示：license 字符串可以随便写。）
3. kallsyms 压缩为什么用“相对偏移+token 表”而不是哈希表？（启动早期无分配器的约束 + 查找性能的取舍。）
4. `kallsyms_lookup_name` 撤销导出后，kprobe 的按名注册（`kprobe_events` 写 `do_sys_open`）还能工作吗？它走的哪条路径？
5. namespace 与 GPL-only 是两把不同的锁——一个闭源且声明了 `MODULE_IMPORT_NS` 的模块会卡在哪一关？

---

> 下一章：[KO 构建与 BTF](03-KO构建与BTF.md)。
