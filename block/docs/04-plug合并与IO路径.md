# plug 合并与 IO 路径

> 阅读目标：plug（插头）是提交线程私有的"蓄水池"：把即将下发的 request 暂存在 `current->plug` 里，用一小段延迟换取巨大的合并机会。本章拆解 6.6 的 plug 结构与冲刷链路、四次合并时机及其判据（含 `bio_attempt_back_merge` 的全部边界）、`io_context/icq` 的历史残响、flush 状态机（REQ_PREFLUSH/FUA 的三段序列），并澄清流传甚广的"3ms unplug 定时器"传说。核对自 `block/blk-core.c`、`block/blk-mq.c`、`block/blk-merge.c`、`block/blk-flush.c`、`block/elevator.c`、`include/linux/blkdev.h`。

---

## 一、plug 机制全解

### 1.1 结构与生命周期

6.6 真实定义（include/linux/blkdev.h）——比传闻的小得多：

```c
struct blk_plug {
    struct request *mq_list;     /* 待发 request 的单链(rq_next 串联) */
    struct request *cached_rq;   /* 批量预分配、尚未使用的 request */
    unsigned short nr_ios;       /* 本轮预计提交数(批量分配预算) */
    unsigned short rq_count;     /* mq_list 上已攒数量 */
    bool multiple_queues;        /* 跨多个 queue? (决定冲刷策略) */
    bool has_elevator;           /* 含调度器 tag 的请求? */
    struct list_head cb_list;    /* md 等模块的 unplug 回调 */
};
```

> ⚠️ **两个澄清**：① 6.6 的 plug **没有** `cur_kick` 字段、没有任何时间驱动的自动冲刷——那是 6.8 之后的演进（配合短延迟 kick 的按需 flush）。② "blk 层默认 3ms unplug"是 **blk-mq 之前**单队列时代 `blk_unplug_timeout` 的行为，v3.13 起 mq 路径无定时器。6.6 中 plug 的冲刷只由下面五个事件驱动。

```text
blk_start_plug(plug)  ←—— blk_start_plug_nr_ios(plug, 1)
 ├─ tsk->plug 已存在? 嵌套 plug 直接忽略(只用最外层)
 ├─ mq_list=cached_rq=NULL; nr_ios=min(nr_ios, BLK_MAX_REQUEST_COUNT=32)
 └─ current->plug = plug          /* 挂进 task_struct, 任意深度可见 */

冲刷(blk_mq_flush_plug_list 的调用者):
 ① blk_finish_plug(plug):  提交批次自然结束
 ② io_schedule_prepare():  睡眠前 blk_flush_plug(current->plug, true)
     —— 防死锁: 若 plug 中锁着内存回收在等的脏页, 睡眠=互等
 ③ 攒量阈值: blk_add_rq_to_plug 中
     rq_count >= 32 (multiple_queues 时 64, 即 BLK_MAX_REQUEST_COUNT*2)
     或 链头单请求 >= BLK_PLUG_FLUSH_SIZE(128KB) 且队列允许合并
 ④ bio_poll(): 轮询前必须放干净 (被 poll 的请求不能还压在 plug 里)
 ⑤ __blk_flush_plug 里: cached_rq 残余无条件 blk_mq_free_plug_rqs()
     (防阻塞任务持有队列引用, 卡住 freeze/quiesce)
```

**`cb_list` 的老朋友**：md/raid 用 `blk_check_plugged(unplug_fn, data, size)` 在 plug 上挂一个 `blk_plug_cb` 回调——冲刷时 `flush_plug_callbacks()` 先于请求列表执行（blk-core.c）。这是 md 借"提交方即将睡眠/冲刷"的时机推进自身状态机的钩子，也解释了为什么 plug 属于 task_struct 而非队列：**它描述的是提交者行为，不是设备状态**。

### 1.2 攒入：blk_add_rq_to_plug 与提交主路径

