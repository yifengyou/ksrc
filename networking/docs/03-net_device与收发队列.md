# net_device 与收发队列

`struct net_device` 是内核对"一块网卡"（或一个虚拟设备：loopback、vlan、bridge、veth、tunnel……）的完整抽象，定义在 `include/linux/netdevice.h`（6.6 中约 4900 行头文件）。它既是驱动注册的载体，又是协议栈寻址的锚点，还内嵌了 TX 队列数组（`netdev_queue`）、qdisc 挂载点与 XPS 映射。本章解读其关键字段、分配/注册全链路、TX 锁模型与队列选路，以及链路状态机。

### 🗂️ struct net_device 关键字段（Linux 6.6）

结构体按**缓存行用途**精心排布（源码注释："Read-mostly cache-line for fast-path access"、"Cache lines mostly used on receive path / transmit path"），以下按实际分区解读。

#### 1) 身份与命名

```c
char name[IFNAMSIZ];              /* 16B, "eth0"/"enp3s0" */
struct netdev_name_node *name_node; /* 挂入 net->dev_name_head 哈希 */
struct dev_ifalias __rcu *ifalias;  /* ip link set alias */
unsigned char name_assign_type;     /* NET_NAME_UNKNOWN/PREDICTABLE/
                                       USER/ENUM/RENAME(稳定命名审计) */
int ifindex;                        /* net->dev_index_head 索引 */
struct hlist_node index_hlist;
possible_net_t nd_net;              /* 所属 netns(struct net 指针) */
```

*   `ifindex` 在 `register_netdevice()` 中由 `dev_index_reserve()` 从 `net->dev_by_index` xarray 分配，全局（per-netns）单调递增；`skb->skb_iif` 记录的就是它。

#### 2) 操作表（函数指针集）

```c
const struct net_device_ops *netdev_ops;   /* 驱动主操作表 */
const struct ethtool_ops    *ethtool_ops;  /* ethtool 命令实现 */
const struct header_ops     *header_ops;   /* L2 头构造(eth_header) */
const struct xdp_metadata_ops *xdp_metadata_ops;
unsigned long long priv_flags;             /* IFF_* 私有标志 */
unsigned int flags;                        /* IFF_UP/LOOPBACK/BROADCAST... */
```

`net_device_ops` 中数据面最关键的两个回调：

*   `ndo_start_xmit(skb, dev)`：发包唯一入口（`netdev_start_xmit` → `__netdev_start_xmit` 调用），返回 `NETDEV_TX_OK/BUSY`；
*   驱动 RX 不在 ops 中——收包由驱动中断主动上报（第 4 章）。
其余高频：`ndo_open/ndo_stop`（ifup/ifdown）、`ndo_set_rx_mode`（mc/uc 过滤）、`ndo_select_queue`（TX 选队）、`ndo_get_stats64`、`ndo_tx_timeout`、`ndo_bpf`/`ndo_xdp_xmit`（XDP）。

`priv_flags` 是内核内部语义（区别于用户可见的 `flags`），常用位：

| priv_flags 位 | 含义 | 典型设备 |
| :--- | :--- | :--- |
| `IFF_NO_QUEUE` | 无队列，默认挂 noqueue qdisc | loopback、tunnel、veth |
| `IFF_XMIT_DST_RELEASE` / `_PERM` | 发包时可释放 skb 的 dst | 多数以太网设备 |
| `IFF_DONT_BRIDGE` | 禁止加入 bridge（语义不符） | ppp、某些 wifi |
| `IFF_MACVLAN_PORT` / `IFF_BRIDGE_PORT` / `IFF_BONDING` / `IFF_TEAM_PORT` | "我是谁的 lower"类型标记 | 堆叠设备打标 |
| `IFF_NO_RX_HANDLER` | 禁止注册 rx_handler | 纯点对点设备 |
| `IFF_PHONY_HEADROOM` | headroom 虚高（PPP 的历史包袱） | ppp |
| `IFF_TX_SKB_SHARING` | 允许 skb 跨设备共享 | dummy |

