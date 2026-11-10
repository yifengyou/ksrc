# Linux 网络子系统学习笔记

本目录是 Linux 6.6 内核网络子系统的深度学习笔记（GitBook）。笔记以**源码为准**：所有结构体、函数、宏均对照 Linux 6.6 真实源码核对（主要文件：`include/linux/skbuff.h`、`include/linux/netdevice.h`、`net/core/dev.c`、`net/core/skbuff.c`、`net/core/gro.c`、`net/core/page_pool.c`、`net/sched/sch_generic.c`，进阶章另含 `include/net/tcp.h`、`net/ipv4/tcp_cong.c`、`net/ipv4/tcp_cubic.c`、`net/ipv4/tcp_bbr.c`、`include/net/xdp.h`），中文行文、术语保留英文。

目录下同时存有**内核官方文档的镜像**（大量 `.rst` 文件，来自 `Documentation/networking/`），是本笔记的第一手参考资料；笔记与它们互补：`.rst` 讲"接口约定与用法"，本笔记讲"内部实现与数据流"。

### 🗺️ 学习路线图

```text
                    ┌────────────────────────────┐
                    │ 01 总体架构 (必读, 先建地图) │
                    └──────────┬─────────────────┘
              ┌────────────────┼────────────────┐
              ▼                ▼                 ▼
      ┌──────────────┐  ┌─────────────┐  ┌─────────────────┐
      │ 02 sk_buff   │  │ 03 net_device│  │ (可并行: 收/发)  │
      │ 数据结构基石  │  │ 设备与队列    │  │                 │
      └──────┬───────┘  └──────┬──────┘  └────────┬────────┘
             │    必须先于       │                  │
             └────────┬─────────┘                  │
                      ▼                            ▼
           ┌────────────────────┐      ┌────────────────────┐
           │ 04 收包路径与 NAPI  │◄────►│ 05 发包路径与 qdisc │
           │ (RX: NAPI/GRO/RPS) │ 对称  │ (TX: qdisc/BQL)    │
           └────────────────────┘      └────────────────────┘
```

建议顺序：**01 → 02 → 03 → 04 → 05**。02/03 是数据结构基础（一个讲"包"，一个讲"设备"），04/05 是两条数据面（各自可独立阅读，但互有引用）；06 起是进阶专题（传输层拥塞控制、XDP 可编程数据面），可在 04/05 之后任意切入；每章末尾的思考题用于自检。

### 📖 各章导读

| 章 | 文件 | 一句话导读 |
| :--- | :--- | :--- |
| 总体架构 | [01-网络子系统总体架构.md](docs/01-网络子系统总体架构.md) | 分层地图 + 子系统初始化（net_dev_init/sock_init/inet_init）+ 核心结构关系图 + netns 泛化 + 高性能技术索引（RSS/RPS/XPS/GRO/TSO/XDP） |
| sk_buff 详解 | [02-sk_buff详解.md](docs/02-sk_buff详解.md) | 报文元数据卡全字段解读、head/data/tail/end 与 skb_shared_info 内存布局、分配（__alloc_skb/build_skb/page_pool）、clone/pskb_copy 写时复制、truesize 记账、析构链与上下文规则 |
| net_device 与收发队列 | [03-net_device与收发队列.md](docs/03-net_device与收发队列.md) | 设备抽象全字段、alloc/register_netdevice 注册链路、默认 qdisc 挂载、notifier 与 upper/lower 拓扑、TX 锁模型（_xmit_lock/XOFF）、XPS 选路、carrier/linkwatch 状态机 |
| 收包路径与 NAPI | [04-收包路径与NAPI.md](docs/04-收包路径与NAPI.md) | 硬中断→软中断全链路、NAPI budget/IRQ 合并/线程化、GRO 深挖、__netif_receive_skb_core 分发、ip_rcv→tcp_v4_rcv 简述、page_pool 全解、RPS/RFS/aRFS、端到端时序与 softnet_stat 逐列解读 |
| 发包路径与 qdisc | [05-发包路径与qdisc.md](docs/05-发包路径与qdisc.md) | __dev_queue_xmit 全链路、qdisc 双锁模型与回退状态机、默认 qdisc 选择规则、BQL 动态字节上限算法、virtio TX 环模型、软件 TSO 分段与 skb_orphan、XDP TX、观测调优清单 |
| TCP 拥塞控制框架 | [06-TCP拥塞控制框架.md](docs/06-TCP拥塞控制框架.md) | tcp_congestion_ops（6.6 实为 include/net/tcp.h）、注册与选择链、CA 五态机、rate_sample、CUBIC（beta=717/1024/Hystart 双探测/bictcp_update）、BBR v1（BDP 模型/四态增益/8 相位循环） |
| XDP 与 eBPF 数据面 | [07-XDP与eBPF数据面.md](docs/07-XDP与eBPF数据面.md) | native/generic/offload 三形态、xdp_buff（6.6 含 txq）、五动作语义、DEVMAP/CPUMAP/XSKMAP redirect 家族、tail call 组合、观测工具箱 |

