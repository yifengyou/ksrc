# 收包路径与 NAPI

收包路径是内核中"单位时间执行次数最多"的代码路径之一：一个 64B 小包从网卡到 socket 要穿越约 20 个函数、3 次上下文切换点（硬中断→softirq→进程）。Linux 用 **NAPI（New API）混合中断/轮询模型**在高 pps 下同时压低中断开销与处理时延。本章基于 6.6 的 `net/core/dev.c`、`net/core/gro.c`、`net/core/page_pool.c` 逐层拆解，并给出端到端时序与开销分析。

### 🎯 从纯中断到 NAPI 的演进

*   **纯中断模型**（`netif_rx` 时代）：每包一次硬中断。高 pps 下中断风暴：CPU 全在处理中断上下文切换，softirq 饿死，丢包。
*   **NAPI 模型**：中断只负责"叫醒"——驱动 ISR 调 `napi_schedule_irqoff()` 后立刻关中断返回；真正的收包在 `NET_RX_SOFTIRQ` 里以 **budget 轮询**方式进行，一次 poll 收干 RX 环上所有可收的包。环收空（`napi_complete_done`）才重新开中断。高负载时中断次数与"包数"解耦，趋近常数。
*   **6.6 的演进**：`gro_flush_timeout`/`napi_defer_hard_irqs`（中断合并）、threaded NAPI（poll 移入内核线程）、busy polling（应用主动轮询）进一步覆盖时延敏感与 RT 场景。

### ⚡ 从硬中断到软中断

```text
网卡硬中断 (驱动 ISR, IRQ 已关)
 └→ napi_schedule_irqoff(&rq->napi)            [include/linux/netdevice.h 内联]
     └→ napi_schedule_prep(napi)               [net/core/dev.c]
     │    cmpxchg(&napi->state, val, val | NAPIF_STATE_SCHED)
     │    ├─ 原状态已含 SCHED → 置 NAPIF_STATE_MISSED, 返回 false
     │    │   (正在被 poll, 无需重复挂链; poll 结束时见 MISSED 会自动续)
     │    └─ 成功占用 → 返回 true
     └→ ____napi_schedule(sd, napi)             [dev.c, 要求 IRQ 关闭]
          ├─ test_bit(NAPI_STATE_THREADED)?
          │    └─ wake_up_process(napi->thread); return  (线程化路径)
          ├─ list_add_tail(&napi->poll_list, &sd->poll_list)
          ├─ WRITE_ONCE(napi->list_owner, smp_processor_id())
          └─ if (!sd->in_net_rx_action)
                raise_softirq_irqoff(NET_RX_SOFTIRQ)   /* 就地标记 */

irq_exit()                                    [kernel/softirq.c]
 └→ !local_softirq_pending() / in_interrupt 判定
     ├─ 常规: invoke_softirq → __do_softirq (本 CPU 就地执行, 限 2ms/10 次)
     └─ 超限: wakeup_softirqd → ksoftirqd/n (CFS 调度的内核线程兜底)

NET_RX_SOFTIRQ → net_rx_action(h)             [dev.c]
```

要点：

*   `napi_schedule_prep` 的 cmpxchg 是**整个 NAPI 的同步基石**："谁把 SCHED 位从 0 抢到 1，谁负责挂链"；poll 结束者若看到 MISSED 位，知道自己还得再跑一轮（`napi_complete_done` 里处理）。
*   `raise_softirq_irqoff` 只置 per-CPU `softirq_pending` 位；真正执行点在 `irq_exit`（借中断返回的"免费上下文"）或 ksoftirqd。
*   **同一个 NAPI 实例只会绑在一个 CPU 上被调度**（首次 schedule 的 CPU），这是 `pp_alloc_cache`/`napi_alloc_cache` 无锁 per-CPU 缓存安全性的前提。

### 🧵 softnet_data 与 net_rx_action

per-CPU 状态枢纽 `struct softnet_data`（include/linux/netdevice.h，`DECLARE_PER_CPU_ALIGNED`）：

```c
struct softnet_data {
    struct list_head    poll_list;        /* 待 poll 的 napi 实例 */
    struct sk_buff_head process_queue;    /* backlog 正在处理的队列 */
    unsigned int        processed;        /* /proc/net/softnet_stat 第 1 列 */
    unsigned int        time_squeeze;     /* 第 3 列: 预算耗尽计数 */
    struct softnet_data *rps_ipi_list;    /* 等待发 IPI 的远端 sd */
    bool in_net_rx_action;                /* ____napi_schedule 的防重入标记 */
    struct Qdisc    *output_queue;        /* 待运行 qdisc (发包侧, 第 5 章) */
    struct Qdisc   **output_queue_tailp;
    struct sk_buff *completion_queue;     /* 待释放 skb (IRQ 延迟 free) */
    struct { u16 recursion; u8 more; u8 skip_txqueue; } xmit;
    /* RPS 专用缓存行: */
    unsigned int    input_queue_head ____cacheline_aligned_in_smp;
    call_single_data_t csd;               /* RPS IPI 载体 */
    struct softnet_data *rps_ipi_next;
    unsigned int    cpu, input_queue_tail;
    unsigned int    received_rps;          /* 第 10 列 */
    unsigned int    dropped;               /* 第 2 列: backlog 溢出 */
    struct sk_buff_head input_pkt_queue;   /* backlog 输入队列(RPS 目标) */
    struct napi_struct backlog;            /* "伪设备" NAPI: poll=process_backlog */
    spinlock_t defer_lock; int defer_count;
    struct sk_buff *defer_list; call_single_data_t defer_csd;
};
```

