# seqcount_latch：双缓冲乐观读（Linux 6.6）

> 阅读目标：吃透 seqlock 的**双缓冲变体**——`seqcount_latch_t`（include/linux/seqlock.h:638-780）：模 2 的奇偶选择**两份拷贝**中的一份（读者永远能拿到稳定快照——不需要重试！）；写者的**四步双写协议**（wmb→seq++→改 data[0]→wmb→seq++→改 data[1]）；★头注释原话 "A sequence counter variant where the counter even/odd value is used to switch between two copies of protected data. This allows the read path, **typically NMIs**, to safely interrupt the write side critical section"——NMI 安全的根源。两个消费者深挖（printk 的 NMI 安全 log buffer / sched_clock 的 NMI 安全时钟）。全部对照 v6.6。前置：[seqlock](seqlock.md)｜[modular-arithmetic](modular-arithmetic.md)（模 2 的空间应用）。

---

## 一、问题：NMI 打不断写者怎么办

```
普通 seqcount 的困境：
  写者：seq++（变奇）→ 改数据 → seq++（变偶）
  读者：seq1=读；读数据；seq2=读
        若 seq1==seq2 且偶 → 成功
        若写者在读者中间改了 → 读者检测到并重试

★NMI 场景：NMI handler 里读 seqcount——
  如果 NMI 打断了写者（seq 为奇数），读者重试
  但写者被 NMI 打断后不会前进 → 读者永远读到奇数 → 死循环！
```

`seqcount_latch` 的答案：**不做重试——做双缓冲选择**。头注释原话（:640-643）：*"A sequence counter variant where the counter even/odd value is used to switch between two copies of protected data. This allows the read path, typically NMIs, to safely interrupt the write side critical section."*

## 二、结构与读写协议（seqlock.h:649-780）

### 2.1 数据结构

```c
typedef struct {
	seqcount_t seqcount;        /* 就是普通 seqcount——复用 */
} seqcount_latch_t;

/* 用户结构（头注释 :712-715 的示例）*/
struct latch_struct {
	seqcount_latch_t	seq;
	struct data_struct	data[2];   /* ★两份拷贝 */
};
```

### 2.2 写者：四步双写协议（头注释 :717-729 原话）

```c
void latch_modify(struct latch_struct *latch, ...)
{
	smp_wmb();               /* 确保 data[1] 的上次更新已可见 */
	latch->seq.sequence++;   /* 奇→读者选 data[1]（旧稳定版）*/
	smp_wmb();               /* 确保 seq 更新可见 */

	modify(latch->data[0], ...);   /* ★改 data[0]（读者此刻不读它）*/

	smp_wmb();               /* 确保 data[0] 更新可见 */
	latch->seq.sequence++;   /* 偶→读者选 data[0]（新稳定版）*/
	smp_wmb();               /* 确保 seq 更新可见 */

	modify(latch->data[1], ...);   /* 改 data[1]（为下一轮准备）*/
}
```

### 2.3 读者：无重试的选择读取（头注释 :731-740 原话）

```c
struct entry *latch_query(struct latch_struct *latch, ...)
{
	struct entry *entry;
	unsigned seq, idx;

	do {
		seq = raw_read_seqcount_latch(&latch->seq);
		idx = seq & 0x01;          /* ★模 2 选择哪份拷贝 */
		entry = data_query(latch->data[idx], ...);
	} while (raw_read_seqcount_latch_retry(&latch->seq, seq));

	return entry;
}
```

★与普通 seqcount 的关键差异：读者**始终有一份完整的稳定拷贝可读**——`idx = seq & 1` 指向的拷贝在写者当前阶段中**一定不在被修改**。重试只在"恰好跨越了 seq 的两次翻转"时发生（极短窗口）——但**NMI 场景下重试一定能退出**（写者不需要前进，读者下次读到的 seq 已经不同）。

## 三、头注释的设计哲学（:702-710 原话）

```
"The latch technique is a multiversion concurrency control method
 that allows queries during non-atomic modifications. If you can
 guarantee queries never interrupt the modification -- e.g. the
 concurrency is strictly between CPUs -- you most likely do not
 need this.

 Where the traditional RCU/lockless data structures rely on atomic
 modifications to ensure queries observe either the old or the new
 state the latch allows the same for non-atomic updates. The
 trade-off is doubling the cost of storage; we have to maintain
 two copies of the entire data structure.

 Very simply put: we first modify one copy and then the other.
 This ensures there is always one copy in a stable state, ready
 to give us an answer."
```

★**MVC（Multiversion Concurrency Control）**——数据库领域的概念在内核的化身：存储换正确性（双份内存 vs 原子性保证）。

