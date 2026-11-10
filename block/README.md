# Introduction — 块设备层分册

本分册深入剖析 Linux 6.6 的块设备层（block layer）：从 `submit_bio()` 到驱动 `queue_rq()`，再到完成中断与统计口径，按"架构 → 数据结构 → 引擎 → 合并 → 策略 → 完成"的顺序层层展开。6.6 是一个纯粹的 blk-mq 世界：单队列 legacy 路径已死，只剩 blk-mq 设备与 bio-based 栈式设备两类。

## 章节导读

| 章节 | 主题 | 一句话 |
| :--- | :--- | :--- |
| [00-一页速查](docs/00-一页速查.md) | 速查 | 四件套/提交路径/plug/调度器/完成/数值表（各章浓缩） |
| [01-块层总体架构](docs/01-块层总体架构.md) | gendisk/bdev/queue、分层全景 | 三大对象的关系与 4.x 弃单队列的因果 |
| [02-bio与bio_vec](docs/02-bio与bio_vec.md) | bio/bvec/迭代器 | 块层通用货币的编码、克隆、链式完成 |
| [03-blk-mq多队列框架](docs/03-blk-mq多队列框架.md) | ctx/hctx/tags、dispatch 全链 | 两级队列与双 tag 池、freeze/quiesce |
| [04-plug合并与IO路径](docs/04-plug合并与IO路径.md) | plug、四次合并、flush 状态机 | merge window 的经济学与 PREFLUSH/FUA |
| [05-IO调度器](docs/05-IO调度器.md) | elevator/none/dd/kyber/bfq | 准入派发策略的四种答案与选型 |
| [06-完成路径与统计](docs/06-完成路径与统计.md) | complete/IPI/软中断、17 列统计 | 完成分派、批量回收、iostat 的真相 |

## 学习路线

```text
① 先立骨架(01): 谁调用 submit_bio? 请求长什么样? 设备怎么注册?
② 再认货币(02): bio 的游标/引用/链 是后面一切的地基
③ 引擎核心(03): ctx→hctx 映射、tag、dispatch 循环 —— 本分册最重的一章
④ 提交优化(04): plug 与合并回看 03 的"为什么留队列不直发"
⑤ 策略层(05): 四个调度器对照 03 的 elevator 钩子逐一落位
⑥ 收尾闭环(06): 完成路径 + 观测 —— 排障时最先用到的知识
```

## 源码索引（Linux 6.6）

```
block/blk-core.c           submit_bio/blk_queue_enter/plug 起止/kblockd (~1200 行)
block/bio.c                bio 分配/克隆/链/endio/split (~1800 行)
block/blk-mq.c             引擎主体: 分配/派发/完成/requeue/freeze (~4900 行)
block/blk-mq-sched.c       调度器桥: dispatch_requests 三分支
block/blk-mq-tag.c         sbitmap tag 与共享公平配额
block/blk-merge.c          合并判据/back/front/discard merge/split
block/blk-flush.c          PREFLUSH/FUA 三段状态机
block/blk-timeout.c        request 超时定时器
block/elevator.c/.h        elevator 框架、rqhash、sysfs 切换
block/mq-deadline.c        期限调度器(默认, 单队列设备)
block/kyber-iosched.c      域令牌 + p99 自适应调度器
block/bfq-iosched.[ch]     比例公平(WF2Q+)+cgroup 调度器
block/genhd.c / bdev.c     gendisk/block_device 生命周期与分区
block/fops.c               def_blk_fops: 裸设备文件接口
include/linux/blk_types.h  bio/bi_opf/BLK_STS_* 定义
include/linux/blk-mq.h     request/blk_mq_ops/blk_mq_tags(对外)
block/blk-mq.h             blk_mq_ctx/blk_mq_hw_ctx(内部) —— 注意两个同名头!
```

## .rst 镜像导读

本目录同时保存内核 `Documentation/block/` 的官方 .rst 镜像，与本分册交叉阅读：

