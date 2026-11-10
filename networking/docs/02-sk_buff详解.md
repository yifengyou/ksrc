# sk_buff 详解

`struct sk_buff`（socket buffer）是 Linux 网络栈的"通货"：一个报文从网卡 DMA 进内存，到被应用进程 `recvmsg` 读走（或反过来），全程由同一个（或一族共享数据的）sk_buff 描述。它定义于 `include/linux/skbuff.h`，实现在 `net/core/skbuff.c`（约 6500 行）。本章基于 Linux 6.6 逐一解读其字段、数据区布局、分配/克隆/析构全链路，以及 truesize 内存记账模型。

### 🎯 核心设计：元数据与数据分离

sk_buff 本身**不含报文数据**，它是一张"报文元数据卡"：

*   **元数据**（`struct sk_buff`，约 200 字节）走 slab 快速分配；
*   **数据**（`skb->head` 指向的 head buffer + 可选的 page frag）独立分配，可被多个 sk_buff **共享**（clone）；
*   数据又被切分为**线性区**（linear data，位于 head 内）与**非线性区**（frags/frag_list，指向 page），大包不必物理连续。

这个设计支撑了内核网络几乎所有的高性能路径：GRO 聚合（往 frags 追加）、TCP 重传（clone 共享 payload）、zerocopy 发送（payload 直接指向用户页）。

### 🗂️ 字段逐一解读（Linux 6.6 布局）

以下按 `include/linux/skbuff.h` 中的实际顺序分组解读（保留真实字段名）。

#### 1) 链表挂载区（union，必须位于结构体开头）

```c
union {
    struct {
        struct sk_buff  *next;
        struct sk_buff  *prev;      /* 与 sk_buff_head 兼容 */
        union {
            struct net_device *dev; /* 到达/离开的设备 */
            unsigned long dev_scratch; /* dev==NULL 时的复用(UDP rx) */
        };
    };
    struct rb_node    rbnode;   /* netem / ip4 defrag / tcp 红黑树 */
    struct list_head  list;     /* 链表挂载(如 GRO list) */
    struct llist_node ll_node;  /* socket defer_list 等无锁链表 */
};
struct sock *sk;
union {
    ktime_t tstamp;             /* 到达时间戳(收) */
    u64     skb_mstamp_ns;      /* 最早离开时间(EDT/qdisc 用) */
};
```

*   `next/prev`：sk_buff 总是成队出现的；`struct sk_buff_head { next; prev; qlen; lock; }` 与之头部兼容，`skb_queue_head_init()` 初始化的就是 sk_buff_head。
*   `dev`：收包时由驱动设置（`eth_type_trans` 前），发包时指向出口设备；进入 TCP 后被置 NULL（`tcp_v4_rcv` 中 `skb->dev = NULL`），因此才有 `dev_scratch` 复用该存储。
*   `tstamp` 与 `skb_mstamp_ns` 是同一 64bit 的两种语义：ingress 记录到达时间（`net_timestamp_check`），egress 在启用 EDT（Earliest Departure Time）时记录最早发出时间。

#### 2) 层间传参区与路由/DST

```c
char cb[48] __aligned(8);          /* 控制块：每层自由复用 */
union {
    struct {
        unsigned long _skb_refdst; /* dst_entry 指针|低 1 位标记 NOREF */
        void (*destructor)(struct sk_buff *skb);
    };
    struct list_head tcp_tsorted_anchor; /* TCP tsorted_sent_queue */
    unsigned long _sk_redir;       /* skmsg 重定向 */
};
unsigned long _nfct;               /* conntrack(含 nfctinfo 低 3 位) */
```

*   `cb[48]`：内核版"消息传递区"。任何层都能用，但**不跨 clone 保存**——队列的拥有者独占使用。经典用途：qdisc 的 `qdisc_skb_cb`（`pkt_len`）、TCP 的 `tcp_skb_cb`（seq/flags/sacked，约占 44B）、IP 的 `inet_skb_parm`（`struct inet_skb_parm` 经 `sock_skb_cb_check_size` 保证 ≤48B）。
*   `_skb_refdst`：挂路由结果 `dst_entry`。低位置 1（`SKB_DST_NOREF`）表示未加引用计数（rcu 保护下的只读借用），置 0 表示持引用（`skb_dst_force`）——发包路径在设备不需要 dst 时尽早 `skb_dst_drop`（`IFF_XMIT_DST_RELEASE`），正是为了省引用操作。
*   `destructor`：析构回调（如 `sock_wfree`/`sock_rfree`），在 `skb_release_head_state()` 中调用。

#### 3) 长度几何区

```c
unsigned int len, data_len;   /* len=线性+非线性总长; data_len=非线性长 */
__u16 mac_len, hdr_len;       /* L2 头长; clone 的可写头长 */
__u16 queue_mapping;          /* 多队列设备上的队列号 */
```

*   `len - data_len` = 线性区有效字节数（`skb_headlen()`）。头部操作永远只碰线性区；非线性区追加只动 `data_len` 与 `len`。
*   `hdr_len` 专用于"payload-only clone"（见写时复制一节）。

