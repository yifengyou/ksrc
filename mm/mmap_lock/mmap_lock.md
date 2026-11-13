# mmap_lock：进程地址空间的表锁（Linux 6.6）

> 阅读目标：吃透内核**争用最高昂的读写锁**——`mm_struct->mmap_lock`（rw_semaphore）的包装层 API 与三 tracepoint（零开销静态键门控）；★**per-VMA lock 联动协议**（`vma_end_write_all` 的 mm_lock_seq 推进 / `vma_start_read` 的两段式乐观检 / `lock_vma_under_rcu` 的四道校验——"无 mmap_lock 缺页"的完整代价模型）；★**0x120 偏移的 cacheline 论证**（mm_types.h 注释原文——"please be careful with adding new fields before mmap_lock"）；**改名考古**（mmap_sem→mmap_lock：`9740ca4e95b4` 2020-06-08 Michel Lespinasse、v5.8 首现，commit message 两条理由原文含 range lock 展望；per-VMA lock 由 Suren Baghdasaryan 系列于 v6.4 落地）；RT 形态与争用缓解谱。全部对照 v6.6 当轮核对。前置：[vma 与 mmap 生命周期](../vma/vma与mmap生命周期.md)（maple tree 与 VMA 结构）｜[rwsem](../../locking/rwsem.md)。

---

## 一、它锁的是什么：地址空间表的读写不对称

```
mm->mmap_lock 保护的对象：整个地址空间的"表"本身——
  VMA 集合（maple tree）、VMA 的增删合并拆分、所有跨 VMA 的遍历

读写比（极端不对称）：
  读侧高频：缺页处理、GUP（get_user_pages）、/proc/PID/maps、perf、
             移交 swap、NUMA balancing 扫描……每秒百万级
  写侧低频：mmap/munmap/mprotect/mremap、fork 的 dup_mmap、
             exec 的 exec_mmap、用户态伸缩栈……低频但必须独占

★它是 rwsem 的教科书战场：读者海量、写着稀少但持有期长
 （写者要改树结构+等 mmap_sem 全部落定）——正是 [concession]
 章讲的让步机制与 [rwsem] handoff 存在的动机
```

## 二、v6.6 的 API 层（include/linux/mmap_lock.h，187 行）

### 2.1 包装族（:105-165 附近）

```
mmap_write_lock / _nested / _killable / _trylock / _unlock / _downgrade
mmap_read_lock  / _killable / _trylock / _unlock / _unlock_rec? (无)
mmap_assert_locked / mmap_assert_write_locked（lockdep 断言）
mmap_init_lock / mmap_drop_all_locks（exit 时）
全部薄包装到 down_read/down_write(&mm->mmap_lock)——API 化的意义见 §五
```

★**降级也推序列号**：`mmap_write_downgrade()` 内部先 `vma_end_write_all(mm)` 再降锁——写者降级为读者的瞬间，此前"以写者身份看到的 VMA 状态"必须作废（per-VMA 读锁依赖 mm_lock_seq 判定新旧）。

### 2.2 三 tracepoint 的零开销门控（:16-59）

```c
DECLARE_TRACEPOINT(mmap_lock_start_locking);
DECLARE_TRACEPOINT(mmap_lock_acquire_returned);
DECLARE_TRACEPOINT(mmap_lock_released);

static inline void __mmap_lock_trace_start_locking(struct mm_struct *mm, bool write)
{
	if (tracepoint_enabled(mmap_lock_start_locking))   /* ★静态键——未启用时近零 */
		__mmap_lock_do_trace_start_locking(mm, write);
}
```

三事件恰好框住**等待区间**（start_locking→acquire_returned 之间=排队时间）与持有区间——锁延迟直方图的原料。实现侧（mm/mmap_lock.c:246 行）自带 **memcg 路径缓冲**体系：per-CPU `struct memcg_path { local_lock_t lock; char __rcu *buf; local_t buf_idx; }`（:43-48）+ 头注释原文 *"Our various events all share the same buffer (because we don't want or need to allocate a set of buffers \*per event type\*)"（:22-24）+ `CONTEXT_COUNT 4`（"normal, softirq, irq, and NMI"，:40）+ `reg_lock`/`reg_refcount` 引用计数注册（:27-29）。

## 三、★per-VMA lock：绕过表锁的缺页快路径（6.4+，CONFIG_PER_VMA_LOCK）

### 3.1 为什么能绕：把"锁表"换成"逐行验序"

```
mmap_lock 读锁的语义：整张表在我读取期间不变
per-VMA lock 的语义：我拿到的这一个 vma 在我使用期间不变
                    （表可以变——变了我就失败重试）

代价模型：写者每次动表，mm_lock_seq++；
          读者比对 vma->vm_lock_seq 与 mm->mm_lock_seq
          —— 序号相等 = 这个 vma 正被写者持有 → 退回慢路径
```

### 3.2 写侧：vma_end_write_all（mmap_lock.h:75-80 附近）

```c
static inline void vma_end_write_all(struct mm_struct *mm)
{
	/*
	 * Nobody can concurrently modify mm->mm_lock_seq due to exclusive
	 * ... Pairs with ACQUIRE semantics in vma_start_read().
	 */
	smp_store_release(&mm->mm_lock_seq, mm->mm_lock_seq + 1);
}
```

