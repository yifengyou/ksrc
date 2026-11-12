# 内核中的模算术：回绕即正确（Linux 6.6）

> 阅读目标：提炼内核反复使用的**无符号模算术**惯用法——CPU 字长的自然回绕（mod 2^64）被当作**特性**而非 bug：percpu-rwsem 的 "modular sum"（求和不需要同步）、percpu_ref 的 `PERCPU_COUNT_BIAS` 偏置（防碰零的巨型正数）、`time_after` 的**带符号差判序**（回绕安全的 jiffies 比较）、seqcount 的**奇偶语义**（回绕不变的写入标记）、percpu_counter 的**近似求和**（batch + 全局计数的精度换算）。理解为什么"算术正确性来自模的性质而非同步"是内核无锁设计的数学底座。全部对照 v6.6。前置：[percpu-refcount](percpu-refcount.md)｜[seqlock](seqlock.md)。

---

## 一、核心定理：模加法的交换律

```
无符号整数溢出 = mod 2^N 运算（N=64）
数学事实：(a + b) mod M = (a mod M + b mod M) mod M
推论：任意分组、任意顺序的加法，mod 结果相同

lib/percpu-refcount.c:24-33 注释原话：
  "(More precisely: because modular arithmetic is commutative the sum
    of all the percpu_count vars will be equal to what it would have
    been if all the gets and puts were done to a single integer, even
    if some of the percpu integers overflow or underflow)."
```

★这是 percpu 计数器**不需要全局同步**就能保持正确性的数学基础——每个 CPU 独立 inc/dec（本地计数器可以回绕！），需要精确值时逐 CPU 求和即可。

## 二、案例一：percpu-rwsem 的"modular sum"（kernel/locking/percpu-rwsem.c:185-218）

```c
#define per_cpu_sum(var)                          \
({                                                \
	typeof(var) __sum = 0;                        \
	int cpu;                                      \
	for_each_possible_cpu(cpu)                    \
		__sum += per_cpu(var, cpu);              \
	__sum;                                        \
})

/* :202-207 注释原话："Return true if the modular sum of the
   sem->read_count per-CPU variable is zero.  If this sum is zero,
   then it is stable due to the fact that if any newly arriving
   readers increment a given counter, they will immediately
   decrement that same counter." */
static bool readers_active_check(struct percpu_rw_semaphore *sem)
{
	if (per_cpu_sum(*sem->read_count) != 0)
		return false;
	smp_mb();   /* C matches B */
	return true;
}
```

★**零的稳定性论证**：读者进出**总是配对**（enter 加一→读→exit 减一，同一 CPU）。因此模和为零时，不可能有"一个读者的 inc 在途而 dec 未到"——inc 和 dec 在同一 CPU 上配对完成，模和恰好反映**净在线数**。

## 三、案例二：percpu_ref 的 PERCPU_COUNT_BIAS（lib/percpu-refcount.c:37/:90/:195/:250）

```c
#define PERCPU_COUNT_BIAS  (1LU << (BITS_PER_LONG - 1))  /* 2^63 */
```

```
初始化（:88-91）：percpu 模式 start_count = 1 + BIAS
切换到原子（:195）：atomic_long_add((long)count - BIAS, &data->count)
切回 percpu（:250）：atomic_long_add(BIAS, &data->count)
```

★**BIAS 的目的**：切换时要把 percpu 求和灌入原子值。**中间值可能为负**（某 CPU 残留 put），如果从 1 起步，求和过程中的中间原子值可能碰零——**误触发 release 回调**！BIAS = 2^63 保证 `count - BIAS` 在任何中间状态下都远离零。

**模算术的作用**：BIAS 的加与减就是普通 mod 2^64 加法——不需要特殊处理。

## 四、案例三：time_after 的带符号差判序（include/linux/jiffies.h:114-126）

```c
/* 注释原话："time_after - returns true if the time a is after time b." */
#define time_after(a, b)                          \
	(typecheck(unsigned long, a) &&               \
	 typecheck(unsigned long, b) &&               \
	 ((long)((b) - (a)) < 0))
```

```
jiffies 是 unsigned long——会回绕（64 位机约 584 年，32 位机 49 天）
比较"a 是否在 b 之后"不能直接 a > b（回绕后大小关系反转）

诀窍：(b - a) 的结果视为带符号数：
  a 比 b 晚（差值在前半圆）→ (long)(b-a) < 0
  a 比 b 早（差值在后半圆）→ (long)(b-a) >= 0

数学：mod 2^64 的圆环上，两点距离的前后半圆判定
      = "环形缓存的序比较"同款
```

## 五、案例四：seqcount 的奇偶语义（include/linux/seqlock.h）

