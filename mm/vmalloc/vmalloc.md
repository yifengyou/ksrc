# vmalloc 与 vmap 惰性释放（Linux 6.6）

> 阅读目标：吃透 vmalloc 区的**vmap area 基础设施与惰性释放协议**——vfree 的两难（立即回收=立即多核 TLB 击落，而 vfree 常成串发生）；★**"noflush" 协议**（`free_unmap_vmap_area` 拆页表但不刷 TLB → `free_vmap_area_noflush` 把 area 挂**候审树** `purge_vmap_area_root/list`）；`lazy_max_pages` 阈值的公式与★设计哲学注释原文（log 缩放的取舍）；purge 主体的**批量并区 + 单次跨度 TLB 刷**（`resched_threshold` 的让步）；触发四路（超阈异步 worker/自旋复查/分配失败回收/`vm_unmap_aliases` 强制）；以及 vmap_block 脏位的**第二个惰性宇宙**。全部对照 v6.6（mm/vmalloc.c 4495 行）当轮核对。前置：[kmalloc](../kmalloc/kmalloc.md)｜[KASAN_VMALLOC](../kasan/kasan-vmalloc.md)（释放路径的 shadow 侧）。

---

## 一、基础设施：一个 area 的两棵树

```
struct vmap_area（虚拟区间的身份证）
  ├─ free_vmap_area_root + free_vmap_area_list（:754/:766）
  │    ★真·空闲树——addr 排序的 augmented rbtree + 配套链表
  │    （"This linked list is used in pair with free_vmap_area_root" :751）
  │    分配时 __alloc_vmap_area 在此找洞（:1615）
  └─ purge_vmap_area_root + purge_vmap_area_list（:739）
       ★候审树——vfree 后的 area 先挂这里（可并区！）
       purge 时才 merge_or_add 进真·空闲树
```

★**两树分离是惰性的载体**：候审期间 area 可与相邻候审者合并（purge 树里就做 merge_or_add_vmap_area :1832-1834），purge 时整批搬入真树——**并区在 TLB 刷之前完成**，刷一次跨度即覆盖整批。

## 二、vfree 的两难与 noflush 协议

```
两难：
  立即回收 → 每个 vfree 一次 flush_tlb_kernel_range（多核 IPI 击落）
  vfree 的现实 → 拆模块/删表常成串发生（一串几百个 area）

free_unmap_vmap_area（:1848-1855）的三步：
  ① flush_cache_vunmap(start, end)        /* 架构 cache 维护 */
  ② vunmap_range_noflush(start, end)       /* ★拆页表——但名字就说明不刷 TLB */
     （debug_pagealloc 开启时才被迫立即刷 :1853-1854）
  ③ free_vmap_area_noflush(va)             /* 挂候审 */
```

```c
static void free_vmap_area_noflush(struct vmap_area *va)   /* :1818-1843 */
{
	unsigned long nr_lazy_max = lazy_max_pages();
	...
	nr_lazy = atomic_long_add_return((va->va_end - va->va_start) >>
				PAGE_SHIFT, &vmap_lazy_nr);   /* 按页数记账 */

	spin_lock(&purge_vmap_area_lock);
	merge_or_add_vmap_area(va, &purge_vmap_area_root, &purge_vmap_area_list);
	spin_unlock(&purge_vmap_area_lock);

	trace_free_vmap_area_noflush(va_start, nr_lazy, nr_lazy_max);

	/* After this point, we may free va at any time */
	if (unlikely(nr_lazy > nr_lazy_max))
		schedule_work(&drain_vmap_work);    /* ★超阈——异步触发 purge */
}
```

## 三、lazy_max_pages：阈值的公式与哲学（:1684-1706）

```c
static unsigned long lazy_max_pages(void)
{
	unsigned int log;

	log = fls(num_online_cpus());
	return log * (32UL * 1024 * 1024 / PAGE_SIZE);   /* log2(cpus)+1 × 32MB */
}
```

设计哲学注释原文（裁剪）：