软中断主循环 `net_rx_action`（6.6 实测结构）：

```text
net_rx_action
 ├─ budget = netdev_budget (默认 300), time_limit = jiffies + netdev_budget_usecs
 │    (netdev_budget_usecs 默认 2*USEC_PER_SEC/HZ, 即 2 个 jiffies)
 ├─ list_splice_init(&sd->poll_list, &list)   /* 关中断摘下,防竞态 */
 └─ for (;;) {
       skb_defer_free_flush(sd)               /* defer_list 批量释放 */
       n = list_first_entry(&list, struct napi_struct, poll_list)
       budget -= napi_poll(n, &repoll)        /* 调 __napi_poll → n->poll() */
       if (budget <= 0 || time_after_eq(jiffies, time_limit)) {
           sd->time_squeeze++;                /* 关键观测指标! */
           break;
       }
    }
    /* repoll + 追加进来的 poll_list 拼回 sd->poll_list,
       若非空则 __raise_softirq_irqoff(NET_RX_SOFTIRQ) 续跑,
       net_rps_action_and_irq_enable(sd) 顺手发 RPS IPI */
```

**time_squeeze 的语义**：本次 softirq 窗口内没干完（预算或时间耗尽），被迫挂起等下一轮。它**不是丢包**，但持续增长说明：pps 超过 softirq 配额、或 ksoftirqd 长期占用 CPU——调 `net.core.netdev_budget`/`netdev_budget_usecs`、开 RPS 分流、或线程化 NAPI。

### 🔁 NAPI 核心机制

```c
struct napi_struct {
    struct list_head poll_list;      /* 由 SCHED 位拥有者管理 */
    unsigned long    state;          /* NAPIF_STATE_* */
    int              weight;         /* 单次 poll 的包预算 */
    int              defer_hard_irqs_count;  /* IRQ 合并倒计数 */
    unsigned long    gro_bitmask;    /* gro_hash[8] 非空桶位图 */
    int            (*poll)(struct napi_struct *, int);
    int              list_owner;     /* 挂在哪个 CPU 的 poll_list */
    struct net_device *dev;
    struct gro_list  gro_hash[GRO_HASH_BUCKETS]; /* 8 桶 GRO 聚合表 */
    struct sk_buff  *skb;            /* napi_get_frags 复用外壳 */
    struct list_head rx_list;        /* GRO_NORMAL 暂存链 */
    int              rx_count;
    unsigned int     napi_id;        /* busy poll 句柄 */
    struct hrtimer   timer;          /* gro_flush_timeout 定时器 */
    struct task_struct *thread;      /* threaded NAPI 内核线程 */
    struct list_head dev_list; struct hlist_node napi_hash_node;
};
```

*   **weight**：`netif_napi_add(dev, napi, poll)` 默认 `NAPI_POLL_WEIGHT=64`；`netif_napi_add_weight()` 可自定义（大 weight = 单实例更强吞吐但更差公平性）。
*   **poll 完成的三步曲**（驱动模板）：`work < budget` 时 `napi_complete_done(napi, work)` → 内部清 SCHED/MISSED/THREADED 位，若发现 MISSED 被置（期间又有包到、ISR 又想 schedule）则**返回 false**，驱动据此重新 `napi_schedule`（或像 virtio 一样配合 `virtqueue_enable_cb_prepare/virtqueue_poll` 判断中断窗口）。`work == budget` 时**禁止**驱动动 state——net_rx_action 自己把它挪进 repoll 下一轮继续。
*   **IRQ 合并**（`gro_flush_timeout` + `napi_defer_hard_irqs`）：`napi_complete_done` 里若 `defer_hard_irqs_count>0` 或有 GRO 残留且 timeout>0，则不立刻开中断，而是 `hrtimer` 定时到期后 `napi_watchdog` 再 poll——用可控时延换中断次数（sysfs：`/sys/class/net/DEV/gro_flush_timeout`、`napi_defer_hard_irqs`）。
*   **threaded NAPI**（6.6）：`/sys/class/net/DEV/threaded=1` 或驱动侧 `dev->threaded`。`netif_napi_add` 时 `napi_kthread_create` 建 `napi->thread`；`____napi_schedule` 检测 `NAPI_STATE_THREADED` 后 `wake_up_process(thread)`——poll 从 softirq 上下文移到可抢占、可设置 RT 优先级的内核线程，PREEMPT_RT 的必选配置。
*   **busy polling**：应用 `SO_BUSY_POLL`（`sk->sk_ll_usec>0`）在 `recvmsg` 空转时直接 `napi_busy_loop(napi_id)`——用 CPU 空转省掉"中断→唤醒"微秒级时延。skb 记录来源 `napi_id`（`skb_mark_napi_id`），`napi_by_id` 经 `napi_hash` 反查实例。