#### 3) 几何与 MTU

```c
unsigned int mtu; unsigned short hard_header_len;
unsigned short needed_headroom, needed_tailroom; /* 协议栈预留 L2 空间 */
unsigned int min_mtu, max_mtu;
unsigned char type;    /* ARPHRD_ETHER=1, ARPHRD_LOOPBACK=772... */
```

`needed_headroom` 影响所有经过该设备的 skb 预留空间（`ip_finish_output2` 检查 `skb_headroom < ll_headroom` 则扩头）——隧道设备正是靠它让外层头有处可 push。

#### 4) offload 能力矩阵（`netdev_features_t` 位图）

```c
netdev_features_t features;       /* 当前生效 */
netdev_features_t hw_features;    /* 硬件可变的(用户可开关) */
netdev_features_t wanted_features;/* 用户意愿 */
netdev_features_t vlan_features;  /* VLAN 子设备可继承 */
netdev_features_t hw_enc_features;/* 隧道封装后可保留 */
netdev_features_t gso_partial_features;
unsigned int gso_max_size;   u16 gso_max_segs;
unsigned int tso_max_size;   u16 tso_max_segs;
```

常用位：`NETIF_F_SG`（scatter-gather，frags 直接送硬件）、`NETIF_F_IP_CSUM/TX_CSUM`（L4 校验和 offload）、`NETIF_F_TSO/TSO6`（TCP 大包分卸）、`NETIF_F_GSO_ROBUST`、`NETIF_F_GRO_HW`（硬件 LRO）、`NETIF_F_HW_VLAN_CTAG_TX/RX/FILTER`。注册时内核强制注入软件位：`dev->features |= NETIF_F_SOFT_FEATURES`（GSO/GRO 等纯软件能力），并把 `features & hw_features` 归入 `wanted_features`（`ethtool -k` 修改的就是 features∩hw_features）。`register_netdevice` 里还有一系列"继承性修正"（如 `vlan_features |= NETIF_F_HIGHDMA`）。

#### 5) 统计（三条通道）

```c
struct net_device_stats stats;              /* 旧驱动单计数,不推荐 */
struct net_device_core_stats __percpu *core_stats; /* 内核统一口径 */
enum netdev_stat_type pcpu_stat_type:8;
union {   /* 驱动选择的 per-CPU 模板 */
    struct pcpu_lstats __percpu *lstats;   /* 仅 bytes/packets(loopback) */
    struct pcpu_sw_netstats __percpu *tstats; /* 软件 rx/tx 字节数 */
    struct pcpu_dstats __percpu *dstats;   /* dummy/纯 drop */
};
atomic_t carrier_up_count, carrier_down_count;
```

`dev_core_stats_rx_dropped_inc(dev)` 等静态键内联函数写 `core_stats`（RX dropped/RX nohandler/RX errors/TX dropped...）；`dev_get_stats()` 汇总三通道合出 `rtnl_link_stats64`。**语义约定**：`rx_dropped` = 内核（backlog/qdisc/协议）丢弃；驱动/硬件丢的必须实现 `ndo_get_stats64` 单列（或 ethtool -S）。

#### 6) 地址列表

```c
spinlock_t addr_list_lock;
struct netdev_hw_addr_list uc, mc, dev_addrs; /* 单播/组播/设备地址链 */
const unsigned char *dev_addr;                /* 指向 dev_addrs 第一项 */
```

`dev_uc_sync/dev_mc_sync` 用于 macvlan/vlan 把过滤表同步到 lower 设备；`promiscuity/allmulti` 计数决定是否置 IFF_PROMISC。

#### 7) RX 侧缓存行

