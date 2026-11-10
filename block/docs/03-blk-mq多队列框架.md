# blk-mq 多队列框架

> 阅读目标：blk-mq 是 6.6 块层唯一的请求引擎。本章吃透它的四层结构——`request_queue`（整体）→ `blk_mq_ctx`（per-CPU 软队列）→ `blk_mq_hw_ctx`（硬件队列）→ `blk_mq_tags`（tag/请求池），理清 CPU→队列映射、tag 分配与共享、从 `blk_mq_run_hw_queue` 到驱动 `queue_rq` 的完整派发链、直发快路径、freeze/quiesce 同步原语，以及 IRQ+queue+CPU 对齐的性能工程。全部结构/函数核对自 6.6 的 `block/blk-mq.c`、`block/blk-mq.h`（内核内部版）、`block/blk-mq-tag.c`、`block/blk-mq-sched.c`、`block/blk-mq-cpumap.c`。

---

## 一、四层结构总览

```text
 struct request_queue (每盘一个, blk_alloc_queue 创建)
 ┌─────────────────────────────────────────────────────────────────┐
 │ queue_ctx: per-CPU blk_mq_ctx        hctx_table: xarray          │
 │ elevator | limits | tag_set ──▶ blk_mq_tag_set (驱动注册的拓扑)  │
 │ q_usage_counter (percpu_ref, freeze 用) | requeue_list/work      │
 └──────────┬──────────────────────────────────────┬───────────────┘
            │ per-CPU                              │ 按 index
            ▼                                      ▼
 struct blk_mq_ctx (软队列, 无锁快路径)     struct blk_mq_hw_ctx (硬队列)
 ┌──────────────────────────┐             ┌──────────────────────────────┐
 │ lock + rq_lists[3]       │   ctx_map    │ lock + dispatch list         │
 │  (按 hctx 类型分 3 条:    │◀──sbitmap───│ run_work (kblockd 延迟执行)  │
 │   DEFAULT/READ/POLL)     │   位图:哪个  │ cpumask (本 hctx 服务哪些CPU)│
 │ cpu, index_hw[3]         │   ctx 有货  │ tags / sched_tags (双 tag 池) │
 │ hctxs[3] (直接反查)      │             │ dispatch_busy (EWMA)         │
 └──────────────────────────┘             │ state: STOPPED/TAG_ACTIVE/   │
            提交 CPU 专属                   │        SCHED_RESTART/INACTIVE│
            (blk_mq_get_ctx)               └──────────────┬───────────────┘
                                                          │ queue_rq
 struct blk_mq_tags (每 hctx 一对)                         ▼
 ┌────────────────────────────┐              驱动硬件环 (NVMe SQ/CQ,
 │ bitmap_tags (sbitmap_queue)│               virtqueue, SCSI LLD)
 │ breserved_tags (保留 tag)  │
 │ rqs[tag]→struct request    │  ← tag 与 request 一一对应:
 │ static_rqs[] 静态池        │    拿到 tag 就拿到预分配的 request
 └────────────────────────────┘
```

### 1.1 struct request（include/linux/blk-mq.h，节选）

```c
struct request {
    struct request_queue *q;
    struct blk_mq_ctx     *mq_ctx;    /* 提交时的软队列(定完成 CPU) */
    struct blk_mq_hw_ctx  *mq_hctx;   /* 所属硬件队列 */
    blk_opf_t cmd_flags;              /* bi_opf 的副本(继承 bio) */
    req_flags_t rq_flags;             /* RQF_* 内部状态 */
    int tag;                          /* 驱动 tag: hctx->tags 里的下标 */
    int internal_tag;                 /* 调度器 tag: sched_tags 的下标 */
    unsigned int __data_len;          /* 剩余字节数(blk_rq_bytes) */
    sector_t __sector;                /* 当前扇区游标(blk_rq_pos) */
    struct bio *bio, *biotail;        /* 挂载的 bio 链(合并的本质) */
    union { struct list_head queuelist;   /* 挂队列用 */
            struct request *rq_next; };   /* plug/批量 rqlist 用(单链) */
    struct block_device *part;        /* 统计归属的分区 */
    u64 start_time_ns, io_start_time_ns;  /* 排队/下发时刻(延迟分解) */
    unsigned short nr_phys_segments;  /* 物理段数(建 sg 用) */
    unsigned short ioprio;
    enum mq_rq_state state;           /* IDLE→IN_FLIGHT→COMPLETE */
    atomic_t ref;                     /* request 引用(passthrough 用) */
    unsigned long deadline;           /* 超时到期 jiffies */
    union { struct hlist_node hash;   /* 调度器合并哈希 */
            struct llist_node ipi_list; }; /* 完成路径 IPI 软中断链 */
    union { struct rb_node rb_node;   /* 调度器排序树 */
            struct bio_vec special_vec; };
    struct { struct io_cq *icq; void *priv[2]; } elv;  /* 调度器私有 */
    struct { unsigned int seq; rq_end_io_fn *saved_end_io; } flush;
    rq_end_io_fn *end_io;             /* 完成回调(passthrough/flush 换装) */
    void *end_io_data;
};
```