### 📥 驱动 poll 实例（virtio_net，6.6）

```c
static int virtnet_poll(struct napi_struct *napi, int budget)
{
    virtnet_poll_cleantx(rq, budget);   /* 顺手收割同 queue 的 TX 完成环 */
    received = virtnet_receive(rq, budget, &xdp_xmit);
    if (xdp_xmit & VIRTIO_XDP_REDIR)
        xdp_do_flush();                 /* XDP_REDIRECT 批量冲刷 */
    if (received < budget)
        virtqueue_napi_complete(napi, rq->vq, received); /* 开中断+complete */
    ...
    return received;
}

static int virtnet_receive(struct receive_queue *rq, int budget, ...)
{
    while (packets < budget &&
           (buf = virtnet_rq_get_buf(rq, &len, &ctx))) {  /* 取描述符 */
        receive_buf(vi, rq, buf, len, ctx, xdp_xmit, &stats);
        /* receive_buf: 小包→build_skb 线性; 大包→napi_alloc_skb +
           skb_add_rx_frag(挂页); 走 napi_gro_receive 或 XDP 先行 */
        packets++;
    }
    if (rq->vq->num_free > min(budget, vring_size)/2)
        try_fill_recv(vi, rq, GFP_ATOMIC);  /* 补充 RX 描述符(半空才补) */
    return packets;
}

static void virtqueue_napi_complete(struct napi_struct *napi,
                                    struct virtqueue *vq, int processed)
{
    int opaque = virtqueue_enable_cb_prepare(vq);  /* 重新挂回调窗口 */
    if (napi_complete_done(napi, processed)) {
        if (unlikely(virtqueue_poll(vq, opaque)))  /* 窗口内又来了? */
            virtqueue_napi_schedule(napi, vq);     /* 立刻再 poll */
    } else
        virtqueue_disable_cb(vq);                  /* defer 模式: 不开中断 */
}
```

通用模板抽象为：**取环 → 构 skb（build_skb/add_rx_frag）→ XDP → napi_gro_receive → 补环 → complete**。virtio 的"cleantx"把 TX 完成也塞进 RX poll（同队列同 CPU，一举消灭 TX 中断），是 6.6 驱动的潮流写法。

### 🌊 GRO 深挖（net/core/gro.c）

GRO（Generic Receive Offload）在 NAPI 层把**同流连续小包**聚合成大 skb，目标是摊薄 per-packet 的 L3/L4 处理成本；聚合产物直接就是合法的 GSO skb（`gso_size` 记录段长），发给协议栈"当大包处理"。

```text
napi_gro_receive(napi, skb)
 ├─ skb_mark_napi_id(skb, napi)          /* busy poll 记号 */
 ├─ skb_gro_reset_offset(skb, 0)         /* NAPI_GRO_CB 初始化(frag0 等) */
 └─ napi_skb_finish(napi, skb, dev_gro_receive(napi, skb))
      │
     dev_gro_receive:                       [gro.c]
      ├─ bucket = skb_get_hash_raw(skb) & 7  /* GRO_HASH_BUCKETS=8 */
      ├─ gro_list_prepare(&gro_list->list, skb)
      │    /* 逐个比对已挂包: eth/ip头/tcph 全等且无脏 flag → same_flow */
      ├─ 查 offload_base(struct packet_offload, inet_gro_receive 注册)
      │    没有对应 Ethertype 的回调 → GRO_NORMAL(不聚合,直送上送)
      ├─ NAPI_GRO_CB(skb) 初始化:
      │    count=1 (GSO 包则=gso_segs), flush=有 frag_list,
      │    csum_valid/csum_cnt ← CHECKSUM_COMPLETE/UNNECESSARY 折算
      ├─ ptype->callbacks.gro_receive(&gro_list->list, skb)
      │    /* L2=inet_gro_receive → L4=tcp_gro_receive: 检查 seq 连续性、
      │       flags 可合并性、csum 合法性 → MERGE(塞 frags/frag_list)
      │       或返回 pp(超龄/需 flush 的旧包) */
      ├─ 若 same_flow → GRO_MERGED / GRO_MERGED_FREE(新包被并入旧包, 旧壳复用)
      ├─ 若 flush(不可并) → GRO_NORMAL
      └─ 否则挂入 gro_list (count>MAX_GRO_SKBS=8 时 gro_flush_oldest 挤出最老的)
      └─ 维护 napi->gro_bitmask 桶位图

 napi_skb_finish 的善后:
  GRO_NORMAL → gro_normal_one(napi, skb, 1)  → 挂 napi->rx_list
  GRO_MERGED_FREE → 释放被吸收的新壳
  (napi_complete_done / __napi_poll 末尾统一 gro_normal_list(napi):
   把 rx_list 逐包 __netif_receive_skb_list 上送 —— 6.6 的批量上送)
```

