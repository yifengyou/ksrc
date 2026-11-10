# bio 与 bio_vec：块层的通用货币

> 阅读目标：`bio` 是块层与所有上层（文件系统、swap、dm/md、ioctl）之间的唯一契约。本章逐字段精读 `struct bio`、`struct bio_vec`、`struct bvec_iter`（include/linux/blk_types.h、include/linux/bvec.h、block/bio.c，全部核对自 6.6），吃透多页 bvec 的两套语义、bio 的引用/克隆/链式完成规则，以及 `submit_bio` 在普通盘与栈式设备上的分派差异。

---

## 一、数据结构总览

```text
        struct bio                          struct bio_vec[] (bi_io_vec)
┌─────────────────────────────┐   ┌─────────────────────────────────────┐
│ bi_next      ─(队列链)      │   │ [0] bv_page  bv_len  bv_offset       │
│ bi_bdev      ─目标分区      │   │ [1] bv_page  bv_len  bv_offset       │  ← bv_len 可以
│ bi_opf       ─op+flags 打包 │   │ ...                                  │     跨页!(mp_bvec)
│ bi_iter      ─游标(下)      │───┼─▶ 当前遍历位置: bi_iter.bi_idx       │
│ bi_vcnt      ─数组长度      │   │ [bi_vcnt-1]                         │
│ bi_io_vec ───┼──────────────┘   └─────────────────────────────────────┘
│ bi_max_vecs  ─容量           │    (小 bio 直接内嵌在 bi_inline_vecs[],
│ bi_end_io    ─完成回调        │     省一次独立分配)
│ bi_private   ─回调上下文      │
│ __bi_cnt     ─pin 引用        │
│ __bi_remaining ─链式计数      │
│ bi_status    ─完成状态        │
└─────────────────────────────┘
```

**一句话**：bio = **"目标（bi_bdev + bi_iter.bi_sector）+ 载荷（bvec 数组）+ 游标（bi_iter）+ 完成语义（bi_end_io/bi_status）"**。驱动永远只消费"游标剩余的部分"。

---

## 二、struct bio_vec 与 bvec_iter

### 2.1 定义（include/linux/bvec.h）

```c
struct bio_vec {
    struct page     *bv_page;    /* 起始页 */
    unsigned int    bv_len;      /* 字节数(可 > PAGE_SIZE!) */
    unsigned int    bv_offset;   /* 相对 bv_page 起点的偏移 */
};

struct bvec_iter {
    sector_t        bi_sector;      /* 设备地址(512B 扇区) —— 剩余部分的起点 */
    unsigned int    bi_size;        /* 剩余字节数(residual) */
    unsigned int    bi_idx;         /* 当前 bio_vec 下标 */
    unsigned int    bi_bvec_done;   /* 当前 bvec 内已消费的字节数 */
} __packed;

struct bvec_iter_all {          /* 只用于"全量遍历"的慢速辅助状态 */
    struct bio_vec  bv;
    int             idx;
    unsigned        done;
};
```

**bi_iter 的四个字段就是"剩余 IO"的完整定义**：

| 字段 | 语义 | 谁推进它 |
| :--- | :--- | :--- |
| `bi_sector` | 尚未完成的起始扇区 | `bio_advance()`（`+bytes>>9`） |
| `bi_size` | 尚未完成的字节数 | `bio_advance()` / `bvec_iter_advance()` |
| `bi_idx` | 当前走到哪个 bvec | `bvec_iter_advance()` |
| `bi_bvec_done` | 当前 bvec 内部已走多少 | `bvec_iter_advance()` |

`__packed` 不是装饰：`bvec_iter` 会被嵌进 request 的各种迭代器，内核要它 24 字节紧凑无空洞。

### 2.2 两套遍历语义：mp_bvec vs 单页 bvec

这是 bio 子系统最反直觉的设计。`bv_len` 允许跨页（多页 bvec，multi-page bvec/mp_bvec）——前提是物理页连续（bvec.h 头注释：`nth_page(bv_page, n) == bv_page + n`，由 `page_is_mergeable()` 保证）。于是同一份数据有两种"看法"：

