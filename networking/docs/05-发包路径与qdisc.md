# 发包路径与 qdisc

发包是收包的"镜像"，但多了两个独有组件：**qdisc（排队规则）**——软件 shaping/AQM/优先级的实现层；**BQL（Byte Queue Limits）**——驱动环的字节级背压。本章从 `__dev_queue_xmit()`（net/core/dev.c）出发，走通 `__dev_xmit_skb → __qdisc_run → qdisc_restart → sch_direct_xmit → ndo_start_xmit` 全链路，并以 virtio_net 为样本解读驱动 TX 环与完成回收。

### 🎯 路径总览

```text
协议栈 (ip_output / neigh_output / ...)
 └→ dev_queue_xmit(skb) → __dev_queue_xmit(skb, sb_dev)   [net/core/dev.c]
     ├─ egress netfilter/tc 挂钩 (nf_hook_egress / sch_handle_egress)
     ├─ 选 TX 队列: netdev_core_pick_tx (XPS/哈希, 见第 3 章)
     ├─ q = txq->qdisc
     ├─ q->enqueue 存在? ──┬─ 是 → __dev_xmit_skb(skb, q, dev, txq)
     │                     │      (qdisc 路径: 绝大多数物理设备)
     │                     └─ 否 → 无队列快速路径 (loopback/tunnel:
     │                            validate 后直接 HARD_TX_LOCK + xmit)
     └─ netdev_start_xmit → __netdev_start_xmit
         └→ dev->netdev_ops->ndo_start_xmit(skb, dev)     [驱动]
```

### 🚀 __dev_queue_xmit 全链路（6.6 实测）

```text
__dev_queue_xmit(skb, sb_dev)
 ├─ skb_reset_mac_header; skb_assert_len
 ├─ SKBTX_SCHED_TSTAMP? → __skb_tstamp_tx(..., SCM_TSTAMP_SCHED)
 ├─ rcu_read_lock_bh()            /* 关 BH + RCU: 全程保护 */
 ├─ skb_update_prio(skb)          /* net_prio cgroup(静态键,默认跳过) */
 ├─ qdisc_pkt_len_init(skb)       /* 计算"线上长度"写入 qdisc_skb_cb:
 │     GSO 包 = 数据长 + (gso_segs-1)×各层头长 —— qdisc 计费按真实字节 */
 ├─ CONFIG_NET_EGRESS (静态键):
 │    ├─ nf_hook_egress (netfilter egress, 若启用)
 │    └─ sch_handle_egress(skb, ...)   /* tc egress / tcx BPF: 可改包/丢包 */
 ├─ IFF_XMIT_DST_RELEASE? skb_dst_drop : skb_dst_force
 │    /* 趁 dst 还热尽早释放引用 —— 一个微优化胜过千次 cache miss */
 ├─ txq = netdev_core_pick_tx(dev, skb, sb_dev)  /* XPS/哈希选队列 */
 ├─ q = rcu_dereference_bh(txq->qdisc)
 ├─ q->enqueue?
 │    ├─ 有 → __dev_xmit_skb(...)            /* 下文展开 */
 │    └─ 无 (noqueue 设备) →
 │        if (dev->flags & IFF_UP && txq->xmit_lock_owner != cpu) {
 │            skb = validate_xmit_skb(skb, dev, &again);  /* 见下 */
 │            HARD_TX_LOCK(dev, txq, cpu);
 │            if (!netif_xmit_stopped(txq))
 │                skb = dev_hard_start_xmit(skb, dev, txq, &rc);
 │            HARD_TX_UNLOCK(dev, txq);
 │        }  /* 递归保护: dev_xmit_recursion 计数, 防虚拟设备套娃死循环 */
 └─ rcu_read_unlock_bh()
```

**`validate_xmit_skb()`——发往驱动前的最后校验**（dev.c）：