#### 4) 标志位区（bitfield，注意 `__cloned_offset`/`__pkt_type_offset` 锚点）

```c
__u8 cloned:1,      /* head 可能被 clone,需检查 dataref */
    nohdr:1,        /* payload-only 引用,禁止改头 */
    fclone:2,       /* SKB_FCLONE_UNAVAILABLE/ORIG/CLONE */
    peeked:1,       /* 已做过统计,勿重复 */
    head_frag:1,    /* head 来自 page frag 而非 kmalloc */
    pfmemalloc:1,   /* 来自内存回收保留池 */
    pp_recycle:1;   /* page_pool 回收标记 */
__u8 active_extensions;  /* CONFIG_SKB_EXTENSIONS */
```

*   `pp_recycle`：6.6 中 page_pool 回收的"意图位"。由 `skb_mark_for_recycle()` 设置，驱动 RX 页来自 page_pool 时使用；释放路径据此把 head page 送回池子而不是伙伴系统。
*   `head_frag` 决定 head 的释放方式（`skb_free_frag` vs `kfree`），`build_skb`/`napi_alloc_skb` 产物都是 1。

#### 5) headers group（单次 memcpy 拷贝的字段集合）

`struct_group(headers, ...)` 把热字段打包，`__copy_skb_header()` 对整块 `memcpy`，`CHECK_SKB_FIELD()` 用 `BUILD_BUG_ON` 锁定成员偏移。包含（按实际位序节选）：

*   `pkt_type:3`：PACKET_HOST/OTHERHOST/BROADCAST/MULTICAST（`eth_type_trans` 对比 `dev_addr` 设置）；
*   `ip_summed:2`：**校验和三态**，驱动→协议的契约（见下表）；
*   `ignore_df`、`ooo_okay`（允许换队列，用于多队列重排规避）、`dst_pending_confirm`、`remcsum_offload`；
*   `mono_delivery_time`（tstamp 是 mono 基 EDT）、`tc_at_ingress`/`tc_skip_classify`（tc/IFB 语义）、`encapsulation`（内层头有效，隧道场景）、`encap_hdr_csum`、`csum_valid`、`csum_complete_sw`、`csum_level:2`（嵌套校验和深度，QUIC/UDP tunnel 关键）、`csum_not_inet`（SCTP CRC32c）、`slow_gro`（含 GRO 不友好状态：orpahn/ct/ext）、`inner_protocol_type`；
*   `l4_hash`/`sw_hash`：`hash` 的来源标注（硬件 4 元组 / 软件计算）；
*   `wifi_acked_valid:1`/`wifi_acked:1`（CONFIG_WIRELESS，无线 ACK 状态）；
*   `tc_index`（tc 动作标记）、`alloc_cpu`（分配 CPU，供 per-CPU 缓存回收）。

| `ip_summed` 值 | 含义 | 生成者 |
| :--- | :--- | :--- |
| `CHECKSUM_NONE` | 校验和未验证/不可信，L4 自行计算 | 朴素驱动、虚拟设备 |
| `CHECKSUM_UNNECESSARY` | 硬件已验证（`csum_level` 指示可信层数） | 多数 NIC（RSS 时代主流） |
| `CHECKSUM_COMPLETE` | `skb->csum` = 全包伪头校验和（L4 用增量式验证） | 老驱动 / virtio |
| `CHECKSUM_PARTIAL` | 发送方向：硬件只算 L4，`csum_start/csum_offset` 定位 | 协议栈（TSO 前置条件） |

#### 6) 校验和/哈希/协议与头偏移区

```c
union {
    __wsum csum;                   /* CHECKSUM_COMPLETE 的结果 */
    struct { __u16 csum_start, csum_offset; }; /* CHECKSUM_PARTIAL */
};
__u32 priority;                    /* qdisc/DS 映射的排序键 */
int  skb_iif;                      /* 入设备 ifindex */
__u32 hash;                        /* 流哈希(RSS/RPS/GRO 分桶都用) */
union { u32 vlan_all; struct { __be16 vlan_proto; __u16 vlan_tci; }; };
union { unsigned int napi_id;      /* busy poll 用 */
        unsigned int sender_cpu; };/* XPS 用(共享存储!) */
__u32 secmark; __u32 mark;         /* skb mark(tc/nftables) */
union { __be16 inner_protocol; __u8 inner_ipproto; };
__u16 inner_transport_header, inner_network_header, inner_mac_header;
__be16 protocol;                   /* Ethertype(htons(ETH_P_IP)...) */
__u16 transport_header, network_header, mac_header; /* 相对 head 的偏移 */
```

*   三个 `*_header` 是**相对 `skb->head` 的偏移**（64 位平台如此），不是指针——配合 `skb_network_header()` 等访问器。
*   `napi_id`/`sender_cpu` 是同一 u32 的两个身份：收路径存"哪个 NAPI 实例产出我"（busy poll 据此直达），发路径存"哪个 CPU 送我"（XPS 映射的 key）。`skb_set_queue_mapping` 前由 `netdev_core_pick_tx` 换算。
*   老 sk_buff 的 `sp`（xfrm secpath）、`seq` 等字段已迁出主体，进入 `extensions`（见下）。

