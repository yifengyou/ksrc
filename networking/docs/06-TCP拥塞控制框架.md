# TCP 拥塞控制框架：从 tcp_congestion_ops 到 CUBIC/BBR（Linux 6.6）

> 阅读目标：掌握 TCP 拥塞控制的**框架层**（ops 接口、注册/选择机制、CA 状态机、rate_sample 采样）与两大默认算法的实现级细节（CUBIC 的函数形窗口增长 + Hystart、BBR 的基于模型 pacing）。前置：[04-收包路径与NAPI](04-收包路径与NAPI.md)（知道 ACK 怎么进来）、[05-发包路径与qdisc](05-发包路径与qdisc.md)（知道 pacing 作用在哪）。

---

## 一、框架：struct tcp_congestion_ops（include/net/tcp.h）

**勘误先行**：6.6 里没有独立的 `include/net/tcp_congestion.h`（老资料常引这个路径）——ops 结构体就定义在 `include/net/tcp.h` 中。

```c
struct tcp_congestion_ops {
	/* 快路径字段排在最前，填满一条 cache line */
	u32   (*ssthresh)(struct sock *sk);                  /* 必需 */
	void  (*cong_avoid)(struct sock *sk, u32 ack, u32 acked); /* 必需 */
	void  (*set_state)(struct sock *sk, u8 new_state);   /* CA 状态切换通知 */
	void  (*cwnd_event)(struct sock *sk, enum tcp_ca_event ev);
	void  (*in_ack_event)(struct sock *sk, u32 flags);
	void  (*pkts_acked)(struct sock *sk, const struct ack_sample *sample);
	void  (*cong_control)(struct sock *sk, const struct rate_sample *rs);
	u32   (*undo_cwnd)(struct sock *sk);                 /* 必需：恢复 cwnd */
	u32   (*sndbuf_expand)(struct sock *sk);
	size_t (*get_info)(struct sock *sk, u32 ext, ...);   /* ss -i 的数据源 */
	/* 慢路径字段在后 */
	char  name[TCP_CA_NAME_MAX];
	struct module *owner;   u32 key;   u32 flags;
};
```

**两条路线的分水岭**：实现 `cong_avoid`（事件驱动，每个 ACK 算窗口，CUBIC 路线）或实现 `cong_control`（拿到 `rate_sample` 全权接管 pacing_rate 与 cwnd，BBR 路线）。6.6 中 `tcp_cong_control()` 优先调 `cong_control`，未提供才走 ssthresh/cong_avoid 组合。

## 二、注册与选择（net/ipv4/tcp_cong.c）

```
模块加载/内建注册：tcp_register_congestion_control(ops)
  → 挂入 per-net 的 tcp_cong_list（RCU 保护，tcp_ca_find 按名查找）
选择优先级（谁说了算）：
  ① setsockopt(TCP_CONGESTION, "bbr")        ← 每连接
  ② net.ipv4.tcp_congestion_control（per-netns）
  ③ 编译默认（CONFIG_DEFAULT_TCP_CONGestion，发行版几乎都是 cubic）
可用清单：cat /proc/sys/net/ipv4/tcp_available_congestion_control
```

## 三、CA 状态机（include/uapi/linux/tcp.h）

```
TCP_CA_Open(0) ──乱序迹象──▶ TCP_CA_Disorder（疑似丢，FACK/乱序记分）
      │ 任何状态收到 ECE ──▶ TCP_CA_CWR（拥塞通知降窗：一次过）
      │ RACK/重传超时判定丢 ─▶ TCP_CA_Recovery（快速恢复）
      │                      TCP_CA_Loss（RTO，一退到底）
icsk->icsk_ca_state 由 tcp_set_ca_state 切换，切前回调 ops->set_state，
事件（CA_EVENT_CWND_RESTART 等）经 ops->cwnd_event 通知算法
```

`rate_sample`（net/ipv4/tcp_rate.c）：现代框架的地基——按"发送间隔内已送达字节"算带宽样本（`rs->delivery_rate`），并把 RTT/是否应用受限（`rs->is_app_limited`）一并给算法；BBR 的 bw 估计与 CUBIC 的 hystart 都吃这份采样。

## 四、CUBIC（net/ipv4/tcp_cubic.c，已核对 6.6）

窗口增长用**三次函数**而非二进制对数：`W(t) = C·(t−K)³ + W_max`（K 为回到 W_max 的时间，`cube_root()` 近似计算）。逐项核对到的实现事实：