```text
blk_mq_submit_bio(bio)                        [blk-mq.c, 详见第 3 章]
 ├─ plug = blk_mq_plug(bio)   /* 注意: zoned 设备的顺序写不 plug!
 │                               (bdev_op_is_zoned_write → 返回 NULL,
 │                                防多上下文提交顺序重排破坏区写指针) */
 ├─ blk_mq_get_cached_request: 复用 plug->cached_rq 现成 request
 │   (同 q、同 hctx 类型、同 flush 属性才命中; 顺带先试 plug 合并)
 ├─ 未命中 → blk_mq_get_new_requests:
 │     blk_mq_attempt_bio_merge(q, bio, nsegs)   ◀—— 合并第一现场
 │     rq_qos_throttle (wbt/iocost 在此可能冲刷整个 plug!)
 │     __blk_mq_alloc_requests (批量 tag, 塞进 cached_rq)
 ├─ blk_mq_bio_to_request(rq, bio, nsegs): bio 挂上 request, 记账 start
 ├─ op_is_flush → blk_insert_flush(rq) (进入 flush 状态机, §五)
 └─ plug 存在 → blk_add_rq_to_plug(plug, rq); return   ←—— 到此为止, 不下发
```

`blk_add_rq_to_plug` 还维护两个布尔：请求的 `q` 与链上第一个不同 → `multiple_queues=true`；出现 `RQF_SCHED_TAGS` → `has_elevator=true`。它们决定冲刷时能否走"整链直发"捷径。

### 1.3 冲刷：blk_mq_flush_plug_list 的三级策略

```text
blk_mq_flush_plug_list(plug, from_schedule)
 ├─ 快路径(单队列+无调度器+非调度点冲刷):
 │    q->mq_ops->queue_rqs 存在且非共享 tag?
 │      → __blk_mq_flush_plug_list: 整条 mq_list 原样交驱动(virtio-blk 用)
 │    否则 blk_mq_plug_issue_direct: 逐请求 blk_mq_request_issue_directly
 │      (每请求直接 budget+tag+queue_rq; 换 hctx 时 commit_rqs)
 ├─ 慢路径循环 blk_mq_dispatch_plug_list:
 │    从链头切出 (同 hctx, 同 ctx, 同 passthrough 性质) 的极大前缀
 │    passthrough → 直挂 hctx->dispatch + run
 │    有 elevator → e->ops.insert_requests(hctx, &list, 0) + run
 │    否则        → blk_mq_insert_requests (先试直发, 再入 ctx 软队列)
 │    循环直至 mq_list 清空
 └─ trace_block_unplug(q, depth, explicit=!from_schedule)
```

`from_schedule`（睡眠触发的冲刷）会抑制直发优化——调度点上下文里不指望设备资源即时可用。

---

## 二、合并的四个现场

同一条 bio，有四次机会被吸收进已有请求，成本由低到高：

### 2.1 现场一：plug 链内合并（免锁）

```c
bool blk_attempt_plug_merge(struct request_queue *q, struct bio *bio,
                            unsigned int nr_segs)
{
    rq_list_for_each(&plug->mq_list, rq) {
        if (rq->q == q) {
            /* sched_allow_merge=false: plug 上的请求尚未进调度器,
               无锁环境不能问调度器意见 */
            if (blk_attempt_bio_merge(q, rq, bio, nr_segs, false) == BIO_MERGE_OK)
                return true;
            break;              /* 单队列只看链头一个; 多队列继续扫 */
        }
        if (!plug->multiple_queues)
            break;
    }
    return false;
}
```

`blk_mq_plug` 注释点明动机：plug 是**发行机制**而非调度机制，它在 `q->queue_lock` 之外运作，因此只做"基本参数合并"。

### 2.2 现场二：调度器合并（blk_mq_sched_bio_merge）

plug 未命中才走到这里，分两支：

