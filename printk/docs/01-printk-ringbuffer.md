# printk ringbuffer：无锁日志环（Linux 6.6）

> 阅读目标：掌握 5.10+ 重写后的 lockless printk ringbuffer（prb）三环结构；吃透描述符状态机（reserved→committed→finalized）、数据块的回收协议（block states 与 push_tail）、读者侧的 seq 验证复制；理解 NMI/递归安全（printk_safe）与 /dev/kmsg、dmesg 的数据来源。

---

## 一、为什么重写：旧设计的三宗罪

```
旧（≤5.9）：logbuf + log_next_seq + 自旋锁 + 环形 log 数组
  ① 读者持锁（dmesg 抓取与写入互斥——NMI 写者直接死锁风险）
  ② 单锁在 NUMAI/高频写者下成为系统最热的锁之一
  ③ dump_kmsg（panic 路径）与正常读路径纠缠
新（5.10+，John Ogness）：读-复制-原子（descr. ring + data ring + seq）
  写者：per-描述符状态机，无全局锁
  读者：seq 验证复制（copy 出来后核对序号——失败即重来）
```

## 二、三环结构（include/linux/printk_ringbuffer.h，已核对 6.6）

```c
struct printk_ringbuffer {
	struct printk_ringbuffer desc_ring;?? 实为三字段：
	struct prb_desc_ring	desc_ring;      /* 描述符环：prb_desc[] */
	struct printk_text_sring? text_data_ring;  /* 文本数据环（字节池）*/
	struct printk_dict_data_ring dict_data_ring;/* dict 数据环 */
	atomic_long_t		fail;            /* 数据环分配失败计数 */
};

struct prb_desc {
	struct prb_data_blk_lpos	text_blk_lpos;  /* 本记录文本在数据环的位置
	                                           （BEGIN|BITS：包含状态标志）*/
	struct prb_data_blk_lpos	dict_blk_lpos;
	struct printk_info		info;         /* ts_ns/seq/caller_id/
	                                            text_len/dict_len/level... */
	atomic_long_t		state_var;          /* ★描述符状态机（状态+ID 复合）*/
};

struct printk_info {
	u64	ts_ns;             /* 时间戳（ktime_get_mono_fast_ns? 由
	                             printk_clock 侧决定）*/
	u64	seq;               /* 全局单调序号（读者的核心锚点）*/
	u32	caller_id;         /* 写者身份（task id / nmi id / printk_safe）*/
	u16	text_len; u16	dict_len; u8 facility; u8 flags:5; u8 level:3;
};
```

**关键思想：数据环没有"释放"操作**——记录的存活期由描述符状态决定；数据块的复用只发生在**新写者需要空间时**（`data_push_tail` 推进尾指针，淘汰最老记录）。这是一种"隐式 GC"。

## 三、写者状态机与内存序

```
desc 状态（state_var 的编码）：reserved → committed → finalized
  vprintk_store:
   ① prb_reserve：在 desc 环占位（状态=reserved，分配新 ID）、
      text 数据环 data_alloc（可能触发 data_push_tail 淘汰最老）
   ② 文本拷入数据环（一次性，不原子也无妨——读者在 finalized 前不读）
   ③ data_commit + _prb_commit：reserved→committed（对读者部分可见？
      ——不，committed 仍不算"可安全复制"）
   ④ prb_final_commit（5.13+）：committed→finalized
      （拆两段的原因：committed 后写者还能被打断而不阻塞回收；
        finalized 才是读者可复制且回收器可淘汰的终态）
```

内存序设计是全内核教科书级案例（printk_ringbuffer.c 的 `@desc` 大注释，已核对）：逐对列出 `desc_reserve:D / desc_reserve:B`、`desc_reserve:D / data_push_tail:B` 等**跨 CPU 事件对的读写序保证**——每一条对应一处 acquire/release 配对。排障 printk 数据竞争时，该注释是唯一权威。

## 四、读者：seq 验证复制

```
prb_read_valid(ring, seq, record):
  ① 按 seq 定位描述符（环上二分/线性探查）
  ② 无锁复制：desc + info + 文本 memcpy 出来
  ③ 重新核对 state/ID/seq —— 期间被回收者改写则重试
```

dmesg / /dev/kmsg / kmsg_dump（panic 遍历）全走这条协议——**读路径零锁**，与写者全并发。慢读者（正被覆盖）表现为"缺号"（`/dev/kmsg` 读到的 seq 跳跃），dmesg 的 `read failed` 旧消息已成历史。

## 五、特殊写者：printk_safe 与 NMI

```
普通上下文：直写 prb
NMI/递归（printk 在 printk 里 fault）：
  printk_safe（NMI_PATH）：per-CPU 静态缓冲暂存 → 退出 NMI 后 relay
  （防止：NMI 写者在 desc 状态机中途再入 prb —— 不可重入协议自死锁）
printk_deferred：中断关闭区的延迟输出（console 部分交给 irq_work——下一章）
```

## 六、观测与实战

```
dmesg --time-format reltime/iso    # ts_ns 的两种视图
cat /dev/kmsg                      # 原始流（SYSLOG_ACTION_READ_ALL）
   注：kmsg 兼容环形 seq——读到"-----"分隔即覆盖发生
boot 参数 log_buf_len=16M          # 环大小（CONFIG_LOG_BUF_SHIFT 默认 17→128K）
/sys/module/printk/parameters/*    # always_kmsg_dump 等
dump 验证：echo c > /proc/sysrq-trigger → panic 的 kmsg_dump
             路径回读（ras 分册 kdump 联动）
```

## 七、思考题

1. 数据环淘汰（`data_push_tail`）为什么必须先确认"目标描述符已是 finalized/可复用"？若漏查一个 committed 态的记录会发生什么？
2. `prb_read_valid` 的复制-验证协议中，"验证失败重试"的上界由什么决定？（提示：读速 vs 写速与环大小。）
3. caller_id 为什么区分 task/nmi/printk_safe 三类？（对照 dmesg 里 `[nmi]` 标注的来源。）
4. cont（续行）语义在 6.6 已死：`pr_cont` 打印的原子性无保证——给出一个多 CPU 交织把 KERN_CONT 输出撕碎的实例。
5. 环满时新写者会不会阻塞？（对照 fail 计数与静默丢消息的语义。）

---

> 下一章：[console 驱动模型](02-console驱动模型.md) —— prb 之外的第二半：谁把日志搬出内核。
