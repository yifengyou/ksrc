# Summary

* [Introduction](README.md)
* [spinlock 与 qspinlock](spinlocks.md)
* [seqlock](seqlock.md)
* [mutex](mutex.md)
* [rwsem](rwsem.md)
* [让步模式 concession](concession.md)
* [percpu_ref 深挖](percpu-refcount.md)
* [上刑测试特性](debug-adversarial.md)
* [ww_mutex 事务序让步](ww-mutex.md)
* [rcu_sync 宽限期合并器](rcu-sync.md)
* [内核中的模算术](modular-arithmetic.md)
* [static_call](sanitizers.md)
* [ww_rt_mutex RT 变体](ww-rt-mutex.md)
* [dma-resv GPU 预约](dma-resv.md)
* [seqcount_latch 双缓冲](seqcount-latch.md)
* [PREEMPT_RT 替换矩阵](preempt-rt-matrix.md)
* [dma_fence 异步完成](dma-fence.md)
* [latch vs RCU 对比](latch-vs-rcu.md)
* [rwbase_rt RT 基座](rwbase-rt.md)
* [KCSAN encoding](kcsan-encoding.md)
* [lockdep chains：依赖链·BFS·wait-context](lockdep-chains.md)

## 内核官方文档镜像（本目录 .rst）

* [locktypes.rst](locktypes.rst) — 各锁类型的上下文约束总表
* [spinlocks.rst](spinlocks.rst) / [mutex-design.rst](mutex-design.rst) / [rt-mutex-design.rst](rt-mutex-design.rst)
* [ww-mutex-design.rst](ww-mutex-design.rst) — wound/wait 互斥设计
* [pi-futex.rst](pi-futex.rst) / [futex-requeue-pi.rst](futex-requeue-pi.rst) / [robust-futexes.rst](robust-futexes.rst) — futex 与 PI
* [percpu-rw-semaphore.rst](percpu-rw-semaphore.rst) — per-CPU 读写信号量
* [lockdep-design.rst](lockdep-design.rst) / [lockstat.rst](lockstat.rst) — 死锁检测与锁统计
* [seqlock.rst](seqlock.rst) / [hwspinlock.rst](hwspinlock.rst) / [locktorture.rst](locktorture.rst)
