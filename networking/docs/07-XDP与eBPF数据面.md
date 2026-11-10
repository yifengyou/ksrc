# XDP 与 eBPF 数据面（Linux 6.6）

> 阅读目标：掌握 XDP 的三种挂钩位（native/generic/offload）、`xdp_buff` 线性包上下文、五种动作的语义与实现路径（`netif_receive_generic_xdp`/`ndo_bpf`）、redirect 家族（DEVMAP/CPUMAP/XSKMAP/AF_XDP）与 tail call 组合拳。前置：[04-收包路径与NAPI](04-收包路径与NAPI.md)（知道 XDP 站在哪个位置）。

---

## 一、XDP 站位：比 sk_buff 更早

```
native XDP（驱动原生，性能形态）：
  驱动 RX 环取包 → 构造 xdp_buff（不分配 sk_buff！）→ 运行 prog
    ├── XDP_PASS    → 才走 build_skb → NAPI/协议栈（正常路径）
    ├── XDP_DROP    → 页直接还回 page_pool（零开销丢包）
    ├── XDP_TX      → 原网卡直接发（ hairpin，同一 RSS 队列）
    ├── XDP_REDIRECT→ 转发到 map 指定的对端（DEVMAP/CPUMAP/XSKMAP）
    └── XDP_ABORTED → 异常（tracepoint xdp_exception 捕获）
generic XDP（通用兜底，net/core/dev.c，已核对 6.6）：
  do_xdp_generic() → netif_receive_generic_xdp()
    → bpf_prog_run_generic_xdp(skb, xdp, prog)
  站位在 __netif_receive_skb_core 之前——包已是 skb，需做线性化/
  拉头处理；static_key generic_xdp_needed_key 仅在挂了 prog 才打开
offload XDP：prog 下推到网卡（可编程交换 ASIC / 智能网卡）执行
```

## 二、程序上下文：struct xdp_buff（include/net/xdp.h，已核对 6.6）

```c
struct xdp_buff {
	void	*data;		void	*data_end;	/* 仅可访问区间 [data, data_end) */
	void	*data_meta;	/* 元数据区起点（XDP_PASS 后经 xdp_data_meta
				   递给 tc/协议栈——bpf_xdp_adjust_meta） */
	void	*data_hard_start;	/* 页/帧起点（bpf_xdp_adjust_head 界限） */
	struct xdp_rxq_info	*rxq;	/* 收包队列身份（队列 id/设备） */
	struct xdp_txq_info	*txq;	/* 6.6 新增：XDP_TX 的发送队列描述 */
	u32	frame_sz;	u32	flags;
};
```

eBPF 侧：`BPF_PROG_TYPE_XDP`，helper 族 `bpf_xdp_adjust_head/meta`（改包）、`bpf_redirect`/`bpf_redirect_map`（转发）、`bpf_perf_event_output`（打点上抛）。验证器只允许访问 `[data, data_end)` 内的字节——越界直接拒载。

## 三、redirect 家族与 map

```
DEVMAP（map 类型 BPF_MAP_TYPE_DEVMAP / DEVMAP_HASH）
  键=ifindex → 对端 net_device；bpf_redirect_map(&dm, key, 0)
  bulk 批量转发：xdp_do_redirect → ndo_xdp_xmit 批量队列
CPUMAP（BPF_MAP_TYPE_CPUMAP）
  键=CPU → 包带着 xdp_buff 语义重定向到远 CPU 构造 skb 入栈
  （RSS 的可编程版：每核一个队，配合 cpumap snd 端批量 flush）
XSKMAP（BPF_MAP_TYPE_XSKMAP）+ AF_XDP socket
  包直达用户态零拷贝通道（UMEM 共享内存）——DPDK 替身路线
```

## 四、加载与组合

```
加载：ip link set dev eth0 xdp obj xdp_prog.o sec xdp
     bpftool prog load xdp_prog.o /sys/fs/bpf/xdp && bpftool net attach
更换/卸载原子性：驱动 ndo_bpf(XDP_SETUP_PROG) 换指针，旧包走旧 prog
tail call：BPF_MAP_TYPE_PROG_ARRAY 组合多段 prog（分类器→动作），
  跳转开销近零——"eBPF 程序链"的标准姿势
eBPF 数据面全景位：XDP（RX 最早）→ tc/cls_bpf（有 skb，可改路由）
  → SO_ATTACH_BPF/SOCK_OPS（socket 层）→ cgroup/connect 族（控制面）
```

## 五、观测与实战

```
bpftool prog show                      # XDP prog 与 run_time_ns/_CNT
bpftool net attach/detach              # 挂载点管理
ip -details link show eth0             # prog/xdp 附加信息
cat /proc/net/bpf/devmap? → bpftool map show + bpftool map dump
perf record -e xdp:xdp_exception -a    # XDP_ABORTED 溯源
性能基线：native XDP 单核丢包可达 NIC 线速（DDoS 清洗/LB 前置）；
generic XDP 仅为兼容形态（无 page_pool 直收、每包多一层 skb 处理）
```

## 六、思考题

1. native XDP 为什么必须拿到"单页线性区"（frame_sz 内）？分片/多缓冲包在 6.6 如何处理？（提示：驱动须保证 XDP 程序视角的线性；非线性包走 passthrough。）
2. `data_meta` 区为什么要在 XDP 写、在 tc/协议栈读？直接塞进包头不行吗？（提示：不改报文本身的带外通道。）
3. generic XDP 位于 `netif_receive_generic_xdp`，为什么它做不到"零开销丢包"？（对照 sk_buff 已分配的成本结构。）
4. CPUMAP 与 RPS 都能跨 CPU 派包，语义差在哪？（提示：RPS 派 skb 入 softirq；CPUMAP 在远端做 XDP 语义构造/flush。）
5. 用 DEVMAP+tail call 实现一个"按 DSCP 分流到 3 个出口"的转发器，程序如何拆分？（提示：分类 prog tail call 到动作 prog。）

---

> 前一章：[TCP 拥塞控制框架](06-TCP拥塞控制框架.md)｜回看分册：[04-收包路径与NAPI](04-收包路径与NAPI.md)