```
 * There is a tradeoff here: a larger number will cover more kernel page
 * tables and take slightly longer to purge, but it will linearly reduce
 * the number of global TLB flushes that must be performed. It would seem
 * natural to scale this number up linearly with the number of CPUs
 * (because vmapping activity could also scale linearly with the number of
 * CPUs), however it is likely that in practice, workloads might be
 * constrained in other ways that mean vmap activity will not scale
 * linearly with CPUs. Also, I want to be conservative and not introduce
 * a big latency on huge systems, so go with a less aggressive log scale.
 * It will still be an improvement over the old code, and it will be
 * simple to change the scale factor if we find that it becomes a problem
 * on bigger systems.
```

——**对数而非线性缩放**：保守防大系统的 purge 长尾，且公式留了"改系数就好"的后门。8 核机（fls=4）：阈值 128MB 虚拟地址的候审量。

## 四、purge 主体：批量并区 + 单次跨度刷（__purge_vmap_area_lazy，:1724-1782）

```
持 vmap_purge_lock（:1716-1719 注释原话："There is no actual critical
  section protected by this lock, but we want to avoid concurrent calls
  for performance reasons and to make the pcpu_get_vm_areas more
  deterministic."——串行化纯为性能与确定性）

① spin_lock(purge_vmap_area_lock) + list_replace_init 摘整批到本地链
   ——★顺手把本批的 min/max 跨度算出（:1741-1750）
② flush_tlb_kernel_range(start, end)      ★一次 IPI 批量击落整批
   resched_threshold = lazy_max_pages() << 1
③ 逐 area：merge_or_add_vmap_area_augment 并入真·空闲树
   （:1761——此前 TLB 已刷，页面真正回到可用池）
   is_vmalloc_or_module_addr → kasan_release_vmalloc(...)  ★shadow 页表
   同批拆除（[KASAN_VMALLOC 章] §五/§六 的对齐难题与双 ASCII 图即此）
   atomic_long_sub(nr, &vmap_lazy_nr) 记账回退
   cond_resched_lock（resched_threshold 内不切，超则让出）
④ trace_purge_vmap_area_lazy(start, end, num_purged_areas)
```

## 五、触发面四路

| 触发 | 路径 | 时机 |
| :--- | :--- | :--- |
| **超阈异步** | `free_vmap_area_noflush` → `schedule_work(&drain_vmap_work)`（:1842-1843，work 定义 :795-796） | 候审量越过 lazy_max_pages |
| **自旋复查** | `drain_vmap_area_work`（:1798-1810）的 `do { purge } while (nr_lazy > lazy_max_pages())` ——purge 期间又有新 vfree 也一并吃掉 | worker 内 |
| **分配失败回收** | `reclaim_and_purge_vmap_areas`（:1788-1795）：`purge_fragmented_blocks_allcpus` + 全量 purge | `__alloc_vmap_area` 找不到洞时先收垃圾再重试 |
| **强制（范围）** | `vm_unmap_aliases`（:2295-2320）：全量 purge + vmap_block 脏段 + `if (!__purge && flush) flush_tlb_kernel_range` | 外部契约（见 §七） |

## 六、第二个惰性宇宙：vmap_block 的脏位

`vm_map_ram()` 的小分配（≤ VMAP_MAX_ALLOC 页）不走 vmap_area 而走 **vmap_block**（大块预映射+按位分配）——它的惰性是**脏位段**：释放只置 `vb->dirty_min/max` 记录范围（"Prevent that this is flushed again" :2298 的置回技巧——VMAP_BBMAP_BITS 归零即"已刷"），等 `vm_unmap_aliases`/块耗尽时统一刷。两个宇宙在 vm_unmap_aliases 汇合（先扫块脏段再 purge 候审链）。

## 七、消费者契约：vm_unmap_aliases（:2306 注释原文裁剪）