```text
blk_mq_sched_bio_merge(q, bio, nr_segs)
 ├─ e->ops.bio_merge 存在(mq-deadline/bfq 都实现了)?
 │    → 调度器自己的合并: 拿调度器锁(如 dd->lock) → blk_mq_sched_try_merge
 │        elv_merge() 三级查: ①q->last_merge 一击缓存
 │                           ②elv_rqhash_find(bio 起始扇区) —— 哈希表按
 │                             "请求结束扇区"索引(ELV_HASH_BITS=6 桶),
 │                             命中即 back-merge 候选
 │                           ③e->ops.request_merge (dd: sort_list 红黑树
 │                             elv_rb_find(bio_end_sector) → front-merge)
 └─ 无调度器(默认路径):
      ctx = 本 CPU 软队列; 检查 ctx->rq_lists[hctx->type] 非空
      → blk_bio_list_merge: 逆序扫链上最多 8 个请求逐个试
        ("hand-wavy stop count of 8" — blk-mq-sched.c 注释原话)
```

### 2.3 现场三：插入合并（elv_attempt_insert_merge）

带调度器时，新 request 进入调度器队列前（`blk_mq_sched_try_insert_merge`）还有一次"以请求并请求"的机会：拿 `blk_rq_pos(rq)` 查哈希，若能与已有请求衔接则 `attempt_merge()` 吸收之，并把被吸收者挂入 free 列表。

### 2.4 现场四：请求-请求合并（attempt_merge）

`blk_mq_sched_try_merge` 在 bio-merge 成功后立刻追加：合并后的请求若与后/前邻居（`elv_latter/former_request`）连续，继续合并请求对（`attempt_merge`：校验 op/方向/ioprio 相同 → `ll_merge_requests_fn` → biotail 拼接 → `elv_merge_requests` 重挂哈希 → 取较小 `start_time_ns` 记账）。这是把 4KB 风暴滚成 1MB 大请求的雪球引擎。

---

## 三、合并判据：bio_attempt_back_merge 的完整边界

```c
static enum bio_merge_status bio_attempt_back_merge(struct request *req,
                struct bio *bio, unsigned int nr_segs)
{
    if (!ll_back_merge_fn(req, bio, nr_segs))   /* 全部硬边界在这里 */
        return BIO_MERGE_FAILED;
    ...
    req->biotail->bi_next = bio;                /* 挂链尾 */
    req->biotail = bio;
    req->__data_len += bio->bi_iter.bi_size;    /* 长度生长 */
    return BIO_MERGE_OK;
}
```

`ll_back_merge_fn`（blk-merge.c）逐层拒绝：

```text
ll_back_merge_fn(req, bio, nr_segs)
 ① req_gap_back_merge → bio_will_gap(q, bio):
      前一 bio 的最后页 与 本 bio 首页 物理不连续,
      且队列要求 virt_boundary(如 NVMe SGL 段界限) → 拒 (虚拟连续段必须
      物理连续)
 ② 带块完整性(integrity)且校验数据段不连续 → 拒
 ③ bio_crypt_ctx_back_mergeable: 内联加密上下文不匹配 → 拒
 ④ blk_rq_sectors(req) + bio_sectors(bio) > blk_rq_get_max_sectors():
      超 max_sectors; ZNS 还要落在 chunk_sectors(区)内 → 拒且
      req_set_nomerge (惩罚: 该请求后续不再参与合并, 直到重新排队)
 ⑤ ll_new_hw_segment:
      blk_cgroup_mergeable(req, bio) —— bi_blkg 不同(cgroup 隔离) → 拒
      integrity merge_bio 检查
      req->nr_phys_segments + nr_phys_segs > max_segments → 拒
      通过 → nr_phys_segments += ...  /* 段数生长 */
```

**方向判定**（`blk_try_merge`）：

```text
req 尾扇区 == bio 起始扇区            → ELEVATOR_BACK_MERGE   (最常见: 顺序写)
req 起始 - bio 长度 == bio 起始扇区    → ELEVATOR_FRONT_MERGE  (前置合并,
                                                             __sector 回退)
discard 可并                          → ELEVATOR_DISCARD_MERGE
其余                                  → ELEVATOR_NO_MERGE
```

