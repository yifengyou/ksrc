# MTE 与 LAM：地址位里的元数据编码（Linux 6.6）

> 阅读目标：吃透两大"往地址高位塞元数据"的硬件方案在 v6.6 的完整落地——**arm64 MTE**（Memory Tagging Extension：4-bit 标签藏在 bits 63:56、每 16 字节一个 granule、SCTLR_EL1.TCF 三种检查模式、TCO 覆盖位与 futex 独占的恩怨、`mte_set_mem_tag_range` 的 STG/DC-GVA 块优化循环、KASAN 的 `0xF<x>` 标签格式、★带标签页拒绝 KSM 合并）与 **x86 LAM**（Linear Address Masking：CR3 驱动的 per-mm 掩码 62:57、`arch_prctl` 四命令 ABI、per-CPU `tlbstate_untag_mask` 的 ALTERNATIVE 零开销读取、★`__access_ok` "容忍而非掩除"的新哲学），以及两者共享的 **`untagged_addr()` 系统调用拆标签设施**。全部对照 v6.6 当轮核对。前置：[KASAN_VMALLOC](kasan-vmalloc.md)｜[sanitizers](../../locking/sanitizers.md)。

---

## 一、两种"地址带私货"的方案

| 维度 | arm64 MTE | x86 LAM |
| :--- | :--- | :--- |
| **元数据位** | bits **63:56**（byte 7 高 nibble，4-bit 标签） | bits **62:57**（U57，6-bit）或 62:48（U48，位已定义未开放） |
| **粒度** | **每 16 字节**一个标签（专用标签内存） | 无粒度——整段地址高位被忽略 |
| **硬件检查** | **有**（TCF 模式下访问时比对指针标签 vs 内存标签） | **无**（只改地址规范化检查——高位直接不翻译） |
| **使能单位** | 系统（SCTLR_EL1.TCF）+ 每进程（prctl TCF0） | **每 mm**（CR3 里的 LAM_U57 位） |
| **内核用途** | KASAN_HW_TAGS（[前章](kasan-vmalloc.md) §8） | **无**（6.6 仅为用户态开——见 §3 的 LAM_SUP 注释） |
| **用户态用途** | 应用自查（PR_MTE_TCF_*） | ASAN 加速、JIT 元数据（Kconfig 原文） |
| **6.6 状态** | 已上线（CONFIG_ARM64_MTE） | **6.6 新合入**（CONFIG_ADDRESS_MASKING） |

## 二、arm64 MTE：4-bit 标签与硬件检查

### 1. 常量与指令族（arch/arm64/include/asm/mte-def.h）

```c
#define MTE_GRANULE_SIZE	UL(16)     /* 每 16B 一个 granule */
#define MTE_GRANULES_PER_PAGE	(PAGE_SIZE / MTE_GRANULE_SIZE)   /* 256 */
#define MTE_TAG_SHIFT		56        /* 标签在 bits 59:56 */
#define MTE_TAG_SIZE		4
#define MTE_PAGE_TAG_STORAGE	(MTE_GRANULES_PER_PAGE * MTE_TAG_SIZE / 8)  /* 128B/页 */
#define __MTE_PREAMBLE		ARM64_ASM_PREAMBLE ".arch_extension memtag\n"
```

每页只需 **128 字节**标签存储（256 granule × 4 bit / 8）——硬件用专用内存（`DC GVA`/`STG` 指令直接写标签区）而非影子页。

### 2. bulk 打标的块优化（mte-kasan.h:150-197 的 mte_set_mem_tag_range）

```
mte_set_mem_tag_range(addr, size, tag, init)：
  从 DCZID_EL0 读 data cache block size（dczid_bs）
  三段循环（宏 SET_MEMTAG_RANGE）：
    ① STG/STZG 后增式打到第一个块边界
    ② DC GVA/GZVA 整块设标签（跨 [end1, end2)）
    ③ STG/STZG 收尾
头注释原话："The following code uses STG on the first DC GVA block even
if the start address is aligned - it appears to be faster than an
alignment check + conditional branch."
init=true 时用 STZG+GZVA——★打标签与清零一趟完成
（HW_TAGS 的 init_vmalloc_pages/KASAN_VMALLOC_INIT 的底牌，见前章 §8）
```