```
 * The vmap/vmalloc layer lazily flushes kernel virtual mappings primarily
 * to amortize TLB flushing overheads. What this means is that any page you
 * have now, may, in a former life, have been mapped into kernel virtual
 * address by the vmap layer and so there might be some CPUs with TLB
 * entries still referencing that page (additional to the regular 1:1
 * kernel mapping).
 *
 * vm_unmap_aliases flushes all such lazy mappings. After it returns, we
 * can be sure that none of the pages we have control over will have any
 * aliases from the vmap layer.
```

★**"in a former life" 的幽灵页**：惰性窗口内，物理页已回到 buddy 但**某些 CPU 的 TLB 还留着它的 vmalloc 别名**——任何把页交给不能容忍别名方的代码（DMA 入口、改页属性）都必须先 `vm_unmap_aliases()`。这是[ dma-buf 章](../../driver-model/docs/09-dma-buf跨设备缓冲共享.md)与文件系统路径的隐形前提。

## 八、观测实战

```bash
# 1) 候审量的两条 tracepoint（vmalloc.c :1841/:1782 的埋点）
echo 1 > /sys/kernel/debug/tracing/events/vmalloc/free_vmap_area_noflush/enable
echo 1 > /sys/kernel/debug/tracing/events/vmalloc/purge_vmap_area_lazy/enable
cat /sys/kernel/debug/tracing/trace_pipe    # nr_lazy/nr_lazy_max 实时追赶

# 2) 空闲树与分配现状
cat /proc/vmallocinfo | head -20            # 每个 vmalloc 分配的调用者
grep -c 'vfree' /proc/vmallocinfo?          # 正确位面：
awk '{s+=$2} END {print "已用 vmalloc:", s}' /proc/vmallocinfo

# 3) purge 频率诊断（大跨度 vs 高频小刷）
#   若 purge 频繁且 num_purged_areas 小 → vfree 分散（阈值形同虚设）
#   若 nr_lazy 长期贴 lazy_max_pages → 生产者速率 > 回收速率

# 4) vm_unmap_aliases 的调用者画像（谁在在意幽灵页）
grep -rn 'vm_unmap_aliases' drivers/ | head
```

## 九、思考题

1. 候审树与真·空闲树为什么必须是两棵？（提示：候审期间 TLB 还留着映射——若立即入真树，新分配可能拿到"TLB 还有旧别名"的地址；purge 的 TLB 刷是地址回到可用池的**先决条件**。）
2. `lazy_max_pages` 用 `fls(cpus)` 而非 `cpus`——注释给出的两条反对线性缩放的理由分别针对什么？（提示：工作负载未必随 CPU 线性缩放（需求侧假设不牢）；大系统 purge 长尾（供给侧风险）——log 是两边都留余地的折中。）
3. purge 里 TLB 刷（②）在并区入真树（③）**之前**——顺序能颠倒吗？（提示：颠倒后新分配可能立刻拿到未刷地址→对旧映射的访问穿越；先刷后并，"可用"天然意味着"已干净"。）
4. `drain_vmap_area_work` 的 do-while 自旋复查为什么不能省？（提示：purge 期间新 vfree 仍在挂链——只跑一轮会留下"刚超阈又挂新"的尾巴；自旋直到低于阈值才真正平衡。）
5. `vm_unmap_aliases` 的"ghost page"契约对 DMA 意味着什么？（提示：页交设备后若 CPU 的 TLB 残留 vmalloc 别名，任何推测性访问/错误路径都可能写穿新主人——先刷别名才安全移交。）
6. vmap_block 的脏位（dirty_min/max）为什么用"置 VMAP_BBMAP_BITS 归零"表示"已刷"？（提示：min>max 即空段——一个比较判定空/非空，免去单独的状态字段。）

---

> 回看：[KASAN_VMALLOC](../kasan/kasan-vmalloc.md)（purge 时 kasan_release_vmalloc 的 shadow 侧——本章 §四 的搭档）｜[kmalloc](../kmalloc/kmalloc.md)（小对象世界）｜[dma-buf](../../driver-model/docs/09-dma-buf跨设备缓冲共享.md)（vm_unmap_aliases 的契约消费者）｜[TLB](../tlb/tlb.md)（批量击落的机制本体）｜[mm/README](../README.md)（分册导读）