```c
/* 写者：进入时 seq++（变奇数）；退出时 seq++（变偶数） */
/* 读者：seq1 = 读 seq；读数据；seq2 = 读 seq
         成功 = seq1 == seq2 && seq1 为偶数 */
```

★**奇偶即状态**（模 2 语义）：
- 偶数 = 无写者 → 数据稳定可读
- 奇数 = 写者正在改 → 读者重试
- 任意时刻只看**模 2 的余数**——序列号本身可以回绕无数次

这不需要 BIAS 或差值——mod 2 的天然周期性恰好编码了"正在写/不在写"的二值状态。

## 六、案例五：percpu_counter 的近似求和（include/linux/percpu_counter.h:22-30）

```c
struct percpu_counter {
	raw_spinlock_t lock;
	s64 count;            /* ★全局精确值（持锁更新）*/
	s32 __percpu *counters; /* percpu 近似值（无锁 inc/dec）*/
};
```

```
写入侧（percpu_counter_add）：
  本地计数器 += n（无锁，s32 回绕）
  |本地| > batch 时 → 全局 += 本地累加值，本地清零

读取侧（percpu_counter_read）：
  return fbc->count + per_cpu_sum(fbc->counters)
  /* 全局精确值 + percpu 近似和 = 当前总值的估计 */

精度：误差 < nr_cpus * batch（batch 通常 = max(10, nr_cpus*2)）
```

★与案例一（精确和）的区别：percpu_counter 的 `count` 是**主动同步的全局值**，percpu 计数器只是缓冲——精确度要求高时持锁取全局，宽松时用近似和。**分层的模算术**。

## 七、总结表

| 案例 | 模算术性质 | 对象 | 同步需求 |
| :--- | :--- | :--- | :--- |
| percpu-rwsem modular sum | **交换律**（任意分组求和等价） | percpu read_count | 零（读者进出配对） |
| percpu_ref BIAS | **加法消去**（BIAS 加减不碰零） | percpu 计数 + 原子值 | 切换时一个 GP |
| time_after | **带符号差**（圆环序比较） | jiffies | 零（纯算术） |
| seqcount 奇偶 | **模 2 周期**（奇偶编码状态） | sequence 号 | 写者互斥 |
| percpu_counter | **分层近似**（全局精确+percpu 缓冲） | percpu s32 | 持锁更新全局 |

**共同哲学**：正确性来自**算术结构**（模性质）而非**同步机制**（锁/原子/屏障）——"让数学代替锁做正确性保证"。

## 八、观测实战

```bash
# 1) percpu-rwsem 的读计数（BPF 侧读 per_cpu_sum）
bpftrace -e 'kprobe:readers_active_check { printf("checking\n"); }'

# 2) percpu_counter 的精度实验
cat /proc/sys/vm/percpu_pagelist_high  # 间接观察 batch 大小
#   mm 的 percpu_counter 消费者之一

# 3) jiffies 回绕的极端测试（QEMU 加速）
#   32 位机：HZ=1000 → 49.7 天回绕 → time_after 的正确性关键

# 4) seqcount 的奇偶验证
bpftrace -e 'kprobe:read_seqcount_begin /@seq%2!=0/ { @odd++; }'
```

## 九、思考题

1. per_cpu_sum 的正确性依赖"读者进出配对"——如果某读者睡了（在临界区内 schedule），sum 会怎样？（提示：sum 非零——写者正确地等待；但**percpu-rwsem 的读者禁止睡**——这就是为什么。）
2. BIAS 取 2^63 而不是 2^31——为什么不取小值？（提示：percpu 残留 put 的绝对值理论上可接近 2^64；BIAS 必须大于最大可能负值。）
3. time_after 在差值恰好为 2^63（半圆点）时怎样？（提示：`(long)(b-a) < 0` 在 `b-a == 2^63` 时为假——恰好在"a 比 b 晚整整半圈"的边界。实际 jiffies 差不会这么大。）
4. seqcount 的 seq 回绕（32 位→20 亿秒）需要处理吗？（提示：回绕本身无害（奇偶不变）；但**极旧读者**可能刚好跨越两次回绕——概率极低但理论存在。）
5. percpu_counter 的 `counters` 是 s32 而 `count` 是 s64——为什么？（提示：s32 允许回绕（模 2^32 求和后仍正确）；s64 全局避免频繁溢出。）
6. 把"模算术保证正确性"与"锁保证正确性"对比——各付什么代价？（提示：模算术零同步成本但精度受限（percpu_counter 近似）/查询时机受限（只能特定结构上用）；锁可精确但付同步代价。）

---

> 回看：[percpu-refcount](percpu-refcount.md)（BIAS 的完整算术）｜[seqlock](seqlock.md)（奇偶语义的完整机制）｜[concession](concession.md)（percpu-rwsem 的 block 位让步）｜[locking/README](README.md)（锁族谱）