```c
struct netdev_rx_queue *_rx;  unsigned int num_rx_queues, real_num_rx_queues;
struct bpf_prog __rcu *xdp_prog;              /* generic XDP 程序 */
unsigned long gro_flush_timeout; int napi_defer_hard_irqs;
unsigned int gro_max_size, gro_ipv4_max_size;
rx_handler_func_t __rcu *rx_handler;          /* bridge/bond/ipvlan 独占 */
struct bpf_mprog_entry __rcu *tcx_ingress;    /* tc ingress BPF */
struct nf_hook_entries __rcu *nf_hooks_ingress;
```

`netdev_rx_queue`（include/net/netdev_rx_queue.h）本身很薄：`xdp_rxq_info`、RPS 的 `rps_map`/`rps_dev_flow_table`、sysfs kobj。`real_num_rx_queues` 才是硬件实际启用的数量（`ethtool -L` 调整时由驱动 `ndo_set_channels` 修改）。

#### 8) TX 侧缓存行（与 qdisc/锁模型核心）

```c
struct netdev_queue *_tx ____cacheline_aligned_in_smp;
unsigned int num_tx_queues, real_num_tx_queues;
struct Qdisc __rcu *qdisc;        /* 根 qdisc(多队列时为 mq) */
unsigned int tx_queue_len;        /* qdisc 默认 qlen 上限(默认 1000) */
spinlock_t tx_global_lock;        /* 传统全局 TX 锁(netif_tx_lock) */
#ifdef CONFIG_XPS
struct xps_dev_maps __rcu *xps_maps[XPS_MAPS_MAX]; /* XPS_CPUS/XPS_RXQS */
#endif
struct timer_list watchdog_timer; int watchdog_timeo; /* TX 超时看门狗 */
```

#### 9) 生命周期与私有区

```c
enum { NETREG_UNINITIALIZED=0, NETREG_REGISTERED, NETREG_UNREGISTERING,
       NETREG_UNREGISTERED, NETREG_RELEASED, NETREG_DUMMY } reg_state:8;
bool needs_free_netdev;  void (*priv_destructor)(struct net_device *);
void *ml_priv;                     /* mid-layer( Historic ) */
struct list_head link_watch_list;  /* linkwatch 待处理链 */
unsigned threaded:1;               /* 设备级 NAPI 线程化开关 */
/* 设备私有数据: netdev_priv(dev) = dev + ALIGN(sizeof(*dev),32) */
```

### 🏗️ 设备分配：alloc_netdev_mqs()

```c
struct net_device *alloc_netdev_mqs(int sizeof_priv, const char *name,
        unsigned char name_assign_type,
        void (*setup)(struct net_device *),
        unsigned int txqs, unsigned int rxqs)
{
    alloc_size = sizeof(struct net_device);
    if (sizeof_priv) {                     /* 私有区 32B 对齐紧随其后 */
        alloc_size = ALIGN(alloc_size, NETDEV_ALIGN);
        alloc_size += sizeof_priv;
    }
    alloc_size += NETDEV_ALIGN - 1;        /* 整体 32B 对齐 */
    p = kvzalloc(alloc_size, GFP_KERNEL_ACCOUNT|__GFP_RETRY_MAYFAIL);
    dev = PTR_ALIGN(p, NETDEV_ALIGN);
    dev->padded = (char *)dev - (char *)p; /* 供 free 时还原 */

    /* 初始化: refcnt/dev_addr/mc/uc/adj_list/napi_list/... */
    dev_net_set(dev, &init_net);           /* 默认 init_net */
    dev->gso_max_size = GSO_LEGACY_MAX_SIZE; ...
    setup(dev);                            /* ether_setup 等模板 */

    if (!dev->tx_queue_len) {              /* 无队列设备(loopback/隧道) */
        dev->priv_flags |= IFF_NO_QUEUE;
        dev->tx_queue_len = DEFAULT_TX_QUEUE_LEN; /* 1000 */
    }
    dev->num_tx_queues = dev->real_num_tx_queues = txqs;
    netif_alloc_netdev_queues(dev);        /* _tx[] 数组 + dql_init */
    dev->num_rx_queues = dev->real_num_rx_queues = rxqs;
    netif_alloc_rx_queues(dev);            /* _rx[] 数组 + sysfs */
    ...
}
/* 驱动常用宏:
 * alloc_etherdev_mqs(sizeof_priv, txqs, rxqs)
 *   = alloc_netdev(priv, "eth%d", NET_NAME_UNKNOWN, ether_setup, ...) */
```

