# page cache 与 writeback：address_space 的一生（Linux 6.6）

> 阅读目标：掌握文件页缓存的**容器层**（`struct address_space` 全字段、folio 时代的 API、i_pages xarray 与两枚标签）、**脏页节流层**（global/domain 阈值、pos_ratio 比例控制器、`balance_dirty_pages`）与**回写执行层**（`writeback_control`、tag-and-write 防活锁、`write_cache_pages` 主循环）。前置：[cache 页缓存](../cache/cache.md)、[block 分册 bio 章](../../block/docs/02-bio与bio_vec.md)。

---

## 一、容器：struct address_space（include/linux/fs.h，已核对 6.6）

```c
struct address_space {
	struct inode		*host;         /* 属主 inode（匿名页的 mapping 无 host） */
	struct xarray		i_pages;       /* ★page cache 本体：index(folio) → folio */
	struct rw_semaphore	invalidate_lock;
	gfp_t			gfp_mask;
	atomic_t		i_mmap_writable;
	struct rb_root_cached	i_mmap;    /* interval tree：映射了本文件的 vma 集合 */
	unsigned long		nrpages;       /* 缓存页数 */
	pgoff_t			writeback_index;  /* 周期回写的断点游标（循环模式续点） */
	const struct address_space_operations *a_ops;
	struct rw_semaphore	i_mmap_rwsem;   /* 文件侧反向映射的锁 */
	errseq_t		wb_err;         /* fsync 族的错误上报通道 */
	spinlock_t		private_lock;  struct list_head private_list;
	/* ... */
};
```

`a_ops` 的关键回调：`writepage`/`writepages`（回写执行）、`read_folio`/`readahead`（读侧）、`release_folio`（回收前问询）、`dirty_folio`（标记脏的钩子——大多数 FS 用 filemap_dirty_folio）。

## 二、folio 时代的 API（include/linux/pagemap.h，已核对 6.6）

```
查找：__filemap_get_folio(mapping, index, FGP_*, gfp)
      FGP_LOCK/FGP_CREAT/FGP_NOWAIT… —— filemap_get_folio/finding 的统一后端
插入：filemap_add_folio(mapping, folio, index, gfp)  ← 大页缓存（XFS 只读 THP）
标脏：__folio_mark_dirty → __xa_set_mark(&mapping->i_pages, index,
        PAGECACHE_TAG_DIRTY)                ← xarray 标签位，O(1)
回写起点：folio_start_writeback / folio_end_writeback（WB tag 的置/清）
```

**页在 xarray 上有三枚影子标签**：`PAGECACHE_TAG_DIRTY`（脏）、`TOWRITE`（本轮要写）、`WRITEBACK`（在途中）。标签是回写子系统的"账本"——`nrpages` 是页数，标签才是状态。

## 三、脏页节流：balance_dirty_pages（mm/page-writeback.c，已核对）

```
阈值计算：domain_dirty_limits → global_dirty_limits
   背景：bg_thresh = dirty_background_ratio × 可用内存（或 _bytes）
   上限：thresh    = vm.dirty_ratio × 可用内存
   两个域：gdtc（全局）+ mdtc（memcg 级，CONFIG_CGROUP_WRITEBACK 下生效）
   —— dirty_throttle_control{ .gdtc/.pos_ratio } 两级串联

比例控制器 pos_ratio（dirty_position_ratio 的核心几何，源码注释原样）：
   freerun  = (thresh + bg_thresh) / 2     ← 脏页低于此完全不节流
   setpoint = (freerun + limit) / 2         ← 控制目标（非 limit！）
   pos_ratio 随（nr_dirty−setpoint）平滑下探：超出越多，允许的脏速越低
   补偿项：memcg 慢于全局时压低全局 pos_ratio（公平性）；bdi 窗口
   调整防止快设备被慢设备拖死（bdi_min_ratio 动态调整）

调用点：balance_dirty_pages_ratelimited(mapping)（每 CPU 侧
   nr_dirtied 计数摊薄开销）→ __balance_dirty_pages → 睡眠节流
   → trace_balance_dirty_pages（bdi/pos_ratio/... 全暴露给 tracefs）
```

## 四、回写执行：writeback_control 与 write_cache_pages