每章固定结构：**概述 → 机制详解（真实函数名/字段/文件路径）→ ASCII 调用链/内存布局图 → 深挖难点 → 观测与调试 → 思考题（附提示）**。

### 📚 官方文档镜像（.rst）导读

本目录的 `.rst` 是内核 `Documentation/networking/` 的镜像。与笔记最互补、最值得先读的几篇：

| 文档 | 内容 | 配合章节 |
| :--- | :--- | :--- |
| [skbuff.rst](skbuff.rst) | skb 数据区/共享 info/`dataref` 语义的**官方口径**（内核自己的 DOC 注释整理） | 02 |
| [napi.rst](napi.rst) | NAPI 驱动接口约定：`napi_schedule`/`napi_complete_done` 的正确用法、driver 如何挂 poll | 04 |
| [scaling.rst](scaling.rst) | RSS/RPS/RFS/aRFS/XPS 官方原理+调优文档（经典必读） | 04/05 |
| [page_pool.rst](page_pool.rst) | page_pool API 契约（flags/alloc/put 语义、与驱动的责任边界） | 04 |
| [segmentation-offloads.rst](segmentation-offloads.rst) | GSO/TSO/GRO/UDP GSO 家族的语义与使用约束 | 04/05 |
| [netdevices.rst](netdevices.rst) | net_device 驱动接口总览（ndo_* 契约、锁规则） | 03 |

其它按需查阅：[kapi.rst](kapi.rst)（内核 API 索引）、[statistics.rst](statistics.rst)（ethtool/netlink 统计接口）、[checksum-offloads.rst](checksum-offloads.rst)（校验和 offload 约定，配合 02 章 ip_summed）、[multiqueue.rst](multiqueue.rst)（多队列）、[msg_zerocopy.rst](msg_zerocopy.rst)（MSG_ZEROCOPY）、[af_xdp.rst](af_xdp.rst)（AF_XDP）、[filter.rst](filter.rst)（BPF/过滤器）。

### 🧭 阅读方法

*   **手边开一份 6.6 源码**：笔记中的每个函数名都可直接跳转定位；推荐从 `net/core/dev.c` 的 `__dev_queue_xmit` 与 `net_rx_action` 两个函数开始通读。
*   **图先于码**：每章 ASCII 图是"心智模型"，先看懂图再读源码可以省一半时间。
*   **动手观测**：结合每章"观测与调试"小节，在自己的机器上跑 `/proc/net/softnet_stat`、`tc -s`、`ethtool -S`，把抽象计数变成直觉。
*   **术语约定**：笔记默认 x86-64、`SMP_CACHE_BYTES=64`、`PAGE_SIZE=4096`、`MAX_SKB_FRAGS=17`（CONFIG_MAX_SKB_FRAGS 由 PAGE_SIZE 派生）；其它架构数值会不同，机制不变。