```
beta = 717（BICTCP_BETA_SCALE=1024 → 缩减系数 ≈0.7，非流传的 0.75）
Hystart（混合慢启动，默认开）：
  hystart_detect = HYSTART_ACK_TRAIN|HYSTART_DELAY（两种探测同时使能）
  ACK train：ACK 间隔 < hystart_ack_delta_us(2ms) 视为仍未排空 → 继续
  RTT delay：RTT 相比窗口最小 RTT 增长超 HYSTART_DELAY_THRESH
             （clamp 在 4ms~16ms）→ 提前退出慢启动进拥塞避免
  hystart_low_window = 16：窗口小于它不启用 Hystart
bictcp_update()：把三次函数目标差值换算成 ca->cnt（每 ACK 增 1/cwnd 中的
                等效计数——ca->cnt = cwnd/(bic_target - cwnd) 族公式）
近邻凹凸段有 TCP 友好补偿（与 Reno 竞争时贴着 Reno 增长）
```

## 五、BBR v1（net/ipv4/tcp_bbr.c，已核对 6.6）

**换范式**：不猜"丢包=拥塞"，直接测 `btl_bw`（瓶颈带宽，`struct minmax bw` 滑窗最大值）与 `min_rtt`（10s 窗最小值），按模型发数据（源文件头注释原文）：

```
pacing_rate = pacing_gain × btl_bw
cwnd        = max(cwnd_gain × btl_bw × min_rtt, 4)     ← BDP 模型
```

四态状态机（`enum bbr_mode`）与增益（全部核对自 6.6 源码）：

```
STARTUP：pacing_gain=cwnd_gain=high_gain=2.885（2/ln2，指数探测）
   │ 估到带宽不再涨（连续 3 轮）→
DRAIN：pacing_gain=1/2.885（排空 STARTUP 积压）,cwnd_gain 仍 2.885
   │ 排空（inflight≤bdp）→
PROBE_BW（稳态主循环）: bbr_pacing_gain[] 8 相位随机起点
   [1.25, 0.75, 1, 1, 1, 1, 1, 1]   ← 探带宽/让带宽/巡航×6
PROBE_RTT：每 10s（min_rtt_win_sec）至少一次：cwnd 压到 4 包
   测 RTT 底噪；出态按带宽估计回 PROBE_BW 或 STARTUP
长时带宽：bbr_lt_bw_ratio=BBR_UNIT/8 —— 空闲后流量用 8 倍窗的
   低通带宽防突刺；packet_conservation 用于 RTT 未稳时的保守发送
```

## 六、观测与实战

```
ss -tin                                    # get_info 暴露的算法私有字段
                                           #（bbr: (bbr bw:…/mrtt:…)；cubic: …）
nstat -az TcpExtTCPTimeouts TcpExtTCPLostRetransmit
cat /proc/sys/net/ipv4/tcp_congestion_control        # 当前默认
sysctl net.ipv4.tcp_congestion_control=bbr           # per-netns 生效（需内核支持）
tc qdisc show dev eth0                     # BBR pacing 需要 fq/fq_codel 配合
                                           #（pacing_rate 靠 fq 类 qdisc 兑现）
内核侧：CONFIG_TCP_CONG_ADVANCED、CONFIG_DEFAULT_TCP_CONGestion="cubic"
```

## 七、思考题

1. `cong_avoid` 与 `cong_control` 的分野为什么在 6.6 框架里保留两条路？（提示：事件驱动改 cwnd vs 模型驱动定 rate；BBR 需要每个 rs 的全量信息。）
2. CUBIC 的 `beta=717/1024≈0.7` 与传统 Reno 的 0.5 相比，对"与 Reno 流竞争"意味着什么？Hystart 又解决了慢启动的哪个老毛病？
3. BBR 的 PROBE_BW 循环里 1.25/0.75 相位为什么必须成对出现？（对照：队列探出与排空、带宽公平分享。）
4. `min_rtt` 为什么要用 10 秒滑窗而不是即时值？PROBE_RTT 把 cwnd 压到 4 包对吞吐的代价如何被 10s 一次的频率摊薄？
5. `ss -i` 看到一条 BBR 连接 pacing_rate 远大于 btl_bw×cwnd_gain 的可能原因？（提示：is_app_limited 与 lt_bw 的介入条件。）

---

> 下一章：[XDP 与 eBPF 数据面](07-XDP与eBPF数据面.md) —— 收包最早点的可编程旁路。
