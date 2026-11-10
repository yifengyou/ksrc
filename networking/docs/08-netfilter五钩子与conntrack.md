# netfilter 五钩子与 conntrack（Linux 6.6）

> 阅读目标：掌握 netfilter 的**钩子框架层**（五个 IPv4/IPv6/INGRESS 挂载点、verdict 语义、`nf_hook_entries` 动态数组与批量执行路径、优先级阶梯）与 **conntrack** 的实现级细节（`struct nf_conn`、正反 tuple 双向查找、confirm 时机、ctinfo 状态机、NAT 联动、表满与 GC）。前置：[04-收包路径与NAPI](04-收包路径与NAPI.md)。

---

## 一、五个钩子点与六个挂载位（include/uapi/linux/netfilter.h，已核对 6.6）

```c
enum nf_inet_hooks {
	NF_INET_PRE_ROUTING,   /* 0: 路由前（最早，defrag/conntrack/raw 站这） */
	NF_INET_LOCAL_IN,      /* 1: 发往本机、路由后 */
	NF_INET_FORWARD,       /* 2: 转发路径 */
	NF_INET_LOCAL_OUT,     /* 3: 本机发出（最早可改包） */
	NF_INET_POST_ROUTING,  /* 4: 出网卡前（SNAT/confirm） */
	NF_INET_NUMHOOKS,
	NF_INET_INGRESS = NF_INET_NUMHOOKS,  /* 第 6 位：ingress（tc 前的 eBPF/过滤） */
};
```

verdict：`NF_DROP(0)`/`NF_ACCEPT(1)`/`NF_STOLEN(2)`（包归钩子，如排队、注入）/`NF_QUEUE(3)`（上抛用户态 nfnetlink_queue）/`NF_REPEAT(4)`（重走本钩子）/`NF_STOP(5)`（已废弃，仅兼容）。`NF_DROP_GETERR(verdict)`：负值 verdict 编码了 errno（丢弃原因上抛给监听者）。

## 二、注册与执行（include/linux/netfilter.h + net/netfilter/core.c）

```
注册：nf_register_net_hook(net, ops)（nf_hook_ops{hook, pf, hooknum,
     priority, priv}）→ 合并进该 (pf, hooknum) 的 nf_hook_entries
存储：struct nf_hook_entries { u16 num_hook_entries;
       struct nf_hook_entry hooks[]; }  ← 按 priority 排序的动态数组，
       尾部挂 orig_ops 指针 + rcu_head；替换走 RMU 延迟释放
执行：nf_hook_slow(skb, state, e, i)     ← 单包：从钩子 i 起逐个回调
      nf_hook_slow_list(head, state, e)  ← 5.14+ 批量路径：一串 skb
      （list_head 串起的 GRO 批次）共享一次遍历开销——ingress 大流量优化
```

**优先级阶梯**（include/uapi/linux/netfilter_ipv4.h，iptables 各表的真实站位）：

```
-450 RAW_BEFORE_DEFRAG   -400 CONNTRACK_DEFRAG（分片重组，为 conntrack 服务）
-300 RAW（最早可匹配）    -225 SELINUX_FIRST
-200 CONNTRACK（建表查询）-150 MANGLE
-100 NAT_DST（DNAT）       0 FILTER（filter 表的家）
  50 SECURITY            100 NAT_SRC（SNAT）
 225 SELINUX_LAST
```

## 三、conntrack：struct nf_conn（include/net/netfilter/nf_conntrack.h，已核对）

```c
struct nf_conn {
	struct nf_conntrack ct_general;
	spinlock_t	lock;      u32 timeout;      /* 超时轮询的期限 */
	struct nf_conntrack_zone zone;            /* ct zone（策略路由多表）*/
	struct nf_conntrack_tuple_hash tuplehash[IP_CT_DIR_MAX];
	                                          /* ★正反两个方向的 tuple */
	unsigned long status;                     /* IPS_* 位图 */
	possible_net_t ct_net;
	struct hlist_node nat_bysource;           /* NAT 源复用链 */
	struct nf_conn *master;                   /* 期望连接的父 ct */
	/* 之后是 extension 区：helper/timeout/nat/seqadj/acct/… */
};
```

