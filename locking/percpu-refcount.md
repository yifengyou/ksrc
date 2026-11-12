# percpu_ref：per-CPU 引用计数的两阶段关闭（Linux 6.6）

> 阅读目标：吃透 percpu_ref 的完整机制——percpu 模式的**本地 inc/dec 零原子开销**、kill 时 **percpu→atomic 的模切换协议**（`PERCPU_COUNT_BIAS` 的防归零偏置 + RCU 同步收拢 percpu 计数 + `confirm_switch` 回调）、resurrect/reinit 的反向切换、以及两个消费端的实战（blk-mq 的 `q_usage_counter` freeze/unfreeze 与 percpu-rwsem 的 `read_count`/`block` 写者屏障）。全部对照 v6.6（include/linux/percpu-refcount.h:1-135、lib/percpu-refcount.c 479 行、kernel/locking/percpu-rwsem.c 282 行、block/blk-mq.c）。前置：[driver-model/07 §5.2](../driver-model/docs/07-资源生命周期三模式.md)（percpu_ref 的简介）｜[concession](concession.md)（排序让步的邻近家族）。

---

## 一、问题：计数热到跨核弹跳怎么办

```
atomic_t 的 get/put 是一次跨核缓存行弹跳（~100ns）：
  高频计数对象（blk-mq 队列引用/读锁）在多核下弹跳成本爆炸

percpu_ref 的答案：
  percpu 模式：get/put 各自操作本 CPU 计数器 → 零原子、零弹跳
  但 percpu 模式下 put 查不到归零 → 生命周期终点需要切换到原子模式

代价：关闭协议从"一次 dec_and_test"变为"模切换 + RCU 宽限期 + 归拢"
```

## 二、数据布局（percpu-refcount.h:59-121）

```c
/* 低位模式标记（:59-66）*/
enum {
	__PERCPU_REF_ATOMIC	= 1LU << 0,   /* 原子模式 */
	__PERCPU_REF_DEAD	= 1LU << 1,   /* （正在）被 kill */
	__PERCPU_REF_ATOMIC_DEAD = __PERCPU_REF_ATOMIC | __PERCPU_REF_DEAD,
	__PERCPU_REF_FLAG_BITS	= 2,
};

struct percpu_ref_data {               /* 慢路径字段——移出快路径 :95-103 */
	atomic_long_t		count;     /* 原子计数值（切换后生效）*/
	percpu_ref_func_t	*release;  /* 归零回调 */
	percpu_ref_func_t	*confirm_switch;
	bool			force_atomic:1;
	bool			allow_reinit:1;
	struct rcu_head		rcu;
	struct percpu_ref	*ref;
};

struct percpu_ref {                    /* :105-121 —— 常嵌入用户结构 */
	unsigned long		percpu_count_ptr;  /* ★低位=模式标记；
	                                            高位=percpu 计数器指针 */
	struct percpu_ref_data	*data;
};
```

★**热路径只有 2 个字段**（percpu_count_ptr + data 指针）——`percpu_ref_data` 的全部慢路径字段外置到堆上（"reduce memory footprint in fast path" 注释原话）。get/put 时读 `percpu_count_ptr`，低位测出模式后走不同分支。

## 三、模切换协议：kill 的三步（lib/percpu-refcount.c）

### 3.1 PERCPU_COUNT_BIAS：防归零偏置（:37）

```c
#define PERCPU_COUNT_BIAS	(1LU << (BITS_PER_LONG - 1))
/* 初始化时（:88-91）：percpu 模式 start_count += BIAS */
/* 切换时（:195）：atomic_long_add((long)count - BIAS, &data->count) */
/* 切回时（:250）：atomic_long_add(BIAS, &data->count) */
```

★为什么需要 BIAS：切换到原子模式时要把各 CPU 计数**求和后**灌入原子值。如果直接从 1 开始，某个 CPU 残留 put 会让**原子值在求和完成前经过 0**——触发 release（但此时还有别的 CPU 的计数没归拢）！BIAS 是一个巨大的正数（2^63），灌入时减掉——保证求和期间的中间值远离零。

### 3.2 RCU 同步收拢（percpu_ref_switch_to_atomic_rcu，:171-204）