要点：**tag 就是 request**。`blk_mq_tag_to_rq(tags, tag)` 直接索引出预分配的 `struct request`（驱动命令 PDU 紧跟其后，`blk_mq_rq_to_pdu`）——这就是 NVMe completion 里 `command_id` 反查回 request 的原理。

### 1.2 struct blk_mq_ctx / blk_mq_hw_ctx（block/blk-mq.h 内部头）

```c
struct blk_mq_ctx {                       /* per-CPU, ____cacheline_aligned */
    struct {
        spinlock_t        lock;
        struct list_head  rq_lists[HCTX_MAX_TYPES];  /* 6.6: 按 hctx 类型
                                                        3 条链, 不按 ioprio! */
    } ____cacheline_aligned_in_smp;
    unsigned int        cpu;
    unsigned short      index_hw[HCTX_MAX_TYPES];     /* 在各 hctx 里的序号 */
    struct blk_mq_hw_ctx *hctxs[HCTX_MAX_TYPES];      /* 直接反查表 */
    struct request_queue *queue;
} ____cacheline_aligned_in_smp;

struct blk_mq_hw_ctx {                    /* 每个硬件队列一个 */
    struct {
        spinlock_t      lock;             /* 只保护 dispatch list */
        struct list_head dispatch;        /* "就绪但发不出去"的暂存 */
        unsigned long   state;            /* BLK_MQ_S_* */
    } ____cacheline_aligned_in_smp;
    struct delayed_work run_work;         /* kblockd 上延迟跑队列 */
    cpumask_var_t      cpumask;           /* 本 hctx 的 CPU 集 */
    int next_cpu, next_cpu_batch;         /* run_work 的轮转目标 */
    struct sbitmap     ctx_map;           /* 位图: 哪些 ctx 有请求 */
    struct blk_mq_ctx  *dispatch_from;    /* 无调度器时的轮转游标 */
    unsigned int       dispatch_busy;     /* EWMA 忙闲度(0~1<<4) */
    unsigned short     type;              /* HCTX_TYPE_DEFAULT/READ/POLL */
    unsigned short     nr_ctx;
    struct blk_mq_ctx  **ctxs;            /* 服务哪些软队列 */
    wait_queue_entry_t dispatch_wait;     /* 等 tag 的挂入点 */
    struct blk_mq_tags *tags;             /* 驱动 tag 池 */
    struct blk_mq_tags *sched_tags;       /* 调度器 tag 池(若启用) */
    atomic_t           nr_active;         /* 共享 tagset 时的活跃数 */
    unsigned int       numa_node, queue_num;
    void *driver_data;                    /* 驱动私有(nvmeq 等) */
    ...
};
```

> 📌 **6.6 变化**：老版本的 `ctx->rq_lists[]` 按 IOPRIO 级别分链（per-prio）。6.6 已删掉 per-prio 软队列，改为**按 hctx 类型**（DEFAULT/READ/POLL）分三条——ioprio 语义下沉到各调度器内部（如 mq-deadline 的 per-prio 数据）。读旧博客时注意甄别。

---

## 二、CPU → 硬件队列映射

### 2.1 三张映射表

`struct blk_mq_tag_set` 里最多有三张 `blk_mq_queue_map`（`map[HCTX_MAX_TYPES]`, `nr_maps` 由驱动决定，1~3）：

| hctx 类型 | 用途 | 谁选它 |
| :--- | :--- | :--- |
| `HCTX_TYPE_DEFAULT` | 一切非读非轮询请求 | `blk_mq_get_hctx_type()` |
| `HCTX_TYPE_READ` | 读请求（可配 `QUEUE_FLAG_READ` 独立队列） | `opf` 无 POLL 且 op==READ |
| `HCTX_TYPE_POLL` | `REQ_POLLED`（io_uring HIPRI） | `opf & REQ_POLLED` |

