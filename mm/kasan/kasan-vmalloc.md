# KASAN_VMALLOC：shadow 覆盖非连续映射（Linux 6.6）

> 阅读目标：吃透 KASAN 把检测能力扩展到 **vmalloc 区**的完整机制——为什么 32TB 的 vmalloc 区不能预映射 shadow（只能"浅填 PGD + 按需下探"）、分配路径的 `kasan_populate_vmalloc_pte`（`__get_free_page` + `memset 0xF8` + init_mm 页表锁双检）、释放路径的**毒化即刻/页表回收延迟**两段式、★`kasan_release_vmalloc` 头注释的双 ASCII 图（区域与 shadow 页错位 (1)(2)(3) 的对齐难题 + 并发安全论证）、★跨 CPU 排序注释原文（`WRITE_ONCE(p, vmalloc(100))` 竞态剧本）、boot 期自举（热插拔 shadow 自己住在 vmalloc 区——"we'll just leak the memory" 原话）、以及 HW_TAGS 的正交实现（MTE 标签直接写物理页 + `__GFP_SKIP_KASAN|__GFP_SKIP_ZERO` 免清零优化）。全部对照 v6.6 当轮核对。前置：[vmalloc](../vmalloc/vmalloc.md)｜[sanitizers](../../locking/sanitizers.md)。

---

## 一、问题：vmalloc 区的 shadow 空洞

```
KASAN 软件模式（GENERIC/SW_TAGS）的原理：
  每字节 shadow 描述 8 字节内存（GENERIC，1:8）
  直接映射区（64TB）的 shadow 在 boot 期一次性建立——因为物理内存范围已知

vmalloc 区（32TB）的困境：
  映射是运行期动态建立的——boot 期不知道哪些虚拟地址会被用
  若预映射全部 shadow：32TB / 8 = 4TB shadow 页表+物理页——不可接受
  若完全不映射：vmalloc 分配的越界读写全部漏检
```

**不开 KASAN_VMALLOC 时到底是什么状态**？x86 的 `kasan_init()` 给出了权威答案（`arch/x86/mm/kasan_init_64.c:395-407` 原文）：

```c
	/*
	 * If we're in full vmalloc mode, don't back vmalloc space with early
	 * shadow pages. Instead, prepopulate pgds/p4ds so they are synced to
	 * the global table and we can populate the lower levels on demand.
	 */
	if (IS_ENABLED(CONFIG_KASAN_VMALLOC))
		kasan_shallow_populate_pgds(
			kasan_mem_to_shadow((void *)VMALLOC_START),
			kasan_mem_to_shadow((void *)VMALLOC_END));
	else
		kasan_populate_early_shadow(
			kasan_mem_to_shadow((void *)VMALLOC_START),
			kasan_mem_to_shadow((void *)VMALLOC_END));
```

| KASAN_VMALLOC | vmalloc 区 shadow 的状态 | 检测能力 |
| :--- | :--- | :--- |
| `=n` | **early shadow page（全 0 字节）** | 静默放行一切访问——越界/用后释放全漏检（模块是例外，见 §8） |
| `=y` | 只预填 PGD/P4D 上层目录，下层 PTE **按需填充** | 全覆盖，每分配建立真实 shadow |

## 二、开关与模式矩阵（lib/Kconfig.kasan:179-190）

```
config KASAN_VMALLOC
	bool "Check accesses to vmalloc allocations"
	depends on HAVE_ARCH_KASAN_VMALLOC
	help
	  Makes KASAN check the validity of accesses to vmalloc allocations.

	  With software KASAN modes, all types vmalloc allocations are
	  checked. Enabling this option leads to higher memory usage.

	  With Hardware Tag-Based KASAN, only non-executable VM_ALLOC mappings
	  are checked. There is no additional memory usage.
```

* **`HAVE_ARCH_KASAN_VMALLOC` 的选择者（7 架构）**：arm、arm64、powerpc、riscv、s390、um、x86（Kconfig.kasan:13 的哑选项，由各 arch Kconfig select）
* **三模式 × 开关的能力矩阵**：