```text
validate_xmit_skb(skb, dev, &again)
 ├─ features = netif_skb_features(skb)   /* vlan/tunnel 特性交集修正 */
 ├─ validate_xmit_vlan: 需要则硬件 tag 不到位就 push 802.1Q
 ├─ netif_needs_gso(skb, features)?
 │    /* 设备不支持该 GSO 类型/特性被关 → 软件分段 */
 │    segs = skb_gso_segment(skb, features)
 │      /* 逐段复制头, 分摊 truesize; 段间以 skb->next 串链
 │         分段前 master 已被 orphan(记账已归还 socket) */
 ├─ else:
 │    ├─ skb_needs_linearize? → __skb_linearize    /* 拍平 frags */
 │    └─ CHECKSUM_PARTIAL && 设备不 offload?
 │          → skb_csum_hwoffload_help → skb_checksum_help
 │             /* 软件算全量 L4 校验和: TSO 的 fallback 必经之路 */
 └─ validate_xmit_xfrm(skb, features, &again)  /* XFRM offload 处理 */
```

软件 TSO 分段（`skb_gso_segment` → `__skb_gso_segment`）是发包路径最贵的操作之一：一个 64KB GSO 包拆成 44 段要复制 44 份头并重新校验——所以"设备开了 TSO 却被 vlan/tunnel 丢特性"的配置会静默吃掉大量 CPU。

### 🎯 Qdisc 层：__dev_xmit_skb

```text
__dev_xmit_skb(skb, q, dev, txq)            [net/core/dev.c]
 ├─ qdisc_calculate_pkt_len(skb, q)          /* Stab(尺寸→字节)修正 */
 │
 ├─ TCQ_F_NOLOCK (lockless qdisc, 6.6 主流):
 │    ├─ 空队列 + TCQ_F_CAN_BYPASS + qdisc_run_begin(q) 成功?
 │    │    /* 快速旁路: 无排队需求时连队都不入, 直接发! */
 │    │    └─ sch_direct_xmit(skb, ...) → 成功即返回 NET_XMIT_SUCCESS
 │    ├─ 否则 dev_qdisc_enqueue(skb, q, &to_free, txq)  /* 入队, 满则丢 */
 │    └─ qdisc_run(q)  /* 竞态失败置 __QDISC_STATE_MISSED 由别人续跑 */
 │
 └─ 传统锁 qdisc:
     ├─ contended = qdisc_is_running(q) || PREEMPT_RT
     │    /* 竞争启发: 有人正在跑 → 先抢 busylock 排队, 让跑者尽快拿 root_lock */
     ├─ spin_lock(root_lock = qdisc_lock(q))
     ├─ __QDISC_STATE_DEACTIVATED? → drop   /* 换 qdisc 中途, 丢弃 */
     ├─ CAN_BYPASS && qdisc_qlen==0 && qdisc_run_begin(q)?
     │    /* 队列空 + work-conserving: 等价于"trylock 后直发" */
     │    └─ sch_direct_xmit(...) 失败才 __qdisc_run
     └─ else: dev_qdisc_enqueue → qdisc_run_begin(q) → __qdisc_run(q)
        /* __QDISC_STATE_SCHED 位防并发运行: 已有人在跑则只入队 */

__qdisc_run(q)                               [net/sched/sch_generic.c]
 └─ while (qdisc_restart(q, &packets)) {     /* 循环直到队列空/受限 */
        quota -= packets;
        if (quota <= 0) {                    /* quota = dev_tx_weight, 默认 64 */
            if (TCQ_F_NOLOCK) set_bit(__QDISC_STATE_MISSED);
            else __netdev_schedule_qos(q);   /* 挂 output_queue + NET_TX */
            break;                           /* 让出 CPU, 交给软中断续 */
        }
     }

qdisc_restart(q, &packets)
 ├─ skb = dequeue_skb(q, &validate, packets) /* dequeue_packet 包装:
 │     先 try peek 再真 dequeue; 返回 NULL 即队空 */
 ├─ txq = skb_get_tx_queue(dev, skb)         /* 复用入队时的 queue_mapping */
 └─ sch_direct_xmit(skb, q, dev, txq, root_lock, validate)

sch_direct_xmit(skb, q, dev, txq, root_lock, validate)
 ├─ spin_unlock(root_lock)          /* 发送期间放掉 qdisc 锁! */
 ├─ validate? skb = validate_xmit_skb_list(skb, dev, &again)
 ├─ HARD_TX_LOCK(dev, txq, cpu)     /* 拿驱动 TX 队列锁 */
 ├─ netif_xmit_frozen_or_stopped(txq)?
 │    └─ qdisc_maybe_clear_missed(q, txq)   /* 顺路清 MISSED, 防卡死 */
 ├─ else dev_hard_start_xmit(skb, dev, txq, &ret)
 ├─ HARD_TX_UNLOCK
 ├─ ret == NETDEV_TX_BUSY?
 │    └─ dev_requeue_skb(skb, q)    /* 挂回队头(requeue 链), 等队列唤醒 */
 └─ spin_lock(root_lock) 返回
```

