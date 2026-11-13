# mte_sync_tags：标签随 PTE 走的同步协议（Linux 6.6）

> 阅读目标：吃透 MTE 标签作为**页属性**的生命周期管理——`PG_mte_tagged`（复用 `PG_arch_2`）与 `PG_mte_lock` 的★**一次性锁协议**（"PG_mte_tagged is never cleared and therefore the locking only happens once for page initialisation"+两态表+`smp_cond_load_acquire` 等待）；`mte_sync_tags` 的**首映清标签**语义（smp_wmb 与 PTE 写入的配对）；★`set_pte` 的**触发三条件**（含 "instruction fetches don't check tags" 的 exec-only 豁免注释）；`copy_highpage` 的迁移复制路径；**换页标签存取**（mteswap.c 的 128B kmalloc + 全局 xarray 以 swap entry 为键 + 四个 `__HAVE_ARCH_*` 钩子）；KSM 互斥的机制闭环（兑现 [MTE 与 LAM 章](mte-lam.md) §2.6 彩蛋）。全部对照 v6.6 当轮核对。前置：[MTE 与 LAM](mte-lam.md)｜[tagged_addr prctl](tagged-addr-prctl.md)。

---

## 一、问题：标签是页的属性，不是 PTE 的

```
MTE 的 4-bit 标签住在页的专用存储（每页 128B——256 granule×4bit/8，
见 [MTE 与 LAM 章] §2.1），不在页表项里。
→ 映射一个页时三个问题：
  ① 首次映射：页的标签存储可能有前任房客的残留——必须清
  ② 再次映射：标签已在（用户改过）——绝不能清（清=数据损坏）
  ③ 并发初始化：两个 CPU 同时首映同一页——必须有仲裁

解法：页标志位 PG_mte_tagged 记"②已初始化"，
      PG_mte_lock 做一次性仲裁
```

## 二、PG_mte_tagged 双位协议（arch/arm64/include/asm/mte.h:37-90）

```c
#define PG_mte_tagged	PG_arch_2   /* ★复用通用 arch 页标志位 */
#define PG_mte_lock	PG_arch_3    /* "simple lock to avoid multiple
                                      threads tagging the same page" */

static inline void set_page_mte_tagged(struct page *page)
{
	/* Ensure that the tags written prior to this function are visible
	   before the page flags update. */
	smp_wmb();
	set_bit(PG_mte_tagged, &page->flags);
}

static inline bool page_mte_tagged(struct page *page)
{
	bool ret = test_bit(PG_mte_tagged, &page->flags);
	/* If the page is tagged, ensure ordering with a likely subsequent
	   read of the tags. */
	if (ret)
		smp_rmb();     /* ★与置位侧的 wmb 配对——读到标志必读到标签 */
	return ret;
}
```

★`try_page_mte_tagging` 的一次性锁（头注释原文）：

```
 * Lock the page for tagging and return 'true' if the page can be tagged,
 * 'false' if already tagged. PG_mte_tagged is never cleared and therefore
 * the locking only happens once for page initialisation.
 *
 * The page MTE lock state:
 *   Locked:	PG_mte_lock && !PG_mte_tagged
 *   Unlocked:	!PG_mte_lock || PG_mte_tagged
 *
 * Acquire semantics only if the page is tagged (returning 'false').
```

```c
static inline bool try_page_mte_tagging(struct page *page)
{
	if (!test_and_set_bit(PG_mte_lock, &page->flags))
		return true;               /* 我抢到首映权 */

	/* 输家：标签正在被初始化或已就绪——★自旋等 PG_mte_tagged 置位 */
	smp_cond_load_acquire(&page->flags, VAL & (1UL << PG_mte_tagged));
	return false;
}
```

——**锁只活一次**：`PG_mte_tagged` 永不清除，此后所有 `try_page_mte_tagging` 都因 `PG_mte_lock` 已置而走等待→立即看到 tagged→返回 false（快路径）。

## 三、mte_sync_tags：首映清标签（kernel/mte.c:38-51）

