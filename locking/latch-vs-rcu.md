# latch vs RCU：读者零开销的两种方案（Linux 6.6）

> 阅读目标：把内核"读者零开销"的两大方案收进一张决策表——**RCU**（读指针→写者替换→GP 后释放旧版——指针间接 + 延迟回收）vs **seqcount_latch**（模 2 选两份拷贝之一——无间接 + 双倍存储 + 永久覆写）。理解选择逻辑：**小而固定的内联数据选 latch（省指针追逐）、大而可变的指针结构选 RCU（省双倍内存）**；NMI/极早 boot 场景的倾向；sched_clock 的 "single 64-byte cache line" 布局论证。全部对照 v6.6 源头注释。前置：[seqcount-latch](seqcount-latch.md)｜[seqlock](seqlock.md)。

---

## 一、共同目标：读侧零开销

```
两方案都要解决：
  读者不需要锁/原子操作/屏障就能拿到一致数据
  写者可以非原子地更新数据

共同代价：
  写者付出额外成本（RCU 的 GP 等待 / latch 的双写）
  内存布局付出额外成本（RCU 的旧版驻留窗口 / latch 的 2× 存储）
```

## 二、核心差异：间接 vs 复制

```
RCU 的模式（指针间接）：
  读者：rcu_read_lock → 读指针 → 解引用 → rcu_read_unlock
  写者：构造新版 → 替换指针 → call_rcu/synchronize_rcu 等读者退出 → 释放旧版

  ★读者拿到的数据来自"指针指向的那份"——新旧是两个独立分配
  ★旧版在 GP 之后释放——内存回收与读者退出绑定

latch 的模式（双份复制）：
  读者：读 seq → idx = seq & 1 → 读 data[idx] → 校验 seq
  写者：seq++ → 改 data[0] → seq++ → 改 data[1]

  ★读者拿到的数据来自"两份中的稳定那份"——新旧在同一结构体内
  ★两份永久存在——不释放，反复覆写
```

头注释原话（seqlock.h:708-712）：*"Where the traditional RCU/lockless data structures rely on atomic modifications to ensure queries observe either the old or the new state the latch allows the same for non-atomic updates. **The trade-off is doubling the cost of storage**; we have to maintain two copies of the entire data structure."*

## 三、决策表

| 维度 | RCU | seqcount_latch |
| :--- | :--- | :--- |
| **读者成本** | preempt_disable + 指针解引用 | seq 读取（1 次 load）+ `seq & 1` |
| **写者成本** | 新版分配 + 指针替换 + **GP 等待**（ms 级） | 四步双写（~ns 级） |
| **内存** | 1×（旧版仅 GP 窗口内驻留） | **永久 2×** |
| **数据形态** | 指针可达的**任意大小**结构 | **小的内联数据**（两份须在同一 cacheline） |
| **NMI 安全** | ✓（读者只是 preempt_disable） | ✓（读者只做 load） |
| **极早 boot** | ✗（GP 机制需 RCU 子系统就绪） | ✓（纯算术，无依赖） |
| **读者可睡？** | ✗（不可睡——spin_lock 的替代方案） | ✗（不可睡——须在两次 load 间保持原子性） |
| **更新频率** | 低频（GP 成本高） | **任意频率**（双写极快） |
| **内存回收** | 需要（kfree_rcu） | **不需要**（不分配不释放） |
| **典型用户** | 链表/哈希表/xarray（大量节点） | **时钟/计数器/固定元组** |

## 四、sched_clock 的选择论证

`kernel/time/sched_clock.c` 选择了 latch——头注释原话（:34-36）：

```
"The ordering of this structure has been chosen to optimize cache
 performance. In particular 'seq' and 'read_data[0]' (combined)
 should fit into a single 64-byte cache line."
```

```c
struct clock_data {
	seqcount_latch_t	seq;          /* + read_data[0] 共一行 */
	struct clock_read_data	read_data[2];  /* 两份时钟参数 */
	...
} ____cacheline_aligned;
```

★**为什么不用 RCU**：sched_clock 的 read_data 是一个**小的固定元组**（epoch/mult/shift/read_sched_clock 函数指针）——如果用 RCU：每次时钟源更新需分配新 read_data + 指针替换 + GP 等待 + 释放。latch 只需双写两个 24 字节的结构体，且 seq + data[0] 在同一 cacheline——**读者的两次 load 一次缓存行搞定**。

★**为什么不用普通 seqcount**：sched_clock 可能从 **NMI 调用**（perf 的时钟源）——普通 seqcount 在写者中间重试死循环。

## 五、printk 的选择论证

`kernel/printk/printk.c` 的 `struct latched_seq`——头注释（:460-464）：

```
"The next printk record to read after the last 'clear' command.
 There are two copies (updated with seqcount_latch) so that reads
 can locklessly access a valid value. Writers are synchronized
 by @syslog_lock."
```

```c
struct latched_seq {
	seqcount_latch_t	latch;
	u64			val[2];    /* 就是一个 u64——最小可能的数据 */
};
```

★latch 保护的是**一个 u64**——RCU 的指针间接开销完全不成比例。且 printk 的 dump 路径（sysrq-crash）运行在**不可睡眠**的最恶劣上下文。

## 六、RCU 用户的反面论证

```
链表遍历（如 fs/dcache 的 dentry）：
  节点数可达千万——2× 存储 = 多耗 1GB 级内存
  节点动态分配/释放——latch 的"两份"无法适应
  → 必须用 RCU（指针间接 + GP 回收）

xarray（如 page cache 的 mapping）：
  树形结构——latch 只能平面复制，树的双份不现实
  → RCU 是唯一选择
```

## 七、总结口诀

```
数据小而固定（≤ cacheline）→ latch（双份 + 模 2）
数据大或动态（指针可达）   → RCU（间接 + GP）
NMI/极早 boot              → latch（无依赖）
需要释放内存              → RCU（GP 后 kfree）
更新极频繁               → latch（双写 ns 级）
```

## 八、思考题

1. RCU 读者只需 preempt_disable——latch 读者需两次 load+校验——为什么 latch 反而可能更快？（提示：RCU 的指针解引用可能跨 cacheline miss；latch 的 seq+data[0] 可挤同一行——sched_clock 的布局论证。）
2. latch 的写者"externally serialized"——RCU 的写者也需要吗？（提示：RCU 的指针替换本身原子（store-release）——多写者也可不互斥；latch 的四步双写必须串行。）
3. 能否把 latch 推广到三份拷贝（模 3）？（提示：理论上可以——但 seq 的编码复杂化（需 2 bit）且收益递减（三写者并发？）——内核实际无三份需求。）
4. sched_clock 在**极早 boot**（RCU 未初始化）用 latch——什么时候才能切 RCU？（提示：sched_clock 永远不切——它在整个内核生命周期都可能是 NMI 上下文的时钟源。）
5. RCU 的 `kfree_rcu` 批量释放与 latch 的"不释放"——长跑系统的内存碎片差异？（提示：RCU 频繁 alloc/free 导致碎片；latch 零碎片但双倍常驻。）
6. 把 sched_clock 的 read_data[2] 改成 RCU——最大的实际问题是什么？（提示：每次时钟源更新（suspend/resume/hotplug）都要 alloc 新结构+替换+等 GP+释放——suspend 路径的延迟不可接受。）

---

> 回看：[seqcount-latch](seqcount-latch.md)（latch 的完整机制）｜[seqlock](seqlock.md)（普通 seqcount 的基础）｜[concession](concession.md)（读者让步 vs 读者选择的框架）｜[locking/README](README.md)（锁族谱）