合并前置检查 `blk_rq_merge_ok`：op 相同、方向相同、**ioprio 相同**、cgroup 相同、integrity/crypto 兼容——`REQ_NOMERGE_FLAGS`（REQ_NOMERGE|REQ_PREFLUSH|REQ_FUA）直接出局，所以 flush 请求永不合并。

---

## 四、io_context / icq：历史与 6.6 现状

```text
2.6 时代: CFQ 为每个 (task, queue) 维护 struct io_cq(icq) —— 存放
   per-task 的 seek 代价估计、最后请求位置等, 挂在 task->io_context 的
   icq_list 上, q->icq_list 反向可查。blk-ioc.c 管理引用与生命周期。
6.6 现状: io_context 框架仍在(267 行的 blk-ioc.c), 但用户寥寥:
   - elevator_mq_ops 保留 init_icq/exit_icq 钩子
   - 真正使用 icq 的只剩 BFQ (bfq_io_cq: 最后请求位置/寻求成本/预算
     建议等 per-(task,queue) 状态)
   - mq-deadline/kyber/none 均不用 —— 多队列时代 per-task 状态的收益
     覆盖不了维护成本, 调度器改用 per-hctx/全局状态
```

结论：读 6.6 的提交路径**不必**再关心 io_context（除非研究 bfq）；它更多是一块活的化石，证明"调度器想要 per-task 私有状态"这个需求曾经真实存在。

---

## 五、flush 语义：REQ_PREFLUSH / FUA 状态机

### 5.1 为什么要状态机

设备写缓存（WC）让"写完成"撒谎。文件系统需要两种真话：**REQ_PREFLUSH**（先刷缓存再写数据）、**REQ_FUA**（这条数据本身必须直达非易失介质）。块层把这两种语义编译成最多三段的序列（block/blk-flush.c）：

```c
enum {
    REQ_FSEQ_PREFLUSH  = 1 << 0,   /* 先刷 */
    REQ_FSEQ_DATA      = 1 << 1,   /* 数据本体 */
    REQ_FSEQ_POSTFLUSH = 1 << 2,   /* 后刷 */
    REQ_FSEQ_DONE      = 1 << 3,
    FLUSH_PENDING_TIMEOUT = 5 * HZ, /* C3 的饿死保险 */
};

static unsigned int blk_flush_policy(unsigned long fflags, struct request *rq)
{
    unsigned int policy = 0;
    if (blk_rq_sectors(rq))  policy |= REQ_FSEQ_DATA;
    if (fflags & (1UL << QUEUE_FLAG_WC)) {
        if (rq->cmd_flags & REQ_PREFLUSH) policy |= REQ_FSEQ_PREFLUSH;
        if (!(fflags & (1UL << QUEUE_FLAG_FUA)) &&   /* 设备不支持 FUA 时,
                                                        用 POSTFLUSH 模拟 */
            (rq->cmd_flags & REQ_FUA))     policy |= REQ_FSEQ_POSTFLUSH;
    }
    return policy;
}
```

| 设备能力 | REQ_PREFLUSH+数据 | 数据+REQ_FUA | 纯 flush |
| :--- | :--- | :--- | :--- |
| 无写缓存 | DATA | DATA | 直接完成（第 1 章入口已剥） |
| WC，支持 FUA | PREFLUSH→DATA | DATA(FUA) | PREFLUSH |
| WC，无 FUA | PREFLUSH→DATA | PREFLUSH→DATA→**POSTFLUSH** | PREFLUSH |

### 5.2 状态机流转图