```c
void mte_sync_tags(pte_t pte)
{
	struct page *page = pte_page(pte);
	long i, nr_pages = compound_nr(page);

	/* if PG_mte_tagged is set, tags have already been initialised */
	for (i = 0; i < nr_pages; i++, page++) {     /* 大页逐子页 */
		if (try_page_mte_tagging(page)) {
			mte_clear_page_tags(page_address(page));  /* ★清残留 */
			set_page_mte_tagged(page);
		}
	}

	/* ensure the tags are visible before the PTE is set */
	smp_wmb();                                    /* ★与 PTE 发布配对 */
}
```

★语义澄清：名为 "sync" 实为 **init-once**——首次映射把标签存储清成全零（未打标签=可访问的默认态）；此后用户态打标签（`stg`/`set_tag`）直接写存储，再次映射时 `try_page_mte_tagging` 返回 false **什么都不做**——用户标签安全。

## 四、set_pte 的触发三条件（arch/arm64/include/asm/pgtable.h:330-345）

```c
	if (system_supports_mte() && pte_access_permitted(pte, false) &&
	    !pte_special(pte) && pte_tagged(pte))
		mte_sync_tags(pte);
```

条件注释原文：*"If the PTE would provide user space access to the tags associated with it then ensure that the MTE tags are synchronised. Although pte_access_permitted() returns false for exec only mappings, **they don't expose tags (instruction fetches don't check tags)**."*

三条件拆解：**数据可达**（access_permitted——exec-only 映射豁免：取指不查标签）+ **非 special**（device/迁移条目不参与）+ **PTE 带 tagged 属性**（`pte_tagged`——PTE 的 attr 侧开关）。

## 五、copy_highpage：迁移/复制的标签搬运（arch/arm64/mm/copypage.c:14-33）

```c
void copy_highpage(struct page *to, struct page *from)
{
	copy_page(kto, kfrom);
	...
	if (system_supports_mte() && page_mte_tagged(from)) {
		/* It's a new page, shouldn't have been tagged yet */
		WARN_ON_ONCE(!try_page_mte_tagging(to));   /* 新页必须首映可抢 */
		mte_copy_page_tags(kto, kfrom);             /* 128B 标签随数据走 */
		set_page_mte_tagged(to);
	}
}
```

——源页已带标签（数据相等但**标签不等**）时，目标页必须连标签一起搬；WARN 断言目标页必是"未首映"的新页（否则生命周期管理出了错）。

## 六、换页：标签的 out-of-band 仓库（arch/arm64/mm/mteswap.c，85 行）

swap 设备不存标签——**标签留在内存的旁路仓库**：

```c
static DEFINE_XARRAY(mte_pages);        /* 全局 xarray，键=swap entry 值 */

int mte_save_tags(struct page *page)    /* arch_prepare_to_swap 钩子 */
{
	if (!page_mte_tagged(page))
		return 0;                      /* 无标签页零成本 */
	tag_storage = mte_allocate_tag_storage();   /* kmalloc(128B) */
	mte_save_page_tags(page_address(page), tag_storage);
	xa_store(&mte_pages, page_swap_entry(page).val, tag_storage, GFP_KERNEL);
	...
}

void mte_restore_tags(swp_entry_t entry, struct page *page)  /* arch_swap_restore */
{
	void *tags = xa_load(&mte_pages, entry.val);
	if (!tags) return;
	if (try_page_mte_tagging(page)) {           /* 换入新页——必可抢 */
		mte_restore_page_tags(page_address(page), tags);
		set_page_mte_tagged(page);
	}
}

void mte_invalidate_tags(int type, pgoff_t offset)   /* swap 槽位释放 */
{	... xa_erase(&mte_pages, entry.val); mte_free_tag_storage(tags); }

void mte_invalidate_tags_area(int type)              /* 整个 swap 区作废 */
{	/* xas_for_each 批量 erase+free */ }
```

四个 `__HAVE_ARCH_*` 钩子（pgtable.h:1027-1058）：`PREPARE_TO_SWAP`（换出前存）/`SWAP_INVALIDATE`（槽位释放时弃）/`SWAP_INVALIDATE_AREA` 整区弃/`SWAP_RESTORE`（换入后恢复）——mm/swap 核心的标准 per-arch 插槽。

★设计取舍：旁路 xarray 换来**swap 设备零改动**（标签不进 swap I/O），代价是换出页的 128B 元数据常驻内存 + xa 键空间随 swap entry 复用。

## 七、生命周期总图与 KSM 闭环

