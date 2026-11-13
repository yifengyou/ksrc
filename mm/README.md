# Introduction — 内存管理分册（mm）

Linux 内存管理子系统学习笔记。按"物理内存的诞生 → 页分配 → 小对象分配 → 地址空间与缺页 → 优化机制"的学习顺序组织，完整目录见 [SUMMARY.md](SUMMARY.md)。

## 学习路线

```
【速查】    00-一页速查.md（先扫表建索引）
【启动期】  bootmem → memblock →（移交）→ buddy
【页分配】  zone/node → page → buddy  ← 物理页的所有者
【小对象】  slab → slub → slob → kmalloc → vmalloc
【地址空间】mmu / canonical_address_space（x86 线性空间布局、四/五级页表）
【地址空间】vma（maple tree 时代的 mmap 生命周期）
【缺页】    page fault → cow → vma（PER_VMA_LOCK 快路径）
【缓存】    cache（页缓存）→ buffer
【大页】    hugetlbfs / thp
【旁路】    tlb（结构与管理）、pagefualt（注意目录名的拼写沿用）
```

## 章节速览

| 主题 | 文档 | 一句话 |
| :--- | :--- | :--- |
| 启动期分配器 | [bootmem](bootmem/bootmem.md) / [memblock](memblock/memblock.md) | memblock 是 6.6 现役、bootmem 已成历史 |
| 伙伴系统 | [buddy](buddy/buddy.md)（另附[理论大纲](buddy/理论大纲.md)） | order/伙伴/迁移类型/规整全景 |
| 物理组织 | [node](node/node.md) / [zone](zone/zone.md) / [page](page/page.md) | NUMA 三级模型 |
| slab 家族 | [slab](slab/slab.md) / [slub](slub/slub.md) / [slob](slob/slob.md) | 6.6 默认 SLUB；slob 已在 6.4 被移除（笔记保留其历史分析） |
| 内核小内存 | [kmalloc](kmalloc/kmalloc.md) / [vmalloc](vmalloc/vmalloc.md) | 连续 vs 虚拟连续 |
| KASAN vmalloc 检测 | [kasan-vmalloc](kasan/kasan-vmalloc.md) | shadow 按需填充/毒化即刻拆表延迟/对齐难题双图/HW_TAGS 正交实现 |
| MTE 与 LAM | [mte-lam](kasan/mte-lam.md) | 地址位元数据双方案：arm64 4-bit 标签+TCF/TCO、x86 CR3 掩码+__access_ok 容忍哲学、untagged_addr 设施 |
| KASAN boot 初始化 | [kasan-boot-init](kasan/kasan-boot-init.md) | 先有鸡还是先有蛋的三段解法（全零模板/双页表戏法/kasan_depth 门控）、x86 kasan_init 逐区清单（E820 循环/vmalloc 二选一/CEA on-demand 的 512GiB 论证/五级 PGD 碰撞）、★RO 收尾（memset+__PAGE_KERNEL_RO+双刷 TLB 原文）、arm64 对照（tmp_pg_dir/for_each_mem_range/SHADOW_INIT=0xFE 语义反转）、boot 产物复用表 |
| 地址空间 | [mmu](mmu/README.md) / [canonical_address_space](canonical_address_space/README.md) | x86 线性空间布局、四级/五级页表 |
| 缺页与 COW | [pagefault](pagefualt/pagefault.md) / [cow](cow/cow.md) | 匿名/文件缺页、写时复制 |
| vma 与 mmap 生命周期 | [vma](vma/vma与mmap生命周期.md) | vm_area_struct 新解剖（vm_prev/vm_next 已删）、maple tree 与 VMA_ITERATOR、vma_merge 与两阶段提交、PER_VMA_LOCK |
| mmap_lock 表锁 | [mmap_lock](mmap_lock/mmap_lock.md) | 地址空间读写不对称、三 tracepoint 零开销门控、★per-VMA lock 联动协议（两段式乐观检+溢出论证+四道校验）、0x120 偏移 cacheline 论证、mmap_sem→mmap_lock 改名考古（v5.8 Lespinasse）与 per-VMA 落地（v6.4）、争用缓解谱（handoff/RT/range lock 未合入） |
| page cache 与 writeback | [pagecache](pagecache/pagecache与writeback.md) | address_space 全字段、folio API 与三枚标签、pos_ratio 节流、write_cache_pages 与 tag-and-write |
| ksm 与 zswap | [ksm](ksm/ksm与zswap.md) | 双树去重协议与 COW 解合并、same-filled 直存、32 路 zpool 分流、限额回写 |
| 页缓存 | [cache](cache/cache.md) / [buffer](buffer/buffer.md) | address_space 与块缓冲 |
| 大页 | [hugetlbfs](hugetlbfs/hugetlbfs.md) / [thp](thp/thp.md) | 静态预留 vs 透明合并 |
| TLB | [tlb](tlb/tlb.md) | 页表缓存与 flush 语义 |

## 与其他分册的接口

* [init 分册](../init/README.md)：`mm_core_init → mem_init` 是 memblock→buddy 的交接点
* [调度器分册](../scheduler/README.md)：COW/缺页路径上的调度交互（`memory pressure`、psi）
* [RCU 分册](../RCU/README.md)：slab 回收与 RCU 延迟释放

## 源码索引（Linux 6.6）

```
mm/memblock.c  mm/page_alloc.c  mm/slab.c  mm/slub.c  mm/kmalloc? (slub 内)
mm/memory.c（缺页/COW）  mm/mmap.c  mm/hugetlb.c  mm/khugepaged.c  mm/vmalloc.c
mm/compaction.c  mm/vmscan.c  arch/x86/mm/*  arch/arm64/mm/*
mm/mmap_lock.c  mm/memory.c（lock_vma_under_rcu）（mmap_lock/per-VMA）
mm/kasan/shadow.c  mm/kasan/hw_tags.c  lib/Kconfig.kasan（KASAN_VMALLOC）
arch/arm64/kernel/mte.c  arch/x86/kernel/process_64.c（LAM 使能）（MTE/LAM）
include/linux/mmzone.h（zone/free_area/page）  include/linux/mm_types.h
```