**两级锁的交接**是这条路径的灵魂：qdisc 的 `root_lock`（保护队列数据结构）与 txq 的 `_xmit_lock`（保护驱动环）**互斥持有**——`sch_direct_xmit` 先放前者再拿后者，避免"拿着队列锁等驱动"，其它 CPU 可以并发入队。

TX 回退状态的完整联动图（qdisc 状态位 × 队列状态位）：

```text
                    ┌─────────────────────────────────────┐
   发送成功          │ txq->state: __QUEUE_STATE_DRV_XOFF=1│
   ┌────────┐       │ qdisc:   __QDISC_STATE_SCHED=1      │
   │        │       └──────────────┬──────────────────────┘
   ▼        │                      │ 驱动环满(BQL/描述符不足)
 ndo_ok     │                      ▼
 complet-   │            dev_requeue_skb / 停 dequeue
 ion+free   │                      │
   ▲        │                      │ TX 完成回收
   │        ▼                      ▼
   │   netif_tx_wake_queue ──► qdisc_run 续跑
   │   (清 DRV_XOFF)         (清 SCHED, 或 NOLOCK 下清 MISSED)
   │                                │
   └── NETDEV_TX_BUSY(重入队) ◄──────┘
       兜底: 驱动短暂不可用, 包回 qdisc 队头, 等 wake

   另两个独立位:
   __QDISC_STATE_DEACTIVATED: 换 qdisc 中途, enqueue 直接丢弃
   __QUEUE_STATE_FROZEN:      队列重建(通道切换/复位), 整体冻结
```

**回退与续跑闭环**（设备暂时不能收）：

```text
驱动 netif_tx_stop_queue → 回退
 └→ dev_requeue_skb / 直接不 dequeue
 └→ __netdev_schedule_qos: q 挂本 CPU sd->output_queue
     + __raise_softirq_irqoff(NET_TX_SOFTIRQ)
驱动 TX 完成后 netif_tx_wake_queue (或 napi poll 中 free_old_xmit_skbs)
 └→ NET_TX_SOFTIRQ → net_tx_action(sd)          [dev.c]
      ├─ 先清 completion_queue(IRQ 上下文攒下的待释放 skb, 见下文)
      └─ 遍历 output_queue: 每个 qdisc 再 __qdisc_run (续跑)
```

### 🗃️ qdisc 概览与默认选择

| qdisc | 类型 | 机制 | 典型用途 |
| :--- | :--- | :--- | :--- |
| `noqueue` | — | `enqueue==NULL`，`__dev_queue_xmit` 直通 | loopback/隧道（IFF_NO_QUEUE） |
| `pfifo_fast` | classless | 3 个 band FIFO；`prio2band[16]`（`1,2,2,2,1,2,0,0,1,...`）按 `skb->priority`（DSCP/TOS 映射后）选 band；band0 最优先 | 默认兜底（`default_qdisc_ops`） |
| `mq` | 分类根 | 多队列设备的根 qdisc，`attach()` 时每 txq 挂一个子 qdisc | 多队列 NIC 的默认 root |
| `fq_codel` | classless AQM | flow 队列 + CoDel 时延控制 | 现代默认推荐 |
| `cake` | classless AQM | fq_codel 加强（shaper/区分队列） | 家用 WAN |
| `htb`/`tbf` | classful | 令牌桶分层限速 | 云主机/多租户 |
| `fq` | classless | EDT 友好 pacing（`skb_mstamp_ns`） | TCP BBR 标配 |