#### 7) 尾部几何区（**必须位于结构体末尾**，`__alloc_skb` 依赖 `memset(0, offsetof(struct sk_buff, tail))` 只清前半）

```c
sk_buff_data_t tail, end;   /* 64位下是 u32 偏移而非指针 */
unsigned char *head, *data;
unsigned int   truesize;    /* 本 skb 实际占用(含共享部分) */
refcount_t     users;       /* sk_buff 外壳引用计数 */
struct skb_ext *extensions; /* CONFIG_SKB_EXTENSIONS: xfrm secpath 等 */
```

*   `users` 与 `dataref` 是**两级不同的引用计数**：`users` 数"多少个指针指着这张元数据卡"（`skb_get` +1），`dataref`（在 shared_info 里）数"多少引用共享这份数据"。
*   `struct skb_ext { refcount_t refcnt; u8 offset[SKB_EXT_NUM]; u8 chunks; char data[]; }` 按需分配的扩展块（`SKB_EXT_SEC_PATH`/`SKB_EXT_BRIDGE_NF`/`TC_SKB_EXT`/`MPTCP_EXT`），跨 clone 共享，仅 `active_extensions` 非零时可访问。

### 📐 数据区内存布局

```text
sk_buff (元数据卡, slab 分配)
 ┌───────────────────────────────────────────────────────────┐
 │ next/prev/sk/tstamp/cb/_skb_refdst/len/.../tail/end/head/data │
 └──────────┬────────────────────────────────────────────────┘
            │ head 指向一次性分配的"数据缓冲"(kmalloc 或 page frag)
            ▼
 ┌───────────┬─────────────┬────────────┬───────────┬──────────────────────┐
 │  padding  │  headroom   │    data    │ tailroom  │  skb_shared_info      │
 │           │(push 空间)  │............│ (put 空间)│  (紧贴 end 之后)      │
 └───────────┴─────────────┴────────────┴───────────┴──────────────────────┘
            ▲              ▲           ▲            ▲
          head            data        tail         end
                         [线性数据: len - data_len]

skb_shared_info（__alloc_skb 中清零, 跨 clone 不变）:
 ┌────────────────────────────────────────────────────────────┐
 │ flags(meta_len,nr_frags,tx_flags...)  gso_size gso_segs    │
 │ gso_type(SKB_GSO_TCPV4/...)  frag_list(整包链)  hwtstamps  │
 │ tskey  dataref(atomic)  xdp_frags_size  destructor_arg     │
 │ frags[MAX_SKB_FRAGS]  ← 必须 last,见 pskb_expand_head()     │
 └────────────────────────────────────────────────────────────┘
        │ frags[i] = skb_frag_t
        ▼
 ┌──────────────┐  ┌──────────────┐  ┌──────────────┐
 │ page +offset │  │ page +offset │  │ ...最多 17 个 │   (非线性区)
 │ +size        │  │ +size        │  │ (MAX_SKB_FRAGS)│
 └──────────────┘  └──────────────┘  └──────────────┘
```

关键事实（6.6）：

*   **`tail`/`end` 是偏移**：`#ifdef NET_SKBUFF_DATA_USES_OFFSET`（BITS_PER_LONG>32）时 `typedef unsigned int sk_buff_data_t`，省内存且天然支持 `__copy_skb_header` 的整块拷贝（偏移在新旧 head 间可平移）。
*   **shared_info 的对齐**：`SKB_DATA_ALIGN(X) = ALIGN(X, SMP_CACHE_BYTES)`，`SKB_HEAD_ALIGN(X) = SKB_DATA_ALIGN(X) + SKB_DATA_ALIGN(sizeof(struct skb_shared_info))`。分配时 `__build_skb_around()` 把 `shinfo` 放在 `ksize(data)` 的**最末端**（`kmalloc_size_roundup` 常给出比请求更大的块，多余空间全部变成 tailroom）。
*   `skb_frag_t` 就是 `struct bio_vec { struct page *bv_page; unsigned int bv_len, bv_page 偏移(bv_offset); }`，访问器 `skb_frag_page()/skb_frag_size()/skb_frag_address()/__skb_frag_ref()/__skb_frag_unref()`。
*   `dataref` 是**双计数器**：低 16 位总引用数，高 16 位 payload-only 引用数（`SKB_DATAREF_SHIFT=16`）。TCP 发送时 `__skb_header_release()` 置 `nohdr` 并把引用加在高 16 位上——之后的 clone 只要不改头就无需拷贝（`skb_header_cloned()` 检查）。
*   `destructor_arg`（即 `skb_uarg(skb)`）：zerocopy 发送时指向 `struct ubuf_info`（用户页 pin 状态与完成回调 `ubuf_info.callback`），TX 完成时触发 `sock_zerocopy_callback`。

