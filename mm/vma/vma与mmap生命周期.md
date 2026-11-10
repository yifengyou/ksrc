# vma 与 mmap 生命周期：maple tree 时代（Linux 6.6）

> 阅读目标：掌握 6.6 时代的进程地址空间管理——`vm_area_struct` 新解剖（**勘误：vm_prev/vm_next 链表已删除**）、`mm->mm_mt` maple tree 索引与 `VMA_ITERATOR`、mmap/munmap 的完整生命周期（`vma_merge` 合并规则与 `vma_iter_config→prealloc→store` 两阶段提交）、6.4+ 的 PER_VMA_LOCK 缺页快速路径。前置：`pagefualt/pagefault.md`（缺页视角）、[process-mgmt 分册 fork 章](../../process-mgmt/docs/01-fork与clone.md)（dup_mmap 复制视角）。

---

## 一、vm_area_struct 的新解剖（include/linux/mm_types.h，已核对 6.6）

```c
struct vm_area_struct {
	union {
		struct { unsigned long vm_start, vm_end; };
#ifdef CONFIG_PER_VMA_LOCK
		struct rcu_head vm_rcu;   /* detached 后 RCU 延迟释放 */
#endif
	};
	struct mm_struct *vm_mm;
	pgprot_t vm_page_prot;
	union { const vm_flags_t vm_flags; vm_flags_t __private __vm_flags; };
#ifdef CONFIG_PER_VMA_LOCK
	int vm_lock_seq;  struct vma_lock *vm_lock;  bool detached;
#endif
	struct { struct rb_node rb; unsigned long rb_subtree_last; } shared;
	      /* ↑ 文件映射侧：挂进 mapping->i_mmap 的 interval tree（按 vm_pgoff 区间） */
	struct list_head anon_vma_chain;   /* 挂进每个 anon_vma 的链 */
	struct anon_vma *anon_vma;         /* 反向映射锚点（父/子链，见下） */
	const struct vm_operations_struct *vm_ops;
	unsigned long vm_pgoff;  struct file *vm_file;  void *vm_private_data;
#ifdef CONFIG_ANON_VMA_NAME
	struct anon_vma_name *anon_name;   /* prctl PR_SET_VMA_ANON_NAME */
#endif
	...
};
```

**与老资料的关键差异（逐项核对 6.6 源码）**：
- `vm_prev`/`vm_next` **已删除**——VMA 主索引完全交给 maple tree（`mm_mt`），既无链表也无旧 rbtree；遍历靠迭代器
- 双重索引：**mm 侧 maple tree**（地址序）+ **文件侧 interval tree**（`shared.rb`，区间序，供 unmap/截断反向查找）+ **匿名侧 anon_vma 链**（COW 反向映射）
- PER_VMA_LOCK（6.4 合入）：VMA 自带读锁，缺页可绕过全局 `mmap_lock`

## 二、索引结构：maple_tree 与 VMA_ITERATOR

```c
struct maple_tree {              /* include/linux/maple_tree.h */
	union { spinlock_t ma_lock; lockdep_map_p ma_external_lock; };
	unsigned int ma_flags;       /* 类型：dense/range/RCU 安全标志 */
	void __rcu *ma_root;        /* 单条目直存根指针；多条目退化为节点树 */
};
struct vma_iterator { struct ma_state mas; };   /* mm_types.h */
#define VMA_ITERATOR(name, __mm, __addr) ... .tree = &(__mm)->mm_mt ...
```

maple tree 是 **RCU 安全的 B-tree 变体**（节点 64/128B 对齐，leaf 内藏 pivot），专为"范围查询 + 无锁读"设计：`mas_find()`/`mas_walk()` 走 lockless 读路径；写路径必须**先预分配**（`vma_iter_prealloc`）再提交——这是两阶段协议的根因：插入中途分配失败会留下半更新树。

```c
/* mm/mmap.c 的标准写姿势（已核对） */
vma_iter_config(vmi, vma->vm_start, vma->vm_end);  /* 声明目标区间 */
if (vma_iter_prealloc(vmi, vma))                    /* 先拿节点 */
	goto nomem;
vma_iter_store(vmi, vma);                           /* 原子提交 */
```

## 三、mmap 生命周期主链（mm/mmap.c）