```c
static void percpu_ref_switch_to_atomic_rcu(struct rcu_head *rcu)
{
	...
	unsigned long count = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		count += *per_cpu_ptr(percpu_count, cpu);

	/*
	 * :178-194 注释原话（关键算术）："It's crucial that we sum the
	 * percpu counters _before_ adding the sum to &ref->count; since
	 * gets could be happening on one cpu while puts happen on another,
	 * adding a single cpu's count could cause @ref->count to hit 0
	 * before we've got a consistent value - but the sum of all the
	 * counts will be consistent and correct.
	 * Subtracting the bias value then has to happen _after_ adding
	 * count to &ref->count; we need the bias value to prevent
	 * &ref->count from reaching 0 before we add the percpu counts."
	 */
	atomic_long_add((long)count - PERCPU_COUNT_BIAS, &data->count);
	...
	percpu_ref_call_confirm_rcu(rcu);   /* 通知调用者切换完成 */
}
```

### 3.3 整个 kill 流程

```
percpu_ref_kill(ref)
  └─ __percpu_ref_switch_mode(ref, NULL)  /* 切原子模式 */
       └─ __percpu_ref_switch_to_atomic
            ① 置 __PERCPU_REF_ATOMIC 位（此后 get/put 走原子分支）
            ② 设 confirm_switch（noop 兜底）
            ③ call_rcu(&data->rcu, percpu_ref_switch_to_atomic_rcu)
                 ★等一个 RCU 宽限期——保证所有 CPU 都看到了
                   ATOMIC 位（即此后再无 CPU 写 percpu 计数器）
               RCU 回调：
                 ④ for_each_possible_cpu 求和 percpu 计数
                 ⑤ atomic_long_add(sum - BIAS, &data->count)
                 ⑥ percpu_ref_call_confirm_rcu：
                      data->confirm_switch(ref)  —— 调用者回调
                      wake_up_all(&percpu_ref_switch_waitq)
                      percpu_ref_put(ref)  /* 释放切换自身持有的引用 */
```

★**为什么需要 RCU 宽限期**：置 ATOMIC 位后，某 CPU 可能还在执行旧模式的 put（本地 dec）。必须等所有这样的 in-flight 操作退出（它们持有的 RCU 读锁释放），才能安全读 percpu 计数——否则求和值不完整。

### 3.4 切换后恢复：resurrect / reinit（:240-262）

```c
void percpu_ref_resurrect(struct percpu_ref *ref)   /* 死→活 */
{
	...
	atomic_long_add(PERCPU_COUNT_BIAS, &ref->data->count);
	for_each_possible_cpu(cpu)
		*per_cpu_ptr(percpu_count, cpu) = 0;   /* 清零 percpu 计数 */
	smp_store_release(&ref->percpu_count_ptr,
			  ref->percpu_count_ptr & ~__PERCPU_REF_ATOMIC);
	/* ★smp_store_release 与 __ref_is_percpu 的 READ_ONCE 配对——
	   保证"清零 percpu"对"清 ATOMIC 位之后的所有访问"可见 */
}
```

## 四、消费端一：blk-mq 的 q_usage_counter（block/blk-mq.c）

```
初始化（blk_mq_init_queue 系列）：
  percpu_ref_init(&q->q_usage_counter, blk_queue_usage_counter_release,
                  PERCPU_REF_INIT_ATOMIC, GFP_KERNEL)
  ★以原子模式起步——注册前（register 之前）快速安全
  blk_register_queue 后调 percpu_ref_switch_to_percpu 转入 percpu 模式

freeze（blk_mq_freeze_queue，:128-150）：
  percpu_ref_kill(&q->q_usage_counter)   /* 冻结 = kill */
  wait_event(q->mq_freeze_wq, percpu_ref_is_zero(...))
  /* 等所有在途 request 完成（__blk_mq_free_request → blk_queue_exit
     → percpu_ref_put_many :1072） */

unfreeze（blk_mq_unfreeze_queue，:188）：
  percpu_ref_resurrect(&q->q_usage_counter)  /* 复活 */
  + percpu_ref_switch_to_percpu

每次 request 入队（blk_queue_enter → percpu_ref_get_many :430）
每次完成（blk_queue_exit → percpu_ref_put_many :1072）
→ freeze 期间新请求 blk_queue_enter 失败（-EIO）或等待
```

## 五、消费端二：percpu-rwsem（kernel/locking/percpu-rwsem.c）

```
数据布局：
  struct percpu_rw_semaphore {
      __percpu int      *read_count;   /* percpu 读者计数 */
      struct rcu_sync   rss;           /* GP 进度追踪器 */
      struct rcuwait    writer;        /* 写者等待 */
      struct wait_queue_head waiters;
      atomic_t          block;         /* ★写者屏障位 */
  };

读路径（__percpu_down_read :167-183）：
  this_cpu_inc(*sem->read_count);      /* ★本地 inc，零原子 */
  if (unlikely(atomic_read(&sem->block))) {  /* 写者在场？*/
      this_cpu_dec(*sem->read_count);  /* 回退 */
      ... 等待 / 返回 false
  }

写路径（percpu_down_write :237-282）：
  ① atomic_xchg(&sem->block, 1)     /* ★置屏障——新读者见到即回退 */
  ② rcu_sync_enter(&sem->rss)       /* 等 GP——旧读者全部退出 */
  ③ for_each_possible_cpu 求和 read_count == 0 ?  /* 全清？ */
     （readers_active_check :215-230——同 percpu_ref 的求和模式）
```