| 模式 | KASAN_VMALLOC=y 覆盖面 | shadow/标签载体 | 额外内存 |
| :--- | :--- | :--- | :--- |
| GENERIC | **所有** vmalloc 分配（含 vm_map_ram/get_vm_area） | shadow 页表按需填充 | 有（shadow 页 + 页表） |
| SW_TAGS | 所有 vmalloc 分配（可执行内存不打标签） | shadow 页表按需填充 | 有 |
| HW_TAGS（MTE） | **仅非可执行 VM_ALLOC 映射** | MTE 标签直接写物理页 | **零** |
| 三模式 + `=n` | 仅模块（module shadow 专路，§8） | — | — |

* **HW_TAGS 专属 boot 参数**：`kasan.vmalloc=off/on`（hw_tags.c:116-127 的 `early_param`）——因为 HW_TAGS 无 shadow 内存开销，这个能力可以运行时开关；init 完成时打印 `vmalloc=%s`（hw_tags.c:254-257 的 pr_info 原文："KernelAddressSanitizer initialized (hw-tags, mode=%s, vmalloc=%s, stacktrace=%s)"）。

## 三、flags 三比特（include/linux/kasan.h:24-29）

```c
typedef unsigned int __bitwise kasan_vmalloc_flags_t;

#define KASAN_VMALLOC_NONE		((__force kasan_vmalloc_flags_t)0x00u)
#define KASAN_VMALLOC_INIT		((__force kasan_vmalloc_flags_t)0x01u)
#define KASAN_VMALLOC_VM_ALLOC		((__force kasan_vmalloc_flags_t)0x02u)
#define KASAN_VMALLOC_PROT_NORMAL	((__force kasan_vmalloc_flags_t)0x04u)
```

**flags 在 `__vmalloc_node_range` 的计算**（mm/vmalloc.c:3295-3338）——三个比特来源不同：

```
KASAN_VMALLOC_PROT_NORMAL：pgprot_val(prot) == pgprot_val(PAGE_KERNEL) 时置
  ——"普通"保护属性的映射（可执行页不置——HW_TAGS 拒给 PC 所在页打标签）
  HW_TAGS 同时做两件事（vmalloc.c:3301-3314）：
    prot = arch_vmap_pgprot_tagged(prot)          // 改保护位允许打标签
    gfp_mask |= __GFP_SKIP_KASAN | __GFP_SKIP_ZERO // 物理页跳过 page_alloc 的毒化/清零
                                                   //——改由 kasan_unpoison_vmalloc 一趟完成

KASAN_VMALLOC_VM_ALLOC：__vmalloc_node_range 尾部恒置（vmalloc.c:3330）
  ——"由 vmalloc() 建立的第一个虚拟映射"（区别于 vm_map_ram 等非 VM_ALLOC 映射）

KASAN_VMALLOC_INIT：!want_init_on_free() && want_init_on_alloc(gfp_mask)
                    && (gfp_mask & __GFP_SKIP_ZERO)（vmalloc.c:3331-3333）
  ——头注释原话："The condition ... should complement the one in post_alloc_hook()
     with regards to the __GFP_SKIP_ZERO check to make sure that memory is
     initialized under the same conditions."（与 page_alloc 的初始化条件互补）
```

## 四、分配路径：shadow 页表按需填充（shadow.c:307-428）

### 1. 入口与调用点

| 调用点（mm/vmalloc.c） | 场景 |
| :--- | :--- |
| `:1641` `__get_vm_area_node` 内 `kasan_populate_vmalloc(addr, size)` | **所有** vm_area 预留（含模块——`is_vmalloc_or_module_addr` 放行） |
| `:4160` `pcpu_get_vm_areas` 内逐 area 调用 | per-CPU 大块（`setup_per_cpu_areas`） |
| `:2497` `kasan_populate_early_vm_area_shadow(vm->addr, vm->size)` | **early vmalloc**（memblock 期，arch 覆盖 `__weak` 空实现——shadow.c:309-312） |

### 2. PTE 级填充回调（shadow.c:314-339）