**默认选择规则**（6.6 实测）：

```text
attach_default_qdiscs(dev)                      [sch_generic.c]
 ├─ 单队列或 IFF_NO_QUEUE → attach_one_default_qdisc(每队列):
 │     IFF_NO_QUEUE  → noqueue_qdisc_ops
 │     ARPHRD_CAN    → pfifo_fast_ops
 │     其余          → default_qdisc_ops        /* 全局变量 */
 ├─ 多队列 → mq_qdisc_ops 作 root; 每队列子 qdisc 仍是 default_qdisc_ops
 └─ 失败兜底 → 退回 noqueue

default_qdisc_ops 初值 = &pfifo_fast_ops       [sch_generic.c:35]
改变方式: sysctl net.core.default_qdisc = "fq" (qdisc_set_default)
          或 CONFIG_NET_SCH_DEFAULT + CONFIG_DEFAULT_NET_SCH_FQ
          (late_initcall(sch_default_qdisc) 在启动末尾覆盖)
```

**lockless qdisc（TCQ_F_NOLOCK）**：pfifo_fast 等简单队列在 6.6 默认以无锁模式运行——`qdisc_run_begin` 改为 `spin_trylock(&q->seqlock)`，失败者入队后置 `__QDISC_STATE_MISSED` 让占有者续跑；入队/出队走 per-CPU 或 seqcount 保护。这消除了多核发送下对 root_lock 的 cacheline 争抢（配合 XPS 分队列后，每 CPU 每 qdisc 独立运行）。

**tc 用户态接口**：`tc qdisc add dev eth0 root fq_codel` 经 rtnetlink `RTM_NEWQDISC` 系列消息（`struct tcmsg` + 嵌套属性 TCA_OPTIONS）下发，内核 `tc_modify_qdisc`（持 RTNL）走 `qdisc_create`/`qdisc_graft`：graft = 构新 qdisc → `dev_deactivate`（数据面切 noop + `synchronize_net`）→ 换 `txq->qdisc_sleeping` → `dev_activate`。老 qdisc 排空后经 RCU 释放。常用命令对照：

```bash
tc qdisc replace dev eth0 root fq_codel limit 8192 target 5ms   # 无类 AQM
tc qdisc replace dev eth0 root handle 1: htb default 30          # 分层限速
tc class add dev eth0 parent 1: classid 1:30 htb rate 100mbit
tc filter add dev eth0 parent 1: protocol ip u32 match ip dport 22 0xffff flowid 1:10
tc -s -d qdisc show dev eth0        # -s 带统计: qlen/dropped/backlog
```

### 📏 BQL：Byte Queue Limits

qdisc 只管"软件队列"；而驱动 TX 描述符环是另一条不受 qdisc 约束的队列（硬件 FIFO + 环内待完成帧）。环太深 = 队头占用 + bufferbloat；环太浅 = 吞吐塌陷。BQL 用**字节级动态上限**自动把环内 in-flight 调到刚好够带宽×RTT：

```c
struct dql {                        /* include/linux/dynamic_queue_limits.h */
    /* enqueue 侧(xmit 时写): */
    unsigned int num_queued;        /* 累计入队字节数 */
    unsigned int adj_limit;         /* limit + 本轮完成量(快照, 供 dql_avail) */
    unsigned int last_obj_cnt;      /* 上次入队量 */
    /* completion 侧(TX 完成时写, 独立缓存行): */
    unsigned int limit ____cacheline_aligned_in_smp;  /* 当前字节上限 */
    unsigned int num_completed;
    unsigned int prev_ovlimit, prev_num_queued, prev_last_obj_cnt;
    unsigned int lowest_slack;  unsigned long slack_start_time;
    /* 配置: */
    unsigned int max_limit, min_limit, slack_hold_time;
};
```

驱动用法（模板，真实驱动如 virtio 未用 BQL，物理网卡普遍使用）：