```c
static inline struct blk_mq_hw_ctx *blk_mq_map_queue(struct request_queue *q,
                blk_opf_t opf, struct blk_mq_ctx *ctx)
{
    return ctx->hctxs[blk_mq_get_hctx_type(opf)];   /* 提交热路径: 一次数组索引 */
}
```

### 2.2 建表算法

```c
/* 通用: 不看 IRQ 亲和, 把 CPU 均匀分组 */
void blk_mq_map_queues(struct blk_mq_queue_map *qmap)
{
    masks = group_cpus_evenly(qmap->nr_queues);   /* kernel/core 的均衡分组 */
    for (queue = 0; queue < nr_queues; queue++)
        for_each_cpu(cpu, &masks[queue])
            qmap->mq_map[cpu] = qmap->queue_offset + queue;
}

/* PCI 驱动(nvme-pci): 跟着中断向量亲和走 */
void blk_mq_pci_map_queues(struct blk_mq_queue_map *qmap, struct pci_dev *pdev, int offset)
{
    for (queue = 0; queue < qmap->nr_queues; queue++) {
        mask = pci_irq_get_affinity(pdev, queue + offset);  /* MSI-X 向量的 CPU 亲和 */
        for_each_cpu(cpu, mask)
            qmap->mq_map[cpu] = qmap->queue_offset + queue;
    }
}
```

* **NVMe**（nvme-pci.c）：`set->map[HCTX_TYPE_DEFAULT]` 用 `blk_mq_pci_map_queues`——**队列与 MSI-X 中断一一绑定**，IRQ 亲和表就是队列映射表。POLL 队列用通用 `blk_mq_map_queues`（无中断，无亲和可言）。
* **virtio-blk**：常规队列 `blk_mq_virtio_map_queues`（virtio 设备的 per-vq irq affinity），POLL 队列同上走通用分组。
* **hctx→NUMA**：`blk_mq_hw_queue_to_node()` 建表时反查，`hctx->numa_node` 决定 tags/请求池的分配节点。

### 2.3 共享 tag set 的公平配额：nr_active 与 active_queues

多个 queue 共用一个 tag set（`BLK_MQ_F_TAG_HCTX_SHARED`，如 SCSI 多 LUN、nvme 分 ns 共享 tagset）时，谁活跃谁分粮：

```c
/* hctx_may_queue() (blk-mq.h):
   depth = max((bt->sb.depth + users - 1) / users, 4U);
   return __blk_mq_active_requests(hctx) < depth;   */
```

`__blk_mq_tag_busy()/__blk_mq_tag_idle()`：hctx 首次拿 tag 时置 `BLK_MQ_S_TAG_ACTIVE` 并 `tags->active_queues++`（伴随 `sbitmap_queue_recalculate_wake_batch` 调整批量唤醒阈值）；长期无货（timeout 扫描发现全空闲）时 idle。单用户独占全 depth，N 用户各分 `depth/N`（保底 4）——`hctx->nr_active`（或共享时的 `q->nr_active_requests_shared_tags`）就是活跃请求计数。

---

## 三、request 分配与 tag

### 3.1 分配主流程：__blk_mq_alloc_requests

```text
__blk_mq_alloc_requests(data)                    [blk-mq.c]
 ├─ q->elevator 存在?
 │    data->rq_flags |= RQF_SCHED_TAGS           /* 一律用调度器 tag */
 │    且非 flush/passthrough → RQF_USE_SCHED
 │        → e->ops.limit_depth(cmd_flags, data)  /* 调度器改 shallow_depth */
 ├─ retry:
 │    data->ctx  = blk_mq_get_ctx(q)             /* 本 CPU 软队列 */
 │    data->hctx = blk_mq_map_queue(q, opf, ctx) /* 按 op 类型选硬队列 */
 │    !RQF_SCHED_TAGS → blk_mq_tag_busy(hctx)    /* 共享时计活跃用户 */
 ├─ data->nr_tags > 1?
 │    __blk_mq_alloc_requests_batch:             /* 批量! plug cached_rq 的来源 */
 │       blk_mq_get_tags() → __sbitmap_queue_get_batch()
 │       一把拿连续 N 个 tag, 逐个 blk_mq_rq_ctx_init, 串成 cached_rq 单链
 ├─ 单个: blk_mq_get_tag(data)
 │    ├─ __blk_mq_get_tag: hctx_may_queue 检查 → sbitmap_queue_get_shallow
 │    │   (shallow_depth = 调度器限制的位深, 如 dd 的 async_depth)
 │    └─ 拿不到且允许睡眠:
 │        循环 { blk_mq_run_hw_queue(hctx, false);  ← 先踢队列,可能释放 tag
 │               再取; 取不到 → io_schedule() 挂 sbq wait queue;
 │               醒来后重新选 ctx/hctx (CPU 可能已迁移!) }
 ├─ blk_mq_rq_ctx_init: 从 tags->static_rqs[tag] 取出预分配 request,
 │    填 q/ctx/hctx/cmd_flags; RQF_SCHED_TAGS? tag=NO_TAG, internal_tag=tag
 │    : tag=tag, internal_tag=NO_TAG
 │    RQF_USE_SCHED → e->ops.prepare_request(rq)
 └─ blk_mq_rq_time_init: start_time_ns / alloc_time_ns(REQ_RQ_ALLOC_TIME)
```