### 🗃️ 源码地图（笔记 ↔ 内核文件对照）

| 内核文件 | 内容 | 对应章节 |
| :--- | :--- | :--- |
| `include/linux/skbuff.h` | struct sk_buff / skb_shared_info / 全部内联 API | 02 |
| `net/core/skbuff.c` | 分配/clone/拷贝/析构实现、tracepoint | 02 |
| `include/linux/netdevice.h` | net_device / netdev_queue / napi_struct / softnet_data / ptype_all | 01/03/04 |
| `net/core/dev.c` | 收发主干（__dev_queue_xmit、net_rx_action、__netif_receive_skb_core、net_dev_init、register_netdevice） | 全部 |
| `net/core/gro.c` | GRO 引擎（dev_gro_receive、napi_gro_receive） | 04 |
| `net/core/page_pool.c` + `include/net/page_pool/` | page_pool 实现 | 04 |
| `net/sched/sch_generic.c` | qdisc 通用层（__qdisc_run、sch_direct_xmit、attach_default_qdiscs、pfifo_fast） | 05 |
| `net/sched/sch_api.c` | qdisc 注册与 netlink 接口 | 05 |
| `lib/dynamic_queue_limits.c` | BQL 算法 | 05 |
| `net/socket.c` | sockfs、sock_init | 01 |
| `net/ipv4/af_inet.c` | inet_init、协议注册 | 01 |
| `net/ipv4/{ip_input,ip_output}.c` | IP 收/发 | 04/05 |
| `net/ipv4/tcp_ipv4.c` | tcp_v4_rcv | 04 |
| `drivers/net/virtio_net.c` | 驱动样本（poll/start_xmit） | 04/05 |
| `include/net/net_namespace.h` | struct net 泛化 | 01 |

### 📇 术语速查表

| 术语 | 全称/出处 | 一句话解释 |
| :--- | :--- | :--- |
| skb / sk_buff | socket buffer | 报文元数据结构（第 2 章） |
| truesize | sk_buff 字段 | skb 分摊的真实内存字节数，socket 记账单位 |
| NAPI | New API | 中断+轮询混合收包模型（第 4 章） |
| budget | net_rx_action 参数 | 单次软中断/单次 poll 的包数配额（64/300） |
| ksoftirqd | per-CPU 内核线程 | softirq 超预算后的兜底执行者 |
| GRO / LRO | Generic/Large Receive Offload | 软/硬件收包聚合 |
| GSO / TSO | Generic/TCP Segmentation Offload | 大包分段下推（软件/硬件） |
| qdisc | queueing discipline | 发包排队规则（第 5 章） |
| pfifo_fast | — | 3 band 优先级 FIFO，默认 qdisc |
| BQL | Byte Queue Limits | 驱动环 in-flight 字节动态上限 |
| bufferbloat | — | 深队列造成的排队时延膨胀（AQM/BQL 的靶子） |
| RSS | Receive Side Scaling | 网卡多队列硬件哈希分流 |
| RPS / RFS | Receive Packet/Flow Steering | 软件按包/按流跨 CPU 重派 |
| aRFS | accelerated RFS | 网卡下推的 RFS |
| XPS | Transmit Packet Steering | 发包 CPU→队列映射 |
| XDP | eXpress Data Path | 驱动收包最早点的 eBPF 钩子 |
| page_pool | — | 驱动 RX 页回收池（第 4 章） |
| RTNL | rtnetlink mutex | 网络配置面全局串行锁 |
| netns | network namespace | struct net 级的网络栈实例化 |
| EDT | Earliest Departure Time | 用 skb->tstamp 携带发送时刻（fq qdisc） |
| softnet_data | per-CPU 结构 | 收发包软中断的 per-CPU 状态枢纽 |