```text
ndo_start_xmit:
    if (dql_avail(&txq->dql) < skb->len) {   /* 超限 */
        netif_tx_stop_queue(txq);            /* 停队列, 让 qdisc 排管 */
        return NETDEV_TX_BUSY;               /* 或先入队后停 */
    }
    ... 写描述符 ...
    netdev_tx_sent_queue(txq, skb->len);     /* dql_queued: num_queued += len
                                                若越过 limit → 立即停队列 */

TX 完成中断/NAPI poll:
    netdev_tx_completed_queue(txq, pkts, bytes);
      /* dql_completed(bytes): 见下算法; 若队列已停且
         in-flight 回到 limit 以下 → netif_tx_wake_queue 唤醒 */
```

**`dql_completed()` 的自适应算法**（lib/dynamic_queue_limits.c，实测源码归纳）：

*   **饥饿（starved）判定**：上一周期越过 limit 且队列已排空（`ovlimit && !inprogress`），或上周期越限且本轮全部完成——说明 limit 太小，喂不饱硬件 → `limit += 本周期完成量 + 上周期越限量`，即"放开到观察到的实际吞吐"。
*   **收缩（slack）判定**：连续两周期都在途非空且未全完成——队列一直忙，检查是否有"富余"（slack = 在途与两倍完成量的正差，取多周期最小值 `lowest_slack` 抗抖动）——超过 `slack_hold_time` 后 `limit -= lowest_slack`，即"裁掉从未用到的部分"。
*   结果：limit 收敛到"恰好保持硬件满速的最小 in-flight 字节"，逼近 BDP（带宽×传播时延）。**这直接对抗 bufferbloat**：传统深环（如 512 描述符 × 4KB）可缓存数 MB，排队时延秒级；BQL 化后多余字节被挡在 qdisc（可被 AQM/shaping 治理），驱动环不再藏私队。

sysfs：`/sys/class/net/DEV/queues/tx-N/byte_queue_limits/{limit,limit_max,limit_min,hold_time}` 可观测与调整。

### 📤 发包完成与释放路径

skb 进入驱动后，"谁在什么上下文释放它"由驱动模式决定，这是发包路径最容易写错的部分：

```text
                       skb 被 ndo_start_xmit 送走
                              │
        ┌─────────────────────┼──────────────────────────┐
        │ NAPI TX 模式        │ TX 中断模式               │ 无中断(空轮询)
        │ (virtio/现代网卡)   │ (老网卡)                  │ (netconsole 轮询)
        ▼                     ▼                           ▼
  RX poll 顺手收割        TX 完成硬中断                 netpoll poll
  free_old_xmit_skbs(sq, in_napi=true)
   │ napi_consume_skb(skb, budget)
   │   ├ budget==0? → dev_consume_skb_any (netpoll 语义)
   │   ├ fclone?    → __kfree_skb (回 fclone slab)
   │   └ 否则: skb_release_all(SKB_CONSUMED, napi_safe=true)
   │           + napi_skb_cache_put(skb)   ← 外壳进 per-CPU 缓存!
   │           (napi_safe 使 page_pool 页直接回 alloc cache)
   ▼
  dev_kfree_skb_irq(skb)     dev_kfree_skb(skb)   (进程上下文)
   │ IRQ 内不能析构 → 挂 sd->completion_queue
   │ (net_tx_action 前由 __dev_kfree_skb_irq 拉链)
   └→ raise NET_TX_SOFTIRQ
        net_tx_action(sd)                       [NET_TX softirq]
         ├─ 摘下 completion_queue 逐个:
         │    reason==SKB_CONSUMED? trace_consume_skb : trace_kfree_skb
         │    fclone? __kfree_skb : __napi_kfree_skb(napi_safe=false)
         └─ 顺路跑 output_queue 上的 qdisc (续跑被停队列)
```

**上下文规则速查**（与第 2 章呼应，这里是驱动视角）：