```
alloc ──► 首映（set_pte→mte_sync_tags：清标签+置 PG_mte_tagged）
          │
          ├─ 复制/迁移（copy_highpage：try 抢到→搬 128B 标签）
          ├─ 换出（arch_prepare_to_swap→mte_save_tags→xarray 旁路仓）
          ├─ 换入（arch_swap_restore→mte_restore_tags→写回存储）
          └─ KSM 检查（memcmp_pages mte.c:54-74）★
              "If the page content is identical but at least one of the
               pages is tagged, return non-zero to avoid KSM merging. If
               only one of the pages is tagged, set_pte_at() may zero or
               change the tags of the other page via mte_sync_tags()."
              ——内容相等 ≠ 标签相等；且被合并方的 set_pte 会触发
              mte_sync_tags 把幸存页标签清掉！
free ◄── （PG_mte_tagged 永不清除——页回 buddy 后标志随分配复位，
          标签存储由下次首映重清——"never cleared" 只在页的生命期内）
```

## 八、观测实战

```bash
# 1) MTE 生效与换页标签的内存代价
#   换出带标签页数 × 128B —— 无直接导出；间接：
grep Mte /proc/kpageflags 2>/dev/null | head      # 若编译含 KPF_MTE 位
awk '{t+=1} END {print "tagged pages:", t}' /proc/kpageflags  # 视内核配置

# 2) swap 侧
cat /proc/swaps; grep -w SwapTotal /proc/meminfo  # 换出量大时 xarray 膨胀

# 3) KSM 合并率与标签页的关系
grep pages_merged /sys/kernel/mm/ksm/*            # 标签密集负载下合并骤降

# 4) 协议正确性的压测面
#   - 迁移压力（numa balancing/migrate_pages）下的标签完整性
#   - swap 风暴下的 restore 正确性（mte 自测：kselftest arm64/mte*）
```

## 九、思考题

1. `try_page_mte_tagging` 的等待用 `smp_cond_load_acquire` 而非自旋读——acquire 语义买到什么？（提示：返回 false 路径的保证——等待者此后读标签必见初始化者的全部写入；"Acquire semantics only if the page is tagged (returning 'false')"——抢到者走 test_and_set 的原子序，输家走 acquire。）
2. `mte_sync_tags` 的 `smp_wmb` 与 `set_page_mte_tagged` 内部的 `smp_wmb`——两道屏障各防什么？（提示：前者防"标签写完才发布 PTE"（映射可见性）；后者防"标签写完才发布标志"（标志是其它 CPU 的判定依据）——一对外序一内序。）
3. exec-only 映射豁免（"instruction fetches don't check tags"）——为什么这是安全且完整的？（提示：标签检查只发生在数据访问（load/store）；代码页的取指路径架构上不查——豁免精确匹配硬件行为，省一次无谓清标签。）
4. swap 旁路仓为什么选 xarray 而不是把标签塞进 swap entry 的 extra 空间？（提示：swap entry 的 extra 位早已被 migrate/writeback 等占用且位数零碎；128B 标签装不下——只能旁路；键用 entry.val 天然去重（同槽换写覆盖）。）
5. `mte_restore_tags` 里 `try_page_mte_tagging(page)` 返回 false 意味着什么？（提示：换入页不该有前任标签（buddy 出来的新页）——返回 false=协议异常或竞态；当前实现静默跳过 restore…… 这个分支理论上不可达，但若到达则标签丢失——可讨论是否该 WARN。）
6. KSM 互斥的深层原因（"set_pte_at() may zero or change the tags of the other page"）——合并的两个页面各走什么命运？（提示：幸存页保留其标签；被合并方消失——但若有第三者 PTE 仍指它且其 PG_mte_tagged 未置，set_pte 的 sync 会把**幸存页**清标……memcmp 的提前拒绝正是断这条毒链。）

---

> 回看：[MTE 与 LAM](mte-lam.md)（标签硬件与 §2.6 KSM 彩蛋的闭环）｜[tagged_addr prctl](tagged-addr-prctl.md)（进程侧的标签 ABI）｜[pagefault](../pagefualt/pagefault.md)（set_pte 的调用语境）｜[KASAN boot 初始化](kasan-boot-init.md)（kasan_init_tags 的初始排除集）｜[mm/README](../README.md)（分册导读）