### 3.2 双 tag 池：sched_tags vs tags

| | `hctx->sched_tags` | `hctx->tags` |
| :--- | :--- | :--- |
| 何时拿 | 请求**创建**时（挂调度器队列期间持有） | 请求**派发给驱动**时（`blk_mq_get_driver_tag`） |
| 深度 | `q->nr_requests`（默认 `2*min(queue_depth,128)`） | `tag_set->queue_depth`（= 硬件环深度） |
| 关联 | `rq->internal_tag` | `rq->tag` |
| 无调度器 | 不用 | 创建时直接拿 |

派发时 `__blk_mq_alloc_driver_tag()` 从 `tags->bitmap_tags` 取驱动 tag（保留请求走 `breserved_tags`）。**调度器里的请求不占驱动资源**——这是"设备浅深度时把请求留在调度器里继续合并"的基础（`__blk_mq_sched_dispatch_requests` 的注释）。

### 3.3 plug 里的请求缓存（cached_rq）

`blk_mq_get_new_requests()`（第 4 章的提交路径）发现 `plug` 存在时：`data.nr_tags = plug->nr_ios`（`blk_start_plug_nr_ios` 时预算，≤32）→ 批量分配的请求**留在 `plug->cached_rq` 单链上**。后续 bio 直接 `blk_mq_get_cached_request()` 取现成的（校验同队列、同 hctx 类型、同 flush 属性）——把 N 次 sbitmap 操作压缩成 1 次批量。

---

## 四、派发全链：run → dispatch → queue_rq

### 4.1 入口：blk_mq_run_hw_queue

```c
void blk_mq_run_hw_queue(struct blk_mq_hw_ctx *hctx, bool async)
{
    WARN_ON_ONCE(!async && in_interrupt());          /* 硬中断里不许同步跑 */
    /* RCU/SRCU 下检查: 队列没 quiesce 且确有货 */
    need_run = !blk_queue_quiesced(hctx->queue) && blk_mq_hctx_has_pending(hctx);
    if (!need_run) return;
    if (async || !cpumask_test_cpu(raw_smp_processor_id(), hctx->cpumask)) {
        blk_mq_delay_run_hw_queue(hctx, 0);          /* 异步/跨 CPU: 挂 kblockd */
        return;
    }
    blk_mq_run_dispatch_ops(hctx->queue,             /* rcu/srcu 临界区 */
                            blk_mq_sched_dispatch_requests(hctx));
}
```

`blk_mq_hctx_has_pending`：`ctx_map` 有位 / `hctx->dispatch` 非空 / 调度器 `has_work(hctx)`。注意 6.6 **没有** `__blk_mq_run_hw_queue`（旧名），检查逻辑已内联进本函数。

### 4.2 核心调度：__blk_mq_sched_dispatch_requests