**6.6 的实现细节**：

*   **桶选择用原始哈希**（`skb_get_hash_raw`，不做完整 flow dissector），8 桶把不同流天然分开，同桶再由 `gro_list_prepare` 精确比对（MAC/IP/TCP 头逐字段 diff，`NAPI_GRO_CB(p)->same_flow = !diffs`）。
*   **每桶上限 `MAX_GRO_SKBS=8`**：挂第 9 个时 `gro_flush_oldest` 强制完成最老的一个（`napi_gro_complete` → L4 `gro_complete` 回调修正头部长度/校验和 → `gro_normal_one` 上送）。
*   **flush 的常见触发**：TCP flags 变化（SYN/FIN/RST/URG 等）、seq 不连续、`skb_has_frag_list`（已有 frag_list 的包不可再并）、GSO 但非 TCP 或 `SKB_GSO_DODGY`、`skb->slow_gro`（带 ct/orphan 等 GRO 不友好状态）。
*   **napi_gro_frags 模式**：驱动连线性头都不想单独分配（如某些 HDRSPLIT 硬件），`napi_get_frags(napi)` 复用 `napi->skb`，头从 `NAPI_GRO_CB(skb)->frag0`（首个 frag 的头部窥视）构造，收完 `napi_frags_complete` 套上 ETH 头再走合并逻辑。
*   **gro_cells**：隧道设备（ipip/gre 收包函数里）用预分配的 `gro_cell`（per-CPU 的 napi+队列）把解封装包重新 GRO，抵消"解封装后包又变小"的损失。
*   **GRO vs LRO**：

| 维度 | LRO（硬件） | GRO（软件） |
| :--- | :--- | :--- |
| 实现位置 | NIC 固件 | gro.c，随 NAPI poll |
| 合法性 | 只看流四元组（曾违反 TCP 语义，被路由器场景禁用） | 严格检查 TCP 头可合并性（flags、seq 连续、csum），形成的是"合法 GSO 包"可再次转发 |
| 吞吐收益 | 高（零 CPU） | 中高（省协议栈 per-packet 成本） |
| 现状 | 基本淘汰，`NETIF_F_GRO_HW` 罕见 | 默认开启，`ethtool -K gro off` 关闭 |

### 📬 __netif_receive_skb_core：L2 分发核心

GRO 上送的包（`gro_normal_list` → `__netif_receive_skb_list` → `__netif_receive_skb`）最终进入 `__netif_receive_skb_core()`。这是 L2 处理的最后一步、协议分发（`ip_rcv`）的起点：

```text
__netif_receive_skb_core(&skb, pfmemalloc, &pt_prev)
 ├─ net_timestamp_check: 时间戳若未在驱动取则此处补
 ├─ skb_reset_network_header / reset transport / reset mac_len
 ├─ another_round: skb->skb_iif = dev->ifindex; processed++
 ├─ generic XDP: static_key 命中时 do_xdp_generic(dev->xdp_prog, skb)
 │    /* skb 化的 XDP: 慢于 native, 但对任意设备可用 */
 ├─ eth_type_vlan(protocol)? → skb_vlan_untag: 剥硬件未剥的 802.1Q
 ├─ (pfmemalloc 包跳过 taps —— 内存回收期防递归分配)
 ├─ ptype_all 遍历 ×2: 全局表 + dev->ptype_all
 │    /* tcpdump(AF_PACKET) 在这里 clone 收看; pt_prev 机制:
 │       先记下第一个消费者不交付, 直到确认有第二个消费者才 clone */
 ├─ CONFIG_NET_INGRESS: sch_handle_ingress(skb)
 │    /* tc ingress / tcx(BPF) 挂载点, 可改包/重定向/丢包 */
 ├─ skb_vlan_tag_present && vlan_do_receive(&skb)
 │    /* 查 vlan 子设备: 命中则 skb->dev 换成 vlanX, goto another_round
 │       (重走该 vlan dev 的 rx_handler/taps) */
 ├─ rx_handler = rcu_dereference(dev->rx_handler)   /* 独占: 一个设备一个 */
 │    switch (rx_handler(&skb)) {   /* bridge_handle_frame / bond /
 │       RX_HANDLER_CONSUMED: 已被消费(如 bridge 已转交) → 出局
 │       RX_HANDLER_ANOTHER: dev 被改(如 macvlan) → another_round
 │       RX_HANDLER_EXACT:   只准精确投递(vlan 1:1 映射)
 │       RX_HANDLER_PASS:    继续 } */
 ├─ 残留 vlan tag 处理(无 vlan_dev → 视情况标 PACKET_OTHERHOST)
 ├─ deliver_ptype_list_skb(&ptype_base[ntohs(protocol) & PTYPE_HASH_MASK])
 │    /* 协议分发! ETH_P_IP → ip_packet_type(ip_rcv)
 │       ETH_P_IPV6 → ipv6_rcv; ETH_P_ARP → arp_rcv
 │       INDIRECT_CALL_INET 修饰: 避免 retpoline */
 ├─ deliver_ptype_list_skb(&orig_dev->ptype_specific)   /* 802.1X 等 */
 └─ pt_prev 存在 → skb_orphan_frags_rx 后交给 *ppt_prev
     否则 → kfree_skb_reason(UNHANDLED_PROTO), rx_dropped++
```