`skb_shared_info` 两侧的标志位（常被忽略但语义关键）：

*   `flags`（`SKBFL_*`）：`SKBFL_SHARED_FRAG`（frags 与用户 zerocopy 共享——网络栈改写 payload 前必须 `skb_orphan_frags` 先拷贝/取 pin）、`SKBFL_PURE_ZEROCOPY`（完全零拷贝，连头都来自用户）、`SKBFL_MANAGED_FRAG_REFS`（frags 引用由外部管理，释放时不 unref，tls-device/带内加密用）。
*   `tx_flags`（`SKBTX_*`，发送侧）：`SKBTX_NEED_HWSTAMP/SWSTAMP`（硬件/软件时间戳请求，`SO_TIMESTAMPING`）、`SKBTX_SCHED_TSTAMP`（qdisc 入队时刻，EDT 测量）、`SKBTX_ZEROCOPY_FRAG`（ubuf_info 有效）、`SKBTX_TSTAMP_ZEROCOPY`（从用户内存直接读时间戳字）。
*   `gso_type` 是位图（`SKB_GSO_TCPV4|SKB_GSO_TCPV6|SKB_GSO_UDP_L4|SKB_GSO_FRAGLIST|SKB_GSO_DODGY|...`），`validate_xmit_skb` 依此判定设备能否硬件分段（第 5 章）。

### 🏗️ 分配路径

#### `__alloc_skb()`（net/core/skbuff.c）——一切分配的汇合点

```c
struct sk_buff *__alloc_skb(unsigned int size, gfp_t gfp_mask,
                            int flags, int node)
{
    cache = (flags & SKB_ALLOC_FCLONE) ? skbuff_fclone_cache : skbuff_cache;
    if ((flags & (SKB_ALLOC_FCLONE | SKB_ALLOC_NAPI)) == SKB_ALLOC_NAPI && ...)
        skb = napi_skb_cache_get();      /* NAPI per-CPU skbuff 缓存 */
    else
        skb = kmem_cache_alloc_node(cache, gfp_mask & ~GFP_DMA, node);
    ...
    data = kmalloc_reserve(&size, gfp_mask, node, &pfmemalloc);
    /* kmalloc_size_roundup 后把 shinfo 放到实际块末尾 */
    memset(skb, 0, offsetof(struct sk_buff, tail)); /* 只清前半! */
    __build_skb_around(skb, data, size);
    if (flags & SKB_ALLOC_FCLONE) { ... fclone_ref = 1 ... }
}
```

*   `kmalloc_reserve()`：小请求（≤`SKB_SMALL_HEAD_CACHE_SIZE`，即 `SKB_HEAD_ALIGN(MAX_TCP_HEADER)` 附近）优先走专用 `skb_small_head_cache`；否则 `kmalloc_size_roundup()` 向上取整到 kmalloc 档位并**把整块记录回 size**——多出来的部分自动成为 tailroom。
*   **FCLONE 池**：`struct sk_buff_fclones { skb1, skb2, fclone_ref }` 一次分配两个外壳，专为 TCP "本体+clone" 模式优化：`skb_clone()` 发现 `fclone==SKB_FCLONE_ORIG && fclone_ref==1` 时直接取用伴生的 skb2，免一次 slab alloc。

#### 各包装函数的分工

| 函数 | 场景 | 数据区来源 | 备注 |
| :--- | :--- | :--- | :--- |
| `alloc_skb(size, gfp)` | 通用/发送 | kmalloc | = `__alloc_skb(0, flags)` |
| `napi_alloc_skb(napi, len)` | 驱动 RX 头部区 | per-CPU `page_frag`（`napi_alloc_cache`，小包用 `page_frag_alloc_1k`） | `head_frag=1`，预置 `NET_SKB_PAD + NET_IP_ALIGN` headroom；外壳走 NAPI per-CPU 缓存 |
| `netdev_alloc_skb(dev, len)` | 老式 RX（非 NAPI 上下文） | 同上（中断上下文用 `netdev_alloc_cache`） | 现代驱动多用 page_pool + `build_skb` 替代 |
| `build_skb(data, frag_size)` | **围绕已有 page 构造**（page_pool RX、AF_PACKET、virtio） | 调用者给定 | `head_frag=1`；`napi_build_skb` 是其 NAPI 缓存版 |
| `napi_get_frags(napi)` | GRO frags 模式（驱动收到即 frag） | 复用 `napi->skb` | `napi_reuse_skb` 循环使用 |

`NET_SKB_PAD = max(32, L1_CACHE_BYTES)`（64 位 x86 为 64B），`NET_IP_ALIGN = 2`：headroom 预留让 L3 头 16B 对齐、L2 头 4B 对齐，兼顾后续 push（MAC/隧道头）与 cacheline 效率。

#### page_pool 与 `skb_mark_for_recycle()`