```text
        blk_insert_flush(rq) 按 policy 编译序列
             │
             ▼
   ┌── PREFLUSH ──┐   (多个请求可同批共享一次 flush —— 双缓冲的意义)
   │ fq->flush_   │
   │ queue[p0]    │        C1: 只有一个 flush 在飞 (p0==r0 时才 kick)
   └──────┬───────┘        C2: 等 DATA 清零(避免逐请求 POSTFLUSH)
          │ blk_kick_flush │ C3: 等待 > 5s 则强制 kick (防饿死)
          ▼
   flush_rq(借 tag) 发 REQ_OP_FLUSH ──完成──▶ flush_end_io
          │                                    │ 翻 running_idx
          ▼                                    ▼
   各请求 blk_flush_complete_seq(PREFLUSH) → 走 DATA:
   经 q->requeue_list + requeue_work 重入队(普通路径下发)
          │
          ▼
   DATA 完成(mq_flush_data_end_io) → RQF_FLUSH_SEQ 挡住 bio_endio
          │ policy 含 POSTFLUSH?
          ├─ 否 → blk_flush_restore_request → blk_mq_end_request ★真完成
          └─ 是 → 挂 flush_queue[p'] → kick → flush_rq 再飞
                   POSTFLUSH 完成 → restore → blk_mq_end_request ★
```

### 5.3 双缓冲执行

```text
struct blk_flush_queue {                      /* 每个 hctx 一个 */
    spinlock_t mq_flush_lock;
    unsigned flush_pending_idx:1;  ─┐ 两个下标构成双缓冲:
    unsigned flush_running_idx:1;   ┘ 相等=空闲, 不等=flush 在飞
    unsigned long flush_pending_since;   /* 排队起点(算 5s 超时) */
    struct list_head flush_queue[2];
    unsigned long flush_data_in_flight;  /* 正在执行 DATA 的数量 */
    struct request *flush_rq;         /* 独立预分配的替身 request(随 hctx
                                         分配, 含驱动 PDU 空间),
                                          借 tag 时才获得身份 */
};

状态机流转 (blk_flush_complete_seq):
  新请求入机(blk_insert_flush):
    policy==0            → 直接 blk_mq_end_request 完成
    仅 DATA              → 当普通请求发(可能后跟 POSTFLUSH 尾巴)
    其他                 → 置 RQF_FLUSH_SEQ, 按"已完成 policy 之外的部分"
                           入 pending 队列
  PREFLUSH/POSTFLUSH 阶段的请求们 → 挂 fq->flush_queue[pending_idx]
  blk_kick_flush 检查三个条件:
    C1: pending_idx == running_idx (当前无在飞的 flush —— 全局串行)
    C2: flush_data_in_flight == 0  (没有 DATA 在跑 —— 免得它完成后
                                    各自触发一次 POSTFLUSH)
    C3: 或等待超过 FLUSH_PENDING_TIMEOUT(5s) —— 防 FUA 流饿死 flush
    满足 → 翻转 pending_idx, 用 flush_rq 借 tag 替身发 REQ_OP_FLUSH:
        none 调度器: flush_rq->tag = first_rq->tag (借数据请求的 tag,
                     两者不可能同时在飞, 6.6 的省 tag 技巧)
        有调度器:   借 internal_tag 走 sched 路径
  flush 完成(flush_end_io) → 翻 running_idx → 把 running 队列里的每个
    请求推入下一阶段: PREFLUSH 完→去发 DATA(requeue_list 中转);
    POSTFLUSH 完→ blk_flush_restore_request(恢复原 end_io) → 真完成
```

**"完成两次"的把戏**：带数据的序列在 DATA 完成时就会走一遍 `req_bio_endio`——但 `RQF_FLUSH_SEQ` 让它只 `bio_advance` 不 `bio_endio`（第 2 章 §八的伏笔）；POSTFLUSH 真结束后恢复 `rq->bio = rq->biotail` 再整体 endio。上层（fsync 等待者）只感知一次完成，且时刻正确。

统计侧：每次 flush 完成记 `part_stat_inc(part, ios[STAT_FLUSH])` + `nsecs[STAT_FLUSH]`——`/sys/block/*/stat` 的第 16/17 列（第 6 章）。

---

## 六、plug 的延迟经济学