```c
/* mp_bvec 视角: 不切页, 一个 bvec 可能横跨 N 个物理页 */
#define mp_bvec_iter_len(bvec, iter)  \
    min((iter).bi_size, bvec[(iter).bi_idx].bv_len - (iter).bi_bvec_done)
#define mp_bvec_iter_offset(bvec, iter) \
    (bvec[(iter).bi_idx].bv_offset + (iter).bi_bvec_done)

/* 单页视角: 强制按 PAGE_SIZE 切开(给驱动/高端内存映射用) */
#define bvec_iter_offset(bvec, iter)  (mp_bvec_iter_offset(...) % PAGE_SIZE)
#define bvec_iter_len(bvec, iter)     /* min(mp_len, PAGE_SIZE - off) */
#define bvec_iter_page(bvec, iter)    (mp_page + mp_off/PAGE_SIZE)
```

```text
一个 bv_len=10KB、bv_offset=1KB 的 mp_bvec:
[页0: 3KB][页1: 4KB][页2: 3KB]     ← 物理连续
bio_for_each_bvec() 看到它:  1 个 bvec (10KB)      ← 合并/统计用
bio_for_each_segment() 看到它: 3 个段 (3K,4K,3K)   ← 驱动建 DMA scatterlist 用
```

三种遍历宏的分工（include/linux/bio.h，6.6 实测）：

| 宏 | 产出 | 典型用户 | 推进方式 |
| :--- | :--- | :--- | :--- |
| `bio_for_each_segment(bvl, bio, iter)` | **单页** bvec | 驱动 `queue_rq` 内逐段建 sg（`blk_rq_map_sg`） | `bio_advance_iter_single` |
| `bio_for_each_bvec(bvl, bio, iter)` | **多页** bvec | 合并检查、`bio_split_rw` 段数计算 | `bio_advance_iter_single` |
| `bio_for_each_segment_all(bvl, bio, iter_all)` | 单页 bvec，**从头到尾** | 只许块核心/栈式驱动自己用：**bio 可能已被切分，驱动看到的不是全部**（bio.h 注释原话："drivers should _never_ use the all version"） | `bvec_advance` |
| `bio_for_each_folio_all(fi, bio)` | folio 序列 | 需要整 folio 语义的完成端（如 on-stack 写回核对） | `bio_next_folio` |

`bio_advance()` 的两个分支值得背下来：

```c
static inline void bio_advance_iter(const struct bio *bio,
                                    struct bvec_iter *iter, unsigned int bytes)
{
    iter->bi_sector += bytes >> 9;
    if (bio_no_advance_iter(bio))       /* discard/secure-erase/write-zeroes:
                                           bi_size 语义是"区间长度"而非内存 */
        iter->bi_size -= bytes;
    else
        bvec_iter_advance(bio->bi_io_vec, iter, bytes); /* 正常数据 IO */
}
```

---

## 三、struct bio 逐字段精读

6.6 真实定义（include/linux/blk_types.h，节选注释）：

```c
struct bio {
    struct bio          *bi_next;      /* 挂到 request 时串 bio 链 */
    struct block_device *bi_bdev;      /* 目标分区(整盘或某个 part) */
    blk_opf_t           bi_opf;        /* 低 8 位 REQ_OP_*, 高 24 位 REQ_* */
    unsigned short      bi_flags;      /* BIO_* 标志位 */
    unsigned short      bi_ioprio;     /* IOPRIO_* 提示(第 5 章) */
    blk_status_t        bi_status;     /* 完成状态(见 §8) */
    atomic_t            __bi_remaining;/* bio 链完成计数(见 §7) */

    struct bvec_iter    bi_iter;       /* 游标: 剩余 IO 的位置/大小 */

    blk_qc_t            bi_cookie;     /* 轮询 cookie(= hctx->queue_num) */
    bio_end_io_t        *bi_end_io;    /* 完成回调 */
    void                *bi_private;   /* 回调私有(常是父对象) */

    struct blkcg_gq     *bi_blkg;      /* CONFIG_BLK_CGROUP: cgroup 记账锚点 */
    struct bio_issue    bi_issue;      /* iocost 用: 发出时刻+长度编码 */

    unsigned short      bi_vcnt;       /* bvec 数组实际长度 */
    /* —— bio_reset() 保留线(BIO_RESET_BYTES) —— */
    unsigned short      bi_max_vecs;   /* 数组容量 */
    atomic_t            __bi_cnt;      /* pin 计数(bio_get/bio_put) */
    struct bio_vec      *bi_io_vec;    /* 指向数组(常指 bi_inline_vecs) */
    struct bio_set      *bi_pool;      /* 归属池(回收用) */
    struct bio_vec      bi_inline_vecs[]; /* 尾部内嵌数组 */
};
```