调用点两处：`mmap_write_unlock` **与** `mmap_write_downgrade`——写事务收尾必须作废全部存量 per-VMA 读锁。

### 3.3 读侧：vma_start_read 的两段式乐观检（include/linux/mm.h:653-684）

```c
static inline bool vma_start_read(struct vm_area_struct *vma)
{
	/* ① 无锁快检——注释原文："A race might cause false locked result.
	   We can use READ_ONCE() for the mm_lock_seq here, and don't need
	   ACQUIRE semantics, because this is just a lockless check whose
	   result we don't rely on for anything" */
	if (READ_ONCE(vma->vm_lock_seq) == READ_ONCE(vma->vm_mm->mm_lock_seq))
		return false;

	if (unlikely(down_read_trylock(&vma->vm_lock->lock) == 0))
		return false;

	/* ② 拿到 vma 读锁后 ACQUIRE 复验——注释原文：
	   "Overflow might produce false locked result. False unlocked
	    result is impossible...We must use ACQUIRE semantics for the
	    mm_lock_seq so that if we are racing with vma_end_write_all(),
	    we only start reading from the VMA after it has been unlocked.
	    This pairs with RELEASE semantics in vma_end_write_all()." */
	if (unlikely(vma->vm_lock_seq == smp_load_acquire(&vma->vm_mm->mm_lock_seq))) {
		up_read(&vma->vm_lock->lock);
		return false;
	}
	return true;
}
```

★**溢出论证**是这段的数学核心：`vm_lock_seq`（int）回绕只可能造成"误判已锁"（安全侧失败重试），**不可能造成"误判未锁"**——因为比较与修改都在 vma 锁保护下，且 mm_lock_seq 的推进使存量锁全部失效。读侧失败永远安全，成功才需要严格证明。

`vma_end_read`（mm.h:686-691）：`rcu_read_lock(); up_read(&vma->vm_lock->lock); rcu_read_unlock();`——注释 "keeps vma alive till the end of up_read"（VMA 本体由 RCU 保活，锁只保内容）。

### 3.4 入口：lock_vma_under_rcu 的四道校验（mm/memory.c:5431+）

```
rcu_read_lock → mas_walk（maple tree RCU 查找）→ vma_start_read
  ① 匿名页特判：vma_is_anonymous && !vma->anon_vma → 放弃
     注释原文："find_mergeable_anon_vma uses adjacent vmas which are
     not locked. This check must happen after vma_start_read();
     otherwise, a concurrent mremap() with MREMAP_DONTUNAP could
     dissociate the VMA from its anon_vma."
  ② 地址复验："vm_start/vm_end might change before we lock the VMA"
  ③ detached 复验 → vma_end_read + count_vm_vma_lock_event(VMA_LOCK_MISS) + goto retry
  ④ 全过 → 返回持 vma 读锁的 vma（缺页可继续，全程不碰 mmap_lock）
```

（VMA 结构字段与 maple tree 侧见 [vma 章](../vma/vma与mmap生命周期.md) §四——本章不重复。）

## 四、★0x120 偏移：给一把锁做 cacheline 工程（mm_types.h:750-762 注释原文）

```
 * With some kernel config, the current mmap_lock's offset
 * inside 'mm_struct' is at 0x120, which is very optimal, as
 * its two hot fields 'count' and 'owner' sit in 2 different
 * cachelines,  and when mmap_lock is highly contended, both
 * of the 2 fields will be accessed frequently, current layout
 * will help to reduce cache bouncing.
 *
 * So please be careful with adding new fields before
 * mmap_lock, which can easily push the 2 fields into one
 * cacheline.
```

——rwsem 的 `count`（读者计数）与 `owner`（写者链）在高争用时各是乒乓热点，**隔开两行**各打各的；"别在我前面加字段"是对后来者的明确警告。（[cacheline-packing 章](../../locking/cacheline-packing.md)的双原则之"分"的又一实战——且这次对象是锁本身。）

## 五、考古：mmap_sem → mmap_lock（2020）→ per-VMA lock（2023）

```
v5.8（2020-06-08，Michel Lespinasse 系列 12 patch）：
  9740ca4e95b4 "mmap locking API: initial implementation as rwsem wrappers"
  ——commit message 两条理由（原文）：
   "- At first, it provides an easy hooking point to instrument mmap_sem
      locking latencies independently of any other rwsems.
    - In the future, it may be a starting point for replacing the rwsem
      implementation with a different one, such as range locks. This is
      something that is being explored, even though there is no wide
      concensus about this possible direction yet."
  da1c55f1b272 "mmap locking API: rename mmap_sem to mmap_lock"（字段本体）
  c1e8d7c6a7a6 "mmap locking API: convert mmap_sem comments"
  ——★理由①即 §二.2 的三 tracepoint；理由②的 range lock 未走通，
    但"换实现"的预言以另一形态兑现：

v6.4（2023，Suren Baghdasaryan 系列）：CONFIG_PER_VMA_LOCK 合入
  ——不是换掉 rwsem，而是在其外补一层"逐 VMA 验序"的快路径
  （6152e53d9671 "mm: change per-VMA lock statistics to be disabled
   by default" 等系列落定）
```

