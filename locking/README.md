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
