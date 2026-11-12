# Introduction — 模块机制分册（module）

基于 **Linux 6.6** 的可加载模块子系统深度笔记（3 章）。

## 章节导读

| 章节 | 主题 | 一句话 |
| :--- | :--- | :--- |
| [01-模块布局与加载器](docs/01-模块布局与加载器.md) | .ko 与加载 | ELF 专用段、`load_module` 十四段流水线、签名校验、W^X 执行域、init 段一次性生命周期 |
| [02-符号解析与 kallsyms](docs/02-符号解析与kallsyms.md) | 符号世界 | EXPORT_SYMBOL 落盘、GPL-only 判定链、symbol namespace、kallsyms 压缩与 kptr_restrict |
| [03-KO 构建与 BTF](docs/03-KO构建与BTF.md) | 工具链 | modpost/sign-file 流水线、vermagic 与 MODVERSIONS、BTF 与 CO-RE、模块参数 |
| [04-模块签名链](docs/04-模块签名链.md) | 信任链 | 四道开关（sig_enforce 的 bool_enable_only）、.ko 尾部签名布局、mod_verify_sig→PKCS#7 验证链、三密钥环、lockdown 三级别与 Secure Boot 联动 |
| [05-kernel_read_file 加载基础设施](docs/05-kernel_read_file加载基础设施.md) | 统一管道 | module/firmware/kexec/IMA-policy+x509 四方共用的读文件族、ID 枚举 X-macro 双表与 LOADING_* 平行零漂移、★deny_write_access 与 exec 同源 ETXTBSY、双 LSM 门 whole_file 配对契约、from_path_initns 三变体、IMA read_idmap 与 X509 豁免 |
| [06-usermodehelper 机制](docs/06-usermodehelper机制.md) | 内核→用户态通道 | 三难问题（rootfs/凭证/冻结）、两段式 API 与 subprocess_info、三执行形态（prepare_kernel_cred 凭证工程/SIGCHLD 舞步/CLONE_PARENT 转投 kthreadd）、★五等待标志的 xchg 所有权协议（umh_complete 二象性）、三态深度与 initramfs·freeze 双端时序、kmod 的 50 并发闸与 5s 依赖环检测、STATIC_USERMODEHELPER 的 ""=静默成功语义 |
| [07-idempotent 模块加载](docs/07-idempotent模块加载.md) | 并发去重两层 | ★v6.5 引入勘误（曾误记 6.6 新层）、inode 锚点的妙处、栈上对象+全局链、"查+挂"一体注册、per-waiter completion vs complete_all 两种唤醒拓扑、kmod_dup 的 60s 延迟删除与 "let duplicates suffer"、nowait 不做锚点、"两倍模块虚拟内存"动机（Kconfig 原文） |
| [08-umh 消费者全景](docs/08-umh消费者全景.md) | 使用现场 | 19 文件六类普查：coredump 的 stdin 注入与 RLIMIT={1,1} 魔数、request_key 的凭证注入 init 回调、★UMD 的 tmpfs 私有挂载 blob（bpfilter）、cgroup v1 release_agent 安全史、orderly_poweroff、WAIT_EXEC vs WAIT_PROC 选型规律、wireless reg 的注释遗迹如实标注 |

## 与其他分册的联系

* **init 分册**：模块 initcall 与内建 initcall 的同构（`do_initcalls` 的 module 路径）
* **trace/bpf 分册**：kallsyms 的按名注册、BTF 与 CO-RE 的消费端
* **ras 分册**：oops 的符号还原依赖 kallsyms
* **locking 分册**：模块参数与 cmdline 参数的同源机制
* **driver-model / ras 分册**：第 5 章的统一管道横贯两分册——firmware 的 from_path_initns（driver-model/04）与 kexec 镜像加载（ras/kdump/01）都是 kernel_read_file 家族的消费者

## 源码索引（Linux 6.6）

```
kernel/module/main.c（6.6 已拆分：main/elf/kallsyms/strict_rwx/tree_lookup）
kernel/module/kallsyms.c        模块符号域
kernel/kallsyms.c               内核符号表（构建期 scripts/kallsyms 生成）
include/linux/{module.h,export.h,moduleparam.h}
scripts/mod/{modpost.c,sign-file.c?（scripts/sign-file.c）}
```