**pt_prev 优化**：绝大多数包只有一个消费者（如 `ip_rcv`）。内核把"最后一个消费者"记在 `pt_prev`，**不做 clone 直接交付**；只有当出现第二消费者（tcpdump）时才对前一消费者 `deliver_skb`（clone）。单一消费者路径零拷贝。

### 🛣️ L3/L4 路径简述

```text
ip_rcv(skb)                                   [net/ipv4/ip_input.c]
 ├─ ip_rcv_core: 抓包点 trace_ip_rcv、pskb_may_pull(IP 头)、
 │   sanity(len/版本/sanity checksum 选项)、skb->transport_header 定位
 └─ NF_HOOK(NF_INET_PRE_ROUTING, ..., ip_rcv_finish)
      /* nftables/iptables PREROUTING 链; ACCEPT 才继续 */
      ip_rcv_finish
       ├─ ip_rcv_finish_core: 路由查找
       │    ├─ early demux: sk 提前缓存 dst(tcp_v4_early_demux)
       │    ├─ fib_lookup(net, fl4, res) 或 route hint 复用前一包结果
       │    └─ skb_dst_set_noref(skb, &rt->dst)   /* NOREF 位借用 */
       └─ dst_input(skb)   /* rt_type 决定:
            本机 → ip_local_deliver → NF_INET_LOCAL_IN
                    → ip_local_deliver_finish
                        → inet_protos[iph->protocol].handler
                           (tcp_v4_rcv / udp_rcv / icmp_rcv)
            转发 → ip_forward → NF_INET_FORWARD → dst_output ]
```

TCP 收尾（`tcp_v4_rcv`，net/ipv4/tcp_ipv4.c）：

```text
tcp_v4_rcv(skb)
 ├─ tcp_checksum_complete? (csum 校验, 依赖 ip_summed 已折算)
 ├─ __inet_lookup_skb(hashinfo, skb)   /* 四元组查 established 表 */
 │    ├─ TIMEWAIT 命中 → inet_twsk_deschedule + tcp_v4_timewait_ack/丢
 │    └─ LISTEN 命中:
 │         SYN → (syncookie 判定) tcp_conn_request / 加入 syn_queue
 │         ACK → tcp_check_req → tcp_child_process(唤醒 accept 队列)
 │              (req_stolen 时 goto lookup 重查新 accept 的 sock)
 ├─ xfrm4_policy_check / md5 / tcp_filter(cgroup-BPF)
 ├─ sk->sk_state == TCP_LISTEN → tcp_v4_do_rcv
 └─ sk_incoming_cpu_update(sk)
     bh_lock_sock_nested(sk)               /* spin_lock, softirq 可承受 */
     ├─ !sock_owned_by_user(sk) → tcp_v4_do_rcv(sk, skb)  /* 直接处理 */
     └─ else → tcp_add_backlog(sk, skb)    /* 用户态正持锁读 → 挂 backlog */
     bh_unlock_sock(sk)
/* 无论哪条路, 数据入 sk_receive_queue 后:
   sk->sk_data_ready(sk) → sock_def_readable → wake_up(sk_wq)
   → 等待在 recvmsg/epoll 上的进程被调度执行 */
```

backlog 机制的本质：softirq 不能拿 `lock_sock`（会睡眠），只能在 `bh_lock_sock` 临界区快速塞队+唤醒，真正的 `tcp_v4_do_rcv`（状态机推进）留给进程上下文 `release_sock` 时的 `sk_backlog_rcv` 慢慢消化——这就是"快慢分离"。

### 💧 page_pool 全解（6.6）

page_pool（net/core/page_pool.c）解决驱动 RX 的页分配抖动：正常路径下 RX 页**永不回伙伴系统**，在"驱动 alloc cache → NIC DMA → skb → 释放"之间循环。

