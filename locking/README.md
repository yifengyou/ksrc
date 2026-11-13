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
| [modular-arithmetic.md](modular-arithmetic.md) | 模算术横切 | 五案例：percpu-rwsem 的 modular sum（"求和等价"注释原话——读者进出配对的正确性论证）/percpu_ref 的 BIAS 偏置（2^63 防碰零）/time_after 的带符号差判序（圆环序比较）/seqcount 奇偶语义（模 2 周期编码写入状态）/percpu_counter 的分层近似（全局精确+percpu 缓冲）；共同哲学："让数学代替锁做正确性保证" |
| [sanitizers.md](sanitizers.md) | KASAN/KFENCE/KCSAN | 三模式 KASAN（shadow memory 映射与标签族/hw_tags ARM64 MTE）/KFENCE 的采样 guard page（Kconfig 原话"negligible cost...production"/canary pattern 0xAA^addr&7）/KCSAN 的 watchpoint 竞态检测（CHECK_ADJACENT=1 + NUM_SLOTS=3）；★插桩 vs 检查的分野与分工矩阵（开销×覆盖×生产可用×误报率） |
| [ww-rt-mutex.md](ww-rt-mutex.md) | PREEMPT_RT 变体 | WW_MUTEX_BASE 条件编译（mutex ↔ rt_mutex 底座替换）、★rtmutex.c 的四重多重包含（rtmutex_api/ww_rt_mutex/spinlock_rt/rwbase_rt——trace/13 惯用法在 locking 的应用）、build_ww_mutex() 编译期分派（死代码消除零开销）、PI 链与 wound/wait 的共存（boosting 加速 wound 者释放）、RT spinlock 家族对照 |
| [dma-resv.md](dma-resv.md) | GPU 预约协议 | ww_mutex 最大消费者：预约 = ww_mutex + fence 列表双层结构、四档 USAGE 偏序（KERNEL<WRITE<READ<BOOKKEEP 的 fence 查询与升格）、-EDEADLK 的 unlock_all+lock_slow+retry 标准模式、reserve_fences 的 "cannot fail" 契约、RCU 迭代器的 restarted 检测、与 ww-mutex 的对照消费链 |
| [seqcount-latch.md](seqcount-latch.md) | 双缓冲乐观读 | ★NMI 安全的 seqlock 变体：模 2 的空间应用（奇偶选两份拷贝之一——读者总有稳定版）、写者四步双写协议（wmb→seq++→改[0]→wmb→seq++→改[1]）、"multiversion concurrency control" 头注释原话、与普通 seqcount 的读者重试 vs 拷贝选择对照、printk/sched_clock 双消费者 |
| [preempt-rt-matrix.md](preempt-rt-matrix.md) | RT 全景 | 完整替换矩阵（spinlock→rt_mutex_base/mutex→rt/rwsem→rwbase_rt/ww_mutex→rt/local_lock 的 preempt_disable→migrate_disable+spin_lock 三变/raw 不变）；spinlock_rt 的 "explicitly disable migration and take rcu_read_lock() across the lock held section" 注释原话；不可替换的 raw 场景清单 |
| [dma-fence.md](dma-fence.md) | 异步完成原语 | 三元组身份（context/timeline/seqno）、三态标志的原子位操作、★cb_list↔timestamp↔rcu 联合体三阶段生命周期（"only valid until the signal bit is set" 注释原话）、signal 的 test_and_set→list_replace→回调遍历、default_wait 的回调自旋等待、死锁约束清单（lockdep 的 fence annotation） |
| [latch-vs-rcu.md](latch-vs-rcu.md) | 决策对比 | 间接 vs 复制（指针替换+GP 回收 vs 双份覆写）、决策表（读者成本/写者成本/内存/NMI/极早 boot/更新频率/内存回收/数据形态 八维）、sched_clock 的 "single 64-byte cache line" 布局论证、printk 的 latched_seq（最小 u64）、RCU 用户的反面论证（dcache/xarray 的大数据量）、总结口诀 |
| [rwbase-rt.md](rwbase-rt.md) | rwsem 的 RT 基座 | READER_BIAS=2^31 的偏置三段语义、写者三步拿锁协议、读者"快路径+代理锁"（不长持 rtmutex）、★头注释的写者饥饿论证（"not writer fair"+"proxy locked for one reader after the other"+"major surgery for a very dubious value"） |
| [kcsan-encoding.md](kcsan-encoding.md) | KCSAN watchpoint 编码 | 单 long 三字段位布局（bit63=写/size 14b/addr 49b）、INVALID=0/CONSUMED=1 哨兵妙用、槽=PFN 取模+防回绕 padding、★假阳性两条件论证与 report 侧 matching_access 复核、watcher/racer 双剧本（diff=old^new 与 cmpxchg 精确消费）、other_infos 按槽配满的双半合流、限流三闸（skip LCG 随机/delay 随机化/报告去重防 krealloc 死锁）、reorder_access 编译器重排模拟 |

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
