# KASAN boot 初始化全景（Linux 6.6）

> 阅读目标：吃透 shadow 从无到有的完整 boot 旅程——**先有鸡还是先有蛋**的解法（★双页表戏法：x86 的 `early_top_pgt` 拷贝 / arm64 的 `tmp_pg_dir + cpu_replace_ttbr1`——"instrumented code couldn't execute without shadow memory" 注释原文）；早期模板体系（`kasan_early_shadow_page` 全零页 + 四级**自引用模板表**）；x86 `kasan_init` 的逐区清单（E820 循环 / vmalloc 的 shallow-vs-early 二选一 / CEA 的 "512GiB range is prohibitively expensive" / 内核镜像实填 / 五级 PGD 碰撞注释）；★**RO 收尾**（memset 清零 + `__PAGE_KERNEL_RO` 重装 PTE + 再刷 TLB）；`kasan_depth` 门控（boot 期禁报错→init 解封）；arm64 对照路径与热插拔复用面。全部对照 v6.6 当轮核对。前置：[KASAN_VMALLOC](kasan-vmalloc.md)｜[MTE 与 LAM](mte-lam.md)。

---

## 一、问题：先有鸡还是先有蛋

```
KASAN 开启后，内核代码全部被插桩——每条内存访问前都查 shadow
但 shadow 的页表本身要用插桩代码来建立……查的 shadow 还不存在？

三段解法：
  ① 模板：四级页表模板全部指向同一个"全零页"（kasan_early_shadow_page）
     ——零 shadow = 一切可访问 → 插桩查询永远通过（等效未开启）
  ② 双页表：先在副本页表上施工，真身不动——失败不污染 init_top_pgt
  ③ 深度门：init_task.kasan_depth = 1——boot 期间即使 shadow 半成品
     也不报错；kasan_init 收尾置 0 解封
```

## 二、早期模板体系

```c
/* 四级自引用模板：pmd 全部指向 early_shadow_pte，pud 指向 early_shadow_pmd…… */
void __init kasan_early_init(void)          /* arch/x86/mm/kasan_init_64.c:288 */
{
	pteval_t pte_val = __pa_nodebug(kasan_early_shadow_page) |
				__PAGE_KERNEL | _PAGE_ENC;
	pmdval_t pmd_val = __pa_nodebug(kasan_early_shadow_pte) | _KERNPG_TABLE;
	...
	for (i = 0; i < PTRS_PER_PTE; i++)
		kasan_early_shadow_pte[i] = __pte(pte_val);
	for (i = 0; i < PTRS_PER_PMD; i++)
		kasan_early_shadow_pmd[i] = __pmd(pmd_val);
	...
	kasan_map_early_shadow(early_top_pgt);
	kasan_map_early_shadow(init_top_pgt);
}
```

零页本身（`kasan_early_shadow_page`）在 .bss——**一张 4KB 页承包全部地址空间的 shadow 查询**（PTE 全指向它）。这套"全零放行"模板同时也是 **vmalloc 区无 KASAN_VMALLOC 时的长期 shadow**（[KASAN_VMALLOC 章](kasan-vmalloc.md) §一的 early-shadow 分支）。

## 三、x86 kasan_init：逐区清单（arch/x86/mm/kasan_init_64.c:344-456）

```
① memcpy(early_top_pgt, init_top_pgt) + load_cr3(early_top_pgt)   ← 双页表戏法
   五级碰撞注释原文（:352-360）："KASAN_SHADOW_END lands in the last PGD
   entry and it collides with bunch of things like kernel code, modules,
   EFI mapping, etc. We need to take extra steps to not overwrite them."
   （L5 下 tmp_p4d_table 抢救被碰撞的 PGD）

② clear_pgds(KASAN_SHADOW_START & PGDIR_MASK, KASAN_SHADOW_END)     ← 清模板

③ 逐区装配（真 shadow=实填 / 空洞=模板）：
   [hole]        early_shadow
   [physmap]     for (i < E820_MAX_ENTRIES) map_range(&pfn_mapped[i])
                 ——★按 e820 实区段建（hole 之间不浪费页表）
   [直接映射上界 → VMALLOC_START]  early_shadow
   [vmalloc]     KASAN_VMALLOC? shallow_populate_pgds（只填 PGD/P4D）
                 : early_shadow          ← 回扣 [KASAN_VMALLOC 章] §一
   [VMALLOC_END+1 → CEA]          early_shadow
   [CEA 共享段]   kasan_populate_shadow(shadow_cea_begin, per_cpu_begin)
                 注释原文："Shadows for the per-CPU areas are mapped
                 on-demand...mapping the entire 512GiB range is
                 prohibitively expensive."
   [CEA 尾 → 内核镜像]  early_shadow
   [内核镜像]     kasan_populate_shadow(_stext → _end)   ← 实填
   [MODULES_END → KASAN_SHADOW_END]  early_shadow

④ load_cr3(init_top_pgt) + __flush_tlb_all()   ← 换回真身
```