### 3. TCO：内核态访问用户页的"免检通行证"（mte-kasan.h:28-60）

头注释原文（裁剪）：

```
 * The Tag Check Flag (TCF) mode for MTE is per EL, hence TCF0
 * affects EL0 and TCF affects EL1 irrespective of which TTBR is
 * used.
 * The kernel accesses TTBR0 usually with LDTR/STTR instructions
 * when UAO is available, so these would act as EL0 accesses using TCF0.
 * However futex.h code uses exclusives which would be executed as
 * EL1, this can potentially cause a tag check fault even if the
 * user disables TCF0.
 *
 * To address the problem we set the PSTATE.TCO bit in uaccess_enable()
 * and reset it in uaccess_disable().
```

★**症结**：futex 的独占访问（LDXR/STXR）以 EL1 身份执行走 TCF（而非 TCF0）——即使用户关了 EL0 检查也可能炸；**解法**是 `uaccess_enable/disable` 里包一对 `mte_enable_tco()/mte_disable_tco()`（TCO=Tag Check Override，临时免检）。两函数用 `ALTERNATIVE("nop", SET_PSTATE_TCO(x), ARM64_MTE, ...)` 运行时补丁——无 MTE 的 CPU 上零开销。另有 async-only 变体（`__mte_disable_tco_async`），注释原话："the sync mode generates exceptions synchronously and the nofault or load_unaligned_zeropad can handle them"——同步模式宁可接异常也不关检查。

### 4. 三种检查模式的使能（arch/arm64/kernel/mte.c:81-135）

```c
static inline void __mte_enable_kernel(const char *mode, unsigned long tcf)
{
	sysreg_clear_set(sctlr_el1, SCTLR_EL1_TCF_MASK,
			 SYS_FIELD_PREP(SCTLR_EL1, TCF, tcf));
	isb();
	pr_info_once("MTE: enabled in %s mode at EL1\n", mode);
}
```

| 模式 | SCTLR_EL1.TCF | 语义 | 额外语义 |
| :--- | :--- | :--- | :--- |
| sync | TCF_SYNC | 访问即异常（精确地址） | `mte_enable_kernel_sync` 有 `WARN_ONCE(system_uses_mte_async_or_asymm_mode(), "MTE async mode enabled system wide!")`——★sync/async 系统级互斥 |
| async | TCF_ASYNC | 异步报告（批量、地址不精确） | "set system wide by the first PE"——static_key 全局翻转 |
| asymm | TCF_ASYMM | 读同步/写异步 | 需 `ARM64_MTE_ASYMM` cap；注释原话 "behaves as async mode for store operations" |

三函数由 KASAN 的 `kasan.mode=sync/async/asymm` boot 参数选择（hw_tags.c:97-114，见[前章](kasan-vmalloc.md) §2）；注释里还留了伏笔："If in future KASAN acquires a runtime switching mode in between sync and async, this strategy needs to be reviewed."

### 5. KASAN 的标签格式：`0xF<x>`（mte-kasan.h:84-89）

```c
static inline u8 mte_get_ptr_tag(void *ptr)
{
	/* Note: The format of KASAN tags is 0xF<x> */
	u8 tag = 0xF0 | (u8)(((u64)(ptr)) >> MTE_TAG_SHIFT);
	return tag;
}
```

★KASAN 的 8-bit 软件标签高 nibble 恒为 0xF——只有低 4 bit 进指针（硬件只有 4-bit 槽），这样 8-bit 的 `KASAN_TAG_INVALID`（0xFE）等值仍能单条指令映射进硬件标签。

### 6. 彩蛋：带标签页拒绝 KSM 合并（mte.c:60-74）

```c
	/*
	 * If the page content is identical but at least one of the pages is
	 * tagged, return non-zero to avoid KSM merging. If only one of the
	 * pages is tagged, set_pte_at() may zero or change the tags of the
	 * other page via mte_sync_tags().
	 */
	if (page_mte_tagged(page1) || page_mte_tagged(page2))
		return addr1 != addr2;
```

★内容完全相同的两页，只要有一页带标签就不许 KSM 合并——否则 `set_pte_at()` 的 `mte_sync_tags()` 会把另一页的标签抹掉。跨子系统约束：**KSM 的"内容相等"在 MTE 世界里不等于"相等"**。

## 三、x86 LAM：把高位借给软件（6.6 新合入）