```text
blk_mq_sched_dispatch_requests(hctx)
 └─ __blk_mq_sched_dispatch_requests(hctx)      [blk-mq-sched.c]
     ├─ ①先收割 hctx->dispatch 上的残留(上次发不出去的) —— 公平优先
     │    dispatch_list 非空? → mark SCHED_RESTART; blk_mq_dispatch_rq_list()
     │    没发完就 return 0 (货都没消化完, 别问调度器要新货)
     ├─ ② q->elevator?
     │    → blk_mq_do_dispatch_sched(hctx):
     │        外循环(≤1s / need_resched):
     │          内循环(≤max_dispatch, hctx->dispatch_busy 时 =1):
     │            e->has_work? 无 → 停
     │            hctx->dispatch 残留 → 停(记 -EAGAIN 重跑, 防饿死 flush)
     │            get_budget(仅 SCSI 实现) → e->dispatch_request(hctx) 拿请求
     │            拿不到 → 释放 budget + 延迟 3ms 重跑(BLK_MQ_BUDGET_DELAY)
     │            blk_mq_get_driver_tag(rq) 失败 → 停(继续留调度器里!)
     │            多 hctx 混出 → list_sort 按 hctx 分组逐组 dispatch
     ├─ ③ 无调度器且 hctx->dispatch_busy(很忙)?
     │    → blk_mq_do_dispatch_ctx: 从 dispatch_from 游标轮转各 ctx
     │      每次一请求, 公平原语 blk_mq_next_ctx
     └─ ④ 空闲快路径: blk_mq_flush_busy_ctxs(hctx, &rq_list)
          (sbitmap_for_each_set 逐 ctx splice 出全部请求)
          → blk_mq_dispatch_rq_list(hctx, &rq_list, 0)
```

### 4.3 批量执行器：blk_mq_dispatch_rq_list

这是把一列请求真正交给驱动的"发牌机"，batching 语义浓缩在此：

```text
blk_mq_dispatch_rq_list(hctx, list, nr_budgets)      [blk-mq.c, 2010-]
 queued = 0
 do {
     rq = list_first_entry(list)
     ① prep = blk_mq_prep_dispatch_rq(rq, !nr_budgets)
        ├─ 需 budget 时: get_budget() 失败 → 释放已拿 tag, PREP_NO_BUDGET, break
        └─ blk_mq_get_driver_tag() 失败:
             blk_mq_mark_tag_wait(hctx, rq)   /* 挂 tags 的 wait queue,
                                                  tag 释放时 dispatch_wake
                                                  → run_hw_queue; 挂不上则
                                                  直接重试 */
             失败 → PREP_NO_TAG, break
     ② bd.last = list_empty(list)   /* "这是本批最后一个" —— 驱动据此
                                         决定是否立即写 doorbell(延迟提交) */
     ③ ret = q->mq_ops->queue_rq(hctx, &bd)
        BLK_STS_OK            → queued++
        BLK_STS_RESOURCE / BLK_STS_DEV_RESOURCE
                              → 请求放回 list, goto out (设备缺资源)
        BLK_STS_ZONE_RESOURCE → 移 zone_list, 继续找别的 zone 的请求
        其他                  → blk_mq_end_request(rq, ret) (直接失败)
 } while (!list_empty(list));
 out:
 没发干净 || ret != OK → blk_mq_commit_rqs(hctx, queued)  /* 告诉驱动
                              "刚才说 last 是骗你的, 把没 doorbell 的补发" */
 list 剩余 → 挂回 hctx->dispatch (下次 run 优先消化)
 smp_mb();  /* 与 blk_mq_sched_restart 的屏障配对: 防 run 侧漏看新挂入的请求 */
 needs_restart = !SCHED_RESTART 被清掉 || (no_tag 且已不在 tag wait queue)
   → blk_mq_run_hw_queue(hctx, true)     /* 立刻再跑 */
   else if needs_resource → blk_mq_delay_run_hw_queue(hctx, 3 /*ms*/)
 blk_mq_update_dispatch_busy(hctx, !list_empty)   /* EWMA 平滑 */
```

**EWMA 忙闲度**：`dispatch_busy` 以 `新值 = 旧值*7/8 (+1<<4 if busy)/8` 收敛（`BLK_MQ_DISPATCH_BUSY_EWMA_WEIGHT 8 / FACTOR 4`）。它反过来影响第 4.2 步的选择（busy 时 ctx 轮转每次只取一个）和提交侧直发判断（第 4.4 节）。

### 4.4 直发快路径：从 submit 到 queue_rq 不经过任何队列

```text
blk_mq_submit_bio() 尾部 (plug 不存在时):
  ├─ RQF_USE_SCHED 或 (hctx->dispatch_busy && (nr_hw_queues==1 || !is_sync))
  │     → blk_mq_insert_request() + run (常规路径)
  └─ 否则 blk_mq_try_issue_directly(hctx, rq):
        stopped/quiesced?  → 走常规
        拿 budget+tag 失败 → 走常规
        __blk_mq_issue_directly → q->mq_ops->queue_rq(hctx, {rq, last=true})
        RESOURCE            → blk_mq_request_bypass_insert(rq) + run
        其他错误            → blk_mq_end_request
```

