# UDP 收包路径与 KTLS 卸载（Linux 6.6）

> 定位：networking 分册的传输层补章——UDP 的单播快路径（查找 → 入队 → 唤醒）、UDP GRO 族（隧道感知的聚合）与 **KTLS**（内核态 TLS：软件加解密与 NIC 卸载双路径）。前置：[04-收包路径与NAPI](04-收包路径与NAPI.md)、[02-sk_buff详解](02-sk_buff详解.md)。

---

## 一、UDP 收包路径（net/ipv4/udp.c，已核对 6.6）

```
ip_rcv 分发（protocol.c 的 .handler=udp_rcv，:2600）
 └ udp_v4_rcv → udp_unicast_rcv_skb:
     ① udp4_lib_lookup_skb（:534）——四元组查 socket：
     struct udp_table（:122，全局/exported）双哈希：
     hash2（(laddr,lport,faddr,fport) 精确链——已连接 socket 的
       O(1) 快路）+ 旧 hash（(lport,rport) 粗链——bind 后未连）
     ② 校验（csum）→ udp_queue_rcv_skb：
        GRO 时机已过（聚合在 NAPI 层做，见 §二）
     ③ __udp_enqueue_schedule_skb（:1488，exported :1554）：
        sk_rmem_alloc 配额（sk_rcvbuf 超限走丢弃/PEEK_OFF 分支）→
        挂 sk_receive_queue → sock_def_readable 唤醒
慢路径：encap 回调（ESP-in-UDP / VXLAN 的 udp_encap_recv 钩子
  ——tunnel 分册的入口点）
```

## 二、UDP GRO 族（net/ipv4/udp_offload.c，已核对）

```
udp_gro_receive（:545，exported :605）：
  外层判断（端口相同 + 非分段）→ call_gro_receive(
    udp_gro_receive_segment, ...)（:564）——按"同流分段"聚合
  （GSO 段间合并且 csum 复用）
隧道感知：NETIF_F_GRO_UDP_TUNNEL feature 位——
  VXLAN/GUE 隧道的 GRO 需查内层而非盲合（驱动声明 +
  gro_cells）
```

## 三、KTLS 总览：TCP ULP 之上的加密层

```
挂载（net/tls/tls_main.c，已核对）：
  tcp_register_ulp(&tcp_tls_ulp_ops)（:1132）——
  tcp_tls_ulp_ops.name = "tls"、.init = tls_init（:1107-1113）
  setsockopt(SOL_TCP, TCP_ULP, "tls") → tls_init（:932）装 ULP
配置（do_tls_setsockopt :555-563 的 optname 族）：
  TLS_TX / TLS_RX / TLS_TX_ZEROCOPY_RO / TLS_RX_EXPECT_NO_PAD
  → do_tls_setsockopt_conf（:588）→ tls_set_sw_offload（软件）
    + tls_set_device_offload（网卡有 HW_TLS 时，见 §五）
struct tls_context（include/net/tls.h:224，已核对）：
  tx_conf/rx_conf:3（SW/HW/UNSET 三态）+ prot_info +
  priv_ctx_tx/priv_ctx_rx（SW/HW 私有上下文双挂）
```

## 四、软件路径（net/tls/tls_sw.c，已核对）

```
发送：tls_sw_sendmsg（:1193）→ tls_sw_sendmsg_locked（:963）
  ——明文 skb 分段记录化：加密 + TLS record 头尾包装 +
  splice 零拷贝变体（tls_sw_sendmsg_splice :934）
接收：tls_sw_recvmsg——密文 skb → 解密 → 去记录化交付
设备/路径回退：NIC 无 HW_TLS 或记录异常 → 回退 SW
  （tls_sw_fallback 族）
```

## 五、卸载路径（net/tls/tls_device.c，已核对）

```
feature 位（netdev_features.h:79-80）：
  NETIF_F_HW_TLS_TX_BIT / NETIF_F_HW_TLS_RX_BIT
安装：tls_set_device_offload → tls_device_attach（:1029，
  两处调用 :1180/:1266）——tls_context 挂网卡私有槽
★重同步（TX 卸载的核心难题）：
  tls_offload_tx_resync_request（:223，exported :230）——
  密钥轮换/部分卸载后 seq 对不齐时，NIC 请求内核重发记录号
  → tls_device_resync_tx（:232）+ trace_tls_device_tx_resync_*
  观测点（:227/:246）
```

## 六、观测与实战

```
ss -tin 上层看 ULP：ulps 列（ktls 开启显示 tls）
ethtool -k eth0 | grep tls                  # HW_TLS 能力
tracefs: tls:tls_device_tx_resync_req / send
抓包特征：卸载 TX 出网卡的包已加密（tcpdump 只见密文）——
  收发两端不对称时先查对端是否回退 SW
排障链：设置失败 EBADF/EINVAL → 协议版本/密钥格式
  （supported cipher: AES-GCM 128/256、ChaCha20-Poly1305）
```

## 七、思考题

1. UDP hash2 为什么为"已连接 socket"单设？（对照 TCP 的 ehash 全四元组。）
2. udp_gro_receive_segment 的"同流分段"合并在什么负载下收益最大？（QUIC 的大 UDP 分段。）
3. KTLS 为什么挂在 ULP 而不是独立协议栈？（提示：TCP 的拥塞/重传语义必须保留。）
4. TX 卸载为什么需要"重同步"机制？（提示：SW 回退期间 NIC 记录号落后。）
5. tls_sw_sendmsg 的记录化为什么天然适配 sendfile/zero-copy？（对照 mmap 域与 skb 域的交界。）

---

> 回看：[04-收包路径与NAPI](04-收包路径与NAPI.md)｜[02-sk_buff详解](02-sk_buff详解.md)｜[06-TCP拥塞控制框架](06-TCP拥塞控制框架.md)｜[网络分册导读](../README.md)