6.6 中主流驱动的 RX 页来自 **page_pool**（详见第 4 章）：驱动 `page_pool_dev_alloc_pages()` 拿页 → DMA 映射结果写进 `page->dma_addr`（`page_pool_set_dma_addr`）→ `build_skb()` 围绕该页构造 → `skb_mark_for_recycle(skb)` 置 `pp_recycle`。此后该 skb 无论被 clone 多少次，**最后一个释放者**负责把页送回池子（`skb_release_data()` → `napi_frag_unref(..., napi_safe)` → `page_pool_put_defragged_page()`）。没有 page_pool 时（或 `dataref` 仍 >1），走 `put_page` 回伙伴系统。

### 🧬 拷贝 / 克隆语义

四级"复制"语义，从浅到深：

| API | 复制了什么 | 引用变化 | 何时用 |
| :--- | :--- | :--- | :--- |
| `skb_get(skb)` | 什么都不复制（另一处持有同一外壳） | `users++` | 分帧暂存、qdisc 重入队 |
| `skb_clone(skb, gfp)` | 复制元数据外壳，**共享全部数据** | `dataref++`、`cloned=1`（双方） | TCP 重传、packet tap |
| `pskb_copy(skb, gfp)` | 复制外壳+线性区；frags 只加 page 引用 | frags 各 `page_ref++` | 只需改头部（改 IP 地址等） |
| `skb_copy(skb, gfp)` | 外壳+线性区+非线性区全深拷贝（顺便**线性化**） | 无共享 | 需要完整私有副本 |

`__pskb_copy_fclone()`（pskb_copy 的真身，`include/linux/skbuff.h` 中的 `pskb_copy` 是内联包装）允许指定新 headroom；`__copy_skb_header()` 拷贝 headers group 整块 + `cb` + dst + extensions + nfct，但**不拷 `sk`**（所有权不随 clone 转移）。

克隆拓扑（一图看清三级共享关系）：

```text
 skbuff_cache         head buffer(共享)              page(共享)
 ┌──────────┐        ┌────────────────────┐         ┌─────────┐
 │ skb 本体  │──head─►│ headroom|data..tail│         │ page    │
 │ cloned=1 │        │ |shinfo: dataref=2 │    ┌───►│ +offset │
 │ users=1  │        └────────────────────┘    │    │ +size   │
 └──────────┘                                  │    └─────────┘
 ┌──────────┐        本体与 clone 共享 head:     │      frags[0]
 │ clone     │──────────────────────────────────┘
 │ cloned=1 │        两者都可读数据; 任一方要写 head
 │ users=1  │        → pskb_may_pull/expand_head 触发 COW
 └──────────┘        (dataref==1 才是安全写窗口)
 skbuff_fclone_cache 版本: 本体+伴生 skb2 一次分配, fclone_ref=2
```

#### 写时复制（COW）的触发点

*   **`pskb_may_pull(skb, len)`**：确保线性区覆盖 `[data, data+len)`（典型：L4 头）。若不足则调 `__pskb_pull_tail()` 从 frags"借"数据——若 head 被 clone（`skb_cloned()`），必须先 `pskb_expand_head()` 扩容拷贝，这就是最常见的 COW 触发。`__pskb_pull_tail` 借出的数据不会归还 frags，会同时修正 `data_len`。
*   **`pskb_expand_head(skb, nhead, ntail, gfp)`**：分配新 head、搬数据、修正全部头偏移。`frags[]` 必须是 shinfo 最后一个字段正是为了它——扩容时把旧 shinfo 整块 memcpy 后旧 frags 指针依旧有效。
*   **TCP 的 nohdr 技巧**：`skb_reserve → __skb_header_release → skb_clone` 三连（见 skbuff.h 顶部 DOC 注释）把 payload 引用记在高 16 位 dataref，发送路径改头无需拷贝，是 TCP 零拷贝重传的核心。

### 🔧 线性区 / 非线性区操作速查

```c
skb_reserve(skb, len)   /* data += len; tail += len;  划出 headroom */
skb_put(skb, len)       /* tail += len; 返回旧 tail   追加数据区 */
skb_push(skb, len)      /* data -= len; 返回新 data   头部前压 */
skb_pull(skb, len)      /* data += len; len -= len    剥掉头部 */
skb_trim(skb, len)      /* 调整 len(可能退回 frag)    截断 */
skb_headroom / skb_tailroom / skb_end_offset
skb_headlen(skb)        /* len - data_len */

/* 非线性区: */
skb_fill_page_desc(skb, i, page, off, size);  /* 填 frags[i] 并置 nr_frags */
skb_add_rx_frag(skb, i, page, off, size, truesize);
    /* = skb_fill_page_desc + len/data_len/truesize 三连加(驱动 RX 用) */
skb_coalesce_rx_frag(skb, i, size, truesize); /* mergeable buf 追加同一 frag */
skb_frag_ref/eref/unref(page 引用操作)
skb_linearize(skb)      /* __pskb_pull_tail 全量拉平(改 L4 时用) */
```

