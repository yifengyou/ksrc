# Introduction — 内核同步分册

本分册梳理 Linux 6.6 的核心内核同步原语的自研笔记（`*.md`），并索引本目录下的内核官方文档镜像（`*.rst`）。

## 自研笔记

| 文档 | 主题 | 亮点 |
| :--- | :--- | :--- |
| [spinlocks.md](spinlocks.md) | spinlock 四代演进与 qspinlock | 4 字节位编码、pending/MCS 双路径、PV qspinlock、PREEMPT_RT 语义 |
| [seqlock.md](seqlock.md) | 乐观读锁与 seqcount 家族 | 关联锁变体、内存序、latch 双缓冲（NMI 安全时钟） |
| [mutex.md](mutex.md) | 互斥体 | owner 标志位复用、osq 乐观自旋、handoff 防偷锁、ww_mutex、rt_mutex |
| [rwsem.md](rwsem.md) | 读写信号量 | count 位编码、读者批量唤醒、写者 handoff、RT 差异 |
| [concession.md](concession.md) | 排序让步家族 | 三形态：新来者让老等待者（mutex 两级纪律+handoff 位反偷锁）/读者让写者（seqlock 乐观重试——"no writer starvation" 注释）/首战者让同伴（kmod_dup 的 "let duplicates suffer" 一跳）；★rwsem 的 4ms 超时触发（先纵后擒）；公平与吞吐的兑换率 |
| [percpu-refcount.md](percpu-refcount.md) | percpu 引用计数 | 热路径本地 inc/dec 零原子、★kill 的模切换协议（PERCPU_COUNT_BIAS=2^63 防归零偏置+RCU 宽限期收拢+confirm_switch 回调）、resurrect/reinit 的 smp_store_release 反向切换、blk-mq q_usage_counter freeze/unfreeze 与 percpu-rwsem read_count/block 双消费端对照 |
| [debug-adversarial.md](debug-adversarial.md) | 上刑测试特性 | 五件刑具：随机延迟（DEBUG_KOBJECT_RELEASE 的 1~4s）、概率失败注入（fault_attr 与 should_fail 判定链）、链表不变式（DEBUG_LIST 的交叉检查）、上下文断言（might_sleep）、全局图证明（lockdep）；"确定性放大替代概率性触发"设计哲学与组合拳配置矩阵 |
| [ww-mutex.md](ww-mutex.md) | 事务序让步（第四形态） | Wound-Wait（老者 wound 新者→新者释放全部）与 Wait-Die（新者碰老者→新者自杀）的对称反转；ww_acquire_ctx 的 stamp 分配；__ww_mutex_add_waiter 的 stamp 序入队（"waking up waiters that must kill themselves" 注释原话）；check_kill 判定链；-EDEADLK 回退重做协议与 concession 三形态对照 |
| [rcu-sync.md](rcu-sync.md) | 宽限期合并器 | 五态状态机（GP_IDLE/ENTER/PASSED/EXIT/★REPLAY 的追赶延迟）、gp_count 引用计数式 GP 共享（"closely spaced calls can optimize away the grace-period wait" 注释原话）、首写者的 synchronize_rcu 快路径、★GP_REPLAY 的"上次 GP 白等了"追赶语义、与 percpu_ref 每次 call_rcu 的对照（N 个 GP→1 个 GP） |

## 横向选型速记

```
临界区极短/原子上下文      → spinlock（qspinlock）
读多写少小对象、可重试读    → seqlock / seqcount_latch
可睡眠互斥                 → mutex（短）→ ww_mutex（乱序依赖）
读多写少、临界区可睡眠      → rwsem / percpu_rwsem
读极多、写可延迟回收        → RCU（../RCU/）
优先级敏感互斥（RT/用户态） → rt_mutex / futex-PI
```

## 相关分册

* [RCU](../RCU/README.md) — 读侧无锁与延迟回收
* [scheduler](../scheduler/README.md) — preempt_count、PI 与调度器
* [mm](../mm/README.md) — mmap_lock 的同步地狱
* `atomic_t.txt` / `memory-barriers.txt`（仓库根目录镜像）— 原子与内存屏障的官方文档