易混点辨析：

* **`bi_vcnt` vs `bi_max_vecs`**：分配时的容量是 `bi_max_vecs`；`bio_add_page()` 逐步填充时 `bi_vcnt` 递增。切分出的子 bio 复用父数组，故 `bi_vcnt` 不变、游标变。
* **`__bi_cnt` vs `__bi_remaining`**：前者是"**谁持有这个 bio 对象**"（防提前释放，`bio_get/bio_put`，置 `BIO_REFFED`）；后者是"**链式完成还差几个**"（`bio_chain`，置 `BIO_CHAIN`）。互不相干。
* **`bi_cookie`**：`REQ_POLLED` 专用，提交时 `blk_mq_start_request()` 写入 `rq->mq_hctx->queue_num`，`bio_poll()` 据此找到轮询队列。
* **`bi_iter` 永远表示"剩余"**：一个 bio 被部分完成后（分区完成、`blk_update_request` 部分推进），`bi_iter.bi_size` 变小、`bi_sector` 前移；`bi_flags/bi_opf` 等元数据不变。判断"这个 bio 全部做完"只看 `bi_iter.bi_size == 0`。
* **`BIO_RESET_BYTES = offsetof(struct bio, bi_max_vecs)`**：`bio_reset()` 只清前半段（游标/状态/回调），保留后半段的数组与池归属——bio 对象可循环复用。

`BIO_*` 标志（bi_flags 位）速查：`BIO_PAGE_PINNED`（用户页 pin，释放时 unpin）、`BIO_CLONED`、`BIO_CHAIN`、`BIO_REFFED`、`BIO_BPS_THROTTLED`（已限速防二次）、`BIO_TRACE_COMPLETION`（blktrace 用）、`BIO_REMAPPED`（已做过分区平移）、`BIO_ZONE_WRITE_LOCKED`。

---

## 四、bi_opf：REQ_OP_* 与 REQ_FLAG_* 的编码

```c
typedef __u32 __bitwise blk_opf_t;

#define REQ_OP_BITS    8                  /* 低 8 位: 操作码 */
#define REQ_FLAG_BITS  24                 /* 高 24 位: 属性标志 */

static inline enum req_op bio_op(const struct bio *bio)
{ return bio->bi_opf & REQ_OP_MASK; }     /* REQ_OP_MASK = 0xff */
```

编码规则（blk_types.h 注释）：**操作码最低位指示数据方向**——1 表示写向设备（`op_is_write`）。完整操作码表：

| REQ_OP_* | 值 | 说明 |
| :--- | :--- | :--- |
| `READ` | 0 | 读（偶数） |
| `WRITE` | 1 | 写 |
| `FLUSH` | 2 | 刷易失写缓存（无数据 bio） |
| `DISCARD` | 3 | 弃用（trim） |
| `SECURE_ERASE` | 5 | 安全擦除 |
| `WRITE_ZEROES` | 9 | 写零 |
| `ZONE_OPEN/CLOSE/FINISH/APPEND/RESET/RESET_ALL` | 10–17 | ZNS 区管理 |
| `DRV_IN/DRV_OUT` | 34/35 | 驱动私有（passthrough） |
| `REQ_OP_LAST` | 36 | —— |

高频标志（`__REQ_` 位号从 8 起）：

```text
REQ_SYNC        同步语义(读/fua/preflush 自动隐含, op_is_sync)
REQ_FUA         Force Unit Access: 完成时数据必须已落非易失介质
REQ_PREFLUSH    先刷写缓存再执行数据
REQ_NOMERGE     禁止合并(与 REQ_PREFLUSH|REQ_FUA 一起构成 REQ_NOMERGE_FLAGS)
REQ_NOWAIT      不许阻塞(排队满则 BLK_STS_AGAIN)
REQ_POLLED      轮询完成(io_uring HIPRI)
REQ_META/REQ_PRIO/REQ_BACKGROUND/REQ_RAHEAD/REQ_SWAP/REQ_IDLE ...
```