## 四、与普通 seqcount 的对照

| 维度 | seqcount | seqcount_latch |
| :--- | :--- | :--- |
| 数据拷贝 | 1 份 | **2 份** |
| 读者行为 | 读→校验→**可能重试**（读中间态则丢弃重来） | 选择哪份→读→校验（**几乎不重试**） |
| NMI 安全 | ✗（写者被 NMI 打断→读者死循环） | ✓（读者总有稳定拷贝） |
| 写者成本 | 2 次 seq++ | 2 次 seq++ + **双写数据** |
| 内存成本 | 1× | **2×** |
| 适用 | 读多写少，NMI 不参与 | **NMI/中断读者**，写多也可容忍 |

## 五、消费者一：printk 的 NMI 安全 log buffer（kernel/printk/printk.c:456-531）

```
printk 的需求：NMI 里也要能安全读 log buffer（dump 上下文）
  如果用普通 seqcount：NMI 打断写者→奇数 seq→重试死循环

用 seqcount_latch：
  struct printk_logbuffer {
      seqcount_latch_t latch;
      struct log_data data[2];   /* 两份 buffer 元数据 */
  };

  读者（:528-531）：
      do {
          seq = raw_read_seqcount_latch(&ls->latch);
          idx = seq & 0x01;
          /* 从 data[idx] 读取——总是稳定的 */
      } while (raw_read_seqcount_latch_retry(&ls->latch, seq));

  写者（:514-516）：
      raw_write_seqcount_latch(&ls->latch);  /* 两次调用=四步协议 */
```

## 六、消费者二：sched_clock 的 NMI 安全时钟（kernel/time/sched_clock.c:39-74）

```
sched_clock 的需求：NMI/调度器 early boot 时读时钟——不能死循环

struct clock_data {
	seqcount_latch_t	seq;
	struct read_clock	*clock[2];  /* 两份时钟数据 */
};
```

## 七、与 modular-arithmetic 的回扣

[modular-arithmetic](modular-arithmetic.md) 章的案例四讲 seqcount 的**模 2 时间**版本（奇/偶序列号编码写入状态）。latch 是同一模 2 的**空间**版本——奇偶选择**两份拷贝**的哪一份。头注释 :667 的 `idx = seq & 0x01` 正是模 2 运算的空间化。

## 八、观测实战

```bash
# 1) latch 用户清点
grep -r 'seqcount_latch' kernel/ include/ | grep -v seqlock.h | head -6

# 2) sched_clock 的双缓冲验证
#   读取时间戳时底层走了哪个 idx（BPF 侧）
bpftrace -e 'kprobe:sched_clock_read { @clk = count(); }'

# 3) printk 的 NMI 安全 dump 实验
echo c > /proc/sysrq-trigger    # crash dump（会走 NMI 安全的 log 读取路径）
```

## 九、思考题

1. 读者的 `do...while` 循环几乎不会执行第二次——但为什么不能去掉？（提示：跨越两次 seq 翻转的极短窗口——写者刚翻完偶又翻成奇——读者拿到的 idx 可能指向已被覆盖的拷贝。）
2. 写者的四步中第一次 `smp_wmb()` 在 `seq++` **之前**——为什么？（提示：确保上一轮 data[1] 的修改对"即将选择 data[1] 的读者"可见——否则读者可能看到混合的新旧数据。）
3. 双份存储的代价是 2× 内存——printk 和 sched_clock 为什么愿意付？（提示：数据结构本身很小（元数据/时钟值）；NMI 安全是**不可妥协**的硬需求。）
4. latch 的写者要求"externally serialized"（外部串行化）——谁保证 printk/sched_clock 的写者互斥？（提示：printk 用 logbuf_lock；sched_clock 用自己的 spinlock——latch 不提供写者互斥。）
5. 把 latch 与 RCU 对照——都是"读者零开销+双份数据+延迟更新"——本质区别？（提示：RCU 的旧版本在 GP 后释放（内存回收）；latch 的两份永久存在（不回收，反复覆写）。）
6. 如果读者在 NMI 里恰好碰到了 seq 为奇的瞬间——它选哪份拷贝？（提示：奇数→idx=1→读 data[1]——写者此刻正在改 data[0]（第一步后）→data[1] 仍是旧稳定版。）

---

> 回看：[seqlock](seqlock.md)（基础 seqcount 的机制）｜[modular-arithmetic](modular-arithmetic.md)（模 2 的空间应用——案例四的延伸）｜[rwsem](rwsem.md)（读者让步 vs latch 的读者选择对照）｜[locking/README](README.md)（锁族谱）
