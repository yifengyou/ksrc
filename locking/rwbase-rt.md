# rwbase_rt：rwsem 的 RT 基座（Linux 6.6）

> 阅读目标：吃透 rtmutex.c 四重多重包含的最后未深挖成员——`struct rwbase_rt`（include/linux/rwbase_rt.h + kernel/locking/rwbase_rt.c 289 行）：**READER_BIAS=2^31 的偏置设计**（负数=读者快路径可用 / 正数=写者持有中 / 0=有读者但无偏置）、写者的三步拿锁协议（rtmutex→去 BIAS→等读者清零）、读者的**先读快路径再代理锁**双段、以及★头注释关于**写者饥饿**的完整论证——"rw semaphores and rw locks on RT are **not writer fair**"与 proxy-locking 为什么被放弃。全部对照 v6.6。前置：[preempt-rt-matrix](preempt-rt-matrix.md)｜[ww-rt-mutex](ww-rt-mutex.md)。

---

## 一、结构：偏置计数 + rt_mutex（rwbase_rt.h:11-14）

```c
#define READER_BIAS  (1U << 31)   /* 读者偏置：初始计数 */
#define WRITER_BIAS  (1U << 30)   /* 写者标记 */

struct rwbase_rt {
	atomic_t		readers;        /* ★计数 + 偏置的混合编码 */
	struct rt_mutex_base	rtmutex;    /* 写者之间的互斥 */
};
```

★**readers 值的三段语义**：
```
  < 0（如 0x80000000）：READER_BIAS 在位 → 读者可走快路径（无写者）
  == 0：写者已去掉 BIAS，等最后读者退出（写者的过渡态）
  > 0（如 0x40000000）：WRITER_BIAS 在位 → 写者持有中
  > 0 但 < WRITER_BIAS：混合——写者 + 并发读者（RT 允许！）
```

## 二、写者：三步拿锁协议（rwbase.c 头注释 :8-13 原话）

```
头注释原话："down_write/write_lock()
  1) Lock rtmutex
  2) Remove the reader BIAS to force readers into the slow path
  3) Wait until all readers have left the critical section
  4) Mark it write locked"
```

```
down_write(rwb):
  ① rt_mutex_lock(&rwb->rtmutex)       —— 与其他写者互斥
  ② atomic_add(-READER_BIAS, &readers) —— 去 BIAS（新读者走慢路径）
  ③ 等待 readers 降到 0（存量读者陆续退出）
     /* 每个读者的 up_read 看到 readers==0 时唤醒等待的写者 */
  ④ atomic_add(WRITER_BIAS, &readers)  —— 标记写锁

up_write(rwb):
  ① atomic_add(-WRITER_BIAS, &readers) —— 去写标记
  ② atomic_add(READER_BIAS, &readers)  —— 恢复 BIAS（读者快路径）
  ③ rt_mutex_unlock(&rwb->rtmutex)     —— 放其他写者/读者
```

## 三、读者：快路径 + 代理锁（rwbase.c :15-21/:100-140）

```
头注释原话："down_read/read_lock()
  1) Try fast path acquisition (reader BIAS is set)
  2) Take tmutex::wait_lock, which protects the writelocked flag
  3) If !writelocked, acquire it for read
  4) If writelocked, block on tmutex"
```

```c
static __always_inline int rwbase_read_trylock(struct rwbase_rt *rwb)
{
	int r;
	/* 读到负值（BIAS 在位）→ cmpxchg 加一 → 成功 */
	for (r = atomic_read(&rwb->readers); r < 0;) {
		if (likely(atomic_try_cmpxchg_acquire(&rwb->readers, &r, r + 1)))
			return 1;
	}
	return 0;  /* 正值或零 → 写者在 → 走慢路径 */
}

static int __sched __rwbase_read_lock(struct rwbase_rt *rwb, unsigned int state)
{
	/* ① 先拿 rtmutex 的 wait_lock */
	raw_spin_lock_irq(&rtm->wait_lock);
	/* 头注释（:100-118）：完整竞态矩阵——说明为什么必须先持 wait_lock
	   再慢锁 rtmutex——否则 Reader1 会被卡在 Reader2 后面"可能无界" */
	ret = rwbase_rtmutex_slowlock_locked(rtm, state);
	if (!ret)
		atomic_inc(&rwb->readers);  /* ② 成功后递增读者数 */
	raw_spin_unlock_irq(&rtm->wait_lock);
	if (!ret)
		rwbase_rtmutex_unlock(rtm);  /* ③ 立即释放 rtmutex——
		                                  读者不长持 rtmutex！*/
	return ret;
}
```