三个派生判断必须刻进肌肉记忆：

```c
op_is_write(op)   : op & 1                      /* 偶读奇写 */
op_is_flush(op)   : op & (REQ_FUA | REQ_PREFLUSH)/* 需走 flush 状态机 */
op_is_sync(op)    : op==REQ_OP_READ || op & (REQ_SYNC|REQ_FUA|REQ_PREFLUSH)
```

> ⚠️ 统计分组用 `op_stat_group()`：discard 单列，其余按读写分——这是 `/sys/block/*/stat` 17 列的来源（第 6 章）。

---

## 五、bio 的生命周期与引用

### 5.1 分配：bio_set 与 per-cpu 缓存

```text
bio_alloc(bdev, nr_vecs, opf, gfp)
 └─ bio_alloc_bioset(..., &fs_bio_set)          /* 文件系统默认池 */
     ├─ 快路径: bioset 的 per-cpu alloc cache (BIOSET_PERCPU_CACHE)
     │    本 CPU free_list 现取, 无锁; REQ_ALLOC_CACHE 标记的 bio
     │    在 bio_put 时直接归缓存而非 slab
     │    · 中断上下文的 bio_put 无法安全动 free_list → 先入 free_list_irq,
     │      积到 16 个(ALLOC_CACHE_THRESHOLD) 由分配侧在关中断下
     │      bio_alloc_irq_cache_splice 并回 free_list
     ├─ 慢路径: mempool(保证不因内存压力死锁) + bvec_slabs 按 nr_vecs 分档
     └─ 缓存总量超限 → 直接走 slab, 防缓存吞内存
bio_kmalloc(nr_vecs, gfp)      /* 一次性 kmalloc, 不入池, 慎用 */
```

* **`BIOSET_NEED_RESCUER`**：栈式设备的私有池必须开——`punt_bios_to_rescuer()` 把当前递归提交链里属于本池的 bio 转投 `rescue_workqueue`，打破"为了完成你的 IO 需要先分配你的池子"的自锁（bio_alloc_irq_cache_splice 系列就是为其服务）。
* `nr_vecs` 上限 `BIO_MAX_VECS=256`；`bio_add_page()` 放不下时返回 0，由调用者决定切分或换 bio（`blk_next_bio` 自动链式续杯）。

### 5.2 引用与复用

```c
bio_get(bio);   /* __bi_cnt++; 置 BIO_REFFED —— 防止提交后、完成前被释放 */
bio_put(bio);   /* __bi_cnt-- ; 归 0 → 还给 bi_pool 的 per-cpu cache 或 kfree */
bio_init(...)   /* 栈上/嵌入结构初始化(不归还池) */
bio_uninit(bio) /* 释放 bi_blkg 引用、integrity payload、crypt ctx */
bio_reset(bio, bdev, opf) /* 清前半段, 保留池/数组 —— 复用常见于切分 */
```

**铁律**：`submit_bio()` 之后到 `bi_end_io()` 之前，bio **不属于提交者**，除非你先 `bio_get()` 持有 pin。完成回调运行在任意 CPU/softirq 上下文——`bi_end_io` 里不能睡眠。

### 5.3 克隆：bio_alloc_clone / bio_init_clone

6.6 已没有老 API `bio_clone()`，取而代之：

```c
struct bio *bio_alloc_clone(bdev, bio_src, gfp, bs);
int         bio_init_clone(bdev, bio, bio_src, gfp);
```

克隆 = **新 bio 头 + 共享源 bvec 数组**（`bi_io_vec` 指同一片内存，置 `BIO_CLONED`）。这是 dm/md/loop 的日常：把上游 bio 改改目标再下发，数据页零拷贝。代价是**生命周期耦合**——源 bio 必须活得比所有克隆久（`bio_split` 的文档同样强调这一点）。

### 5.4 cgroup 关联：bio_associate_blkg

