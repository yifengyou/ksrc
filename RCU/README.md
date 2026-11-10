# Introduction — RCU 分册

RCU（Read-Copy Update）是 Linux 内核中最核心、最"内核味"的并发原语：**读侧近乎零开销、写侧延迟回收**。VFS、网络路由表、dentry cache、fdtable、cgroup、BPF……内核的绝大多数高热读路径都构建在 RCU 之上。掌握 RCU 是进入内核并发专家领域的门票。

## 分册目录

* [基本概念](docs/基本概念.md) — 从"会 API"到"懂 cache 一致性与 grace period"的分层学习路径
* [rcu 总览](rcu.md) — RCU 家族 API 与机制地图
* [tree RCU](tree_rcu.md) — SMP 主力实现：分层 grace period、callback pipeline、rcu_node 树
* [tiny RCU](tiny_rcu.md) — 单核/嵌入式极简实现
* [preempt RCU](preempt_rcu.md) — 抢占内核下的读临界区追踪
* [SRCU](srcu.md) — 可睡眠读侧的 RCU
* [tasks RCU](tasks_rcu.md) — 以任务为粒度的宽限期

## 快速心智模型

```
写者：                         读者：
publish 新版本指针              rcu_read_lock()  （几乎无开销）
synchronize_rcu()             旧指针读到就先用
 （等所有旧读者退出）           rcu_read_unlock()
回收旧版本内存                 —— 读侧永不阻塞、无写侧争用
```

宽限期（grace period）的实现本质是"旧读者退出的证明"：quiescent state（静止态）观测 → rcu_node 树聚合 → gp kthread 推进；回调经 `call_rcu()` 进入 per-CPU pipeline（nocb 线程化卸载）。

## 相关分册

* [内核同步 locking](../locking/SUMMARY.md) — seqlock/mutex/rwsem 与 RCU 的选型互补
* [调度器](../scheduler/README.md) — context_switch 与 quiescent state、RCU 与调度器的协同
* 官方镜像：`../RCU/` 下另有 `Documentation/RCU` 系列文档与 litmus-tests 目录