要点：

*   **net_device 与驱动私有区一次分配**（`netdev_priv()` 直接算偏移，零额外指针，cache 友好）；`NETDEV_ALIGN=32`。
*   `txqs/rxqs` 至少为 1；多队列驱动（如 virtio_net）按 `vring` 数传入，随后可在 `real_num_*` 上收缩。
*   `ether_setup()`（net/ethernet/eth.c）填充以太网模板：`hard_header_len=ETH_HLEN`、`mtu=1500`、`flags=IFF_BROADCAST|IFF_MULTICAST`、`header_ops=&eth_header_ops`、`type=ARPHRD_ETHER` 等。
*   `name_assign_type` 是 6.6 已有字段，配合 udev 的"稳定命名"审计。

### 📝 注册：register_netdevice() 全链路

调用链（**全程持有 RTNL**，`ASSERT_RTNL()`）：

```text
register_netdevice(dev)                       [net/core/dev.c]
 ├─ ethtool_check_ops / spin_lock_init(addr_list_lock)
 ├─ dev_get_valid_name()                      /* 处理 "eth%d" 模板 */
 ├─ dev->netdev_ops->ndo_init(dev)            /* 驱动补充初始化(可选) */
 ├─ netdev_do_alloc_pcpu_stats()              /* core_stats 等三通道 */
 ├─ dev_index_reserve() → dev->ifindex
 ├─ features 修正(注入 NETIF_F_SOFT_FEATURES 等)
 ├─ call_netdevice_notifiers(NETDEV_POST_INIT)
 ├─ netdev_register_kobject(dev)              /* /sys/class/net/<name> 全套 */
 ├─ __netdev_update_features()
 ├─ set_bit(__LINK_STATE_PRESENT, &dev->state)
 ├─ linkwatch_init_dev(dev)
 ├─ dev_init_scheduler(dev)                   /* qdisc 子系统 attach noop */
 ├─ list_netdevice(dev)
 │    └─ list_add_tail_rcu(&dev->dev_list, net->dev_base_head)  ← 数据面生效点!
 ├─ call_netdevice_notifiers(NETDEV_REGISTER) /* 通知协议(vlan/ipv6...) */
 └─ rtmsg_ifinfo(RTM_NEWLINK)                 /* netlink 广播给 ip 命令 */
(rtnl_unlock 后)
 └─ netdev_run_todo()                         /* 挂 todo_list 的异步收尾 */
```

**`dev_open()`（ifup）之后**（`ip link set dev up`）：

```text
dev_open(dev) → __dev_open
 ├─ netif_running 校验 → dev->netdev_ops->ndo_open(dev)   /* 驱动启队列/IRQ */
 ├─ set_bit(__LINK_STATE_START, &dev->state)
 ├─ dev_activate(dev)                             [net/sched/sch_generic.c]
 │    └─ attach_default_qdiscs(dev)  (若 dev->qdisc == &noop_qdisc)
 │         ├─ 单队列/IFF_NO_QUEUE: attach_one_default_qdisc(dev, txq)
 │         │     ├─ IFF_NO_QUEUE  → noqueue_qdisc_ops(loopback/隧道)
 │         │     └─ 否则          → default_qdisc_ops(默认 pfifo_fast_ops)
 │         │         (qdisc_create_dflt 后置 TCQ_F_ONETXQUEUE|TCQ_F_NOPARENT)
 │         └─ 多队列(netif_is_multiqueue): qdisc_create_dflt(&mq_qdisc_ops)
 │               └→ mq_qdisc_ops.attach(qdisc)  /* 每队列一个子 qdisc */
 │    └─ transition_one_qdisc: txq->qdisc = txq->qdisc_sleeping
 │        (dev->qdisc 运行态与休眠态在此"接通", 启 watchdog)
 └─ call_netdevice_notifiers(NETDEV_UP)
```