同族还有三个直发变体（都在 blk-mq.c）：

* `blk_mq_request_issue_directly(rq, last)`：blk_insert_cloned_request（dm request-based 下发克隆）与 plug 直发用；
* `blk_mq_plug_issue_direct(plug)`：plug 冲刷时**无调度器/单队列**的整链直发（逐请求 issue，换 hctx 时 commit_rqs）；
* `blk_mq_try_issue_list_directly(hctx, list)`：ctx 入队前先试直发整列（`blk_mq_insert_requests` 开头的优化——"hw queue 不忙时省一次进出软队列"）。

而 **`blk_mq_request_bypass_insert(rq)`** 把请求直接挂 `hctx->dispatch`（可 AT_HEAD）——passthrough 与 `REQ_OP_FLUSH` 专用（flush 必须插队头，见 blk-mq.c 中大段注释：NCQ 场景省 10% blktests block/004 时间）。它不 kick 队列，由调用方决定何时 run。

### 4.5 整批下发：queue_rqs（6.6 新形态）

`blk_mq_flush_plug_list()`（第 4 章）在"单队列 + 无调度器 + 非调度点"时优先把**整条 plug 链**原样递给 `q->mq_ops->queue_rqs(&plug->mq_list)`——virtio-blk 实现了该回调（`virtio_queue_rqs`），一次锁、一批描述符下发；没实现/发不完的退回逐请求路径。

---

## 五、freeze 与 quiesce：换引擎前先熄火

任何"改队列结构"的操作（换调度器、改 nr_hw_queues、改 queue_depth、删除设备）都要先保证**没有 request 在飞行**。两档火力：

| | `blk_mq_freeze_queue()` | `blk_mq_quiesce_queue()` |
| :--- | :--- | :--- |
| 阻断对象 | **提交**（`blk_queue_enter` 失败/等待） | **派发**（`QUEUE_FLAG_QUIESCED`，run 被拒） |
| 实现核心 | `percpu_ref_kill(&q->q_usage_counter)` | 置 flag + `synchronize_rcu()`（BLOCKING 用 srcu） |
| 深度计数 | `q->mq_freeze_depth`（嵌套） | `q->quiesce_depth` |
| 等谁 | `percpu_ref_is_zero`：所有在途 request 完成（`__blk_mq_free_request` 里 `blk_queue_exit`） | 所有正在执行的 `queue_rq` 调用返回（RCU 宽限期） |
| 完成回调 | 仍会执行（已提交的不拦） | 仍会执行 |
| 解除 | `blk_mq_unfreeze_queue`：`percpu_ref_resurrect` + wake | `blk_mq_unquiesce_queue`：清 flag + `run_hw_queues(async)` |

```text
freeze 的同步闭环:
  blk_freeze_queue_start(): mq_freeze_depth++==1 → percpu_ref_kill
                            + blk_mq_run_hw_queues(把在途的赶紧赶完)
  提交侧 blk_queue_enter(): percpu_ref_tryget_live_rcu 失败
                            → wait_event(mq_freeze_wq, !depth || dying)
  request 完成 __blk_mq_free_request(): blk_queue_exit() → ref--
  ref 归零 → blk_queue_usage_counter_release → wake_up_all
```

细节：`percpu_ref_init(..., PERCPU_REF_INIT_ATOMIC)`——队列创建时以原子模式起步（未注册前快速），`blk_register_queue` 后转 per-cpu。timeout 工作队列特意用 `percpu_ref_tryget` 而非 `blk_queue_enter`，避免 freeze 等待与超时处理互锁死锁（blk-mq.c 注释）。

---

## 六、run 的触发源与 kblockd

**谁会调 `blk_mq_run_hw_queue`/`blk_mq_delay_run_hw_queue`？**