### 1. Kconfig 官方动机（arch/x86/Kconfig:2229-2238 原文）

```
config ADDRESS_MASKING
	bool "Linear Address Masking support"
	depends on X86_64
	help
	  Linear Address Masking (LAM) modifies the checking that is applied
	  to 64-bit linear addresses, allowing software to use of the
	  untranslated address bits for metadata.

	  The capability can be used for efficient address sanitizers (ASAN)
	  implementation and for optimizations in JITs.
```

### 2. 位域与 per-mm 状态

- `X86_CR3_LAM_U57_BIT = 61`（uapi/asm/processor-flags.h:85）——CR3 置位后 **62:57 位被掩除**（6-bit 元数据）；`X86_CR3_LAM_U48` 位定义存在但 **prctl 未开放**（见下）
- `mm->context` 双字段（asm/mmu.h:49-55）：`lam_cr3_mask`（"X86_CR3_LAM_U48 or X86_CR3_LAM_U57 or 0"）+ `untag_mask`（"Significant bits of the virtual address. Excludes tag bits."）

### 3. 使能 ABI：arch_prctl 四命令（uapi/asm/prctl.h:28-31）

```c
#define ARCH_GET_UNTAG_MASK		0x4001   /* 读当前掩码 */
#define ARCH_ENABLE_TAGGED_ADDR		0x4002   /* 使能（一次性） */
#define ARCH_GET_MAX_TAG_BITS		0x4003   /* 最多几位（=6） */
#define ARCH_FORCE_TAGGED_SVA		0x4004   /* 与 Shared Virtual Address 共存 */
```

使能函数 `prctl_enable_tagged_addr`（arch/x86/kernel/process_64.c:751-790）的护栏：

```
① cpu_feature_enabled(X86_FEATURE_LAM) 否则 -ENODEV
② current->mm != mm → -EINVAL（PTRACE_ARCH_PRCTL 也不行）
③ SVA 互斥：mm_valid_pasid(mm) && !FORCE_TAGGED_SVA → -EINVAL
④ nr_bits: 0 → -EINVAL；1..LAM_U57_BITS(6) → X86_CR3_LAM_U57 + untag_mask=~GENMASK(62,57)
   （>6 → -EINVAL——★U48 未开放）
⑤ write_cr3(__read_cr3() | lam_cr3_mask) + set_tlbstate_lam_mode(mm)
   + 置 MM_CONTEXT_LOCK_LAM——★一次性：再调 -EBUSY
```

### 4. per-CPU 快路径与 CR3 折叠

- `DEFINE_PER_CPU(u64, tlbstate_untag_mask)`（arch/x86/mm/init.c:1067）——当前 mm 掩码的 per-CPU 镜像
- `set_tlbstate_lam_mode(mm)`（asm/tlbflush.h:402-406）：context switch 时同步 `cpu_tlbstate.lam` + `tlbstate_untag_mask`
- `tlbstate_lam_cr3_mask()`（:393-398）：`lam << X86_CR3_LAM_U57_BIT` 折进 CR3——LAM 模式**跟着 mm 走**（每个进程有自己的掩码，切换 mm 即切换掩码）
- ★`__untagged_addr` 的零开销读取（asm/uaccess_64.h:14-29）：

```c
	/*
	 * Refer tlbstate_untag_mask directly to avoid RIP-relative relocation
	 * in alternative instructions. The relocation gets wrong when gets
	 * copied to the target place.
	 */
	asm (ALTERNATIVE("",
			 "and %%gs:tlbstate_untag_mask, %[addr]\n\t", X86_FEATURE_LAM)
	     : [addr] "+r" (addr) : "m" (tlbstate_untag_mask));
```

——无 LAM 的 CPU 上是空操作；有 LAM 时一条 `and %gs:...`。注释还交代了 ALTERNATIVE 拷贝与 RIP 相对重定位不兼容的坑。

### 5. ★`__access_ok` 的新哲学：容忍而非掩除（uaccess_64.h:57-96 注释全文）