注意 **qdisc 的双指针模型**：`txq->qdisc_sleeping` 永远指向"配置态"qdisc（可随时被 `tc qdisc replace` 换掉），`txq->qdisc` 是数据面实际使用的（设备 down 时被换成 `noop_qdisc`，up 时由 `transition_one_qdisc` 恢复）。`default_qdisc_ops` 默认是 `pfifo_fast_ops`（sch_generic.c），可经 `sysctl net.core.default_qdisc` 或 `CONFIG_DEFAULT_NET_SCH_*`（如 fq/fq_codel）改变。

### 🔔 netdev notifier 与 upper/lower 链

**notifier 机制**：内核各子系统通过 `register_netdevice_notifier()` 订阅设备事件，事件沿 `net->netdev_chain`（per-netns）广播：

```c
/* 典型事件(不全): */
NETDEV_UP, NETDEV_DOWN, NETDEV_REGISTER, NETDEV_UNREGISTER,
NETDEV_CHANGEMTU, NETDEV_CHANGEADDR, NETDEV_CHANGE /* carrier 等 */,
NETDEV_FEAT_CHANGE, NETDEV_BONDING_FAILOVER, NETDEV_NOTIFY_PEERS ...
/* 返回 NOTIFY_BAD 可否决 register(如 vlan 冲突) */
```

典型订阅者：`arp_netdev_event`（地址变化刷 ARP 缓存）、ipv6 addrconf、`netdev_event`（ipmr/fib 同步）……这是"控制面解耦"的关键：dev.c 不需要知道谁关心设备。

**upper/lower 链（`adj_list`）**：vlan/bridge/bond/macvlan 等堆叠设备通过 `netdev_upper_dev_link(dev, upper)` 建立拓扑边（`struct netdev_adjacent{dev,master,ref_nr...}`），形成拓扑图：

```text
 bond0 (upper, master)
   │ upper/lower 边
 eth0, eth1 (lower)          ← real_num_tx_queues 可能被 master 限制
```

数据面与拓扑的联动：`netdev_master_upper_dev_get`、`netif_is_multiqueue` 传播、XPS 的 `sb_dev`（subordinate，见下）都基于此图。循环检测（`netdev_has_upper_dev`）防止自环。

### 🔒 netdev_queue 与 TX 锁模型

```c
struct netdev_queue {
    /* read-mostly: */
    struct net_device  *dev;
    struct Qdisc __rcu *qdisc;            /* 运行态 */
    struct Qdisc __rcu *qdisc_sleeping;   /* 配置态 */
    unsigned long tx_maxrate;             /* tc 命令可设 */
    atomic_long_t trans_timeout;          /* TX 超时计数(sysfs 可见) */
    struct net_device *sb_dev;            /* 从属设备(bond 场景 XPS 用) */
    /* write-mostly(独立缓存行): */
    spinlock_t _xmit_lock ____cacheline_aligned_in_smp;
    int xmit_lock_owner;                  /* 持锁 CPU id, -1 空闲 */
    unsigned long trans_start;            /* watchdog 时间基 */
    unsigned long state;                  /* __QUEUE_STATE_* 位 */
  #ifdef CONFIG_BQL
    struct dql dql;                       /* Byte Queue Limits(第 5 章) */
  #endif
};
```

**三层"停止队列"状态**（`state` 位）：