```c
static int kasan_populate_vmalloc_pte(pte_t *ptep, unsigned long addr,
				      void *unused)
{
	unsigned long page;
	pte_t pte;

	if (likely(!pte_none(ptep_get(ptep))))
		return 0;                       // 已有映射——跳过

	page = __get_free_page(GFP_KERNEL);  // ★shadow 页来自 buddy（非 slab）
	if (!page)
		return -ENOMEM;

	memset((void *)page, KASAN_VMALLOC_INVALID, PAGE_SIZE);  // 整页预毒化 0xF8
	pte = pfn_pte(PFN_DOWN(__pa(page)), PAGE_KERNEL);

	spin_lock(&init_mm.page_table_lock);   // ★双检闭合竞态：
	if (likely(pte_none(ptep_get(ptep)))) {  //   拿锁后再看一次是否仍为空
		set_pte_at(&init_mm, addr, ptep, pte);
		page = 0;
	}
	spin_unlock(&init_mm.page_table_lock);
	if (page)
		free_page(page);                //   输了竞争——退页
	return 0;
}
```

* **`KASAN_VMALLOC_INVALID`**：GENERIC 为 `0xF8`（mm/kasan/kasan.h:141，注释原话 "inaccessible space in vmap area"）；SW_TAGS 为 `KASAN_TAG_INVALID`（:147）
* **`KASAN_MEMORY_PER_SHADOW_PAGE = KASAN_GRANULE_SIZE << PAGE_SHIFT`**（mm/kasan/kasan.h:134）——GENERIC（granule 8B）下一页 4KB shadow 管 **32KB** vmalloc 内存
* 外层 `kasan_populate_vmalloc`（shadow.c:341-379）：`is_vmalloc_or_module_addr` 过滤 → `PAGE_ALIGN_DOWN/UP` 对齐 shadow 区间 → `apply_to_page_range(&init_mm, ...)` 逐 PTE 走上面的回调 → `flush_cache_vmap`；UML 特判（boot 期已映射全部 shadow，只需 memset）
* **先整页毒化再由 unpoison 挖出可用区**——这样分配区尾部红区天然是 0xF8

### 3. unpoison：挖出用户区（shadow.c:553-585）

```c
void *__kasan_unpoison_vmalloc(const void *start, unsigned long size,
			       kasan_vmalloc_flags_t flags)
{
	/*
	 * Software KASAN modes unpoison both VM_ALLOC and non-VM_ALLOC
	 * mappings, so the KASAN_VMALLOC_VM_ALLOC flag is ignored.
	 * ... the KASAN_VMALLOC_INIT flag is ignored.   （两 flag 仅 HW_TAGS 用）
	 */
	...
	if (IS_ENABLED(CONFIG_KASAN_SW_TAGS) &&
	    !(flags & KASAN_VMALLOC_PROT_NORMAL))
		return (void *)start;   // SW_TAGS：可执行内存不打标签
	                                //（"The kernel doesn't tolerate having
	                                //   the PC register tagged." :572-574）
	start = set_tag(start, kasan_random_tag());
	kasan_unpoison(start, size, false);   // 挖出 [start, start+size)
	return (void *)start;                     // 尾部红区保持 0xF8
}
```

**调用矩阵**（mm/vmalloc.c）：`:3337` `__vmalloc_node_range`（vmalloc 主路径，全 flags）｜`:2414-2416` `vm_map_ram` 区（`KASAN_VMALLOC_PROT_NORMAL`——非 VM_ALLOC 映射）｜`:2609-2612` `__get_vm_area_node` 返回路径（预留未映射页的场景，注释同指非 VM_ALLOC）。

### 4. ★跨 CPU 排序：为什么 unpoison 与发布指针之间需要屏障（shadow.c:381-414 原文）

```
   CPU#0                                  CPU#1
 WRITE_ONCE(p, vmalloc(100));            while (x = READ_ONCE(p)) ;
                                         p[99] = 1;

 // vmalloc() allocates memory
 // let a = area->addr
 // we reach kasan_populate_vmalloc
 // and call kasan_unpoison:
 STORE shadow(a), unpoison_val
 ...
 STORE shadow(a+99), unpoison_val        x = LOAD p
 // rest of vmalloc process              <data dependency>
 STORE p, a                              LOAD shadow(x+99)

 If there is no barrier between the end of unpoisoning the shadow
 and the store of the result to p, the stores could be committed
 in a different order by CPU#0, and CPU#1 could erroneously observe
 poison in the shadow.
```

**屏障由谁提供**（注释结尾原话）："In the vmalloc() case, this is provided by a `smp_wmb()` in `clear_vm_uninitialized_flag()`. In the per-cpu allocator and in `get_vm_area()` and friends, the caller gets shadow allocated but doesn't have any pages mapped ... Mapping those pages in will involve taking and releasing a page-table lock, which will provide the barrier."——vmalloc 主路径靠显式 `smp_wmb()`，其余路径靠映射物理页时的页表锁。

