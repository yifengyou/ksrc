# spinlock 与 qspinlock（Linux 6.6）

> 阅读目标：理解自旋锁的四代演进（TAS → TAS+backoff → ticket → qspinlock）各自的硬件动机（cache 一致性协议与 cacheline 弹跳）；逐字段拆解 qspinlock 的 4 字节编码与 pending/MCS 队列双路径；掌握 PV qspinlock、PREEMPT_RT 语义、调试设施与使用守则。

---

## 一、问题设定：为什么"自旋"

内核临界区极短（纳秒~微秒级），睡眠等锁（上下文切换 ~几 µs 起）比空转等待更贵——所以拿不到锁时 **CPU 忙等（spin）**。但自旋锁的真正难点不在"等"，而在**等的时候别把总线/缓存搞炸**。这是纯粹的 cache 一致性协议（MESI/MESIF/MOESI）工程。

---

## 二、四代演进

### 2.1 第一代：test-and-set（TAS）

```
lock:   while (xchg(&locked, 1) == 1) ;   /* 拿不到就再试 */
unlock: locked = 0;
```

* **公平性**：无。运气差的 CPU 永远抢不到（starvation）。
* **缓存行为**：N 个 CPU 同时 `xchg`（写操作，抢独占权）→ 锁所在的 cache line 在核间疯狂弹跳（cache line bouncing）。每次 xchg 都是一次 RFO（Read-For-Ownership），微秒级开销 × N 竞争者，总线被打爆。

### 2.2 第二代：TAS + 指数退避（backoff）

拿不到就指数延迟再试（`cpu_relax` 循环变体），减少 RFO 频率。现代实现里 backoff 思想保留在 pending 等待与 `CONFIG_LOCK_SPIN_ON_OWNER` 的启发式里。问题依旧：公平性无解。

### 2.3 第三代：ticket lock（x86 2.6.25~3.14，arm64 直到 5.x）

```
struct ticket { u16 next; u16 owner; };     /* 4 字节 */

lock:   my_ticket = atomic_fetch_add(&next, 1);   /* 一次 RFO，领号 */
        while (owner != my_ticket) cpu_relax();   /* 本地读自己的行？不——owner
                                                      仍是共享行，解锁时逐跳传播 */
unlock: owner++;                                    /* 定向通知下一位 */
```

* **公平性**：FIFO，完美。
* **缓存行为**：每个竞争者只需一次写（领号）；等待读的是 `owner` 字段（shared 状态，核间传播）。**但** `owner++` 时所有 waiter 的缓存行都要逐个失效（每个核一次 RFO），解锁成本 O(waiters)——"坏消息逐跳传播"。5.7 给 x86 加了 4 倍 smp_cond_load 退避（`pending` 位替代 owner 等待），但结构问题未除。

### 2.4 第四代：qspinlock（3.15 主线，x86 3.17 默认；6.6 全主流架构）

把 **MCS lock**（1991 年 Mellor-Crummey & Scott）的思想压进 4 字节：每个 waiter 在**自己的 per-CPU 节点**上自旋，锁字里只留"队尾指针"。等待完全本地化——解锁只通知下一个节点（一次缓存行传输），O(1) 传播。

| 代 | 公平 | 拿锁 RFO | 解锁传播 | 无竞争开销 |
| :--- | :--- | :--- | :--- | :--- |
| TAS | ✗ | O(竞争者×重试) | 广播（全失效） | 1 次 xchg |
| ticket | ✓FIFO | 1/竞争者 | O(waiters) 逐跳 | 1 次 fetch_add |
| qspinlock | ✓FIFO | 1/竞争者 | **O(1) 定向** | 1 次 cmpxchg |

---

## 三、qspinlock 详解（kernel/locking/qspinlock.c + include/asm-generic/qspinlock_types.h）

### 3.1 4 字节的位编码

```c
typedef struct qspinlock {
	union {
		atomic_t val;          /* 整体原子视图 */
		struct {
			u8 locked;     /* bit0-7:  锁定字节（0=空闲 1=持有）*/
			u8 pending;    /* bit8-15: pending 位（第二顺位候补）*/
		};
		struct {
			u16 locked_pending;
			u16 tail;      /* bit16-17: tail idx（该 CPU 的第几个节点）
			                   bit18-31: tail cpu+1（编码进 14 位，NR_CPUS<16K）*/
		};
	};
} arch_spinlock_t;
```

读法：`val = 0x00000000` 全空；`0x00000001` 持有中；`0x0000xxxx | tail<<16 | locked` 表示"持有中且已有排队者，队尾是 cpu/idx"。**locked 用完整字节**是刻意的：解锁 `WRITE_ONCE(locked, 0)` 单字节存储，x86 带隐式 release 语义，且 pending 路径可 16 位合并操作 `locked_pending`。

### 3.2 快路径（`queued_spin_lock`，include/asm-generic/qspinlock.h）