★与 percpu_ref 的对照：percpu-rwsem 的 `block` 位相当于 percpu_ref 的 `ATOMIC` 位——**写者置屏障**让新读者退场，然后等 GP 收拢旧读者。不同的是 rwsem 切换是**暂态**的（up_write 后清 block 恢复），而 percpu_ref 的 kill 是**单向**的（resurrect 才恢复）。

## 六、两消费端对照

| 维度 | blk-mq q_usage_counter | percpu-rwsem |
| :--- | :--- | :--- |
| 热路径成本 | percpu get/put（本地 inc/dec） | percpu inc/dec + 一次 atomic_read(block) |
| 关闭/屏障语义 | kill（单向到归零） | block 位（暂态——可反复） |
| 切换同步 | RCU 宽限期 + confirm 回调 | rcu_sync（可多写者重用 GP） |
| 归零后 | release 回调（释放队列资源） | N/A（rwsem 不释放） |
| 恢复 | resurrect + switch_to_percpu | clear block（即恢复） |
| 溢出保护 | BIAS 偏置 | 无（读者计数是**模和**，不判零——"modular sum"注释 :204-208） |

## 七、观测实战

```bash
# 1) blk-mq freeze 的现场感知
cat /sys/block/*/mq/0/busy           # freeze 时 IO 停滞
#   冻结等 is_zero——dmesg 里 blk_mq_freeze 的 WQ 等待可通过
#   trace 观测（bpftrace 钩 percpu_ref_is_zero）

# 2) percpu-rwsem 的读者计数（间接）
#   /sys/kernel/debug/... 无直接导出；用 BPF:
#   bpftrace -e 'kprobe:__percpu_down_read /comm=="X"/ { @rd=count(); }'

# 3) BIAS 的算术验证（读源码即可）
#   lib/percpu-refcount.c:195 的 atomic_long_add((long)count - BIAS, ...)
#   → 验证 BIAS = 1<<63：percpu 模式 count≈1 + BIAS - 各 cpu 残留

# 4) freeze/unfreeze 的交替实验（q_usage_counter 的 resurrect 用法）
echo 1 > /sys/block/sda/device/delete   # 触发 del_gendisk → freeze
```

## 八、思考题

1. BIAS 取 2^63 而不是 2^31——为什么必须这么大的数？（提示：percpu 求和是**带符号长整型**的和；中间过程可能为负值（put 残留）——BIAS 必须大于任何可能的负值绝对值，才能保证 `count - BIAS` 不碰零。）
2. RCU 宽限期保证"所有 CPU 看到 ATOMIC 位"——如果改用 `on_each_cpu` 同步等待，成本差多少？为什么选 RCU？（提示：RCU 可重叠其他工作；on_each_cpu 是同步 IPI。）
3. `percpu_ref_resurrect` 里先 `atomic_long_add(BIAS)` 再清 percpu 计数再清 ATOMIC 位——这个顺序能倒吗？（提示：清 ATOMIC 后某 CPU 立即 put 到 percpu 计数器——此时计数器必须已清零且 BIAS 已加。）
4. blk-mq 的 freeze 期间 `percpu_ref_tryget`（timeout 路径用）为什么不会等待？（对照 freeze 的 kill 语义：tryget 在 ATOMIC+非零时才成功——freeze 后 count≤0 即失败。）
5. percpu-rwsem 的 `block` 位语义与 `ATOMIC` 位的异同——为什么 rwsem 不需要 BIAS？（提示：rwsem 的求和是判断"全部退出"而非"计数为零"——readers_active_check 只看 sum==0，不存在"中间值碰零触发 release"的问题。）
6. `percpu_ref_kill` 的头注释说"returns true once and false if someone else already called it"——这个一次性保证由谁实现？（提示：`__PERCPU_REF_DEAD` 位的 test_and_set。）

---

> 回看：[concession](concession.md)（让步家族——rwsem handoff 的邻近机制）｜[driver-model/07-资源生命周期三模式](../driver-model/docs/07-资源生命周期三模式.md)（percpu_ref 的模式定位）｜[block 分册 03 章](../block/docs/03-blk-mq多队列框架.md)（freeze/unfreeze 的块层语境）｜[locking/README](README.md)（锁族谱中的定位）