## 五、释放路径：毒化即刻、页表回收延迟

**两段式设计**——vfree 时只写 shadow（便宜），shadow 页表项等 lazy purge 才拆（批量摊销）：

```
vfree(mem)
  → remove_vm_area（vmalloc.c:2697-2704）
      kasan_free_module_shadow(vm)              // 模块专路（§8），通常 no-op
      kasan_poison_vmalloc(vm->addr, size)      // ①即刻毒化：shadow 写 0xF8
      free_unmap_vmap_area(va)                  //    vmap area 进 lazy 链
                                                 //    ——此后对该地址的任何访问
                                                 //      立即被 KASAN 抓（用后释放）
  ……（延迟，直到 vmap 空间吃紧或显式 purge）……
__purge_vmap_area_lazy（vmalloc.c:1740-1775）
  spin_lock(&free_vmap_area_lock)
  va = merge_or_add_vmap_area_augment(va, ...)  // 先并回 free tree——得到完整
                                                 // free region [va->va_start, va->va_end)
  if (is_vmalloc_or_module_addr((void *)orig_start))
      kasan_release_vmalloc(orig_start, orig_end,   // ②延迟拆页表：
                            va->va_start, va->va_end) //   用并区后的 free_region 参数
```

`__kasan_poison_vmalloc`（shadow.c:586-593）：`round_up(size, KASAN_GRANULE_SIZE)` 后整段写 `KASAN_VMALLOC_INVALID`。`vm_unmap_ram` 路径同样即刻毒化（vmalloc.c:2350）。

## 六、★kasan_release_vmalloc 的对齐难题（shadow.c:434-521 头注释全文）

### 1. 症结：区域按页对齐 ≠ shadow 按页对齐

```
头注释第一张图（原文）：
                    start                     end
                    v                         v
 |????????|????????|AAAAAAAA|AA....AA|AAAAAAAA|????????| < vmalloc
  -------- -------- --------          -------- --------
      |        |       |                 |        |
      |        |       |         /-------/        |
      \-------\|/------/         |/---------------/
              |||                ||
             |??AAAAAA|AAAAAAAA|AA??????|                < shadow
                 (1)      (2)      (3)

只被本区域完全覆盖的 shadow 页是 (2)——可整页释放。
半覆盖的 (1)(3) 不能直接动——它们与其他区域共享。
```

### 2. 解法：借 free region 的外延判断半覆盖页

```
头注释第二张图（原文）：
 free_region_start                                    free_region_end
 |                 start                     end      |
 v                 v                         v        v
 |FFFFFFFF|FFFFFFFF|AAAAAAAA|AA....AA|AAAAAAAA|FFFFFFFF| < vmalloc
      ...（同样的投影）...
             |FFAAAAAA|AAAAAAAA|AAF?????|                < shadow
                 (1)      (2)      (3)

free region 也做 shadow 页对齐后：(1) 整页都在 free region 内 → 可释放；
(3) 右邻是未知区域（?） → 不可释放。
注释原话："we don't try to free other pages from the free region or
we'd end up trying to free huge chunks of virtual address space."
```

函数体的四步对齐（shadow.c:522-535）：`region_start = ALIGN(start, KASAN_MEMORY_PER_SHADOW_PAGE)`、`region_end = ALIGN_DOWN(end, ...)`，再以 free_region 边界条件**各外扩一页**（`if (start != region_start && free_region_start < region_start) region_start -= KASAN_MEMORY_PER_SHADOW_PAGE;` 等）。

### 3. 并发安全：populate 不持锁为何不炸（shadow.c:507-521 原话）

```
"We _can_ have kasan_release_vmalloc and kasan_populate_vmalloc
 running at the same time. While we run under free_vmap_area_lock,
 the population code does not.

 free_vmap_area_lock instead operates to ensure that the larger range
 [free_region_start, free_region_end) is safe: because __alloc_vmap_area
 and the per-cpu region-finding algorithm both run under
 free_vmap_area_lock, no space identified as free will become used
 while we are running. This means that so long as we are careful with
 alignment and only free shadow pages entirely covered by the free
 region, we will not run in to any trouble - any simultaneous
 allocations will be for disjoint regions."
```

