# kexec 加载与跳转（Linux 6.6）

> 阅读目标：掌握 kexec 的**两阶段语义**（load 可反复覆盖 / execute 一次性）、`struct kimage` 与页链表段模型（IND_* 协议）、`kexec_load` vs `kexec_file_load` 的分野（签名校验 KEXEC_SIG）、x86 `machine_kexec` 的恒等映射跳转机制、以及 crash 镜像与普通镜像的加载差异。前置：[init 分册 rest_init](../../init/docs/03-rest_init到用户态.md)、[process-mgmt 分册 execve](../../process-mgmt/docs/02-execve与mm替换.md)（"整机级 execve"的心智模型）。

---

## 一、两阶段语义：先 load 后 execute

```
① kexec -l vmlinuz --initrd=...           ← 只加载，随时可覆盖
   sys_kexec_load(246) / sys_kexec_file_load(320)（x86_64，syscall_64.tbl 已核对）
② kexec -e                                 ← 执行跳转
   reboot(LINUX_REBOOT_CMD_KEXEC) → kernel_kexec() → machine_kexec(image)
```

心智模型：kexec ≈ **整机级 execve**——新内核不经过固件（BIOS/UEFI 跳过），冷启动的设备初始化、内存训练全部省去，热重启从分钟级降到亚秒级。

## 二、struct kimage（include/linux/kexec.h，已核对 6.6）

```c
struct kimage {
	kimage_entry_t head, *entry, *last_entry;  /* 段页链表（见下） */
	unsigned long start;                        /* 新内核入口地址 */
	struct page *control_code_page;             /* 搬运代码+恒等页表页 */
	struct page *swap_page;                     /* 搬运期临时栈/数据 */
	void *vmcoreinfo_data_copy;                 /* crash 镜像用 */
	unsigned long nr_segments;
	struct kexec_segment segment[KEXEC_SEGMENT_MAX];  /* 用户/文件给出的段 */
	struct list_head control_pages, dest_pages, unusable_pages;
	unsigned long control_page;
	unsigned int type : 1;         /* KEXEC_TYPE_DEFAULT / _CRASH */
	unsigned int preserve_context : 1;   /* kexec jump（休眠式往返）*/
	unsigned int file_mode : 1;          /* 走 kexec_file_load */
#ifdef CONFIG_CRASH_HOTPLUG
	unsigned int update_elfcorehdr : 1;  /* 6.6：热插拔时重写 elfcorehdr */
#endif
	...
};
```

## 三、段模型：kimage_entry_t 页链表与 IND_* 协议

```
加载器把"哪个物理页搬到哪个目标地址"编译成一条物理页链表，每项
kimage_entry_t 低 4 bit 是标记（kexec.h 已核对）：
  IND_DESTINATION(1<<0) 下一项地址是搬运目的地
  IND_INDIRECTION(1<<1) 下一项是"下一批链表页"（续链）
  IND_SOURCE(1<<3)      本页是要拷贝的源数据
  IND_DONE(1<<2)        链表结束
执行期 relocate_kernel 逐项消费：源页内容→恒等映射下拷到目的页
```

**普通镜像分配**（kimage_alloc_normal）：目的页按段地址逐页 `kimage_alloc_page`——先查与已用页冲突（unusable_pages），源页从 `KIMAGE_SOURCE` 通道分配；**crash 镜像**（`KEXEC_ON_CRASH`）则所有段必须落在 `crashk_res` 预留区内（下一章）。

## 四、kexec_load vs kexec_file_load

| | kexec_load(246) | kexec_file_load(320) |
| :--- | :--- | :--- |
| 输入 | 原始段（用户拼好） | 内核镜像**文件** + cmdline |
| 内核侧解析 | 无（信任用户） | `arch_kexec_kernel_image_probe` 解析 ELF/压缩内核 |
| 签名 | 无 | **CONFIG_KEXEC_SIG**（`sig_enforce`，KEXEC_SIG_FORCE 则拒载未签名） |
| purgatory | 用户态 kexec-tools 提供 | 内核内置（kexec_purgatory，sha256 自校验） |
| 适用 | 老工具链/特殊布局 | Secure Boot 场景事实标准 |

## 五、x86 跳转机制（arch/x86/kernel/machine_kexec_64.c，已核对 6.6）

```
machine_kexec(image):
  ① image->control_code_page 尾部（+PAGE_SIZE）拷入 relocate_kernel
     汇编体（KEXEC_CONTROL_CODE_MAX_SIZE）
  ② 在控制页内搭 64bit 恒等页表（"Setup the identity mapped 64bit
     page table"）——跳转期唯一可用的地址空间
  ③ page_list[PA/VA_CONTROL_PAGE]、[PA_SWAP_PAGE]、[PA_PGD]、
     [start_address] 打包成寄存器可携带的交接清单
  ④ 关中断/关 NMIs → relocate_kernel(page_list, start, ...)
     汇编体：切 cr3 到恒等页表 → 按 IND_* 链表逐页搬运 → 关机级
     收尾（停 AP、刷新缓存）→ jmp image->start：新内核从"冷启动
     的实模式之后"直接进场（x86 走 startup_64）
```

**KEXEC_PRESERVE_CONTEXT**（kexec jump）：不销毁旧内核上下文（设备挂起、页表保留），新内核返回时从断点续跑——kexec+hibernate 混合玩法的地基。

## 六、观测与实战

```
cat /sys/kernel/kexec_loaded         # 普通镜像 0/1
cat /sys/kernel/kexec_crash_loaded   # crash 镜像 0/1（kdump 就绪标志）
cat /sys/kernel/kexec_crash_size     # 预留区大小（可写 0 释放！）
sysctl kernel.kexec_load_disabled    # 1 后两个 syscall 永久失效
kexec -l -p vmlinuz ...              # 加载 panic 镜像（kdump 第一步）
perf probe -a machine_kexec          # 跳转路径打点
```

## 七、思考题

1. `kexec -l` 第二次加载时旧 `struct kimage` 如何善终？（对照 do_kexec_load 的覆盖路径与 unusable_pages 的释放。）
2. IND_* 链表为什么用**物理页链**而不是大数组？（提示：跳转期无内存分配器、恒等映射下物理页号即指针。）
3. `kexec_file_load` 的签名校验挡住的攻击模型是什么？（对照 module 分册 KO 签名：持久化面 vs 一次性引导面。）
4. crash 镜像段为什么必须全部位于 crashk_res？（对照下一章：capture kernel 与被 dump 内存的关系——搬运者不能搬走自己脚下的地板。）
5. `control_code_page + PAGE_SIZE` 为什么拷在**页尾**？（提示：同页前半放恒等页表，一页两用。）

---

> 下一章：[crashkernel 预留与 elfcorehdr](02-crashkernel预留与elfcorehdr.md) —— kdump 的内存布局与证据链。