| 释放调用 | 使用上下文 | 特殊语义 |
| :--- | :--- | :--- |
| `napi_consume_skb(skb, budget)` | NAPI poll（softirq） | 外壳/页全走 per-CPU 快速缓存 |
| `dev_kfree_skb_irq(skb)` | TX 完成硬中断 | 只挂链不析构，真释放在 NET_TX softirq |
| `dev_kfree_skb(skb)` / `dev_consume_skb_any(skb)` | 进程/任意上下文（`_any` 自动判上下文） | 直接析构 |
| `xdp_return_frame(frame)` | NAPI/XDP 上下文 | 作用于 `xdp_frame`（非 skb），走 xdp_mem 回收（含 page_pool bulk） |

completion_queue 的设计动机：硬中断里做 `skb_release_data`（可能触发 page_pool/`sock_wfree`→`wake_up`）是禁忌，而挂链+置 softirq 位只需关中断几条指令——**把昂贵工作搬到 NET_TX_SOFTIRQ 批量执行**，与 RX 侧"中断只调度、softirq 干活"完全对称。

### 🛞 驱动 TX 环模型（virtio_net `start_xmit`，6.6 实测）

```c
static netdev_tx_t start_xmit(struct sk_buff *skb, struct net_device *dev)
{
    int qnum = skb_get_queue_mapping(skb);        /* 选队列的结果在此兑现 */
    struct send_queue *sq = &vi->sq[qnum];
    bool kick = !netdev_xmit_more();              /* BQL 同思路的批量提示:
                                                     栈连续发包时(BH 内)不 kick,
                                                     攒到软中断末尾一次性通知 */
    ...
    do {  free_old_xmit_skbs(sq, false);  }       /* 投机性收割完成环:
                                                     /*      napi_consume_skb/计数 */
    ...
    err = xmit_skb(sq, skb);                      /* hdr 补 virtio 头+描述符入环:
                                                     /*      支持 SG(TSO): frags 直接挂页 */
    if (!use_napi) { skb_orphan(skb); nf_reset_ct(skb); }
        /* 非 napi 模式: 驱动收走后立刻 orphan —— socket 记账即时归还,
           skb 的析构不再依赖 TX 完成时序 */
    check_sq_full_and_disable(vi, dev, sq);
        /* sq->vq->num_free < MAX_SKB_FRAGS + 2 → netif_tx_stop_queue
           (给最大的 frag 包留余量); 同时 schedule napi/延迟work 盯完成 */
    if (kick || netif_xmit_stopped(txq))
        if (virtqueue_kick_prepare(sq->vq) && virtqueue_notify(sq->vq)) ...
            /* 通知设备(doorbell); virtio 的 kick 是显式 mmio/notify,
               所以批量策略直接影响吞吐 */
    return NETDEV_TX_OK;   /* 注意: virtio 永不返回 BUSY —— 停队列代替 */
}
```

要点：

*   **TX 完成的两条路**：napi 模式（`use_napi`）由收包 poll 顺手 `free_old_xmit_skbs`（第 4 章 virtnet_poll_cleantx），skb 走 `napi_consume_skb`（外壳进 per-CPU 缓存、page_pool 页直接回池）；非 napi 模式靠 TX 中断/virtqueue 回调，IRQ 上下文必须 `dev_kfree_skb_irq`。
*   **`skb_orphan` 与 TSO/NAT**：进入设备层前协议栈已调用 `skb_orphan`（TCP 在 clone 时就换了归属）。软件 TSO 分段（`skb_gso_segment`）要求 master 已 orphan——否则 44 个段每个都背着 socket wmem 记账，`sk_wmem_alloc` 立刻爆表。NAT 场景：netfilter 改写 IP/端口后必须对 `CHECKSUM_PARTIAL` 包调 `skb_checksum_help`/增量重算（nf_nat 的 `nf_nat_ipv4_fn` 在 mangle 时处理），否则硬件 offload 会把错伪头和进 L4 校验和。
*   **TX timeout**：`dev_watchdog` 发现 `trans_start` 距今超 `watchdog_timeo` → `ndo_tx_timeout` + `trans_timeout++`——virtio 这类"永不 BUSY"驱动若 kick 丢失就靠它暴露。

### ⚡ XDP TX 与 __dev_forward_skb