## 四、★RO 收尾与 kasan_depth 解封（:444-456）

```c
	/*
	 * kasan_early_shadow_page has been used as early shadow memory, thus
	 * it may contain some garbage. Now we can clear and write protect it,
	 * since after the TLB flush no one should write to it.
	 */
	memset(kasan_early_shadow_page, 0, PAGE_SIZE);
	for (i = 0; i < PTRS_PER_PTE; i++) {
		prot = __pgprot(__PAGE_KERNEL_RO | _PAGE_ENC);
		pte = __pte(__pa(kasan_early_shadow_page) | pgprot_val(prot));
		set_pte(&kasan_early_shadow_pte[i], pte);
	}
	/* Flush TLBs again to be sure that write protection applied. */
	__flush_tlb_all();

	init_task.kasan_depth = 0;
	pr_info("KernelAddressSanitizer initialized\n");
```

★三重语义：**清零**（boot 期间零页可能被半路毒化数据污染过）→ **只读**（此后零页只该被读——写它=bug，硬件直接拦）→ **解封**（`kasan_depth=0`，报错通道开闸）。

**kasan_depth 门控**：`task_struct::kasan_depth`（sched.h:1362）；`kasan_enable_current()` → `current->kasan_depth++`（common.c:56-58）/ `kasan_disable_current()` 递减——**任何在 shadow 施工期间跑的代码可用它自我静音**（apply_alternatives 的 kasan_disable_current 同理，见 [trace/24 章](../../trace/docs/24-ALTERNATIVE深挖.md) §三）。

## 五、arm64 对照路径（arch/arm64/mm/kasan_init.c:214-305）

```
arm64 的双页表戏法（:230-237 注释原文）：
  "At first we should unmap early shadow (clear_pgds() call below).
   However, instrumented code couldn't execute without shadow memory.
   tmp_pg_dir used to keep early shadow mapped until full shadow setup
   will be finished."
  —— memcpy(tmp_pg_dir, swapper_pg_dir) + cpu_replace_ttbr1(tmp_pg_dir)

逐区装配（对照 x86）：
  [内核镜像 kimg]   kasan_map_populate（实填）
  [PAGE_END→MODULES_VADDR / VMALLOC_END→SHADOW_END]  early_shadow
  [physmap]         for_each_mem_range(i, ...) 循环实填  ← arm64 的"E820"
  BUILD_BUG_ON(VMALLOC_START != MODULES_END)          ← 布局不变式入编译

RO 收尾同款（:265-272）："KAsan may reuse the contents of
  kasan_early_shadow_pte directly, so we should make sure that it maps
  the zero page read-only." + PAGE_KERNEL_RO + memset(KASAN_SHADOW_INIT)

kasan_init_depth()（:277-280）→ init_task.kasan_depth = 0
★arm64 实现了 kasan_populate_early_vm_area_shadow（:282-295，kasan_map_populate
  实填 early vmalloc 的 shadow）——[KASAN_VMALLOC 章] §四的 "__weak 空实现、
  arch 覆盖" 承诺在此兑现（early ioremap 期的 shadow 由 arch 自己铺）
```

## 六、复用面：boot 代码的两条第二生命

| boot 产物 | 复用者 | 详见 |
| :--- | :--- | :--- |
| `kasan_populate_early_shadow` + `kasan_remove_zero_shadow`/`kasan_add_zero_shadow`（init.c:236/:457/:486） | 内存热插拔：OFFLINE 拆零 shadow / ONLINE 装回 | [KASAN_VMALLOC 章](kasan-vmalloc.md) §七 |
| `kasan_shallow_populate_pgds` | KASAN_VMALLOC 的 vmalloc 区上层目录 | 本章 §三 |
| `kasan_populate_shadow_for_vaddr`（x86 :333-341） | 任意虚拟区段的 shadow 实填（对外出口） | —— |