★锁协议分层：release 持 `free_vmap_area_lock` 保证 free region 语义不变；populate 只对单 PTE 持 `init_mm.page_table_lock`——两把锁各管一层，互不依赖。拆页由 `apply_to_existing_page_range` + `kasan_depopulate_vmalloc_pte`（shadow.c:416-432：`pte_clear` + `free_page` 同在 init_mm 页表锁下）完成，尾随 `flush_tlb_kernel_range`。

## 七、boot 期与热插拔：shadow 的自举（shadow.c:232-306）

**热插拔新内存的 shadow 是 vmalloc 出来的**——`kasan_mem_notifier` 的 `MEM_GOING_ONLINE` 分支（shadow.c:254-263）：

```c
	ret = __vmalloc_node_range(shadow_size, PAGE_SIZE, shadow_start,
				shadow_end, GFP_KERNEL,
				PAGE_KERNEL, VM_NO_GUARD,
				pfn_to_nid(mem_data->start_pfn),
				__builtin_return_address(0));
```

★**递归奇观**：新内存直接映射区的 shadow，本身用 vmalloc 基础设施**精确落在 shadow 地址区间**（`vstart=shadow_start`）分配——vmalloc 反哺 KASAN，而 vmalloc 自己的 shadow 又由 §4 的机制按需填充（一层递归即止：shadow 的 shadow 只占 1/8）。

`MEM_OFFLINE` 分支的坦白（shadow.c:280-284 原话）：

```
	 * Currently it's not possible to free shadow mapped
	 * during boot by kasan_init(). It's because the code
	 * to do that hasn't been written yet. So we'll just
	 * leak the memory.
```

——boot 期映射的 shadow 离线时**故意泄漏**（只 vfree 热插拔来的那份，用 `find_vm_area` 区分两者）。

## 八、模式分叉：HW_TAGS 正交实现与模块专路

### 1. HW_TAGS：无 shadow，标签写物理页（hw_tags.c:262-341）

```
__kasan_unpoison_vmalloc（hw_tags.c:294-341）的判定序：
  ① !kasan_vmalloc_enabled()（kasan.vmalloc=off）→ 仅按 INIT flag 清零，不打标
  ② !(flags & KASAN_VMALLOC_VM_ALLOC) → 跳过（非 vmalloc() 建立的映射）
  ③ !(flags & KASAN_VMALLOC_PROT_NORMAL) → 跳过（可执行页）
  ④ tag = kasan_random_tag(); set_tag; 逐页 page_kasan_tag_set
unpoison_vmalloc_pages（:262-281）：沿 area->pages 逐物理页设标签
init_vmalloc_pages（:283-291）：clear_highpage_kasan_tagged——清零与打标一趟完成
```

**只标 VM_ALLOC+PROT_NORMAL 的三点理由**（hw_tags.c:308-327 注释原文节选）：① "hardware tag-based KASAN only supports tagging physical memory. Therefore, it can only tag a single mapping of normal physical pages."（物理内存只能单映射打标）② 只有用 `arch_vmap_pgprot_tagged()` 特殊保护位映射的内存才能打标（③ 非可执行限制同软件模式——PC 寄存器不容忍带标签）。配套优化在 vmalloc.c:3310-3314：物理页 `__GFP_SKIP_KASAN|__GFP_SKIP_ZERO`——**page_alloc 的毒化/清零整趟省掉**，由 KASAN 打标时顺带完成（这就是 KASAN_VMALLOC_INIT flag 存在的意义）。

### 2. `KASAN_VMALLOC=n` 的模块专路（shadow.c:596+ / kasan.h:453-460）

kasan.h:453-460 注释原话："These functions allocate and free shadow memory for kernel modules. **They are only required when KASAN_VMALLOC is not supported, as otherwise shadow memory is allocated by the generic vmalloc handlers.**"

```
路径：module_alloc 之后 → kasan_alloc_module_shadow(p, size, gfp)
  实现（shadow.c:596+）：__vmalloc_node_range 在 shadow 地址区间内
  精确分配一块 shadow（与热插拔同款手法）
调用者：arch/x86/kernel/module.c:82 / arch/arm64/kernel/module.c:144
        / arch/s390/kernel/module.c:69（module_alloc 返回后立即调）
释放：remove_vm_area 的 kasan_free_module_shadow（vmalloc.c:2702）
KASAN_VMALLOC=y 时两者都是 no-op（kasan.h:459-460 的 inline 空桩）
```