```
do_mmap（长度/标志校验、file->f_op->mmap 或 shmem）
 └ mmap_region(mm, addr, len, flags, vm_flags, file, pgoff):
    ① mmap_write_lock_killable(mm)                 ← 全局写锁（6.6 仍是大锁）
    ② may_expand_vm/计数检查、addr 归一化、重叠区 vms_complete_munmap_vma 清场
    ③ vma_merge(vmi, mm, prev, ...): 尝试与邻接 VMA 合并（条件：同一 file/pgoff
       连续、vm_flags 可合并、匿名者需兼容 anon_vma、且 vm_pgoff 等价）
       ├ 合并命中 → vma_expand(vmi, ...) 扩区间后 store（一个节点改动收工）
       └ 未命中  → vm_area_alloc + vma_iter_config/prealloc/store 新插
    ④ vma_link：anon_vma 接线 + file 侧 i_mmap interval tree 插入（filelock）
    ⑤ 计数（mm->map_count、total_vm、data_vm/committed as）与 mmap_unlock
```

`vma_merge` 的省节点效果：栈/堆连续 mmap（如 ld.so 的一串 PT_LOAD）被折叠成少数大 VMA——`/proc/self/maps` 行数远小于实际 mmap 调用数的原因。

**munmap**：`vma_iter_clear_range` 摘树 → `detached = true` → `vm_rcu` 延迟释放（缺页快速路径还在 RCU 读侧用旧 VMA 时不能立即回收）；中间劈开的场景走 `split_vma`（vma_adjust 系）。

## 四、PER_VMA_LOCK：缺页快速路径（6.4+）

```
传统：缺页 → mmap_read_lock(mm) → find_vma …   ← 全局锁，写者持有时全体缺页阻塞
6.6：lock_vma_under_rcu(mm, addr):
       rcu_read_lock → mas_walk 到 vma → vma_start_read(vma)（seqcount 确认
       未 detached/未写）→ 成功则 lock_vma() 返回；失败回退 mmap_read_lock 老路
     写侧 vma_end_write/标记 detached 使读侧 seq 检测失效
```

多线程应用的大量只读缺页（JIT/数据库页表预热）不再与 mmap 写者互斥——fork/exec 期间子进程缺页也不再排队等 `mmap_lock`。仍走慢路径的情形：VMA 正被改（detached）、MAP_POPULATE 类需扩锁。

## 五、观测与实战

```
/proc/<pid>/maps        # VMA 行= maple 树的一次 mas_find 序贯
/proc/<pid>/smaps       # + 每区间 RSS/PSS/anon_vma 计数（KV 特化 smaps_rollup）
perf probe -a vma_merge  # 合并命中率观测（fork 后 exec 前的 VMA 数量曲线）
cat /proc/vmstat | grep -e vma -e map_count  # mmap/unmap 事件计数
PER_VMA_LOCK 关闭开关：sysctl vm.unprivileged_userfaultfd 无关，
  编译项 CONFIG_PER_VMA_LOCK（默认 y）；运维关注 perf sched 延迟
  中 lock_vma 慢路径占比即可
```

## 六、思考题

1. 删除 `vm_next` 后，`find_vma(addr)`（老的"第一个 end>addr 的 VMA"）语义由谁保证？（对照 maple tree 的 range 查询。）
2. `vma_iter_prealloc` 为什么不能推迟到 `vma_iter_store` 内部？（提示：树更新中途 GFP_KERNEL 分配失败 = 破碎树；对照 slab 分册的原子上下文约束。）
3. 文件页反向映射为何用 interval tree（按 pgoff 区间）而匿名页用 anon_vma 链？（提示：truncate 与 COW 的找页方向不同。）
4. PER_VMA_LOCK 下 `vma_start_read` 与 `mmap_read_lock` 同时持有会不会死锁？（推演写侧的升级路径。）
5. `vma_merge` 拒绝合并"两个映射同一文件的 VMA，即使 pgoff 连续"的一种情形？（提示：vm_flags 中的 VM_MAYWRITE 差异、VM_DONTEXPAND。）

---

> 下一章：[page cache 与 writeback](../pagecache/pagecache与writeback.md) —— 文件页的另一生：从 i_pages 到磁盘。