`skb_add_rx_frag` 的 truesize 参数由驱动给出（通常是 frag 实际分摊的字节数，如 `SKB_DATA_ALIGN(size) + SKB_DATA_ALIGN(sizeof(shinfo))` 分摊份额），直接决定 socket 记账——驱动少报会导致内存"隐形"。

**队列操作**（skb 几乎总在 `sk_buff_head` 队列里流转）：

```c
struct sk_buff_head {
    struct sk_buff *next, *prev;   /* 与 sk_buff 头部布局兼容! */
    __u32 qlen;
    spinlock_t lock;
};
void skb_queue_head_init(struct sk_buff_head *list);  /* lock+链空 */
void __skb_queue_tail(head, skb);  /* 无锁版: 已持 lock 或 softirq 专属队列 */
void skb_queue_tail(head, skb);    /* 拿锁版 */
struct sk_buff *skb_dequeue(head); /* 摘头并清 skb->next/prev */
void skb_queue_purge(head);        /* 清空并逐个 kfree_skb */
/* 著名消费者: sk->sk_receive_queue / qdisc 队列 / sd->input_pkt_queue
   著名无锁变体: __netif_receive_skb 的 list 化路径用 skb->list + list_head */
```

`next/prev` 必须是 sk_buff 前 16 字节的布局约束，正是为了让 `skb_queue_head` 与 `sk_buff` 用同一套 `__skb_insert/__skb_unlink` 操作——类型双关的老牌优化。

### ⚖️ truesize 与 socket 内存记账

**truesize = 本 skb 分摊的总内存**：`SKB_TRUESIZE(X) = X + sizeof(struct sk_buff) + SKB_DATA_ALIGN(sizeof(struct skb_shared_info))`，非线性 frags 的页按驱动申报值累加。它是 socket 内存治理的计量单位：

```text
TX: skb_set_owner_w(skb, sk)
     └→ sk_wmem_alloc += truesize; destructor = sock_wfree
        (wmem > sk_sndbuf 时 __sk_stream_wait_memory 睡眠等待)
RX: skb_set_owner_r(skb, sk)
     └→ sk_rmem_alloc += truesize; destructor = sock_rfree
        (tcp_v4_rcv → tcp_try_rmem_schedule → sk_mem_schedule:
         超过 sk_rcvbuf → tcp_add_backlog 拒绝/丢弃)

TCP 全局池(netns_ipv4.tcp_mem[] = min/pressure/max):
 memory_allocated(原子) > tcp_mem[1] → tcp_memory_pressure = 1
 > tcp_mem[2] → 拒绝分配, collapsed/push 待收 → 影响发送窗口
```

*   `sk_sndbuf/sk_rcvbuf` 由 `SO_SNDBUF/SO_RCVBUF` 设置（受 `sysctl_wmem_max/rmem_max` 封顶；TCP 会翻倍并至少 `SOCK_MIN_SNDBUF`）。

记账的落点代码（net/core/sock.c，节选）：

```c
void skb_set_owner_w(struct sk_buff *skb, struct sock *sk)
{
    skb_orphan(skb);                  /* 先脱离旧主 */
    skb->sk = sk;
    refcount_inc(&sk->sk_wmem_alloc); /* 预占 1, 防 wfree 竞态归零 */
    skb->destructor = sock_wfree;
    atomic_add(skb->truesize, &sk->sk_wmem_alloc);
    ...
}
/* 发送完成/TX 队列丢弃时: sock_wfree(skb) →
     sub_and_test(truesize, sk_wmem_alloc) → 0 时唤醒等待的发送者
     (TCP: __sk_stream_wait_memory 的 socket_wake_up) */
```

*   **GRO 的记账失真**：GRO 聚合 N 个包后 truesize 只累计一次"页分摊"，拆开分别记 `sk_rmem_alloc` 时（`tcp_add_rx_frag` 路径）又按合并值算——内核用 `tcp_rmem` 相关的 `skb->truesize` 校正处理，但驱动错误申报 truesize 仍会造成 `rmem` 虚高与虚假丢包（生产事故常见根因）。
*   观测：`ss -m`（skmem 行）、`/proc/net/sockstat`（`tcp: inuse ... mem x`，单位是 PAGE_SIZE）、`cat /proc/net/sockstat` 的 `mem` 超过 `tcp_mem[2]` 时出现全局压力。

### ♻️ 生命周期与析构