| 触发点 | 场景 | async? |
| :--- | :--- | :--- |
| `blk_mq_submit_bio` 尾部 | 无 plug、走常规路径时 | 视情况 |
| `blk_mq_flush_plug_list` | plug 冲刷入队后 | `from_schedule` 决定 |
| `blk_mq_sched_restart` | request 完成且 SCHED_RESTART 在 | true |
| `blk_mq_dispatch_wake` | tag wait queue 被唤醒 | true |
| `blk_mq_dispatch_rq_list` | 发不干净（无 restart） | true / 3ms 延迟 |
| `blk_mq_kick_requeue_list` | requeue_work 消化 requeue_list 后 | false |
| `blk_mq_timeout_work` | 超时后确认无货 → tag idle | — |
| `blk_mq_unquiesce_queue` | 解除静默补跑 | true |
| 驱动显式 | `blk_mq_start_stopped_hw_queue` 等 | — |

**kblockd**（blk-core.c `blk_dev_init`）：`alloc_workqueue("kblockd", WQ_MEM_RECLAIM | WQ_HIGHPRI, 0)`。承运三样东西：

1. `hctx->run_work`：`blk_mq_delay_run_hw_queue()` 的延迟执行（`kblockd_mod_delayed_work_on`）。CPU 选择用 `blk_mq_hctx_next_cpu()`——**限制在本 hctx 的 `cpumask` 内轮转**，每 `BLK_MQ_CPU_WORK_BATCH=8` 个 work 换一个 CPU，让 kblockd 实例也分布开。
2. `q->requeue_work`：requeue 列表的消化（第 6 章）。
3. `q->timeout_work`：超时扫描（第 6 章）。

`WQ_MEM_RECLAIM` 是硬要求：删除设备/回收内存的路径依赖它推进；`WQ_HIGHPRI` 保证 unplug 延迟可控（blk-core.c 注释："used for unplugging and affects IO latency/throughput"）。

---

## 七、多队列 × NUMA × 中断亲和的性能工程

blk-mq 的理论收益要靠**三层对齐**才能兑现：

```text
        应用线程        提交 ctx        hctx/驱动环        完成中断
CPU#3 ─────────▶ ctx[3] ─────▶ hctx#1 ─────▶ SQ1/CQ1 ─────▶ MSI-X vec#1
  (NUMA0)         (本CPU缓存行)   (IRQ 亲和 = {CPU3..CPU7})
                     ▲  同一 LLC 内闭环: 提交与完成不跨 NUMA 弹跳
NUMA#0 板载 NVMe: hctx->numa_node=0, tags/request 从 node0 分配
```

实践清单：

* **IRQ 亲和**（联系 interrupt 分册）：`/proc/interrupts` 看 nvmeq0-N 的分布；`irqbalance` 关闭或用 `irqbalance.ban_irqs` 固定。队列映射随亲和表重建（`blk_mq_pci_map_queues` 在 `nvme_reset`/亲和变化时重跑）。
* **观测映射**：`/sys/block/nvme0n1/mq/` 下每 hctx 一个目录（`cpu_list`、`nr_active`…）；`/sys/kernel/debug/block/nvme0n1/` 有 hctxs/ctxs/ 的 debugfs 全景（`cpus`、`dispatch_busy`、`tags`）。

```bash
# 拓扑速查: 每个软/硬队列服务的 CPU
cat /sys/kernel/debug/block/nvme0n1/hctx0/cpus
cat /sys/block/nvme0n1/mq/0/cpu_list
grep nvme /proc/interrupts | awk '{print $NF}'   # IRQ 亲和应与上面一致
# 在途与忙闲
cat /sys/block/nvme0n1/inflight
cat /sys/kernel/debug/block/nvme0n1/hctx0/dispatch_busy
```
* **POLL 队列**：`nvme.poll_queues=N`（模块参数）+ io_uring `IORING_SETUP_IOPOLL`——HCTX_TYPE_POLL 队列无中断，完成靠 `->poll` 主动收割（第 6 章 virtio/nvme poll 对比）。
* **nr_hw_queues 缩放**：`nvme.write_queues`（分离读队列）、`blk_mq_update_nr_hw_queues()`（SR-IOV VF 切换队列数，内部 freeze+quiesce+重建映射）。
* **queue_depth**：`/sys/block/*/queue/nr_requests`（调度器深度）与驱动模块参数（NVMe `q_depth` 由 MQES 决定）；`BLK_MQ_MAX_DEPTH=10240` 是硬上限。

---

## 八、观测与调试