```c
struct page_pool {                    /* include/net/page_pool/types.h */
    struct page_pool_params p;        /* 配置(见下) */
    long frag_users;                  /* 页分裂引用(PP_FLAG_PAGE_FRAG) */
    struct page *frag_page;           /* 正在被切分的页 */
    unsigned int frag_offset;
    u32 pages_state_hold_cnt;         /* 尚未释放的持有计数 */
    struct delayed_work release_dw;   /* 关停前排空(1s 周期) */
    void (*disconnect)(void *pool);
    u32 xdp_mem_id;                   /* xdp_rxq_info 注册句柄 */
    struct pp_alloc_cache alloc ____cacheline_aligned_in_smp;
        /* { u32 count; struct page *cache[128]; }  本 CPU 快速缓存 */
    struct ptr_ring ring;             /* 跨 CPU 回收环(ring_size=pool_size) */
    atomic_t pages_state_release_cnt; /* 已释放计数(与 hold 之差=在途) */
    refcount_t user_cnt;              /* 驱动错误处理简化 */
    /* CONFIG_PAGE_POOL_STATS: alloc_stats + per-cpu recycle_stats */
};

struct page_pool_params {
    unsigned int flags;   /* PP_FLAG_DMA_MAP / PP_FLAG_DMA_SYNC_DEV / PP_FLAG_PAGE_FRAG */
    unsigned int order;   /* 页阶, 0 = 每页一帧 */
    unsigned int pool_size;/* ptr_ring 容量, 建议 = RX 描述符数 */
    int nid;  struct device *dev;  struct napi_struct *napi; /* 绑定 napi! */
    enum dma_data_direction dma_dir;
    unsigned int max_len, offset;
};
```

**分配路径**（`page_pool_dev_alloc_pages` → `__page_pool_alloc_pages`）：

```text
pool->alloc.count != 0
 └→ cache[--count]                     /* 快路径: 一条数组下标, 无锁!
       (安全前提: 同一 pool 只被其绑定的 napi 上下文分配, 见 p.napi 注释) */
count == 0 → 从 ptr_ring 批量搬运 PP_ALLOC_CACHE_REFILL(64) 个
ring 空 → __page_pool_alloc_pages_slow:
   alloc_pages(order) + page_pool_dma_map:
     dma_map_page_attrs(dev, page) → page_pool_set_dma_addr(page, dma)
     /* DMA 地址记进 page->dma_addr, 页由 pool 长期持有映射 */
```

**释放/回收路径**（skb 释放时经 `napi_frag_unref` → `page_pool_put_defragged_page` → `__page_pool_put_page`）：

```text
__page_pool_put_page(pool, page, dma_sync_size, allow_direct)
 ├─ page_ref_count(page) == 1 && !page_is_pfmemalloc ?
 │   ├─ PP_FLAG_DMA_SYNC_DEV → page_pool_dma_sync_for_device  /* 免 unmap+map */
 │   ├─ allow_direct && in_softirq() && page_pool_recycle_in_cache
 │   │     /* XDP_DROP/同 napi 释放: 直接塞回 alloc.cache[128], 零开销 */
 │   └─ 否则 → 返回 page → 调用者塞入 ptr_ring(满则 page_pool_return_page)
 └─ refcount > 1 (页被拆成多个 frag 共享, 如 mergeable buffer)
     → recycle_stats.released_refcnt++ → page_pool_return_page
        /* 交给普通 put_page 世界: 最后一个 frag 持有者可能再回池 */
```

*   **与 skb 的耦合**：`skb_mark_for_recycle(skb)` 置 `pp_recycle`；`skb_release_data()` 对每个 frag 调 `napi_frag_unref(..., napi_safe)`，`napi_safe=true`（`napi_consume_skb` 上下文）时走"直接回收进 alloc cache"快速分支——这正是 6.6 中 `napi_consume_skb` 存在的意义。
*   **与 XDP 的关系**：XDP 要求页**常驻 DMA 映射**且免 alloc——page_pool 是 native XDP 的硬依赖（`xdp_rxq_info_reg_mem_model(XXA_TYPE_PAGE_POOL)`）；XDP_DROP 的页在 poll 内直接回 cache，一次软中断内零分配。`page_pool_put_page_bulk()` 支持批量（XDP_REDIRECT 目端收集后一次归还）。
*   **PP_FLAG_PAGE_FRAG**：驱动用 `page_pool_alloc_frag()` 把一页切成多个 2K/4K frag 供 mergeable RX（frag_page/frag_offset 记进度），refcnt>1 的回收分支就是为它服务。
*   6.6 起主流驱动（如 virtio_net 之外的多数物理网卡）RX 页全部走 page_pool；`page_pool_get_stats`（CONFIG_PAGE_POOL_STATS）经 devlink 暴露 fast/slow/ring_full 等命中率。

### 🌀 RPS / RFS / aRFS