```text
bio_set_dev(bio, bdev) / bio_reset() / __submit_bio 侧:
  bio_associate_blkg(bio)
   ├─ bio->bi_blkg 已有? 直接返回
   ├─ 无 q? (bio-based 中间层未定目标) 暂挂 root blkg
   └─ blkcg → blkg_lookup_create(blkcg, q)  /* (cgroup,queue) 二元组 */
       并拿引用, 在 bio_uninit() 时归还
```

`bi_blkg` 是后面所有记账（throttle/iocost/latency PSR、`io.stat`）的锚点；`bio_clone_blkg_association()` 保证克隆继承归属。

---

## 六、submit_bio → submit_bio_noacct：分派与递归

```text
submit_bio(bio)                                   [blk-core.c]
 ├─ 读: task_io_account_read(); PGPGIN += sectors  /* /proc/vmstat */
 └─ 写: PGPGOUT += sectors
    submit_bio_noacct(bio)
     ├─ REQ_NOWAIT 且设备不支持 → BLK_STS_NOTSUPP
     ├─ should_fail_bio (故障注入)
     ├─ bio_check_ro(写只读) / bio_check_eod(越界)
     ├─ !BIO_REMAPPED 且 bd_partno → blk_partition_remap:
     │      bi_iter.bi_sector += bd_start_sect; 置 BIO_REMAPPED
     ├─ op_is_flush 且 !QUEUE_FLAG_WC: 剥 REQ_PREFLUSH|REQ_FUA
     │      (无数据纯 flush → 直接成功返回)
     ├─ !QUEUE_FLAG_POLL → bio_clear_polled
     ├─ 按 REQ_OP 校验能力(discard/write_zeroes/zone...)
     ├─ blk_throtl_bio() ── 被 io.max 限速则挂起, 稍后重放 (直接 return)
     └─ submit_bio_noacct_nocheck(bio)
          ├─ blk_cgroup_bio_start / trace_block_bio_queue
          ├─ current->bio_list 非空?  → bio_list_add()  /* 递归中转(见下) */
          ├─ !bd_has_submit_bio       → __submit_bio_noacct_mq() /* 普通盘 */
          └─ 否则                     → __submit_bio_noacct()    /* dm/md 栈 */
```

### 6.1 普通盘 vs 栈式设备：`__submit_bio` 的一行分叉

```c
static void __submit_bio(struct bio *bio)
{
    if (unlikely(!blk_crypto_bio_prep(&bio)))
        return;
    if (!bio->bi_bdev->bd_has_submit_bio) {
        blk_mq_submit_bio(bio);              /* blk-mq 设备(第 3/4 章) */
    } else if (likely(bio_queue_enter(bio) == 0)) {
        struct gendisk *disk = bio->bi_bdev->bd_disk;
        disk->fops->submit_bio(bio);         /* dm/md/loop 的钩子 */
        blk_queue_exit(disk->queue);
    }
}
```

`bd_has_submit_bio` 在 `device_add_disk()` 时由 `fops->submit_bio != NULL` 决定（第 1 章 §2.2）——这就是"mq 设备 vs bio-based 设备"的运行时判定。

### 6.2 递归保护：current->bio_list 与栈式设备

dm 收到一个 bio，可能拆成 N 个子 bio 发往底层盘；底层 `submit_bio` 又可能再拆……若每次递归都真调函数，内核栈必然爆。6.6 的解法（`__submit_bio_noacct`）：

```text
__submit_bio_noacct(bio):
  bio_list_on_stack[2]; current->bio_list = bio_list_on_stack;
  do {
    __submit_bio(bio);              /* dm 的 ->submit_bio 拆分:
                                       子 bio 全部落进 current->bio_list[0] */
    /* 把子 bio 分成 lower(去别的队列)/same(同队列) 两堆,
       先处理 lower 再处理 same —— 保证最底层先走 */
  } while ((bio = bio_list_pop(&bio_list_on_stack[0])));
  current->bio_list = NULL;         /* 恢复 */
```

任何层次的 `->submit_bio` 里再调 `submit_bio_noacct()`，都会发现 `current->bio_list` 非空而只做 `bio_list_add` 入列——**递归被压平成迭代**。深度由数据决定而不是调用栈决定。