★演化的真路径：**API 化（挂钩点）→ 插桩（tracepoint）→ 热点确认 → 绕过（per-VMA）**——先给锁装仪表，再决定换引擎。range lock 的"无共识"与 per-VMA 的"有共识"之间，隔着三年实测数据。

## 六、争用缓解谱（全家族速览）

| 手段 | 层 | 状态 | 详见 |
| :--- | :--- | :--- | :--- |
| rwsem handoff（写者防饿死） | rwsem 本体 | v5.10+ | [concession](../../locking/concession.md) |
| 乐观自旋 | rwsem 本体 | 常驻 | [rwsem](../../locking/rwsem.md) |
| PREEMPT_RT（rwbase_rt） | rwsem 底座 | RT 构建切换 | [rwbase-rt](../../locking/rwbase-rt.md)——头注释点名 "think mmap_sem" |
| per-VMA lock | mmap_lock 之上 | 6.4+，缺页默认启用 | 本章 §三 |
| range lock（区间锁） | 替换 rwsem | **未合入**（"no wide concensus"） | §五 |
| speculative page fault | 缺页侧 | 未合入（Lespinasse 系列被 per-VMA 路线取代） | —— |

## 七、观测实战

```bash
# 1) 锁延迟直方图（三 tracepoint 框出等待区间）
cd /sys/kernel/debug/tracing/events/mmap_lock
echo 1 > mmap_lock_start_locking/enable; echo 1 > mmap_lock_acquire_returned/enable
#   hist：start_locking→acquire_returned 的差 = 排队时长（含 memcg 路径维度）
#   trigger 示例：
#   echo 'hist:key=memcg.path:val=hitcount' events/mmap_lock/mmap_lock_acquire_returned/trigger

# 2) per-VMA lock 的命中率
grep VMA_LOCK /proc/vmstat   # VMA_LOCK_MISS/RELOCK 等事件计数（count_vm_vma_lock_event）
#   MISS 高 = 写侧频繁或回绕/竞态多 → 缺页退慢路径比例

# 3) lockdep 视角
cat /proc/lockdep_stats | grep -i rwsem    # 全局 rwsem 争用画像

# 4) 现场问诊：D 状态进程堆栈里的 down_write_killable?
cat /proc/<pid>/stack | grep -E 'down_write|mmap_write'
#   常见元凶：内存压力下的 mmap/munmap 风暴（THP collapse/reclaim 拆 VMA）
```

## 八、思考题

1. `vma_start_read` 的第一段快检为什么"不需要 ACQUIRE 语义"而第二段必须？（提示：头注释——快检结果"we don't rely on for anything"，错了只是白拿一次 trylock 再放弃；第二段是**成功路径的证明**，必须与 `vma_end_write_all` 的 RELEASE 严格配对。）
2. `mmap_write_downgrade` 为什么必须推 `mm_lock_seq`？（提示：降级瞬间写者仍持有"刚改完表"的最新认知，但其后按读者身份继续——若 per-VMA 读锁不失效，它会读到"新表+旧锁序"的混合态。）
3. 0x120 论证说 count 与 owner 分属两行是"最优"——如果合成一行（真共享）会怎样？（提示：读者海量打 count、写者链打 owner——读写两种乒乓叠进同一行是**最坏**组合；对照 [cacheline-packing] 的"争用组之间分"。）
4. per-VMA lock 为什么能救缺页却救不了 GUP？（提示：GUP 常跨多个 VMA 且对"集合一致性"敏感（如遍历所有映射）；缺页只碰单 VMA——6.6 的 GUP 慢路径仍持 mmap_lock 读。）
5. Lespinasse 的理由②（range lock）最终没走通而 per-VMA 走通了——两者解决的是同一问题的哪两个不同面？（提示：range lock 压缩**写者**互斥域（不同区间可并行写）；per-VMA 压缩**读者**的锁粒度（单 VMA 验序）。后者先兑现是因为缺页读侧的收益面更大且协议可证明。）
6. RT 内核里 mmap_lock 变 rwbase_rt——"not writer fair"（[rwbase-rt] 头注释）对 mmap_lock 意味着什么？（提示：新读者可越过等待中的写者——fork/exec 这类写者延迟无上界波动，但换来的是缺页读者不被阻塞；RT 的取舍向读侧倾斜。）

---

> 回看：[vma 与 mmap 生命周期](../vma/vma与mmap生命周期.md)（VMA 结构与 maple tree——per-VMA 的另一半）｜[pagefault](../pagefualt/pagefault.md)（缺页路径如何消费 per-VMA lock）｜[rwsem](../../locking/rwsem.md)/[concession](../../locking/concession.md)（底层与让步机制）｜[rwbase-rt](../../locking/rwbase-rt.md)（RT 形态——"think mmap_sem" 出处）｜[cacheline-packing](../../locking/cacheline-packing.md)（0x120 偏移案例的谱系）｜[mm/README](../README.md)（分册导读）