```text
kfree_skb(skb) ────────────── 通用释放入口(允许任何上下文,但 IRQ 中应
 │                              用 dev_kfree_skb_irq 延迟)
 ├─ (skb_unref: users-- , 减到 0 才继续)
 ├→ kfree_skb_reason(skb, reason)   /* 6.6: kfree_skb 即 reason=NOT_SPECIFIED */
 │    └→ __kfree_skb
 │        ├→ skb_release_head_state
 │        │    ├→ skb_dst_drop (释放 dst 引用)
 │        │    ├→ destructor(skb)   /* sock_wfree: 归还 wmem 记账+唤醒 */
 │        │    ├→ nf_conntrack_put / skb_ext_put
 │        │    └→ (pp_recycle 相关清理)
 │        ├→ skb_release_data(skb, reason, napi_safe)
 │        │    ├→ dataref-- (cloned 时可能直接退出, 由最后一者负责)
 │        │    ├→ skb_zcopy_clear (ubuf_info 完成回调)
 │        │    ├→ for frags: napi_frag_unref → page_pool 回收 / put_page
 │        │    ├→ kfree_skb_list(frag_list)
 │        │    └→ skb_free_head → skb_pp_recycle / skb_free_frag / kfree
 │        └→ kfree_skbmem → skbuff_cache / skbuff_fclone_cache
 │
 ├→ consume_skb(skb)   /* "成功消费"版本: 走 trace_consume_skb */
 ├→ napi_consume_skb(skb, budget)
 │    /* 驱动 TX 完成于 NAPI poll 中调用: budget!=0 时外壳直接
 │       进 napi_skb_cache_put (per-CPU 复用), 数据页回 page_pool */
 └→ dev_kfree_skb_irq(skb)
      /* IRQ 上下文: 挂到 sd->completion_queue, raise NET_TX softirq,
         net_tx_action 中批量 __kfree_skb —— 避免在中断里做析构 */
```

**上下文规则**（`netdevice.h` 注释原话的工程化转述）：

| 函数 | 允许上下文 | 原因 |
| :--- | :--- | :--- |
| `kfree_skb`/`consume_skb` | 进程 / softirq（IRQ 关闭时危险） | 析构可能调 `sock_wfree`→`wake_up`（不能在硬中断） |
| `napi_consume_skb` | NAPI poll（softirq） | `napi_safe=true` 允许 page_pool 直接回 alloc cache |
| `dev_kfree_skb_irq` | 硬中断 | 只链入 completion_queue，真正释放在 NET_TX softirq |
| `dev_kfree_skb_any` | 自动二选一 | 检测 `in_irq()` |

### 🔎 实战走读：一个 UDP 回显包的一生

把全章知识串起来——`udp_server.sendto(pong)` 回显 `recvfrom(ping)` 的两个 skb（简化但函数真实）：

```text
[RX: ping 包, 512B, 网卡带 page_pool+XDP 支持]
① 硬中断 → napi_schedule_irqoff                    (第 4 章)
② 驱动 poll: page_pool_dev_alloc_pages() 取页       (head 将围绕它 build)
③ build_skb(page, frag_size) → __build_skb_around:
     shinfo 放页尾, data 指向 NET_SKB_PAD+NET_IP_ALIGN 后
④ skb_mark_for_recycle(skb) → pp_recycle=1
⑤ eth_type_trans: data 前推 ETH_HLEN, protocol=ETH_P_IP,
     pkt_type=PACKET_HOST; vlan offload 若有则剥 tag 进 vlan_tci
⑥ napi_gro_receive → (无同流) GRO_HELD 挂桶? 否则 GRO_NORMAL
⑦ gro_normal_one → __netif_receive_skb_list_core
     → ptype_base[ETH_P_IP&15] → deliver ip_rcv
⑧ ip_rcv → ip_rcv_finish_core: 路由命中本机
     → dst_input → ip_local_deliver_finish → udp_rcv
⑨ udp_rcv → __udp4_lib_lookup_skb 找到 server socket
     → udp_queue_rcv_skb: 校验(csum 折算)、入 sk_receive_queue
     → skb_set_owner_r(truesize 记入 sk_rmem_alloc, destructor=sock_rfree)
     → sk_data_ready → sock_def_readable → wake_up
     (用户进程从 recvfrom 返回, skb 挂在队列上等待被读)

[TX: pong 包, 应用构造新 skb]
① tcp/udp_sendmsg: alloc_skb(size, GFP_KERNEL)
     __alloc_skb → kmalloc_reserve(kmalloc-512 档) → shinfo 贴 ksize 尾
② __ip_append_data: skb_reserve(NET_SKB_PAD+...) 逐段 skb_put 拷贝用户数据
     (或 zerocopy: ubuf_info 挂 shinfo->destructor_arg)
③ ip 层 push IP 头, udp push UDP 头, CHECKSUM_PARTIAL 置位
     (csum_start/csum_offset 指向 UDP 头, 硬件只算 pseudo 头增量)
④ dst 挂 _skb_refdst; mark/priority 由策略路由设置
⑤ dev_queue_xmit → __dev_queue_xmit (第 5 章):
     选 txq → __dev_xmit_skb → qdisc enqueue → sch_direct_xmit
     → validate_xmit_skb (设备支持 csum/sg 则原样)
     → ndo_start_xmit: 描述符指向 head+frags, 前面已 skb_orphan
⑥ TX 完成环: napi_consume_skb / dev_kfree_skb_irq
     → skb_release_data: frags/pp_recycle 页回 page_pool 池
     → 若 ①: sock_wfree 已在 orphan 时清账, 此处纯释放外壳
```

注意两条对照：**RX 的 skb 走向 socket 后由用户进程释放**（recvfrom 最后 `consume_skb`）；**TX 的 skb 在 orphan 之后与 socket 脱钩**，完成路径只关心数据页的回收。