单队列网卡或 IRQ 亲和失衡时，`NET_RX_SOFTIRQ` 全压在一两个 CPU。RPS（Receive Packet Steering）在软件层把包**跨 CPU 重派**：

```text
netif_rx_internal(skb)                    /* 非 NAPI 老路径/ __dev_forward_skb */
 ├─ CONFIG_RPS && rps_needed:
 │   cpu = get_rps_cpu(dev, skb, &rflow)
 │    ├─ hash = skb_get_hash(skb)         /* 无硬件哈希时软件 dissector 现算 */
 │    ├─ sock 流表: rps_sock_flow_table.socks[hash & mask]
 │    │    /* 全局表: 每个"当前在某 CPU 上的 socket"的期望 CPU,
 │    │       由 sock_def_readable 等自动更新(recvmsg 时的 CPU) */
 │    └─ 设备流表: rxq->rps_flow_table (RFS), rflow->last_qtail 记序
 │       /* RFS: 保证同流包顺序(都派去"socket 现所在 CPU") */
 └─ enqueue_to_backlog(skb, cpu, &qtail)   /* 可能是远端 CPU! */
      ├─ sd = &per_cpu(softnet_data, cpu)  /* 关中断拿队列锁 */
      ├─ qlen > netdev_max_backlog(默认1000) 或 flow_limit 命中
      │     → sd->dropped++ / kfree_skb_reason(CPU_BACKLOG)
      ├─ __skb_queue_tail(&sd->input_pkt_queue, skb)
      └─ 队列原为空 → __test_and_set NAPI_STATE_SCHED(backlog)
          → napi_schedule_rps(sd)
              → IPI(smp_call_function_single Enclosing sd->csd)
                  远端 CPU: rps_trigger_softirq → ____napi_schedule(backlog)

/* 远端 CPU 的处理即 process_backlog(伪 NAPI 设备的 poll): */
process_backlog(napi, quota)
 ├─ 把 input_pkt_queue 换入 process_queue, 逐包 __netif_receive_skb
 └─ 队列清空 → napi->state = 0 (inline 版 napi_complete)
```

*   **RPS vs RSS**：RSS 硬件哈希分流（多队列），RPS 软件按流哈希重派到任意 CPU 的 backlog 队列——开销是 **IPI + 跨 CPU 队列锁**，`received_rps`/`dropped` 列可观测代价。
*   **RFS（Receive Flow Steering）**：在 RPS 基础上让包跟着 socket 走（`rps_sock_flow_table`），保证应用线程与软中断同 CPU，cache 命中率大增；`/sys/class/net/DEV/queues/rx-N/rps_flow_cnt` 配置。aRFS（accelerated RFS）把同样的映射下推到网卡（`ndo_rx_flow_steer` + `rx_cpu_rmap`），由硬件直接投递到目标 CPU 的队列，免除 IPI。
*   `netdev_max_backlog`（默认 1000）是 per-CPU backlog 上限，超出即丢（`softnet_stat` 第 2 列）——10G+ pps 场景常调到 10000+，但本质是拿内存换丢包率。

### ⏱️ 端到端时序与开销分析

```text
CPU#2 (IRQ 亲和)                 CPU#2 (softirq)                CPU#7 (应用)
──── 硬中断 ────                 ──── net_rx_action ────        ── recvmsg ──
网卡 DMA 完成
→ ISR: napi_schedule_irqoff      sd->poll_list 非空
   [~100ns]                      ksoftirqd/就地 softirq
                                   │ napi_poll → 驱动 poll ×64
                                   │  ├ virtqueue_get_buf       [锁: vq 旋锁]
                                   │  ├ page_pool alloc.cache   [访存: 本CPU行]
                                   │  ├ build_skb+add_rx_frag
                                   │  └ napi_gro_receive        [哈希/cmp]
                                   │       └ GRO 桶聚合(≤8/桶)
                                   │ gro_normal_list → 批量上送
                                   │ __netif_receive_skb_core
                                   │  ├ ptype_all(空则跳过)
                                   │  └ ip_rcv
                                   │      ├ NF PRE_ROUTING(无规则≈跳过)
                                   │      ├ fib: per-netns FIB trie [共享只读]
                                   │      └ tcp_v4_rcv
                                   │          ├ established 哈希 [共享行]
                                   │          ├ bh_lock_sock     [可能竞争!]
                                   │          └ 入队+sk_data_ready ──────►
                                   │                                          wake_up
                                   │                                          (epoll)
                                   │                                      schedule 返回
                                   └ budget 尽 → repoll/complete
```

**关键开销点**（64B 小包 10Mpps 场景的典型权重）：