| 位 | 设置者 | 语义 |
| :--- | :--- | :--- |
| `__QUEUE_STATE_DRV_XOFF` | 驱动（`netif_tx_stop_queue`） | 描述符不足，需停止送包 |
| `__QUEUE_STATE_STACK_XOFF` | 协议栈（`netif_tx_queue_frozen`） | 罕见（如 fiber channel） |
| `__QUEUE_STATE_FROZEN` | 驱动（`netif_tx_lock`/冻结 API） | 队列重建期间整体冻结 |

`netif_xmit_stopped()` 检查任一 XOFF；`netif_tx_wake_queue()` 清 DRV_XOFF 并触发 qdisc 重跑。

**锁层次**（谁保护谁）：

```c
static inline void __netif_tx_lock(struct netdev_queue *txq, int cpu)
{ spin_lock(&txq->_xmit_lock); txq->xmit_lock_owner = cpu; }
/* 宏版本区分 LLTX 设备: */
#define HARD_TX_LOCK(dev, txq, cpu) {          \
    if ((dev->features & NETIF_F_LLTX) == 0)   \
        __netif_tx_lock(txq, cpu);             \
    else __netif_tx_acquire(txq); }            /* 只做 preempt 记账 */
#define HARD_TX_TRYLOCK(dev, txq) ...           /* trylock 版,qdisc 用 */
```

*   **per-queue `_xmit_lock`**：保护 `ndo_start_xmit` 与描述符环尾部指针。多数驱动在此串行化；硬件队列独立则天然并行（配合 RSS/XPS 多队列扩展）。
*   **`NETIF_F_LLTX`（LockLess TX）**：自带锁驱动（如部分无线驱动的自旋队列）声明跳过内核锁，只保留抢占记账。
*   **`xmit_lock_owner` 递归检测**：`__dev_queue_xmit` 无队列路径在取锁前比对 owner，配合 per-CPU `dev_xmit_recursion()` 计数抓"虚拟设备递归发包死循环"（"Dead loop on virtual device"告警即此）。
*   **`netif_tx_lock(dev)`（全局锁）**：老 API，遍历锁全部 `_tx[]`，仅剩少数路径（如 `dev_deactivate`）使用；数据面已不碰。
*   **qdisc 侧的 `__QDISC_STATE_*`**：`SCHED`（已挂 output_queue 待跑）、`MISSED`（lockless 竞争失败需重跑）、`DRAINING`、`DEACTIVATED`（换 qdisc 中途丢弃）——与队列 XOFF 状态共同构成"发包回退"状态机（第 5 章展开）。

### 🧭 队列选路：XPS 与多队列

发包时 `__dev_queue_xmit` 调 `netdev_core_pick_tx()` 决定 txq：

```text
netdev_core_pick_tx(dev, skb, sb_dev)
 ├─ real_num_tx_queues == 1 → 恒 0
 └─ ops->ndo_select_queue ? : netdev_pick_tx(dev, skb, sb_dev)
      │
      netdev_pick_tx (net/core/dev.c):
      ├─ queue_index = sk_tx_queue_get(sk)      /* socket 缓存的旧结果 */
      ├─ if (queue_index < 0 || skb->ooo_okay || 越界) {
      │     new_index = get_xps_queue(dev, sb_dev, skb)
      │       ├─ XPS_RXQS map: 用 sk_rx_queue_get(sk)(收队列号)查表
      │       └─ XPS_CPUS map: 用 skb->sender_cpu 查表
      │          __get_xps_queue_idx: tc = prio_tc_map[skb->priority]
      │             map = dev_maps->attr_map[cpu* num_tc + tc]
      │             map->len==1 ? queues[0] : queues[reciprocal_scale(hash,len)]
      │     if (new_index < 0)
      │        new_index = skb_tx_hash(dev, sb_dev, skb)  /* 兜底哈希 */
      │     sk_tx_queue_set(sk, new_index)      /* 缓存回 socket */
      │  }
      └─ return queue_index
```

