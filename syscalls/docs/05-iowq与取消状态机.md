# io-wq 与取消状态机（Linux 6.6）
> 阅读目标：掌握 io_uring 的**阻塞操作卸载引擎 io-wq**（三层结构、hash 分桶防死锁、双记账、与 worker 生命周期）与 **取消/退出的全量状态机**（`io_uring_try_cancel` 族、`io_uring_cancel_generic` 的 inflight 轮询收尾）。对照 workqueue 分册 cmwq 看"为什么不用现成工作队列"。前置：[03-io_uring](03-io_uring.md)、[workqueue 分册](../../workqueue/README.md)。

---

## 一、为什么需要 io-wq

```
sq→kernel 的三环是"零系统调用"形态——前提是 SQ 里的操作都非阻塞。
但 legacy 块接口（fs read/write、accept…）天然会睡：
  直接跑 → 提交者上下文睡死 = SQ 优势全失
io-wq（io_uring/io-wq.c）：per-task 的私有工作线程池
  ——不是 cmwq 的 wrapper，是另一套专用实现（对照下表）
```

| | cmwq（workqueue 分册） | io-wq |
| :--- | :--- | :--- |
| 全局共享池 | per-task 私有池（`tctx->io_wq`） |
| pwq 无锁入队 | per-wq 锁 |
| worker 调度接调度器 | worker 自旋取活（请求意识驱动） |
| 死锁防线：pwq 本地化 | 死锁防线：**inode hash 分桶**（同桶串行=同锁不并发拿） |

## 二、三层结构（io-wq.c 已核对 6.6）

```
struct io_wq {
	hash（IO_WQ_NR_HASH_BUCKETS=64 桶；64 位阶 IO_WQ_HASH_ORDER=6）
	worker_refs（原子，桶空回收的引用门）
	free_work_fn *free_work ← ★取消时的清理回调（io_uring 侧
	io_wq_work_fn *do_work              提供 io_wq_submit_work）
	acct[IO_WQ_ACCT_NR]（BOUND/UNBOUND 双账本）
	free_list / all_list        cpuhp_node  task
}
struct io_wq_acct { nr_workers max_workers nr_running work_list lock }
	——BOUND/UNBOUND 分账：6.6 两者并存（有亲和候选的操作走 bound 池，
	  其余走 unbound；后续内核删了 bound——记录时点状态）
struct io_worker {
	cur_work / next_work     双槽：执行中的+预备的
	ref nulls_node（哈希桶链）lock create_state
	create_work / create_index    rcu | work 双形态 union
}
```

worker 线程名 `iou-wrk-%d`（`set_task_comm` 已核对）。

## 三、执行链：从 SQ 到 worker（已核对 6.6）

```
io_submit_sq → io_req_prep_async_work（IOSQE_ASYNC 用户指定或
  REQ_F_FORCE_ASYNC 内核判定）→ io_queue_async_work:
    req->work.func = io_wq_submit_work
    io_wq_enqueue(tctx->io_wq, &req->work)   ←（io_uring.c:528）
      挂 hash 桶（inode 哈希）+ acct work_list
      idle 且 nr_running==0 → io_wq_create_worker（限 max_workers）
worker 循环：
  io_wq_worker → 取 cur_work/next_work → io_wq_submit_work:
    __io_complete 已在链（失败）→ 直接收尾
    否则 do_work（调实际 op 处理）→ 完成回调
完成面：结果写 CQE（IORING_CQE_F_MORE 之类流控另计），
  io_req_task_work_add 挂 tctx->task_list（llist）——
  ★fake node 技巧（io_uring.c 1247 段已核对）：完成链里存非本
  任务的引用，批量自增不共享锁；回提交者上下文走 task_work 跑
  尾部（io_handle_local_wake + poll 唤醒）
linked 链：io_issue_sq 提交 link2 时复用 worker
  （io_wq_enqueue(wq, linked) 已核对 607 行）——链内顺序执行
```

**免费回调 `free_work`**：取消/退出时 io-wq 不能直接撤 hash 里的请求（那是提交者的内存），`free_work_fn` 逐个问 io_uring"这个你还要吗"。

## 四、取消：按请求精准反悔（io_uring/cancel.c 已核对）

```
IORING_OP_ASYNC_CANCEL → io_sync_cancel → io_try_cancel(req, fd)：
  io_cancel_req(req)          ← 目标在 hash 桶/sq/wq 里：
                                 标 cancel 语义 + 从桶摘（引用仍由
                                 提交者持有）
  io_async_cancel_one(ring)   ← 潜逃逸面：目标正注册在另一个
                                 ring 的 tctx xarray（poll/zcrx 类）
  失败即 async（IORING_CANCEL_FD 位图决定目标个数）→ 放回
  CQE（IORING_CQE_CANCEL_ALL 之类聚合结果）
时间点差异：已 hash 化的请求取消=标记；在 sq 未提交=直接删除；
  完成瞬间的竞争 → cancel 与 CQE 竞速（后者赢则报无此请求）
```

## 五、退出：io_uring_cancel_generic 全量收尾（io_uring/cancel.c 已核对）

```
任务关闭（io_uring_files_cancel → io_uring_cancel_generic）：
  ① io_wq_exit_start：io-wq 停止收新活（worker 自然饿死）
  ② atomic in_cancel++（防同任务递归取消）
  ③ do { 循环遍历 tctx 注册的所有 ring：摘 io_tctx_node
       inflight=io_uring_poll(cancel_all) 轮询
     } while (inflight 仍在)
     ——等待语义：worker 里的活动完成或被 free_work 抓回
  ④ 取消 sqd（IOPOLL/注册 ring 的 poll 请求面）
exec 退位（io_uring_flush）同理：tctx 的 task_head 由
  fallback_work 接管——任务跑不了 task_work 时的兜底
```

**与 process-mgmt 分册 exit 联动**：`io_uring_files_cancel` 挂 `current->files` 里的注册 ring（`registered_rings`），`io_uring_cancel_generic` 是 `io_uring_exit` 的一环——exit 系统调用驱动的资源善终。

## 六、观测与实战

```
/proc/<pid>/task/*/comm | grep iou    # 池规模观测（64 桶上限 max_workers）
iotop / perf trace io_uring:*      # 调度侧
strace -f -e io_uring_setup,io_uring_enter  # CQE 速率 vs SQ 速率
压测复现：同 inode 并发池深 → 观察桶串行效果（挂锁正交）
取消风暴：大量 POLL_ADD 后批量关闭 fd → cancel 循环轮次
  （inflight 下降曲线）
```

## 七、思考题
1. hash 桶 64 个够不够？（推演：极端单 inode 场景的串行代价；与 cmwq 的 pwq 相比少了什么全局并发面。）
2. `cur_work + next_work` 双槽为什么省一次自旋？（对照单槽的取-放交错。）
3. `free_work` 回调存在的原因？取消方为什么不能直接 free 请求内存？（提示：hash 里挂的是提交者的 work 结构。）
4. `io_wq_exit_start` 之后为什么让 worker "饿死"而不是强杀？（对照 force-finish 的请求语义。）
5. `io_async_cancel_one` 为什么要遍历其它 ring？（提示：请求跨 ring 逃逸面的注册关系。）

---

> 回看：[03-io_uring](03-io_uring.md)｜[01-LSM与安全钩子顺序](04-LSM与安全钩子顺序.md)｜[workqueue 分册](../../workqueue/README.md)