| 环节 | 开销类型 | 优化手段 |
| :--- | :--- | :--- |
| 中断进入/退出 | ~1-2μs/次 | NAPI 合并、IRQ 亲和、`gro_flush_timeout` |
| ksoftirqd 唤醒 | 上下文切换 | 就地 softirq（irq_exit 执行） |
| per-packet 分配 | slab/page | page_pool + `napi_skb_cache` + GRO 聚合 |
| `ptype_base` 分发 | 间接调用 | `INDIRECT_CALL_INET` 编译期直连 |
| TCP 哈希 + `bh_lock_sock` | 跨 CPU 缓存行 | RSS/RFS 对齐（同流同 CPU）、`sk_incoming_cpu_update` |
| `sk_data_ready` 唤醒 | IPI + 调度 | busy poll、`wake_up_*_sync` |

### 🔭 观测与调试

*   **`/proc/net/softnet_stat` 15 列**（`softnet_seq_show` 实测字段）：`processed, dropped, time_squeeze, [4 个历史 fastroute 残留 0], [cpu_collision 0], received_rps, flow_limit_count, input+process qlen 合计, seq index(CPU), input_qlen, process_qlen`（十六进制）。重点看 1/2/3/10 列。
*   **tracepoints**：`napi:napi_poll`（实例/budget/实际 work）、`net:netif_receive_skb`、`net:net_dev_xmit`；`perf stat -e napi:napi_poll` 可统计 poll 分布。
*   **BCC/BPF**：`biolatency` 类思路，`get_kfree_skb` 追 drop reason；`xdp_drop` 直测。
*   **sysfs**：`/sys/class/net/DEV/{gro_flush_timeout,napi_defer_hard_irqs,threaded}`；`/proc/sys/net/core/{netdev_budget,netdev_budget_usecs,netdev_max_backlog}`。
*   **诊断口诀**：第 3 列涨 → 吞吐超配额（加预算/RPS/线程化）；第 2 列涨 → backlog 溢出（调 max_backlog 或分流）；第 10 列高 → RPS IPI 已成热点（考虑 RSS 硬件化/aRFS）。

### 🧠 深挖难点

*   **为什么 `napi_complete_done` 要求 work < budget 才能调用？** budget 耗尽意味着 net_rx_action 还"拥有"该 NAPI（要把它挪进 repoll），驱动此时改 state 会与软中断循环竞态；这也是 `__napi_poll` 末尾 `pr_warn_once("Budget exhausted after napi rescheduled")` 的由来——驱动在耗尽 budget 后又调了 complete/schedule。
*   **GRO 的哈希桶为什么取 8？** `gro_bitmask` 是一个 unsigned long 位图（64 位），桶数必须 ≤ 位宽；8 桶 × 每桶 8 包 = 64 个在册流，兼顾查找长度与内存。
*   **RPS 的 IPI 为什么不可避免？** 包已在本 CPU 的 cache 里，目标 CPU 的 `input_pkt_queue` 也只在目标 CPU 消费——必须由目标 CPU 自己把包挂队并 raise NET_RX_SOFTIRQ；IPI 是"把工作送到正确 CPU"的最短路径（lazy IPI 合并依赖 `rps_ipi_list` 去重）。

### ❓ 思考题

1.  驱动在 poll 里收到了 64 个包（恰好 budget）但环上还有包，它应返回什么？内核接下来做什么？此时中断是开还是关？
    *提示：`__napi_poll` 的 work==weight 分支；repoll 链；ISR 保持关闭直至某轮 work<budget。*
2.  `napi_defer_hard_irqs=3` 且 `gro_flush_timeout=30000`（ns? 读 sysfs 的单位）时，突发流量下的中断次数与时延如何变化？哪个场景必须避免这种配置？
    *提示：单位是 ns（sysfs 显示）/次数；低延迟交易场景 defer 会引入固定微秒级延迟。*
3.  GRO 聚合出的 skb 到达 `tcp_v4_rcv` 时，`skb_shinfo->gso_segs` 与 MIB 的 `InSegs` 是什么关系？关闭 GRO 后 `InSegs` 变化吗？
    *提示：`tcp_segs_in()` 用 `skb_shinfo(skb)->gso_segs` 记段数；MIB 统计以 TCP 段为单位而不是 skb 为单位。*
4.  一个 page_pool 页被拆成 4 个 frag 分别进 4 个 skb，这 4 个 skb 在不同 CPU 上释放——最后一次回收为什么可能退化到 `put_page`（离开池子）？
    *提示：`__page_pool_put_page` 的 refcnt>1 分支；frag_users 的意义（6.6 的 `page_pool_is_last_frag`）。*
5.  为什么 `process_backlog` 可以用普通写 `napi->state = 0` 而不用 cmpxchg（其它 NAPI complete 却要）？
    *提示：注释原话——backlog 只会被本 CPU 拥有和操作，SCHED 是唯一可能置位的标志。*
6.  收包路径上若 `ptype_all` 非空（正在抓包），单消费者与双消费者场景的拷贝次数差多少？
    *提示：pt_prev 延迟交付；`deliver_skb` 对首个消费者做 `skb_clone`（元数据拷贝，数据共享）。*