*   **XPS（Transmit Packet Steering）**：管理员经 `netif_set_xps_queue(dev, cpumask, index)` 写入 `dev->xps_maps[XPS_CPUS]`（sysfs `queues/tx-N/xps_cpus`），建立"本 CPU 上的包走 N 号队列"映射。`xps_dev_maps.attr_map[]` 按 `nr_ids × num_tc` 平铺，`struct xps_map.queues[]` 存候选队列，多个候选时用包哈希 `reciprocal_scale` 分散。
*   **与 RSS 对称**：典型调优是让 RX 队列 i 的 IRQ 亲和 CPU j，同时把 CPU j 的 XPS 指向 TX 队列 i——收发同核同队列，`sk_tx_queue`/`sk_rx_queue` 缓存全命中，cache line 不飘。
*   **`ooo_okay`**：路由明确允许乱序（多路径）时 TCP 置位，允许选路函数放弃旧绑定重新均衡。
*   **`skb_tx_hash` 兜底**：无 XPS 时按 `(skb->hash * dev->real_num_tx_queues)` 类逻辑（实际实现含 sk 与 sender_cpu 混合）散列，保证同流同队列。

### 🚦 链路状态机：IFF_UP / carrier / linkwatch

```text
                       netif_carrier_off()
   [注册] ────────────► [__LINK_STATE_NOCARRIER]
                            │ 驱动检测到链路(phylib/MDIX/PHY IRQ)
                            │ netif_carrier_on(dev)
                            ▼
 dev_open: IFF_UP=1    [carrier OK] ──── dev->flags |= IFF_RUNNING*
   __LINK_STATE_START        ▲                    (由 linkwatch 异步同步)
                            │ netif_carrier_off
                            ▼
                       停 TX 队列/清 IFF_RUNNING, 静默丢包直至恢复

linkwatch(net/core/link_watch.c):
 netif_carrier_on/off
  └→ linkwatch_fire_event(dev)
       ├─ 有 urgent 情况(如波动的 serial link)立即处理
       └─ 否则: 挂 dev->link_watch_list, 延迟调度 lwd 事件(默认 ~1s 合并抖动)
            └→ linkwatch_do_dev: 校正 operstate(IF_OPER_UP/DOWN)
                 + call_netdevice_notifiers(NETDEV_CHANGE)
                 + 若 carrier 恢复则 dev_activate 接回 qdisc
```

*   **三层状态**：`flags & IFF_UP`（管理态，用户写）、`__LINK_STATE_START`（内核启动完成）、carrier（物理层）。`IFF_RUNNING` 是用户可见的合成视图（`dev->operstate`/`IF_OPER_*` RFC 2863 语义，`ip link` 的 state 字段）。
*   **为什么 carrier 变化要走延迟的 linkwatch 而不直接改状态？** 链路抖动（尤其 ppp/serial）一秒内多次翻转，linkwatch 把事件**合并**成每秒一次的状态提交，避免 NETDEV_CHANGE 风暴与路由重算振荡。
*   TX watchdog：`dev_watchdog()` 周期检查 `trans_start` 距今是否超 `watchdog_timeo`（默认 5s，驱动可调），超时则调 `ndo_tx_timeout` 并 `txq->trans_timeout++`（`/sys/class/net/DEV/queues/tx-N/trans_timeout` 可查）。

### 🔭 观测与调试

*   **`/sys/class/net/<dev>/`**：`ifindex/mtu/type/flags/operstate/carrier`、`queues/rx-N/{rps_cpus,rps_flow_cnt}`、`queues/tx-N/{xps_cpus,byte_queue_limits/...,trans_timeout,tx_maxrate}`、`statistics/*`、`device/`（PCI 树）、`threaded`（NAPI 线程化开关，6.6）。
*   **`ethtool`**：`-l` 看通道（`ethtool --show-channels`，即 num_tx/rx_queues 实际启用数）、`-L` 调整（驱动 `ndo_set_channels`）、`-k/-K` offload 开关、`-S` 驱动私有计数、`-x` RSS indirection 表。
*   **`/proc/net/dev` 字段**（`dev_seq_printf_stats()` 生成，注意**合并列**）：