### 🔍 观测与调试

*   **drop reason 机制**：`kfree_skb_reason()` 携带 `enum skb_drop_reason`（`SKB_DROP_REASON_NOT_SPECIFIED/QDISC_DROP/CPU_BACKLOG/UNHANDLED_PROTO/NO_SOCKET/TCP_CSUM/...`，定义于 skbuff.h），存进 cb 的 `kfree_skb_cb`。这是 5.17+ 引入的丢包可观测性核心。
*   **tracepoints**：`skb:kfree_skb`（含 reason 与 location）、`skb:consume_skb`、`net:net_dev_queue`、`net:netif_receive_skb`。用法：`perf record -e skb:kfree_skb -a sleep 10; perf script`，或 `dropwatch -l kas`（BPF 版 `drop_monitor` 更快）。
*   **`skb_dump(level, skb, full_pkt)`**（net/core/skbuff.c）：把 skb 几何（headroom/线性/frags/gso）与十六进制内容打到 dmesg——内核态代码调试利器。
*   **Kconfig 调试开关**：`CONFIG_DEBUG_NET`（net/Kconfig.debug，`DEBUG_NET_WARN_ON_ONCE` 系列断言，如"destructor 在硬中断中调用"）、`CONFIG_DEBUG_SG`（lib/Kconfig.debug，scatterlist 合法性检查）。老的 `SKB_PARANOIA` 类检查已移除；如需"越界审计"，最接近的组合是 CONFIG_DEBUG_NET + KASAN + `skb_dump`。
*   **内存泄漏**：`kmemleak`（skbuff_cache 会自然出现在报告中）、`/proc/net/sockstat` 的 mem 字段持续上涨 → 排查驱动 truesize 申报。

### 🧠 深挖难点

*   **为什么 `dataref` 的 payload-only 位只给 TCP 用？** 因为语义要求"所有 payload-only 引用的 hdr_len 相同且从不修改"，只有 TCP 发送队列满足这个纪律（同一 skb 的重传 clone）。通用协议层拿不到该保证，老实走 pskb_copy。
*   **`skb_clone` 后谁负责 `sk_wmem_alloc`？** clone 不复制 `sk` 与 destructor 归属——发送路径在 `__dev_queue_xmit` 前用 `skb_orphan`（调 destructor、清 sk）把记账还给 socket，设备层拿到的是"孤儿"skb；TX 完成直接 free。TSO 软件分段（`skb_gso_segment`）也要求先 orphan（每段的内存归属不能挂在原 socket 上重复计算）。
*   **`build_skb` 为什么要求调用者保证页尾留了 shared_info 空间？** shinfo 位于 `data + size` 处，`frag_size` 参数就是告诉内核"这块内存实际有多大"（页订单大小），越界写会破坏相邻内存——AF_XDP/page_pool 路径都按 `SKB_HEAD_ALIGN` 预留。

### ❓ 思考题

1.  `alloc_skb(1500)` 实际 kmalloc 出多大的块？headroom 和 tailroom 各是多少？
    *提示：`kmalloc_size_roundup(SKB_HEAD_ALIGN(1500))`；shinfo 放在 ksize 末尾而非请求值末尾。*
2.  TCP 发送一个大 GSO skb（`gso_segs=40`）后被软件分段，`sk_wmem_alloc` 的总量如何变化？分段前后各是谁持有记账？
    *提示：`skb_orphan` 在 `__skb_gso_segment` 中的位置；每段 truesize 的分摊规则（`seg->truesize += truesize/nsegs` 之外的整除余量处理）。*
3.  为什么 `napi_consume_skb` 有 `budget` 参数且 0 表示"非 NAPI 上下文"？netpoll 为什么传 0？
    *提示：`napi_skb_cache_put` 要求本 CPU softirq 串行化；netpoll 在硬中断里跑 poll。*
4.  驱动把 16 个包 GRO 成一个 skb 上送 TCP，`sk_rmem_alloc` 记 1 次 truesize 还是 16 次？若 16 个包后来在收队列里被 `skb_split` 拆开呢？
    *提示：GRO 聚合发生在 `sk_set_owner_r` 之前；拆分用 `skb_split`→`pskb_copy` 路径时 truesize 如何重新分配。*
5.  `skb_clone` 与 `pskb_copy` 都递增 page 引用，两者对 `cloned` 标志的设置差异会导致什么不同行为？`pskb_expand_head` 分别如何处理它们？
    *提示：pskb_copy 后新 skb `cloned=0`（head 私有）但 frags 仍共享；expand head 只搬 shinfo 整块。*
6.  一个 pps 极高的软中断场景中，`napi_alloc_skb` 相比 `alloc_skb` 省掉了哪些 per-packet 开销？
    *提示：per-CPU `napi_skb_cache`（免 slab free/alloc）、`page_frag_alloc_1k`（免 kmalloc）、`napi_skb_cache_put` 在 `napi_consume_skb` 中的闭环。*