```c
struct writeback_control {          /* include/linux/writeback.h，已核对 6.6 */
	long nr_to_write;     long pages_skipped;
	loff_t range_start, range_end;
	enum writeback_sync_modes sync_mode;   /* WB_SYNC_NONE/ALL/… */
	unsigned for_kupdate:1;       /* 周期回写（expire 检查） */
	unsigned for_background:1;    /* 背景回写（低于 bg_thresh 停手） */
	unsigned tagged_writepages:1; /* tag-and-write（防活锁） */
	unsigned for_reclaim:1;       /* 内存回收发起（直写） */
	unsigned range_cyclic:1;      /* 用 writeback_index 续循环 */
	unsigned for_sync:1;          /* sync(2) 的 WB_SYNC_ALL */
	struct bdi_writeback *wb;     /* cgroup writeback 归属（CONFIG_CGROUP_WRITEBACK） */
	...
};
```

```
主循环 write_cache_pages(mapping, wbc, writepage, data)：
  tag = wbc->tagged_writepages ? PAGECACHE_TAG_TOWRITE
                               : PAGECACHE_TAG_DIRTY
  find_fetch_tag 循环批量（xas 批扫）→ 对每个 folio：
     for_kupdate 时检查是否过 expire（dirty_expire_centisecs，默认 30s）
     → 调 a_ops->writepage（→ bio，见 block 分册）
防活锁：tag_pages_for_writeback(mapping, index, end)
  —— xas_for_each_marked(DIRTY) 上先整体打 TOWRITE 标签，
     写循环只消费 TOWRITE：写作期间新脏的页不带标，下一轮再写
     （无此机制：重脏页面会让回写永远追不上而活锁）
```

**发起侧**：flusher 是每 bdi 的 `bdi_writeback`（workqueue 回调 `wb_workfn`）；`wb_do_writeback` 分拣 work 项（kupdate 定时、background 阈值触发、用户 fsync/sync 强制、回收直写）。`for_reclaim` 直写在分配失败路径上同步落盘——与 [buddy 分册](../buddy/buddy.md) 的 watermark 保卫战直接相关。

## 五、观测与实战

```
cat /proc/meminfo | grep -e Dirty -e Writeback      # 当前脏/在途字节数
sysctl vm.dirty_ratio vm.dirty_background_ratio     # 阈值（%）
sysctl vm.dirty_bytes vm.dirty_background_bytes     # 绝对值覆盖版（互斥）
sysctl vm.dirty_expire_centisecs                    # 3000=30s
sysctl vm.dirty_writeback_interval                  # 500=5s 唤醒
/sys/kernel/debug/bdi/*/stats                       # 每 bdi 的 wb 统计
tracefs: balance_dirty_pages / writeback_single_inode / writeback_pages_written
故障模式速查：Dirty 常驻高位+慢设备 → dirty throttling 生效（应用 write 阻塞
  在 balance_dirty_pages）；fsync 报错滞后 → wb_err/errseq 机制（SEE ALSO
  wb_err 字段：一次记录、fsync 集中消费）
```

## 六、思考题

1. `freerun=(thresh+bg_thresh)/2` 意味着即使脏页已超过背景阈值，写入仍完全自由——为什么背景阈值不能直接当刹车点？（对照 flusher 的 for_background 停手条件。）
2. 回写标签为什么需要 DIRTY/TOWRITE 两枚而不是一枚？（推演 tag-and-write 关掉后"重脏文件永写不完"的活锁。）
3. `writeback_index` + `range_cyclic` 的循环游标解决了什么问题？（提示：超大文件的周期回写若每次从 0 扫描的代价。）
4. memcg writeback（mdtc）把 cgroup 的脏限额串进全局 pos_ratio——若某 cgroup 长期高于自己 setpoint 而全局干净，节流发生在谁身上？（对照 cgroup 分册 memory 控制器。）
5. `for_reclaim` 回写为什么不能等 flusher？（对照页分配器在水线以下的同步语义。）

---

> 回看：[vma 与 mmap 生命周期](../vma/vma与mmap生命周期.md)｜[页缓存 cache](../cache/cache.md)｜[block 分册 bio](../../block/docs/02-bio与bio_vec.md)