* `blk-mq.rst` —— blk-mq **驱动作者**指南（对照第 3 章，讲的是"怎么写驱动"视角）
* `biovecs.rst` —— 多页 bvec 语义（对照第 2 章 §2）
* `stat.rst` —— `/sys/block/*/stat` 字段官方口径（对照第 6 章 §6.2）
* `ioprio.rst` —— ionice 用户接口（对照第 5 章 §6.2）
* `switching-sched.rst` / `deadline-iosched.rst` / `kyber-iosched.rst` / `bfq-iosched.rst` —— 调度器官方文档（对照第 5 章）
* `writeback_cache_control.rst` —— REQ_PREFLUSH/FUA 语义（对照第 4 章 §5）
* `cmdline-partition.rst`、`null_blk.rst`（实验利器）、`ublk.rst`、`pr.rst`、`data-integrity.rst`、`inline-encryption.rst` —— 专题扩展

## 推荐实验环境

```
CONFIG_BLK_DEBUG_FS=y               # /sys/kernel/debug/block/ 全景
CONFIG_BLK_DEV_IO_TRACE=y           # blktrace
CONFIG_FAIL_MAKE_REQUEST=y          # 故障注入
CONFIG_BLK_CGROUP=y CONFIG_BLK_DEV_THROTTLING=y  # io.max 限速演练
CONFIG_IOSCHED_BFQ=y CONFIG_KYBER=y # 四个调度器齐全
工具: blktrace/btt、iostat、fio、biolatency(BCC)、null_blk 模块
演练盘: modprobe null_blk queue_mode=2 nr_devices=2 irqmode=1
        (无需真实硬件即可调 scheduler/nr_requests/queue 拓扑)
```

## 快速上手：五分钟建立体感

```bash
# 1. 看队列拓扑与调度器
ls /sys/block/nvme0n1/mq/ && cat /sys/block/nvme0n1/queue/scheduler
# 2. 压一笔顺序写, 观察 plug/合并/请求大小
fio --name=t --filename=/dev/nullb0 --rw=write --bs=4k --iodepth=32 \
    --direct=1 --runtime=10 &
blktrace -d /dev/nullb0 -w 8 -o - | blkparse -i - | head -50   # Q/G/M/D/C 事件流
# 3. 对照关掉合并
echo 2 > /sys/block/nullb0/queue/nomerges   # 重跑 fio, 对比 iostat 的 r/s 与合并列
# 4. 切调度器并复测 p99
echo kyber > /sys/block/nullb0/queue/scheduler
biolatency -d nullb0 5                      # BCC 直方图
```

## 常见误区（先破后立）

* **"块层有个 3ms 的 unplug 定时器"** —— 那是 blk-mq 之前单队列时代的行为。6.6 的 plug 只由 finish/schedule/阈值/poll 冲刷，无定时器（第 4 章 §1.1）。
* **"iostat 的 %util=100% 就是盘满了"** —— 多队列 SSD 并行 IO 也按时间计满，100% 只代表"一直有 IO"；看吞吐与 await 才是饱和证据（第 6 章 §6.3）。
* **"nvme 上装 bfq 更公平"** —— bfq 的单 in-service 语义与多 hctx 并行派发天然冲突，多队列设备请用 blk-throttle/iocost（第 5 章 §5.3）。
* **"svctm 能算出纯服务时间"** —— 该指标早已因数学不成立而被移除；延迟分解用 blktrace/btt（第 6 章 §8）。
* **"调度器负责把小 IO 合并成大 IO"** —— 大部分合并发生在 plug（免锁）与 elevator 哈希，调度器只是参与者之一；none 调度器下合并照样发生（第 4 章 §二）。
* **"多给 queue_depth 总是更好"** —— 深队列放大排队延迟与内存占用，且调度器深度（nr_requests）与驱动深度（queue_depth）是两个旋钮（第 3 章 §3.2）。

## 相关分册

* 调度器（[scheduler/](../scheduler/README.md)）—— `io_schedule`/nr_iowait 与 plug 的耦合（第 1/4 章）
* 内存管理（[mm/](../mm/README.md)）—— writeback/page cache 与 direct IO 入队（第 1 章 §3）
* 中断子系统（[interrupt/](../interrupt/README.md)）—— MSI-X 亲和即队列映射（第 3 章 §2/§7）
* cgroup（cgroup 分册，规划中）—— blkcg/io.max/iocost 与 ioprio 传导（第 5 章）
* 追踪（[trace/](../trace/README.md)）—— blktrace/btt 事件全景（第 6 章 §8）
* 存储（[storage/](../storage/README.md)）、[scsi/](../scsi/README.md)、[nvme/](../nvme/README.md) —— 驱动侧视角