*   **XDP_TX**：native XDP 程序返回 `XDP_TX` 时，驱动把 `xdp_buff` 原样塞回**同一网卡**的 TX 环（`ndo_xdp_xmit` 系列，virtio 的 `virtnet_xdp_xmit` 用 `xdp_frame` 而非 skb）——零 skb、零拷贝的 hairpin 转发。批量语义：驱动在 NAPI 循环里攒 frame，`xdp_do_flush` 统一 kick（`xdp_bulkq`）。
*   **XDP_REDIRECT**：跨设备转发（`bpf_redirect`），帧先进 per-CPU bulk 队列，flush 时 `ndo_xdp_xmit` 到目标设备。
*   **`__dev_forward_skb(dev, skb)`**：把一个已有 skb 重新注入**接收路径**（bridge/mpls/`bpf_redirect` 到另一 netdev 的 skb 形态）：清头检查（`skb_cow_head` 确保可写）→ 设 `skb->dev` → `netif_rx_internal()`（即第 4 章的 backlog 入队/RPS 路径）。与 XDP 的区别：它处理的是 skb，代价高但功能全（可跨 netns 边界检查）。

### ⏱️ TX 端到端时序图

```text
CPU#3 (发送进程/softirq)          CPU#3 (NET_TX_SOFTIRQ)        设备 (virtqueue)
────── tcp_sendmsg ──────         ────── net_tx_action ──────   ──── DMA ────
copy_from_user → skb
tcp_write_xmit (cwnd/mss 判定)
__tcp_transmit_skb(clone+orphan?)
ip_queue_xmit → 路由缓存 dst
neigh_output → eth_header
dev_queue_xmit
 └ __dev_queue_xmit
    ├ sch_handle_egress (无规则≈跳过)
    ├ netdev_core_pick_tx [XPS 命中: 队列2]
    ├ __dev_xmit_skb
    │   ├ qdisc_run_begin(q) 成功
    │   ├ enqueue(fq) + EDT 时间戳调度
    │   └ qdisc_restart → dequeue
    │       └ sch_direct_xmit
    │           ├ unlock qdisc root_lock
    │           ├ validate_xmit_skb [csum/TSO 由设备扛]
    │           ├ HARD_TX_LOCK(txq2)
    │           └ ndo_start_xmit ──────────────────────────► 描述符入环
    │               xmit_skb: push virtio_hdr;               virtqueue_add
    │               kick=false (xmit_more!)                   [等下一包]
    │               最后一包时 kick ────────────────────────► virtqueue_notify
    │               (num_free 低 → netif_tx_stop_queue)      设备消费 DMA
    ...
 qdisc quota 尽 → __QDISC_STATE_SCHED
   + output_queue + NET_TX_SOFTIRQ ──► 续跑剩余 dequeue          │
                                                              TX 完成
                              free_old_xmit_skbs ◄──────────── 描述符回收
                              napi_consume_skb (napi_safe)
                                ├ page_pool 页回 alloc cache
                                └ 外壳进 napi_skb_cache
                              num_free 恢复 → netif_tx_wake_queue
                                └ qdisc 续跑 (MISSED/SCHED)
```

对照收包时序（第 4 章）：TX 的"中断→softirq"只出现在**完成回收**一侧；发送本身可以完全在进程上下文（syscall 内一路下探到 `ndo_start_xmit`），这正是 zero-syscall-sendfile/io_uring 直发能成立的原因。

### 🔭 观测与调优