| 列 | 来源 |
| :--- | :--- |
| RX: bytes, packets, errs | `rx_bytes/rx_packets/rx_errors` |
| RX: drop | `rx_dropped + rx_missed_errors`（内核+驱动缺包） |
| RX: fifo, frame | `rx_fifo_errors`；`length+over+crc+frame` 四合一 |
| TX: bytes, packets, errs, drop, fifo, colls, carrier | 同理，carrier 列为 `carrier+aborted+window+heartbeat` 四合一 |

*   **netconsole**：把 printk 经 UDP 送出（`netconsole=` 启动参数），panic 时唯一的现场；本质是劫持一个 net_device 的 TX 路径（`netpoll` 框架），`netpoll_poll_lock` 也因此出现在 NAPI 代码里。
*   常见排查：TX 队列 `trans_timeout` 增长 → 驱动环死/固件挂；`carrier` 抖动 → 看 `carrier_up/down_count`。

### 🧠 深挖难点

*   **为什么 qdisc 要"sleeping/运行"两个指针？** 数据面在 softirq 里无锁读 `txq->qdisc`（RCU），控制面 `tc` 持 RTNL 改配置。若直接换指针，会出现"旧 qdisc 还有排队包但已被释放"。两指针 + `dev_deactivate`（先切 noop、`synchronize_net()` 排空、再 reset）实现无锁切换。
*   **`real_num_*` 与 `num_*_queues` 的分裂**：分配时按 `num` 建满数组（sysfs/ethtool 可见全量），驱动按硬件资源 `netif_set_real_num_tx_queues()` 收缩——qdisc/XPS/IRQ 都以 `real_num` 为准；通道调整（`ethtool -L`）本质是改 real_num + 重配 IRQ。
*   **veth/loopback 这类"无锁设备"为什么设 `IFF_NO_QUEUE`？** 它们的"硬件"就是内存，`ndo_start_xmit` 直接把 skb 递给对端收包函数，没有排队语义；强行挂 qdisc 反而引入锁与延迟（`pfifo_fast` 也被跳过，noqueue 的 `enqueue==NULL` 使 `__dev_queue_xmit` 走"无队列快速路径"，见第 5 章）。

### ❓ 思考题

1.  `alloc_etherdev_mqs(0, 16, 16)` 之后 `real_num_tx_queues` 是多少？驱动什么时候、为什么要调用 `netif_set_real_num_tx_queues()` 改小它？
    *提示：分配时 num=real=txqs；XPS/sysfs 遍历以 real 为界，看 `ethtool -L` 的中断资源约束。*
2.  把 `net.core.default_qdisc` 改为 `fq` 后，新 up 的多队列网卡 root qdisc 是什么？每个队列的子 qdisc 又是什么？
    *提示：多队列走 `mq_qdisc_ops.attach`，它给每个队列挂的是 default_qdisc_ops。*
3.  两台机器 `ping` 断断续续，`/sys/class/net/eth0/carrier_changes` 增长很快，但 `ip link` 显示 state=UP——解释 operstate 与 carrier 的合成规则，以及 linkwatch 在其中扮演的角色。
    *提示：IFF_UP（管理）+ carrier（物理）合成 IF_OPER_UP；linkwatch 的 1s 合并窗口。*
4.  `HARD_TX_TRYLOCK` 失败时 sch_direct_xmit 的调用者会做什么？这个"乐观尝试"避免了什么开销？
    *提示：lockless qdisc 的 `__QDISC_STATE_MISSED` + 后续重跑；spin 持有者正是同 CPU 的 ndo_start_xmit。*
5.  为什么 XPS 表项可以有多个队列（`map->queues[]`），内核却仍保证"同一 flow 走同一队列"？
    *提示：`reciprocal_scale(skb_get_hash(skb), map->len)`——同流哈希相同则选中恒定。*