* **sysfs**：`/sys/block/*/mq/`（队列拓扑）、`queue/nr_requests`、`queue/scheduler`（第 5 章）。
* **debugfs**：`/sys/kernel/debug/block/<disk>/`：`hctxs/cpu*/{dispatched,dispatch_busy,tags,...}`、`scheduler/`（调度器内部状态，CONFIG_BLK_DEBUG_FS）。null_blk 是演练这些开关的最佳靶子（`null_blk.rst`）。
* **tracepoints**：`block_rq_insert/issue/complete/requeue/merge`、`block_unplug`。BCC 的 `biolatency`/`biosnoop`、`bpftrace` 直接挂。
* **in-flight**：`/sys/block/*/inflight`（读/写两个数）由 `blk_mq_in_flight` 遍历 tag 位图统计——**tag 位图就是"在途表"**（第 6 章统计口径）。
* **假超时演练**：`CONFIG_FAIL_IO_TIMEOUT` + `/sys/block/*/fail_timeout`，验证驱动的 timeout→abort→reset 链路。

## 九、深挖难点

* **为什么 `ctx` 的三条链按 hctx 类型分而不是按 ioprio？** POLL 队列必须物理隔离（poll 不消耗 IRQ 资源但需要独立收割）；READ 队列是可选优化。而 ioprio 属于调度器策略域——mq-deadline 内部自己按 RT/BE/IDLE 建三套数据，blk-mq 核心不再越俎代庖（6.0 前后那次 per-prio 软队列删除正是语义归位）。
* **`bd.last` 与 `commit_rqs` 的协议**：驱动可以攒着 doorbell 等批量。块层保证"说 last 之后不再追加，若追加必先 commit_rqs"。违反协议的驱动会造成尾延迟毛刺——排查方法：tracepoint `block_unplug` 与 NVMe doorbell 计数对账。
* **dispatch_wait 的挂表时机竞态**：`blk_mq_mark_tag_wait` 里"先挂 wait queue 再重试拿 tag，拿到就立刻摘除"——覆盖"挂表瞬间 tag 被释放"的窗口。这个模式和第 2 章网络分册 NAPI 的 SCHED/MISSED cmpxchg 同构：**登记与重查的顺序决定不丢事件**。
* **freeze 为什么用 percpu_ref 而不用计数锁**：提交热路径一次 `percpu_ref_tryget_live_rcu` 是 per-CPU 原子加（无缓存行争抢），而 kill 之后自动退化为全局判断——性能与语义两得。

## 十、思考题

1. `blk_mq_dispatch_rq_list` 在拿不到 driver tag 时**把请求留在调度器/软队列**，而不是塞进 `hctx->dispatch`。反过来做（全部塞 dispatch）会损失什么能力？
    *提示：一旦进入 dispatch list，请求无法再合并/排序；调度器也失去了"浅设备深度下继续攒批"的余地（blk-mq-sched.c 注释原意）。*
2. `blk_mq_complete_send_ipi`（第 6 章）把完成送回 `rq->mq_ctx->cpu`。一个被 `hctx->dispatch` 收留的请求，其 `mq_ctx` 一定是"曾经提交它的 CPU"吗？CPU 热插拔下会发生什么？
    *提示：ctx 结构是 per-CPU 常驻的（`blk_mq_get_ctx` 注释："we don't care about preemption, since we know the ctx's are persistent"）；`blk_mq_hctx_notify_offline/dead` 处理 CPU 消失。*
3. kblockd 的 `run_work` 为什么要在 `hctx->cpumask` 内轮转 CPU 而不是让 workqueue 自由调度？
    *提示：跨 cpumask 运行会破坏"提交-派发-完成"同 LLC 闭环；`blk_mq_hctx_next_cpu` 的 batch=8 兼顾 kblockd 负载均衡。*
4. 共享 tag set 的两个 SCSI LUN，一个跑满、一个空闲。`hctx_may_queue` 如何防止空闲 LUN 的第一批请求被"饿死保护"误伤？
    *提示：idle 状态（BLK_MQ_S_TAG_ACTIVE 清零）直接放行全深度；active 的切换由 timeout_work 的空转检测驱动。*
5. `blk_mq_run_hw_queue(hctx, false)`（同步）为什么禁止在中断上下文调用？`async=true` 时改走 kblockd，这与"完成中断里常有驱动的再入提交"有什么关联？
6. 设计实验：用 null_blk 的 `nr_devices/queue_mode/irqmode` 参数复现"单 hctx 多 CPU 提交"的锁竞争，并通过 `perf lock` / `run.sh` 观察差距。

---

> 下一章：[plug 合并与 IO 路径](04-plug合并与IO路径.md) —— 提交侧的"蓄水池"如何把成千上万个 4KB bio 变成少数大请求。