**plug 用 merge window 换请求质量，代价是提交侧排队延迟。**窗口有多大？单任务串行提交时，窗口就是这批提交的耗时（微秒级）——`blk_start_plug` 与 `blk_finish_plug` 之间没有任何"等人"动作，唯一的等待者是同任务的后续提交；若中途睡眠，`io_schedule_prepare` 立刻冲刷——**不存在 3ms 的魔法延迟**（那是单队列时代的 `blk_unplug_timeout`，v3.13 起 mq 路径无定时器）。真正引入毫秒级延迟的是别的机制（调度器如 bfq 的 idling、NVMe 中断合并、wbt 窗口），别把账算到 plug 头上。

四个典型 plug 建立者（谁在"蓄水"）：

| 场景 | plug 使用 | 窗口特征 |
| :--- | :--- | :--- |
| **writeback**（mm/page-writeback.c） | `write_cache_pages()` 整段 `blk_start_plug(&plug)` | 一个 inode 一批，窗口大且自然结束（finish） |
| **io_uring** | `io_submit_sqes`：`state->need_plug = max_ios > 2` 时 `blk_start_plug_nr_ios(&state->plug, state->submit_nr)` | sqe 批量驱动，`submit_nr` 即 tag 预算 |
| **direct IO 内核侧** | `blkdev_direct_IO`/iomap | 提交循环内 plug |
| **passthrough** | `blk_execute_rq_nowait`：`current->plug` 存在且非插队头时顺路 `blk_add_rq_to_plug` | 零散，不成批 |

**`rq_qos_throttle` 的连带冲刷**：wbt/iocost 若决定节流当前提交，会顺手 `blk_flush_plug`——被按住的任务不能继续囤积（否则配额语义失真）。

---

## 七、观测与调试

* **tracepoints**：`block_plug`（plug 建立）/ `block_unplug`（冲刷，含 explicit 标志与请求数）/ `block_bio_backmerge` / `block_bio_frontmerge` / `block_rq_merge` / `block_split`。`bpftrace` 统计合并率：
  `bpftrace -e 't:block:block_bio_backmerge { @m = count(); } t:block:block_bio_queue { @q = count(); }'`（`@m/@q` 即合并效率）。
* **iostat 的 merges 列**：`aqu-sz` 与 `%util` 之外的 `rMB/s÷r/s` 平均请求大小是 plug+调度器效果的最终读数；`/sys/block/*/stat` 第 2/6/13 列（rd_merges/wr_merges/dscd_merges）为原始计数。
* **关掉合并做对照**：`/sys/block/sda/queue/nomerges`——0=全开（默认），1=禁扩展合并（只留 plug 与 last_merge 一击缓存），2=全禁。A/B 实验必备。
* **unplug 批量分布**：`bpftrace -e 'tracepoint:block:block_unplug { @[args->nr_rq] = count(); }'`——批量集中在 32/64 说明是攒量阈值在冲刷；若普遍为 1~2，说明提交流太碎（或同步 IO 为主），plug 收益有限。
* **flush 观测**：`/sys/block/*/queue/write_cache`（write back/write through/none）决定 policy 走向；`stat` 的 flush_ios/flush_ticks 看代价；blktrace 里 `FUA/FLUSH` 请求类型独立标记。
* **plug 效果直测**：fio `iodepth=1` vs `iodepth=32` 下的 `slat`（提交延迟）与请求大小差——iodepth=1 无 plug 窗口（同步等完成），32 时 io_uring plug 满血。
* **wbt 交互**：`/sys/kernel/debug/block/*/wbt_lat`、`wbt_win_usec`——rq_qos 节流会主动冲刷 plug（§六），wbt 生效期若 unplug 事件异常频繁，先怀疑节流而非调度器。

## 八、深挖难点