**查找协议**（net/netfilter/nf_conntrack_core.c）：`hash_conntrack_raw()` 算原始哈希 → `__nf_conntrack_find_get()` 先查正向 tuple、未中再查反向（同一 skb 命中"回程方向的已有连接"即 ESTABLISHED_REPLY——一次查找、两种身份）。哈希桶数 `nf_conntrack_htable_size`（`nf_conntrack_buckets`），表上限 `nf_conntrack_max`。

**生命周期**：`nf_conntrack_in()`（PRE_ROUTING/LOCAL_OUT，-200）查/建未确认 ct → 路由/NAT 改写 → `__nf_conntrack_confirm()`（POST_ROUTING/LOCAL_IN）把 ct 挂入全局哈希（`__nf_conntrack_hash_insert(ct, hash, reply_hash)`——正反两桶同插）。冲突（同 tuple 已被占）即丢包——并发同流的唯一竞态点。

**状态机**（include/uapi/linux/netfilter/nf_conntrack_common.h）：

```
ctinfo（skb 上携带的快速标记）：
  IP_CT_NEW → IP_CT_ESTABLISHED_REPLY（= ESTABLISHED + IS_REPLY）
  IP_CT_RELATED_REPLY（期望子连接回程）；"No NEW in reply direction"
status 位图（IPS_*）：
  EXPECTED(0) 期望连接  SEEN_REPLY(1) 已见回程  ASSURED(2) 双向保活
  SRC_NAT(4)/DST_NAT(5)  NAT 改写记账（nat_bysource 源复用的依据）
```

## 四、NAT 联动与期望连接

```
NAT 完全寄生于 conntrack：DNAT 在 -100 改 tuple 正向、SNAT 在 100 改反向，
  ct 里不做规则存储——改写即改 tuple（"NAT 是 conntrack 的副作用"）
期望连接（expectation）：master ct + helper（如 FTP 控制通道解析出数据端口）
  → nf_conntrack_expect 等待子连接 → 子 ct status 带 EXPECTED、master 指父
表满/GC：nf_conntrack_max 打满 → 新建走 early_drop（丢最早可回收的）+
  "nf_conntrack: table full, dropping packet" 经典日志；
  GC worker（nf_conntrack_gc_worker）周期扫哈希桶回收超时 ct
```

## 五、观测与实战

```
cat /proc/sys/net/netfilter/nf_conntrack_count       # 当前表项数
sysctl net.netfilter.nf_conntrack_max                # 上限（打满即丢包）
sysctl net.netfilter.nf_conntrack_buckets            # 桶数（只读，模块参数 hashsize）
conntrack -L -o extended                             # 全表（含状态/超时/extension）
conntrack -E                                         # 事件流（NEW/UPDATE/DESTROY）
nstat -az | grep -i conntrack                        # drop/insert/fail 计数器
火焰图站位：nf_hook_slow 高占比 → 看具体钩子（perf -e netfilter:*）
```

## 六、思考题

1. 为什么 conntrack 的 defrag（-400）必须排在 RAW（-300）和 conntrack（-200）之前？（提示：tuple 只能匹配完整传输头——分片包没有 L4 头。）
2. `tuplehash[IP_CT_DIR_MAX]` 双向同插一表的设计，让回程包一次哈希查找即完成"连接+方向"判定——请推演一个方向判断出错的场景（提示：对称端口 50000↔50000 的 UDP）。
3. `__nf_conntrack_confirm` 放在 POST_ROUTING/LOCAL_IN 而不是建表即插，为什么？（提示：未确认 ct 不会出现在全局表——中途 DROP 无需回滚。）
4. 表满时 early_drop 挑"最早可回收"的牺牲者——为什么优先未 ASSURED 的 UDP？（对照 timeout 语义。）
5. nf_hook_slow_list 的批量遍历对哪类负载收益最大？（对照 [04-收包路径与NAPI](04-收包路径与NAPI.md) 的 GRO 批次与 INGRESS 钩子位置。）

---

> 分册至此 8 章。回看：[06-TCP拥塞控制框架](06-TCP拥塞控制框架.md)｜[07-XDP与eBPF数据面](07-XDP与eBPF数据面.md)｜[04-收包路径与NAPI](04-收包路径与NAPI.md)