```
 * User pointers can have tag bits on x86-64.  This scheme tolerates
 * arbitrary values in those bits rather then masking them off.
 *
 * Enforce two rules:
 * 1. 'ptr' must be in the user half of the address space
 * 2. 'ptr+size' must not overflow into kernel addresses
 *
 * Note that addresses around the sign change are not valid addresses,
 * and will GP-fault even with LAM enabled if the sign bit is set (see
 * "CR3.LAM_SUP" that can narrow the canonicality check if we ever
 * enable it, but not remove it entirely).
 *
 * ... we could probably remove the size check entirely, since
 * any kernel accesses will be in increasing address order starting
 * at 'ptr', and even if the end might be in kernel space, we'll
 * hit the GP faults for non-canonical accesses before we ever get
 * there.
```

★**x86 的取舍**：不在 `__access_ok` 里拆标签（省一次掩码），而是**放任高位任意值**，靠 CPU 的规范化检查兜底——符号位一过就 GP，LAM 也救不了。注释还预告了 `CR3.LAM_SUP`（内核侧 LAM）——"if we ever enable it"——6.6 未启用，这就是 LAM 暂不服务内核 KASAN 的原话证据。

## 四、untagged_addr()：内核侧统一拆标签设施

### 1. 三层定义

| 层 | 定义 | 位置 |
| :--- | :--- | :--- |
| 通用桩 | `#define untagged_addr(addr) (addr)` | include/linux/uaccess.h:25 |
| arm64 | `__untagged_addr(addr) = sign_extend64(addr, 55)` | arch/arm64/include/asm/memory.h:240-247（原语层恒剥 byte 7；**ABI 层的 access_ok 有 TIF_TAGGED_ADDR 门**——见 [tagged-addr-prctl](tagged-addr-prctl.md)） |
| x86（LAM） | `and %gs:tlbstate_untag_mask`（§3.4） | arch/x86/include/asm/uaccess_64.h:14-29 |

arm64 版注释："When dealing with data aborts, watchpoints, or instruction traps we may end up with a tagged userland pointer. Clear the tag to get a sane pointer to pass on to access_ok(), for instance."——`sign_extend64(…, 55)` 把 bits 63:56 整字节剥掉并正确符号扩展（**原语与进程无关**，永远剥 byte 7；但"是否对用户指针应用它"由 access_ok 的 TIF_TAGGED_ADDR 门决定——opt-in ABI，见 [tagged-addr-prctl 章](tagged-addr-prctl.md) §一）。

### 2. arm64 的标签三宏（asm/memory.h:252-265）

```c
#if defined(CONFIG_KASAN_SW_TAGS) || defined(CONFIG_KASAN_HW_TAGS)
#define __tag_shifted(tag)	((u64)(tag) << 56)
#define __tag_reset(addr)	__untagged_addr(addr)
#define __tag_get(addr)		(__u8)((u64)(addr) >> 56)
```

SW_TAGS/HW_TAGS 共用同一套 bits 63:56 布局——软件标签是完整 8 bit，硬件标签 4 bit（`0xF<x>` 格式）。

### 3. 系统调用拆标签调用面（v6.6 实测 grep）

```
mm/gup.c:3187          pin 快路径入口
mm/mempolicy.c:1492/:1514/:1714   mbind/get_mempolicy系列
mm/mincore.c:239       sys_mincore
mm/mlock.c:581/:644    sys_mlock*
kernel/entry/syscall_user_dispatch.c:100   SUD selector 的 access_ok
```

——凡是"用户传地址进来"的慢路径入口都先 `untagged_addr()`。**x86 特有新维度**：`untagged_addr_remote(mm, addr)`（uaccess_64.h:36-43）——拆**别的进程**的指针必须先 `mmap_assert_locked(mm)`（掩码因 mm 而异，arm64 无此问题）。

## 五、用户态 ABI 对比

| | arm64 | x86 |
| :--- | :--- | :--- |
| 使能 | `prctl(PR_SET_TAGGED_ADDR_CTRL, 55, PR_TAGGED_ADDR_ENABLE \| PR_MTE_TCF_SYNC/ASYNC)` | `arch_prctl(ARCH_ENABLE_TAGGED_ADDR, 6)` |
| 检查控制 | **per-TCF0**：NONE/SYNC/ASYNC（uapi prctl.h:238-241） | 无检查可开——LAM 只掩码 |
| 撤销 | 可改 | **不可**（MM_CONTEXT_LOCK_LAM 一次性） |
| 位宽 | 4 bit（硬件槽） | 6 bit（U57） |
| 内核侧钩子 | TCO 覆盖 + mte_sync_tags | `__access_ok` 容忍 + untag_mask |