```c
static __always_inline void queued_spin_lock(struct qspinlock *lock)
{
	int val = 0;

	if (likely(atomic_try_cmpxchg_acquire(&lock->val, &val, _Q_LOCKED_VAL)))
		return;                       /* 99.9% 的情况：一条 lock cmpxchg */
	queued_spin_lock_slowpath(lock, val);
}
```

### 3.3 慢路径的三级火箭（`queued_spin_lock_slowpath`，6.6）

```
① pending 快路径（只有 1 个等待者时跳过排队）
    val==_Q_PENDING_VAL（正在交接 0,1,0→0,0,1）→ 有界自旋等交接完成
    若只有 locked 置位：
        queued_fetch_set_pending_acquire(): 0,0,1 → 0,1,1   （设 pending）
        smp_cond_load_acquire(&lock->locked, !VAL)          （等持有者离开）
        clear_pending_set_locked(): 0,1,0 → 0,0,1           （接棒）
        —— 一次 cacheline 竞争完成第二顺位继位，零队列开销

② MCS 排队（≥2 个等待者）
    node = this_cpu_ptr(&qnodes[0].mcs); idx = node->count++;   /* 每CPU 4 个
       嵌套节点（MAX_NODES=4，应对中断嵌套拿锁）；NMI 溢出则裸自旋兜底 */
    tail = encode_tail(smp_processor_id(), idx);                /* 我要当队尾 */
    old = xchg_tail(lock, tail);     /* 原子交换队尾指针：O(1) 且只打扰旧队尾 */
    if (old & _Q_TAIL_MASK) {        /* 已有队列 */
        prev = decode_tail(old);
        WRITE_ONCE(prev->next, node); /* 把自己挂到前驱后面 */
        arch_mcs_spin_lock_contended(&node->locked); /* ★在自己节点的
           locked 字段上自旋（本地 cacheline，独占且不被打扰）直到前驱解锁 */
    } else {
        /* 我是第一个排队者：先自旋等 locked 清零（第二个共享点，不可避免）*/
        smp_cond_load_acquire(&lock->locked, !VAL);
    }

③ 接锁（`queued_spin_setlocked`）
    next = READ_ONCE(node->next);
    if (!next) {  /* 队列变空（我后面的都撤了）→ 尝试原子清尾 */
        if (atomic_try_cmpxchg_release(&lock->val, &mytail|locked, 0)) goto done;
        next = ...（重读，有人刚入队）
    }
    arch_mcs_spin_unlock_contended(&next->locked);   /* ★定向唤醒下家：
       只写 next 的 cacheline，一次传输。这就是 O(1) 传播 */
    WRITE_ONCE(lock->locked, 0);（实际通过 setlocked 完成）
```

三个精髓：

* **队尾交换用 `xchg`**：把"N 个人排队"变成"N 次定向写前驱节点"，锁字本身只被队首/队尾触碰。
* **per-CPU 4 节点**：同 CPU 嵌套上下文（进程→软中断→硬中断→NMI 各拿一把锁）各用各的节点；节点复用 `node->count` 计数，解锁时归还。
* **pending 位是 0~1 等待者的"快速电梯"**：绝大多数争用只有一两个竞争者，排队纯属浪费——用 2 次原子操作代替队列建立。

### 3.4 PV qspinlock：虚拟化的续命改造

虚拟机 oversubscribe 时，持有者可能被 hypervisor 挪下物理 CPU，而自旋者烧着分配到的 CPU 时间干等——经典的 lock-holder preemption。`pvqspinlock`（CONFIG_PARAVIRT_SPINLOCKS，KVM/XEN 默认开启）：

* 慢路径里 `pv_wait`（`kvm_wait`）把 vCPU **halt**（`HLT`）而不是自旋；
* 解锁方 `pv_kick` 唤醒指定 vCPU（`kvm_kick` 写其唤醒事件口/发 IPI）；
* 锁字值 `_Q_SLOW_VAL` + per-CPU `lock_hash` 把 qspinlock 和等待的 vCPU 关联。

```
物理机：     spin 浪费但简单
虚拟机(轻超卖)：spin 可接受
虚拟机(重超卖)：pvqspinlock 的 halt+kick 才是正解
```

---

## 四、API 家族与语义细则

```c
spin_lock(l) / spin_unlock(l)                 基础对
spin_lock_irq(l) / spin_unlock_irq(l)         本地关中断版（unlock 无条件开）
spin_lock_irqsave(l, flags) / ..._irqrestore  保存/恢复中断状态（嵌套/共享场景必用）
spin_lock_bh(l) / spin_unlock_bh(l)           关软中断版（local_bh_disable）
spin_trylock(l) / spin_is_locked(l) / spin_is_contended(l)
DEFINE_SPINLOCK(x) / spin_lock_init(&x)
```