## 九、观测实战

```bash
# 1) 本机是否开了 KASAN_VMALLOC（编译期）
grep -E 'CONFIG_KASAN(_VMALLOC)?=' /boot/config-$(uname -r)
#   CONFIG_KASAN_GENERIC=y + CONFIG_KASAN_VMALLOC=y：软模式全覆盖
#   CONFIG_KASAN_HW_TAGS=y：看下行 dmesg 的 vmalloc=on/off

# 2) HW_TAGS 的运行时开关结果
dmesg | grep KernelAddressSanitizer
#   "... initialized (hw-tags, mode=sync, vmalloc=on, stacktrace=on)"

# 3) 检测形态：vfree 后访问（毒化即刻生效——不等 lazy purge）
#   报告中 shadow 字节为 0xF8 → "inaccessible space in vmap area"
#   （区别于 slab 的 0xFB use-after-free / 0xFA alloc-redzone）

# 4) shadow 页开销估算（GENERIC）
#   vmalloc 已用虚拟内存 / 32KB ≈ shadow 页数（每页 4KB）
cat /proc/vmallocinfo | awk '{s+=$2} END {print "vmalloc bytes:", s}'

# 5) purge 触发时机（shadow 页表在此时才拆）
grep -n 'purge' /proc/vmallocinfo 2>/dev/null   # vmap 空间吃紧时
```

## 十、思考题

1. 为什么 `kasan_populate_vmalloc_pte` 先 `__get_free_page` 再在锁内双检，而不是先锁后分配？（提示：锁外分配允许并行填充不相邻 PTE；双检闭合"两个 CPU 同时填充同一 PTE"的窗口——输家 free_page 退回。对照 §6 的锁分层。）
2. vfree 已经把 shadow 毒化成 0xF8，为什么页表项还要等 lazy purge 才拆？（提示：立刻拆需要立即 `flush_tlb_kernel_range`——多核 TLB 击落昂贵；且该 vmap area 可能马上被新区间复用——purge 时借 free region 批量判断（§6 的 (1)(3) 逻辑）才是安全窗口。）
3. `KASAN_VMALLOC_INIT` 为什么只在 HW_TAGS 有意义？（提示：软模式无法把"清零"与"写 shadow"合并成一趟内存访问；HW_TAGS 的 MTE 标签写在与数据同一物理页的标签位——`clear_highpage_kasan_tagged` 一趟完成，从而敢给 page_alloc 下 `__GFP_SKIP_ZERO`。）
4. 热插拔 shadow 的递归为什么一层就停？（提示：新内存 shadow 用 vmalloc 分配；vmalloc 自身的 shadow 按 1:8 只需再占 shadow 的 1/8——但那一层的 shadow 页又是谁填充的？想想 `apply_to_page_range` 从 PGD 浅填下探的起点。）
5. x86 上 `CONFIG_KASAN_VMALLOC=n` 时模块为什么还能被检测？（提示：`kasan_alloc_module_shadow` 专路——模块加载器单独为模块区间 vmalloc 一块 shadow；那 vmalloc 区其余部分（普通 `vmalloc()` 调用）的 shadow 是什么？（§1 的 early shadow page。））
6. `kasan_release_vmalloc` 若把 `(3)` 号半覆盖页也释放会发生什么？（提示：右邻 `?` 区域可能正在被使用——其 shadow 一旦被 free_page，访问会打到**无映射 PTE**上→缺页/panic，比漏检严重得多。注释只保证释放"entirely covered by the free region"的页。）

---

> 回看：[vmalloc](../vmalloc/vmalloc.md)（vmap area/lazy purge 的机制基础——本章 §5/§6 的前提）｜[sanitizers](../../locking/sanitizers.md)（KASAN 三模式总览与 0xF8/0xFB/0xFA 毒值表）｜[页表布局](../canonical_address_space/docs/x86_64四级页表线性空间布局.md)（vmalloc 区 32TB/模块区的地址常量）｜[mm/README](../README.md)（分册导读）