★**读者不长持 rtmutex**：拿它只为过渡（防止写者插入），增计数后立刻放——**rtmutex 只在写者手里长期持有**。

## 四、★头注释的写者饥饿论证（:24-39 原话）

```
"down_read/read_lock()#3 has the consequence, that rw semaphores
 and rw locks on RT are not writer fair, but writers, which should
 be avoided in RT tasks (think mmap_sem), are subject to the
 rtmutex priority/DL inheritance mechanism.

 It's possible to make the rw primitives writer fair by keeping
 a list of active readers. A blocked writer would force all newly
 incoming readers to block on the rtmutex, but the rtmutex would
 have to be proxy locked for one reader after the other. We can't
 use multi-reader inheritance because there is no way to support
 that with SCHED_DEADLINE.

 Implementing the one by one reader boosting/handover mechanism
 is a major surgery for a very dubious value.

 The risk of writer starvation is there, but the pathological
 use cases which trigger it are not necessarily the typical RT
 workloads."
```

★**设计取舍**：
- **非写者公平**——新读者可以越过等待中的写者
- 补偿：**写者通过 rtmutex 获 PI 优先级继承**（高优先级写者让读者加速）
- 放弃 proxy-locking 的理由：SCHED_DEADLINE 无法多读者继承 + "major surgery for a very dubious value"

## 五、与 rwbase_rt 的三重包含消费链

| 文件 | #define | 生成 |
| :--- | :--- | :--- |
| `kernel/locking/rwsem.c` | 间接（`struct rw_semaphore` 内嵌 `rwbase_rt`） | rwsem 的 RT 实现 |
| `kernel/locking/spinlock_rt.c` | RT_MUTEX_BUILD_SPINLOCKS + include rwbase | RT rwlock |

## 六、观测实战

```bash
# 1) RT 下 rwsem 的行为（读者可越过等待写者）
#   与非 RT 的 handoff 对照
grep PREEMPT_RT /boot/config-$(uname -r)

# 2) 读写争用观测
perf record -e lock:rwsem_* -a sleep 5  # tracepoint（如启用）

# 3) mmap_sem 的争用分析
cat /proc/lockdep_stats | grep rwsem
```

## 七、思考题

1. `READER_BIAS = 2^31`——为什么选负数区（有符号 int 的最高位）？（提示：负数→cmpxchg 只需比较 `< 0` 即知 BIAS 在位；用无符号需要位掩码。）
2. 读者"不长持 rtmutex"——如果读者长持会怎样？（提示：rtmutex 的 PI 链会被读者阻塞——写者的优先级继承失效。）
3. "not writer fair"的补偿是 rtmutex 的 PI——如果写者优先级**不高于**读者呢？（提示：PI 只在高→低传播时生效；同优先级下读者可无限越写者。）
4. `__rwbase_read_lock` 的头注释竞态矩阵（:100-118）——Reader1 如果不先持 wait_lock 会怎样？（提示：注释原话 "That would put Reader1 behind the writer waiting on Reader2 to call up_read(), which might be **unbound**."——可能无界等待。）
5. `rw_base_is_contended` 判断 `readers > 0`——为什么不用 `!= READER_BIAS`？（提示：读者存在时 readers = BIAS + N < 0——不等于 BIAS 但非争用；>0 才意味着写者存在或有活跃读者。）

---

> 回看：[preempt-rt-matrix](preempt-rt-matrix.md)（RT 替换矩阵——rwbase_rt 在其中的位置）｜[ww-rt-mutex](ww-rt-mutex.md)（另一多重包含消费端）｜[concession](concession.md)（rwsem handoff 的让步机制——RT 下如何变化）｜[locking/README](README.md)（锁族谱）