*   **`tc -s qdisc show dev eth0`**：`sent/dropped/backlog/qlen` + qdisc 私有统计（fq 的 `flows`、fq_codel 的 `target/interval/ecn_marks`）。
*   **`/sys/class/net/eth0/queues/tx-N/`**：`xps_cpus`、`tx_maxrate`、`byte_queue_limits/*`、`trans_timeout`。
*   **`ethtool -k eth0`**：`tx-checksumming/tso/gso/sg`——任何一项意外关闭都会把 `validate_xmit_skb` 打进慢路径。
*   **per-CPU softnet**：TX 侧看 `time_squeeze`（NET_TX_SOFTIRQ 预算同样受限）。
*   **iperf 高吞吐调参清单**（常见顺序）：
    1.  `ethtool -L eth0 combined N` 队列数对齐 CPU；IRQ 亲和到独立核（`/proc/irq/*/smp_affinity`）；
    2.  XPS 与 RSS 对称（`xps_cpus` 镜像 `rps_cpus`/IRQ 亲和）；
    3.  `sysctl net.core.netdev_budget=600`（+`netdev_budget_usecs=8000`）缓解 time_squeeze；
    4.  TCP 缓冲 `net.ipv4.tcp_rmem/tcp_wmem` 拉大（BBR/fq 组合：`net.core.default_qdisc=fq`）；
    5.  确认 offload 全开（`ethtool -k`），必要时 `tx-udp-segmentation`（UDP GSO）；
    6.  突发丢包看 `netdev_max_backlog` 与 qdisc `dropped`；`tc -s` 的 backlog 持续高 → 上 BQL/AQM（fq_codel）。

### 🧠 深挖难点

*   **为什么 `sch_direct_xmit` 要在拿驱动锁之前释放 qdisc 锁？** 两个锁保护的临界区不同（队列结构 vs 硬件环），交叉持锁会让"入队者"等"发送者"等"驱动 DMA"，把并发度打回串行；先放再拿是吞吐关键（代价是重入队时需重新拿锁，见 `dev_requeue_skb`）。
*   **`netdev_xmit_more()`（`sd->xmit.more`）的批量语义**：BH 关闭期间连续 `dev_queue_xmit` 多个包时，驱动把 kick/通知推迟到最后一包——与门铃寄存器/mmio 写的代价直接相关（一次 mmio 写 ≈ 1μs）。这是发送路径唯一显式的"跨包状态"。
*   **qdisc `TCQ_F_CAN_BYPASS` 的哲学**：对 work-conserving（不整形）的 qdisc，"队列空 + 没人在跑"时入队再立刻出队是纯浪费——直接把 skb 送 `sch_direct_xmit`，行为与 noqueue 无异；所有状态回退路径（BUSY/stop）都会把包安全送回队列。

### ❓ 思考题

1.  `pfifo_fast` 与 `noqueue` 都"不做整形"，为什么 loopback 必须用 noqueue 而 eth0 用 pfifo_fast？
    *提示：noqueue 的 `enqueue==NULL` 让 `__dev_queue_xmit` 走无锁快路径；eth0 的驱动可能返回 BUSY，需要队列承接重试。*
2.  一个 1500B 的包入 `fq` qdisc 后 `tc -s` 显示 backlog=2000B，为什么比 skb->len 大？
    *提示：`qdisc_pkt_len_init` 对 GSO 包按段数×头长折算"线上长度"。*
3.  BQL 收敛后的 `limit` 与 TCP BDP（带宽×RTT）是什么关系？为什么说"驱动环深 4096 描述符"在长肥管道里反而是病？
    *提示：limit 下界是"喂饱硬件"所需 in-flight；多余排队只是加时延（bufferbloat），AQM 在 qdisc 层才治理得了。*
4.  `NETDEV_TX_BUSY` 与 `netif_tx_stop_queue` 的分工？返回 BUSY 的驱动与"永不 BUSY"的驱动（virtio）在 qdisc 层分别走哪条回退路径？
    *提示：BUSY → `dev_requeue_skb`（回队头）；stop 队列 → 不 dequeue 直至 wake。*
5.  为什么软件 TSO 分段前必须 `skb_orphan`？若 GSO skb 带着已置位的 destructor 进入分段，`sk_wmem_alloc` 会出现什么？
    *提示：每段 `seg->destructor` 均非空时释放会重复执行 sock_wfree 记账；master 记账在分段时被一次性折算到各段 truesize。*
6.  `net_tx_action` 同时处理 completion_queue 与 output_queue，这两条链的"生产者→消费者"上下文分别是什么？
    *提示：completion 的生产者是硬中断（dev_kfree_skb_irq），output 的生产者是任意发包上下文（`__netdev_schedule_qos`）；消费者统一是 NET_TX softirq。*