* **`spin_lock_irq` vs `spin_unlock_irq` 的坑**：`unlock_irq` 无条件 `sti`——若入口时中断本就关着（嵌套调用者），会提前放中断。**规则：除非确证入口开中断，一律用 irqsave/irqrestore。**
* **软中断路径必须 `spin_lock_bh`**：否则 tasklet/softirq 抢进同一临界区（软中断可在进程上下文 `local_bh_enable` 时执行）。
* **非原子上下文禁用**：持自旋锁不许睡眠/缺页/`kmalloc(GFP_KERNEL)`——调度器不会回来，持锁 CPU 上的所有等待者集体烤焦。PREEMPT_RT 下这条从"纪律"变成"强制"（见下）。

### 4.1 CONFIG_PREEMPT_RT 的语义替换

RT 内核里 `spinlock_t` 不再是 `arch_spinlock_t`，而是 rt_mutex 包装的**睡眠锁**（`include/linux/spinlock_types_rt.h`）：

* `spin_lock()` 变成可睡眠但保持"关抢占"观感；真正不可睡眠的路径用 `raw_spinlock_t`（原始语义保留）。
* 中断处理被线程化后，spin_lock 的"关中断需求"大幅消失；`spin_lock_irqsave` 在 RT 上退化为普通 rt 锁 + local_irq_save（保留但语义弱化）。
* **代码要同时在 5.x 主线和 RT 上跑，用 `raw_spinlock_t` 的唯一合法场景是：锁会被 NMI/hardirq（非线程化部分）触碰。**

### 4.2 调试设施

| 配置 | 作用 |
| :--- | :--- |
| `CONFIG_DEBUG_SPINLOCK` | 魔数校验（`SPINLOCK_MAGIC`）、重复解锁/未初始化检测（kernel/locking/spinlock_debug.c 的 `do_raw_spin_lock` 路径） |
| `CONFIG_DEBUG_LOCK_ALLOC` + lockdep | 死锁/锁序静态规则审查（`lockdep-design.rst`，本目录镜像） |
| `CONFIG_LOCK_STAT` | 争用直方图（`/proc/lock_stat`：contention/bounce 统计） |
| `CONFIG_QUEUED_LOCK_STAT`（x86 debugfs） | qspinlock 事件计数（lock_pending/lock_slowpath/lock_no_node…） |
| lockevent / `perf lock` | 离线争用分析 |

---

## 五、`raw_spinlock_t` / `spinlock_t` / 位锁的关系

```
raw_spinlock_t ≡ arch_spinlock_t（qspinlock 原样）
spinlock_t     = { raw_spinlock_t rlock; dep_map; }（主线）
spinlock_t     = rt 锁（PREEMPT_RT）
```

`spin_lock` 的真正开销 = qspinlock + **preempt_disable** + **local_bh 特判** + lockdep 钩子（开 DEBUG_LOCK_ALLOC 后锁路径膨胀 2~3 倍——性能敏感生产内核要权衡）。

---

## 六、与兄弟锁的分工（速查）

| 锁 | 等待 | 适用 | 本分册 |
| :--- | :--- | :--- | :--- |
| spinlock | 自旋 | 极短临界区/原子上下文 | 本文 |
| seqlock | 乐观读 | 读多写少的指针/结构体（时间戳、jiffies、统计） | [seqlock.md](seqlock.md) |
| mutex | 睡眠（含乐观自旋） | 可睡眠上下文的较长临界区 | [mutex.md](mutex.md) |
| rwsem | 睡眠/自旋 | 读多写少且临界区较长 | [rwsem.md](rwsem.md) |
| RT mutex | 睡眠+PI | 优先级敏感/互斥（futex、PREEMPT_RT 全家桶） | `rt-mutex-design.rst`（本目录） |
| RCU | 无锁读 | 读极多、写可延迟回收 | [../RCU/](../RCU/README.md) |

---

## 七、思考题

1. qspinlock 的 pending 位为什么占满第二个字节（而非 1 bit）？提示：`locked_pending` 16 位合并操作 + 小端序下的编译器优化（`clear_pending_set_locked` 在 x86 上是一条 16 位 store）。
2. `xchg_tail` 用 `xchg` 而非 `cmpxchg` 重试。xchg 无条件写但不需要循环——在队列场景这为什么反而更快？
3. per-CPU 只有 4 个 MCS 节点（MAX_NODES=4），第 5 个嵌套持锁者（NMI 套 NMI）直接裸自旋。为什么可以接受？
4. 解锁的 `arch_mcs_spin_unlock_contended(&next->locked)` 写的是**下一个节点自己的 cacheline**。请从 MESI 角角描述这次写如何恰好只让 next 的核拿到 RFO，而不是全体 waiter。
5. PV 场景下 `pv_wait` 前为什么必须把状态发布到 lock_hash？kick 方如何知道该踢哪个 vCPU？（`pv_kick_node`/`__pv_kick` 路径。）

---

> 下一节：[seqlock](seqlock.md) —— 读侧零开销的乐观锁。