## 七、双路径对照表

| 维度 | x86 | arm64 |
| :--- | :--- | :--- |
| 双页表 | `early_top_pgt` 拷贝 + `load_cr3` | `tmp_pg_dir` + `cpu_replace_ttbr1` |
| physmap 实填 | `for (E820_MAX_ENTRIES)` 循环 | `for_each_mem_range` 循环 |
| vmalloc 区 | shallow（KASAN_VMALLOC）/early 二选一 | 同语义（VMALLOC_END 以上 early） |
| 零页 RO | `__PAGE_KERNEL_RO` + 手工 mask | `PAGE_KERNEL_RO` |
| 零页清零 | memset 0（GENERIC 语义） | memset **KASAN_SHADOW_INIT**（SW_TAGS 下 0xFE！） |
| extra | 五级 PGD 碰撞处理、CEA on-demand | early vmalloc shadow 实现体 |

## 八、观测实战

```bash
# 1) 初始化完成标志（两条路径各一句）
dmesg | grep KernelAddressSanitizer
#   x86: "KernelAddressSanitizer initialized"
#   arm64: "KernelAddressSanitizer initialized (generic)" 等

# 2) 开关与模式
#   cmdline: kasan=on/off；ftrace/KASAN_TAGS_OFF etc.（[MTE 章] §二）
grep -E 'CONFIG_KASAN' /boot/config-$(uname -r)

# 3) 零页写保护的自证（故意制造 shadow 写——应 oops 在 RO 页）
#   （fuzz/测试场景：KASAN_KUNIT_TEST 的 shadow 操纵用 kasan_force）

# 4) boot 期静音证据：kasan_depth
#   —— /proc 不导出；观测方式是代码路径（grep kasan_disable_current 调用点）
grep -rn 'kasan_disable_current' kernel/ mm/ arch/ | wc -l   # 自静音用户数
```

## 九、思考题

1. 双页表戏法为什么必要？在真身上直接施工最坏会怎样？（提示：clear_pgds 后到实填完成之间存在"无 shadow 窗口"——真身上的插桩代码此时查询会打到**无映射 PTE**→页表错误；副本施工完一刀切回来，真身从未经历窗口。）
2. 零页在 boot 期间"可能含 garbage"——谁会写它？（提示：boot 早期 kmalloc/vmalloc 的毒化路径（shadow 写 = 写零页）；因此收尾 memset 清零——但 SW_TAGS 的 arm64 用 0xFE 初始化而非 0，差异对应 tags 语义。）
3. `kasan_populate_shadow` 为什么按 e820 区段循环而不是整段 [PAGE_OFFSET, MAXMEM]？（提示：实填要分配真页表+真 shadow 页；physmap 的 hole（e820 空洞）没有内存也不需要 shadow——按段填省下 TB 级 shadow 页表。）
4. CEA 的 per-CPU 部分为何 on-demand？（提示：512GiB 随机窗口 × 每页 shadow 的代价"prohibitively expensive"；共享段小可实填，per-CPU 段等 CPU 上线时（cpu_entry_area 的 per-CPU 建立路径）再铺。）
5. `init_task.kasan_depth = 1` 的默认值在什么时刻生效？（提示：init_task 是静态构造——编译期即 1；kasan_init 前的一切启动代码（含自身）都被静音；fork 出的子任务继承 depth=0 后的值。）
6. arm64 用 `KASAN_SHADOW_INIT` 而非 0 清零页——SW_TAGS 的 0xFE 语义是什么？（提示：[MTE 章] §二——SW_TAGS 的 shadow init 值 0xFE=KASAN_TAG_INVALID——零页作为"无效标签"而非"可访问"，两种模式的零页语义相反！）

---

> 回看：[KASAN_VMALLOC](kasan-vmalloc.md)（vmalloc 区的 shallow/early 二选一与热插拔复用）｜[MTE 与 LAM](mte-lam.md)（KASAN_SHADOW_INIT=0xFE 的 tags 语义）｜[trace/24-ALTERNATIVE](../../trace/docs/24-ALTERNATIVE深挖.md)（kasan_disable_current 的另一位用户）｜[canonical_address_space](../canonical_address_space/docs/x86_64四级页表线性空间布局.md)（逐区清单的地址常量地图）｜[mm/README](../README.md)（分册导读）