* **为什么 zoned 写不许 plug？** 多个上下文并发提交时，plug 的"延迟入队"会重排全局顺序；SMR/ZNS 顺序区要求写请求按扇区序到达。`blk_mq_plug()` 对 zoned write 直接返回 NULL，让每个 bio 立即入队保持次序（blk-mq.h 注释原文）。
* **哈希为什么按"结束扇区"索引？** back-merge 的判定是"请求尾 == bio 头"，用请求**结束扇区**做 key，查找键就是 `bio->bi_iter.bi_sector` 一次哈希命中。front-merge 没有对称的 O(1) 索引，只能靠调度器红黑树（dd 的 `request_merge`）——这就是 back-merge 远比 front-merge 常见的结构性原因之一。
* **`req_set_nomerge` 的惩罚传播**：一次尺寸越界后请求被打上 NOMERGE，**直到重新排队才清除**。临时大 bio 会"污染"邻居请求的合并资格——调 `max_sectors_kb` 时注意这类滞后效应。
* **flush 借 tag 为什么安全？** `flush_rq` 与数据请求**不可能同时在飞**（C1 串行 + 借的是队头请求的 tag），所以借用无竞争；但 6.6 专门加了 `req_ref_set(flush_rq, 1)` 与 RQF_FLUSH_SEQ 状态防止 timeout 路径 double-free（blk-flush.c 注释"avoiding use-after-free"）。
* **plug 为什么挂在 task_struct 而非 per-CPU？** plug 描述的是**提交者**的批量意图：任务迁移时窗口跟着走、睡眠时由它自己冲刷；若做成 per-CPU 共享，两个提交者的批量会互相冲刷合并窗口，且 `blk_mq_plug` 的"zoned 写绕过"决策就无从谈起。对比网络分册的 `napi_struct`（设备侧状态，绑定 CPU）——**状态归属取决于谁产生它**。
* **`has_elevator` 标志的用途**：`blk_add_rq_to_plug` 一旦发现 `RQF_SCHED_TAGS` 就置位——因为 sched tag 的请求**不能**整链直发 `->queue_rqs()`（驱动只认 driver tag），冲刷时必须退回"先 insert_requests 进调度器"的慢路径。这是 plug 快路径与调度器共存协议的关键一位。

## 九、思考题

1. 进程 A 在 plug 里攒了 10 个脏页写请求后调用 `malloc(1GB)` 触发直接回收，回收目标恰好包含其中 3 页。请精确描述这 3 页的 IO 是如何"插队"完成的，如果 `io_schedule_prepare` 不冲刷 plug 会发生什么？
2. `multiple_queues=true` 时 plug 阈值从 32 放宽到 64，理由是"md arrays 更容易受益于合并"（blk-mq.c 注释）。为什么多队列（多底层盘）反而需要更大的窗口？
    *提示：md/raid 下每个底层盘一个 queue，窗口太小则到达每条队列的请求不足以滚动合并。*
3. FUA 每次都要走 PREFLUSH 吗？对照 §5.1 表格说明：WC+FUA 设备上 `REQ_PREFLUSH|REQ_FUA|数据` 会被编译成几段？其中哪段可能被跳过？
4. `bio_attempt_front_merge` 成功后 `req->__sector` 回退、`__data_len` 增长，但 `req->bio` 链表头换成新 bio。此时若 `RQF_MIXED_MERGE`（failfast 混合）已置位，请求的 failfast 属性以谁为准？部分完成后（`blk_update_request`）又如何更新？
    *提示：`blk_update_mixed_merge` front 分支与 `blk_update_request` 的 `req->cmd_flags |= req->bio->bi_opf & REQ_FAILFAST_MASK`。*
5. 设计一个实验，用 blktrace 的 Q→G（plug 内合并点）事件流验证"现场一（plug 免锁合并）命中率随 iodepth 上升而下降"的假设，并解释为什么。
6. 若给 6.6 的 plug 加一个你自己的 `cur_kick`（N 微秒后强制冲刷的软定时器），需要挂在哪里才能避免为每个 plug 增加定时器开销？6.8 内核实际引入了什么机制解决这个问题？

---

> 下一章：[IO 调度器](05-IO调度器.md) —— elevator 框架与 mq-deadline/kyber/bfq 的决策内核。