---

## 七、bio 链（bio_chain）与 __bi_remaining

**问题**：bio 被切成多个子块分别下发（如 dm 条带、`bio_split`），父 bio 何时完成？`bio_chain` 是内核的计数答案：

```c
void bio_chain(struct bio *bio, struct bio *parent)
{
    BUG_ON(bio->bi_private || bio->bi_end_io);
    bio->bi_private = parent;
    bio->bi_end_io  = bio_chain_endio;   /* 子 bio 完成时的跳板 */
    bio_inc_remaining(parent);           /* parent->__bi_remaining++ 且置 BIO_CHAIN */
}
```

```text
提交端:                          完成端(子 bio 结束时):
parent.__bi_remaining = 2        bio_chain_endio(child)
childA ──bio_chain──┐              ├─ 错误传播: child.bi_status 非零且
childB ──bio_chain──┤                parent 为零 → 复制给 parent ★
                    ▼              ├─ bio_put(child)
      submit(childA);              └─ bio_endio(parent)  ← 立刻评估!
      submit(childB);            bio_endio(parent):
                                  if (bio_remaining_done(parent))  /* --__bi_remaining,
                                      __bi_remaining==0 才 true */  清 BIO_CHAIN */
                                  ... 正常完成流程 → parent.bi_end_io(parent)
```

两个精妙处：

1. **错误传播方向**：`__bio_chain_endio()` 只在 `parent->bi_status == 0` 时复制——**第一个错误获胜**，后续子块的成功/失败不再覆盖。
2. **尾递归消除**：`bio_endio` 里 `if (bio->bi_end_io == bio_chain_endio) { bio = __bio_chain_endio(bio); goto again; }`——链再长也是循环，不耗栈（bio.c 注释原话：编译器关掉 sibling-call 优化后 goto 是唯一保险）。

`blk_next_bio(bio, ...)` 是"边发边链"的惯用法（discard/写零构造大区间时用）：分配新 bio，把旧的 chain 到新的并立刻 submit。

---

## 八、bio_endio：完成语义与错误模型

```text
bio_endio(bio)                                    [bio.c]
again:
 ├─ bio_remaining_done(bio)?  /* BIO_CHAIN: --__bi_remaining, 归零才继续 */
 ├─ bio_integrity_endio()     /* 完整性 payload 的后处理 */
 ├─ rq_qos_done_bio(bio)      /* iocost/latency 记账 */
 ├─ BIO_TRACE_COMPLETION → trace_block_bio_complete (blktrace Q→C 的 C)
 ├─ bi_end_io == bio_chain_endio? → 走链, goto again
 ├─ blk_throtl_bio_endio()    /* 限速层收尾 */
 ├─ bio_uninit(bio)           /* 放 blkg 引用、integrity、crypt */
 └─ bio->bi_end_io(bio)       /* 上层回调: 解锁 folio / 文件系统 / io_uring */
```

* **谁来调**：只有"拥有"该 bio 的一端可以调——驱动（passthrough）、`req_bio_endio()`（普通 request 完成时逐 bio 调）、栈式设备。
* **`bi_status`**：`blk_status_t` 是 8 位强类型错误码（`BLK_STS_OK/TIMEOUT/NOSPC/MEDIUM/PROTECTION/RESOURCE/DEV_RESOURCE/AGAIN/...`），错误名表在 `blk_errors[]`（blk-core.c），`blk_status_to_errno()` 做 errno 换算。`blk_path_error()` 区分"换条路重试也许能救"（transport/target）与"重试无意义"（nospc/medium）——多路径 dm 靠它决定 failover。
* **部分完成**：`bio_endio` 只管"全部完成"；部分完成走 `bio_advance(bio, nbytes)` 缩短游标，等剩余部分做完再一次 `bio_endio`。`blk_update_request()`（第 6 章）就是逐 bio 做 `req_bio_endio`（advance + 条件 endio）。

---

## 九、观测与调试