## 六、设计哲学对比（收束）

```
MTE 的路线：硬件比对——标签跟数据走，访问时强制检查
  → 内核 KASAN 白嫖（HW_TAGS 全套），用户态可自查
  → 代价：每页 128B 标签内存 + futex/TCO 一类边角协议

LAM 的路线：纯掩码——高位跟指针走，谁都不检查
  → 内核不用（LAM_SUP "if we ever enable it"），把能力整个让给用户态
    （Kconfig 原话：ASAN 加速 + JIT 优化）
  → 代价：仅 CR3 一个 bit + mm 上下文两个字段——几乎免费
  → 收益的代价是 __access_ok 必须学会"容忍高位"（§3.5 哲学）

共享的底座：untagged_addr()——系统调用边界处把用户指针洗白
  arm64：与进程无关（恒剥 byte 7）
  x86：与进程相关（mm->context.untag_mask）→ 需要 remote 变体与锁断言
```

## 七、观测实战

```bash
# 1) MTE 模式（arm64）
dmesg | grep 'MTE:'          # "MTE: enabled in synchronous mode at EL1"
dmesg | grep KernelAddressSanitizer   # "hw-tags, mode=sync, vmalloc=on, ..."

# 2) x86 LAM 探测（用户态视角）
#   cat /proc/cpuinfo 的 flags 无 lam 时 arch_prctl 返回 -ENODEV；
#   使能后 ARCH_GET_UNTAG_MASK 返回 0xc0ffffffffffffff（~GENMASK(62,57)）

# 3) KASAN 三模式 boot 参数（MTE 硬件）
#   kasan.mode=sync / async / asymm（hw_tags.c early_kasan_mode）

# 4) 带标签页对 KSM 的影响（arm64 MTE）
grep -c merged /sys/kernel/mm/ksm/pages_merged   # 标签页占比高时合并率下降
```

## 八、思考题

1. 为什么 KASAN 的 8-bit 标签要规定高 nibble 恒 0xF？（提示：硬件槽只有 4 bit；`0xF<x>` 让 8-bit 标签值与 4-bit 硬件标签之间只差一次掩码——`KASAN_TAG_INVALID=0xFE` 的低 nibble E 即硬件标签。）
2. futex 的独占访问为什么以 EL1 身份执行而普通 uaccess 不是？（提示：LDTR/STTR（UAO 路径）标注为 EL0 访问走 TCF0；独占指令 LDXR/STXR 没有这种"降权"形态——注释原话 "futex.h code uses exclusives which would be executed as EL1"。）
3. x86 的 `__access_ok` 不掩标签，凭什么安全？（提示：两规则 + 符号位 GP 兜底——"will GP-fault even with LAM enabled if the sign bit is set"；想想 LAM 掩的是 62:57 而不是 63。）
4. LAM 模式为什么放 CR3 而不是每线程寄存器？（提示：掩码语义属于**地址空间**（mm）而非任务——fork/clone(CLONE_VM) 天然继承；放 CR3 还能免费搭 context switch 的便车（tlbstate 同步点）。）
5. arm64 的 `untagged_addr` 为什么敢做成"与进程无关"？（提示：arm64 用户标签永远在 byte 7（bits 63:56），硬件布局固定；x86 LAM 的掩码宽度由用户自选（1-6 bit）→ 必须查 mm。）
6. MTE 的 asymm 模式为什么"读同步/写异步"？（提示：读副作用难回收（数据已被消费）、写异常可重试；对 KASAN 意味着 load 检测精确、store 检测延迟——兼顾精度与写密集负载的吞吐。）

---

> 回看：[KASAN_VMALLOC](kasan-vmalloc.md)（HW_TAGS 的 vmalloc 侧应用——本章 §2.2 的 init=true 路径即其底牌）｜[sanitizers](../../locking/sanitizers.md)（KASAN 三模式总览与毒值表）｜[ksm 与 zswap](../ksm/ksm与zswap.md)（§2.6 带标签页拒绝合并的机制侧）｜[canonical_address_space](../canonical_address_space/docs/x86_64四级页表线性空间布局.md)（bits 62:57 恰在非规范空洞之外的"闲置高位"）｜[mm/README](../README.md)（分册导读）