* **tracepoints**（`/sys/kernel/tracing/events/block/`）：`block_bio_queue`（Q）、`block_bio_complete`（C）、`block_bio_remap`（分区/栈映射）、`block_split`、`block_bio_backmerge/frontmerge`。`bi_size` 直接可读。
* **blktrace/bpftrace 速查**：
  `bpftrace -e 'tracepoint:block:block_bio_queue { @[comm] = count(); }'` 看谁在压盘；`block_split` 频繁 → I/O 跨越了 `max_sectors`/chunk 边界或段数超限，查 `/sys/block/*/queue/max_sectors_kb`。
* **故障注入**：`/sys/kernel/debug/fail_make_request`（CONFIG_FAIL_MAKE_REQUEST，`should_fail_bio`）+ per-bdev `make-it-fail`。
* **错误观察**：`dmesg` 中 `blk_print_req_error` 的输出（"I/O error, dev sda, sector ... op 0x1:(WRITE)"）即 `blk_status_to_str` + `blk_op_str` 的渲染。
* **/proc/vmstat**：`PGPGIN/PGPGOUT`（submit_bio 里累加）与块层吞吐对账。

## 十、深挖难点

* **为什么 `bvec_iter` 要 `__packed` 且把 `bi_bvec_done` 塞进去？** 没有 `bi_bvec_done`，切分点落在 bvec 中间时就必须拆 bvec 数组（拷贝/改写）；有了它，子 bio 只需复制游标（`bi_bvec_done` 记录"当前 bvec 已消费多少"），数组零改动——**克隆/切分的 O(1) 基础**。
* **`bio_split()` 为什么复制 `BIO_TRACE_COMPLETION`？** 追踪者需要"每个提交的 bio 恰好一次 complete 事件"；切分后子 bio 各自 endio，标志必须跟着走，否则 blktrace 会算出负延迟。
* **`bio_no_advance_iter` 的例外**：discard/write-zeroes 的 `bi_size` 描述的是"设备区间长度"，根本没有内存可推进——`bio_advance_iter` 必须绕开 bvec 推进。忘了这个例外的驱动会在 trim 路径上 UB。
* **克隆不复制 `bi_iter` 之外的世界**：`bio_alloc_clone` 后你必须显式改 `bi_iter.bi_sector`（目标地址）；只 clone 不改就提交 = 双写同一位置。

## 十一、思考题

1. `bio_for_each_segment` 与 `bio_for_each_bvec` 对同一个 bio 遍历出的段数何时相等、何时不同？SCSI `sd` 的 sg 表构建用哪个，为什么？
    *提示：`__blk_bios_map_sg`（blk-merge.c）以单页段为基元聚合物理连续段；`bvec_split_segs` 决定切分时的段数预算。*
2. dm 把一个 1MB bio 拆成 2 个 512KB 克隆发往两块底层盘，其中一块失败。父 bio 收到的 `bi_status` 是什么？另一半数据实际上已经落盘——文件系统层如何得知"这条写不完整"？
    *提示：`__bio_chain_endio` 首错获胜；上层只见 endio 与 status，partial 落盘由 fs 的日志/校验兜底。*
3. 为什么 `bio_endio` 要先 `bio_uninit` 再调 `bi_end_io`？反过来会发生什么？
    *提示：`bi_end_io` 回调里常见操作是唤醒等待者并释放"bio 的宿主结构"；blkg 引用若在回调后再放，宿主可能已 free——UAF。*
4. `submit_bio_noacct` 在最前面就把无写缓存设备的纯 flush bio 直接成功完成，而不是下发 `REQ_OP_FLUSH`。NVMe 盘（多数有 volatile write cache）与无缓存企业盘在这条路径上的 fsync 延迟差多少个量级？
5. `current->bio_list` 压平递归后，"先 lower 后 same"的排序为什么是必要的？如果反过来先 same，什么场景会活锁？
    *提示：同队列子 bio 的完成可能触发 dm 再生成新的 lower bio（mirror 重同步）；lower 优先保证向"更底层设备"推进。*
6. 一个 bio 的 `bi_iter.bi_size` 变成 0 但仍被 `bio_endio` 之外的人持有引用（`bio_get`），这块内存此刻"属于"谁？何时可以安全复用？

---

> 下一章：[blk-mq 多队列框架](03-blk-mq多队列框架.md) —— bio 如何变成 request，request 如何找到它的硬件队列。
