# unbound 池与 affinity scope（Linux 6.6）

> 阅读目标：理解 unbound 工作队列的动机与三层结构——`workqueue_attrs` 如何作为池的索引键（unbound_pool_hash）、6.6 新登场的 **CPU pod / affinity scope 模型**（`wq_affn_scope` 的六种作用域、默认从 NUMA 改为 **cache** 的原因、`affn_strict` 的宽严两档）、以及 cpumask 治理全链（`workqueue.unbound_cpus` 启动参数、housekeeping 排除、`wq_unbound_cpumask` 运行时热更新的 prepare/commit/cleanup 协议）。最后给出 per-cpu vs unbound 的工程选型框架。

---

## 一、动机：长任务必须离开 per-CPU 池

per-CPU 池的并发管理（第 1 章）假设 work 是**短小且常睡眠**的：一个不睡眠的 work 会把整池并发额度焊死（6.6 用自动 CPU-intensive 检测兜底，但那只是止血）。真正两类工作必须走 unbound：

1. **CPU 长跑型**：加密、压缩、图像处理——一跑几秒，不能容忍"占着 per-CPU 池导致本 CPU 其他 work 排队"。
2. **非亲和型**：work 与提交 CPU 无 cache 亲缘（如面向某 NUMA node 内存的操作），绑定提交 CPU 反而错。

unbound 的形态：worker 不绑单 CPU（绑一个允许集合），**不参与并发管理**（`WORKER_NOT_RUNNING`，`nr_running` 恒 0 → `need_more_worker` 恒真 → 有活就拉人，`keep_working` 恒真 → 一口气把 worklist 干完），池**按 attrs 动态创建、跨 wq 共享**。

```
per-CPU 池：每 CPU 2 个，静态，CMWQ 全额并发管理
unbound 池：按 (nice, cpumask, __pod_cpumask, affn_strict) 哈希共享
            池数 = 不同 attrs 组合数 × pod 划分数
```

---

## 二、attrs：unbound 池的身份证

### 2.1 结构（6.6 真实定义，含 6.5/6.6 新字段）

```c
struct workqueue_attrs {              /* include/linux/workqueue.h */
	int nice;                    /* 池的 nice（pool 匹配参与哈希）*/
	cpumask_var_t cpumask;       /* 允许 CPU（用户语义的"大池"范围）*/

	/* —— 6.5/6.6 的 pod 化新字段 —— */
	cpumask_var_t __pod_cpumask; /* 内部：本 pod 的实际 CPU 集（cpumask 的子集）*/
	bool affn_strict;            /* 严格模式：worker 绑死 __pod_cpumask；
	                                宽松（默认）：尽力在 pod 内醒，允许迁移出去 */

	/* 以下只影响选池（apply_workqueue_attrs），不参与池哈希/相等比较 */
	enum wq_affn_scope affn_scope;  /* cpu/smt/cache/numa/system/default */
	bool ordered;                   /* ordered wq：单 pwq 串行 */
};
```

**哈希与相等比较的字段集**（`wqattrs_hash`/`wqattrs_equal`，6.6）：`nice + cpumask + __pod_cpumask + affn_strict`——注意 `affn_scope` 和 `ordered` **不在其中**（源码注释："Below fields aren't properties of a worker_pool"）。同一个 scope 语义下算出的不同 pod mask 自然产生不同的池；而 scope 本身只影响"怎么算 pod mask"。这保证了池的复用粒度恰好是"执行资源真正等价"。

### 2.2 池的引用计数与销毁

unbound 池的生死由 `refcnt` 驱动（wq_pool_mutex 保护）：

```
get_unbound_pool 命中复用 → refcnt++
每个 pwq（init_pwq 时）持一份 → 对应 put 在 pwq_release_workfn（第 2 章 §5）
refcnt 归零 → put_unbound_pool:
    idr 摘除 + unbound_pool_hash 摘除
    等 POOL_MANAGER_ACTIVE 退位（manager_wait；源码注释解释了
      "maybe_create_worker 期间放锁可能残留 manager"的窗口）
    全部 idle worker set_worker_dying → wake_dying_workers
    （若仍有 worker 未 detach → 等 detach_completion——6.6 新协议）
    del_timer_sync(idle_timer) + cancel_work_sync(idle_cull_work)
        + del_timer_sync(mayday_timer)
    call_rcu(rcu_free_pool) → 宽限期后 kfree(pool)
```

注意池销毁**先于** wq 销毁可以被触发：改 attrs（apply_workqueue_attrs）换掉所有 pwq 后，旧池随最后一个 pwq 释放而消亡——"池拓扑是可热重构的"这句工程直觉的机制落点就在这里。

### 2.3 get_unbound_pool：哈希查找 + 按需创建

```
get_unbound_pool(attrs)                     [wq_pool_mutex 持有]
  hash_for_each_possible(unbound_pool_hash, pool, hash_node, wqattrs_hash(attrs))
      wqattrs_equal(pool->attrs, attrs) → pool->refcnt++，返回复用
  未命中 → 新建：
      定位 NUMA node：attrs->__pod_cpumask 落在哪个 NUMA pod 内（按
                     WQ_AFFN_NUMA pod 表逐 pod 判 subset）→ pool 内存本地化
      kzalloc_node + init_worker_pool（refcnt=1, POOL_DISASSOCIATED）
      worker_pool_assign_id（idr，进 worker_pool_idr）
      wq_online ? create_worker : 稍后 workqueue_init() 统一补
      hash_add(unbound_pool_hash)   ← 64 桶（UNBOUND_POOL_HASH_ORDER=6）
```

### 2.4 从 wq 到 pwq：alloc_unbound_pwq 与 per-cpu 表

unbound wq 没有"每 CPU 一个标准池"的待遇，但**提交路径查表结构完全相同**（`wq->cpu_pwq[cpu]`，RCU 指针）。差别在于表的内容由 attrs 推导：

```
alloc_unbound_pwq(wq, attrs) = get_unbound_pool(attrs) + init_pwq

apply_wqattrs_prepare(wq, attrs, unbound_cpumask):        [核心铺装]
  new_attrs = copy(attrs) ∩ wq_unbound_cpumask（空则整体回退到全局 mask）
  dfl_pwq = alloc_unbound_pwq(wq, new_attrs)      ← 兜底 pwq：全允许 CPU
  for_each_possible_cpu(cpu):
      ordered ? 全部指向 dfl_pwq（保序！） :
      wq_calc_pod_cpumask(new_attrs, cpu)         ← 算出该 CPU 所在 pod 的 mask
      pwq_tbl[cpu] = alloc_unbound_pwq(wq, new_attrs)
  存用户语义 attrs（cpumask 与 possible 求交，不再 ∩ unbound ——展示原样）
```

`wq_calc_pod_cpumask(attrs, cpu)` 的判定（6.6）：取 cpu 所属 pod 的 CPU 集 ∩ attrs->cpumask ∩ online；为空 → 整个 attrs->cpumask（pod 内无可用 CPU 时退化为大池）；非空 → **possible 交**（注释：online 交 possible 为空会 pr_warn 一次）。

提交时的查表（`__queue_work` → `wq_select_unbound_cpu` → `wq->cpu_pwq[cpu]`）把"提交 CPU"映射到"该 CPU 所在 pod 的 pwq"——这就是 unbound 的**提交侧本地性**。

### 2.5 boot 三阶段：拓扑如何一步步长出来

unbound 拓扑依赖调度器拓扑（LLC/SMT 划分），而 workqueue 初始化又必须极早——6.6 用三阶段解耦（`workqueue_init_early/_init/_init_topology`）：

```
① workqueue_init_early（start_kernel 极早期，sched_smp 初始化前）
     建 WQ_AFFN_SYSTEM 单 pod（全系统一个池的临时拓扑）
     初始化全部 per-CPU 标准池（nice 0/-20）
     创建 7 个 system_* wq —— 此时它们的 cpu_pwq 全部指向 SYSTEM 单池
② workqueue_init（kthread 可用后，early initcall 前）
     给存量池补建初始 worker（wq_online = true 从此可建线程）
     给需要 rescuer 的 wq 建 rescuer、起 watchdog
③ workqueue_init_topology（SMP/调度拓扑就绪后）
     init_pod_type 建满 CPU/SMT/CACHE/NUMA 四张 pod 表
     对每个 wq × 每个 online CPU wq_update_pod 重铺 cpu_pwq
     —— ①②期间排队的 work 已经在 SYSTEM 大池上跑/排队，不受影响
```

这个"先可用后精确"的次序解释了 boot 日志里早期 kworker 的奇怪分布，也解释了为什么 `wqattrs_pod_type` 对空 pod 表要兜底 SYSTEM（BUG_ON 保证至少 SYSTEM 永远存在）。

---

## 三、affinity scope：6.6 的 pod 模型

### 3.1 从"NUMA 池"到"可插拔作用域"

6.4 之前 unbound 池按 NUMA node 划分（每 node 一池），缺陷：现代多 die CPU 的**LLC（末级缓存）边界比 NUMA node 细**（一个 node 内多个 cache 域），NUMA 粒度太粗吃不到 cache 局部性；而小机器上 node 又太多、池太碎。6.5/6.6 引入**作用域可插拔**：

```c
enum wq_affn_scope {          /* include/linux/workqueue.h, 6.6 */
	WQ_AFFN_DFL,      /* 跟随系统默认 */
	WQ_AFFN_CPU,      /* 每 CPU 一个 pod */
	WQ_AFFN_SMT,      /* 每 SMT 兄弟线程组一个 pod */
	WQ_AFFN_CACHE,    /* 每 LLC 一个 pod（cache 域）*/
	WQ_AFFN_NUMA,     /* 每 NUMA node 一个 pod（旧行为）*/
	WQ_AFFN_SYSTEM,   /* 全系统一个 pod（无划分，启动早期唯一可用）*/
};
```

每种作用域对应一张 `wq_pod_type` 表，boot 三阶段初始化（`workqueue_init_early` 先建 SYSTEM 单 pod；`workqueue_init_topology` 在拓扑可用后按 `cpus_dont_share`/`cpus_share_smt`/`cpus_share_cache`/`cpus_share_numa` 建满四张表，并对存量 wq 逐 CPU `wq_update_pod` 重铺）：

```c
struct wq_pod_type {
	int nr_pods;              /* pod 数 */
	cpumask_var_t *pod_cpus;  /* pod -> CPU 集 */
	int           *pod_node;  /* pod -> NUMA node（池内存分配用）*/
	int           *cpu_pod;   /* CPU -> pod（提交查表用）*/
};
static struct wq_pod_type wq_pod_types[WQ_AFFN_NR_TYPES];
static enum wq_affn_scope wq_affn_dfl = WQ_AFFN_CACHE;   /* ★ 6.6 默认 cache */
```

> 📌 **默认作用域在 6.6 是 `WQ_AFFN_CACHE`**（6.4 之前事实上是 NUMA）。理由：多数工作负载的本地性收益 LLC > NUMA（cache 命中是纳秒级、跨 node 是几百纳秒级），而 pod 数增加的内存/线程成本在现代大机器上可接受。可通过启动参数或运行时 `/sys/module/workqueue/parameters/default_affinity_scope` 改回 numa/system 等。

### 3.2 配置接口（已核对 6.6 真实接口）

```
# 系统级默认作用域（运行时可改，改完全部 unbound wq 立即 wq_update_pod 重铺）
/sys/module/workqueue/parameters/default_affinity_scope   # cache（默认）
                                                          # 可写: cpu/smt/cache/numa/system

# per-wq（WQ_SYSFS 的 unbound wq）
/sys/bus/workqueue/devices/<name>/affinity_scope    # "default (cache)" 或具体值
/sys/bus/workqueue/devices/<name>/affinity_strict   # 0=宽松(默认) 1=严格

# 代码级：alloc_workqueue_attrs() 后设 attrs->affn_scope/affn_strict，
#         apply_workqueue_attrs(wq, attrs)（要求 cpus_read_lock 已持有）
```

`affn_strict` 的宽严语义（头文件注释原文转述）：

- **宽松（默认，strict=0）**：worker 的 `cpus_allowed` 是整个 `attrs->cpumask`；池只是**尽力**让它从 pod 内的 CPU 醒来（`kick_pool` 设置 `p->wake_cpu` 到 pod 内的分散 CPU）。调度器仍有权迁出 pod——低延迟弹性好，但可能丢 cache 局部性。
- **严格（strict=1）**：worker `kthread_bind_mask` 到 `__pod_cpumask`，物理上不能出 pod。局部性最强，代价是 pod 全忙时该池 work 只能干等（无溢出迁移）。

宽松模式的"找回"机制：`kick_pool` 发现被唤醒 worker 的 `wake_cpu` 已漂出 pod（说明此前被调度器迁走过）→ 重设 `wake_cpu` 回 pod 内 + `PWQ_STAT_REPATRIATED++`（wq_monitor.py 的 RPR 列）——"repatriate（遣返）"。

### 3.3 一图看穿 6.6 unbound 拓扑

```
              提交 queue_work(uwq, work) on CPU 7
                     │ wq_select_unbound_cpu(7)
                     │  7 ∈ wq_unbound_cpumask → 用 7（本地优先）
                     │  否则 mask∩online 内 round-robin（wq_rr_cpu_last）
                     ▼
        wq->cpu_pwq[7]  ──RCU──►  pwq#12 (attrs: nice=0, __pod_cpumask=LLC1)
                     │                     │ I: pool
                     ▼                     ▼
              pod 查表: cpu_pod[7]=LLC1   pool#17 (unbound)
              （affn_scope=cache）         ├─ worker kworker/u17:0  绑 cpumask
              同 LLC 的 CPU 6,7 → 同 pwq   │   (宽松)或 __pod_cpumask (严格)
                                         ├─ attrs = wqattrs_hash 的键
                                         └─ 池由所有命中同键的 wq 共享

   换 affn_scope=numa：cpu_pod 表换 → pod_cpus 变 node 级 → cpu_pwq 重铺
   → 新 pwq（__pod_cpumask=node 全 CPU）→ get_unbound_pool 未命中建新池
   → 旧池等 in-flight work 排空后 refcnt 归零销毁
```

用一台具体机器把 pod 概念落到实处（2 node × 每 node 2 个 LLC × 每 LLC 8 CPU，含 SMT 简化掉）：

```
                    scope=cache（6.6 默认）          scope=numa
pod 表    cpu_pod:   LLC0={0..7} LLC1={8..15}       N0={0..15}
                    LLC2={16..23} LLC3={24..31}     N1={16..31}
unbound 池数        4 个（每 LLC 一池）              2 个（每 node 一池）
worker 绑定         8 CPU（宽松：醒在 pod 内）       16 CPU
cache 命中          提交与执行同 LLC → 稳定          同 node 跨 LLC → 一半概率丢
跨 node 访问        池内杜绝                        池内杜绝
适用                元数据/小包/锁密集               内存带宽密集（大块搬运）
```

这台机器上如果把 `default_affinity_scope` 写成 `smt`，pod 数 = 16——池更碎、worker 更"贴身"，只对极端 cache 抖动敏感的场景有意义；`cpu` 则 32 个池，几乎等于把 unbound 用成了 per-CPU（且丢了 per-CPU wq 的 FIFO 语义），罕用。

---

## 四、cpumask 治理：三层过滤与热更新

### 4.1 wq_unbound_cpumask 的初值（启动链）

```
workqueue_init_early():
  wq_unbound_cpumask = housekeeping_cpumask(HK_TYPE_WQ)        # ①
                     ∩ housekeeping_cpumask(HK_TYPE_DOMAIN)    # ②
  workqueue.unbound_cpus=1-3 指定过 → 再 ∩ cmdline mask        # ③
```

- ① `HK_TYPE_WQ`：isolcpus/nohz_full 等隔离域**默认把 unbound worker 挡在隔离 CPU 之外**——隔离 CPU 是给实时/专用任务留的，kworker 抢进去等于白隔离。
- ② 再与调度域 housekeeping 交集（无调度域的 CPU 连负载均衡都不参与，更不该跑 unbound work）。
- ③ `workqueue.unbound_cpus=<cpulist>` 启动参数进一步收紧（不是放宽——它只能减不能加）。

### 4.2 运行时热更新：三层协议

改 `/sys/devices/virtual/workqueue/cpumask`（或代码 `workqueue_set_unbound_cpumask()`）走 apply 框架的**批量版**：

```
workqueue_set_unbound_cpumask(mask)
  mask &= cpu_possible_mask（空 → -EINVAL）
  mask == 旧值 → 0（no-op）
  workqueue_apply_unbound_cpumask(mask):        [wq_pool_mutex + cpus_read_lock]
    第一遍（prepare）：对每个 unbound wq：
        __WQ_ORDERED_EXPLICIT → 跳过（保序 wq 不允许多 pwq）
        apply_wqattrs_prepare(wq, wq->unbound_attrs, mask)
          → 新 dfl_pwq + 逐 CPU pod pwq 表（全部按新 mask 计算）
        任何一步 -ENOMEM → 放弃整个事务（ret 记错）—— 全或无
    第二遍（commit）：全部成功才逐 wq apply_wqattrs_commit：
        rcu_assign_pointer(cpu_pwq[cpu], pwq_tbl[cpu])   ← 原子换表
        旧 pwq 被换下（引用随 in-flight work 存续）
    第三遍（cleanup）：新旧 pwq 统一 put_pwq_unlocked
    成功 → wq_unbound_cpumask = mask（在 attach_mutex 下写，unbind_worker 读）
```

> 💡 **换表不搬 work**：已在旧 pwq/pool 上排队的 work 继续在旧池跑完（pwq refcnt 保命）；新提交按新表走。自续（requeue self）的 work 因 last_pool 粘性**永远留在旧池**——源码注释明言"a work item which repeatedly requeues itself back-to-back will stay on its current pwq"。做热迁移方案时必须考虑这个尾巴。

热插拔（CPU 上/下线）走同框架的窄版 `wq_update_pod(wq, cpu, hotplug_cpu, online)`：只重算受影响 pod 的 CPU 的 pwq；分配失败兜底 `dfl_pwq`（"may not be optimal but is always correct"）。per-CPU 池的 hotplug 则是 unbind/rebind 一套（`POOL_DISASSOCIATED` 转当 unbound 池跑，见第 1 章 worker 标志），保证下线 CPU 上的在跑 work 不丢。

### 4.3 wq_select_unbound_cpu：提交时的裁剪

```c
static int wq_select_unbound_cpu(int cpu)      /* 6.6 */
{
	if (likely(!wq_debug_force_rr_cpu)) {
		if (cpumask_test_cpu(cpu, wq_unbound_cpumask))
			return cpu;                  /* 本 CPU 合法 → 本地执行 */
	}
	if (cpumask_empty(wq_unbound_cpumask))
		return cpu;                      /* 极端：mask 空（不该发生）*/
	/* 本 CPU 不合法（隔离域/被裁剪）→ 在 mask∩online 里轮转 */
	new_cpu = cpumask_next_and(this_cpu_read(wq_rr_cpu_last),
				   wq_unbound_cpumask, cpu_online_mask);
	if (new_cpu >= nr_cpu_ids)
		new_cpu = cpumask_first_and(wq_unbound_cpumask, cpu_online_mask);
	this_cpu_write(wq_rr_cpu_last, new_cpu);
	return new_cpu;
}
```

注释点破动机："**to avoid perturbing sensitive tasks**"——从隔离 CPU 提交的 unbound work 被轮转摊到 housekeeping CPU，不扎堆。`CONFIG_DEBUG_WQ_FORCE_RR_CPU`（或运行时 `debug_force_rr_cpu=1`）强制所有 unbound 提交走轮转——专门用来**暴露偷偷依赖"unbound work 会在提交 CPU 本地执行"的驱动 bug**（此依赖官方明确不保证）。

---

## 五、unbound worker 的命名与 sysfs

```
kworker/u<pool_id>:<worker_id>     # unbound 池，如 kworker/u17:0
kworker/<cpu>:<id>[H]              # per-CPU 池（H=高优池）
kworker/R-<wq_name>                # rescuer（WQ_MEM_RECLAIM 专属）
kworker/dying                      # 自毁途中的临时改名（第 1 章）
```

注意 `u` 后面是 **pool id**（动态分配，随池生灭复用）而非 CPU 号——排查时先 `wq_dump.py`/sysrq-w 把 pool id 映射回 attrs（cpumask/nice）。`/proc/<pid>/comm` 会在基础名后拼 `±<wq_name 或 set_worker_desc>`（`wq_worker_comm`），`+` 正在跑、`-` 已跑完。

sysfs 面（第 2 章 §5.1 已列全）：`WQ_SYSFS` 的 unbound wq 暴露 nice/cpumask/affinity_scope/affinity_strict 四个 RW 属性，写入即走 `apply_workqueue_attrs_locked`——**用户态可以直接改生产机的池拓扑**，这是调优利器也是误操作入口（ordered wq 拒绝暴露 sysfs，`workqueue_sysfs_register` 对 `__WQ_ORDERED_EXPLICIT` 返回 -EINVAL）。

---

## 六、NUMA 场景工程

### 6.1 跨 node 提交的本地池命中

提交 CPU 决定入口（`cpu_pwq[提交CPU]`），affn_scope 决定池的覆盖面。跨 node 场景的命中链：

- 亲和目标是**内存**（work 访问 node 2 的内存）：`affn_scope=numa` 时，只要提交发生在 node 2 的任一 CPU（或 `queue_work_node(2, ...)` 显式指定），work 就落进 node 2 的池、worker 绑 node 2 的 CPU——内存访问本地化。
- 亲和目标是**cache**（work 操作提交者刚写过的数据）：`affn_scope=cache`（默认）比 numa 更精准——尤其多 die 处理器上"同 node 跨 LLC"的伪本地性是常见性能坑。

`queue_work_node(node, wq, work)`（6.6 已导出）：`select_numa_node_cpu`——本 CPU 已在目标 node → 用本 CPU；否则 node∩online 的**第一个** CPU（文档自嘲"random otherwise known as first"，尚无轮转）；仅 unbound wq 可用（WARN_ON_ONCE）。

### 6.2 cache vs numa 实测建议

| 负载画像 | 建议 scope | 理由 |
| :--- | :--- | :--- |
| work 重度访问提交者同级 cache 的数据（元数据操作、小包处理） | cache（默认） | LLC 内命中；node 粒度会跨 die 丢 cache |
| work 大块访问特定 node 内存（页迁移、压缩、GC 扫描） | numa | 内存带宽本地化优先于 cache |
| 混合/不确定 | default（cache）先用，看 wq_monitor 的 RPR 列 | 频繁遣返说明 worker 被迁出 pod——考虑 strict |
| 池数量敏感（海量 wq 的小系统） | system | pod 数=1，池数最少，省内存省线程 |

验证方法：`perf stat -M LLC-load-misses` 对比两种 scope 下同负载；或 bpftrace 统计 `workqueue_execute_start` 的 cpu 与提交 cpu 的 `cpu_to_node` 关系。**strict 的取舍**：RPR 持续高（宽松模式 worker 老被迁走）→ strict=1 收紧；但 strict 下 pod 内 CPU 全忙时 work 只能等（不能借外面的核），吞吐尖峰场景慎用。

### 6.3 per-cpu vs unbound：三轴选型

| 轴 | per-CPU wq | unbound wq |
| :--- | :--- | :--- |
| **提交→执行延迟** | 最低（本 CPU 直插 worklist，本 CPU worker 接力） | 本 CPU 在 mask 内时同样低；隔离/下线 CPU 提交则跨 CPU 轮转 |
| **调度器自由度** | 零（绑死单 CPU，worker 抢占 idle core，伤功耗——WQ_POWER_EFFICIENT 的由来） | 高（允许集合内自选；宽松 scope 下可漂移） |
| **隔离性（对别的任务）** | 差：本 CPU 的一切与 kworker 同跑 | 好：可整体圈进 housekeeping CPU |
| **顺序性** | 每 CPU 内 FIFO | pod 内 FIFO（跨 pod 无序） |
| **典型用户** | 短小、高频、cache 亲和（网卡 RX 后处理） | 长跑、可迁移、NUMA 亲和（fsync、压缩、kcompactd） |

> 📌 **默认建议**（综合官方文档与 6.6 现状）：能用 per-CPU 就用（延迟与 cache 双赢）；超过 ~10ms 不睡眠、或需要 NUMA/cpumask 亲和、或想让调度器参与放置决策 → unbound。吃不准用 `system_unbound_wq`/自建 unbound wq 试，再用 `affinity_scope` 微调。**不要**用 per-CPU wq 跑会长时间睡眠等外部资源的 work（把并发额度睡掉，第 1 章 §4.1）——那正是 unbound 的领地。

---

## 七、易错点 FAQ（工程向速答）

**Q1：unbound wq 的 work 一定不在提交 CPU 上跑吗？**
恰好相反——默认**优先**在提交 CPU 跑（`wq_select_unbound_cpu` 本地优先）。但官方**不承诺**这一点：CPU 隔离/热更新后随时轮转走。依赖"本地执行"的代码用 `CONFIG_DEBUG_WQ_FORCE_RR_CPU` 一测就现形。

**Q2：unbound 池会无限开线程吗？**
会开很多但非无限：每个池有 worklist 就拉 worker（无 CM 约束），高并发下 unbound 池的 worker 数≈同时在跑的 work 数（受 max_active 限到 512/pwq）。系统级线程压力 = Σ 各池。这是 unbound 的代价之一——`WQ_CPU_INTENSIVE` 的 per-cpu wq 是"要调度器自由但控制线程数"的中间选项。

**Q3：改了 affinity_scope 旧 work 会搬家吗？**
不会。已在旧 pwq 排队/执行的 work 走完旧池（第 2 章 last_pool 粘性 + pwq refcnt）。只有**新提交**按新表路由。自续 work 甚至可能永远留在旧池（源码注释明示）。

**Q4：affn_strict=1 就绝对不越界吗？**
worker 的 cpus_allowed 绑死 `__pod_cpumask`，是硬边界。但注意 pod 内 CPU 全部 offline 的极端情形：`wq_calc_pod_cpumask` 会退化用整个 attrs->cpumask 重建池——严格性以"有活 CPU"为前提。

**Q5：为什么我的 unbound kworker 出现在 isolated CPU 上？**
按 6.6 初始化链这**不应该**发生（HK_TYPE_WQ 排除）。先查：是否手动把 isolated CPU 写回了 `/sys/devices/virtual/workqueue/cpumask`（接口注释明说"不主动排除 isolated，用户坚持就允许"）；以及该 kworker 是否其实是 per-CPU 池的（形态 `kworker/<cpu>:` 而非 `kworker/u*`）。

**Q6：per-cpu wq 能改成 unbound 吗（反之呢）？**
不能。`apply_workqueue_attrs` 对非 `WQ_UNBOUND` wq 直接 `-EINVAL`（WARN_ON）。wq 的 bound/unbound 属性创建即定（`WQ_POWER_EFFICIENT` 的自动转换发生在 alloc_workqueue 内部）。想要"可切换"，建两个 wq 或改用 `system_unbound_wq`。

---

## 八、观测速查

| 问题 | 手段 |
| :--- | :--- |
| 现在有多少 unbound 池、各自 attrs | `drgn tools/workqueue/wq_dump.py`；sysrq-w 的 pool 行（`cpus=... node=... nice=...`） |
| 某 wq 实际用了哪些池 | sysrq-w 的 wq 段：每个 pwq 一行 `pwq N: cpus=... active=x/y` |
| 池是否过碎（scope 选错） | 数 `kworker/u*` 线程数与 pool id 范围；对比 cache/numa 下 `nr_pods` |
| worker 漂移是否严重 | wq_monitor.py 的 RPR 列（宽松模式遣返计数） |
| unbound mask 现值 | `cat /sys/devices/virtual/workqueue/cpumask` |
| 隔离是否生效 | 该 CPU 的 `kworker/u*` 出现即违规（正常只允许 `kworker/<cpu>:` 的 per-CPU 形态短暂存在） |

---

## 九、思考题

1. `wqattrs_equal` 不比较 `affn_scope`。两个 wq 分别声明 `affn_scope=numa` 和 `affn_scope=cache`，在单 LLC 的单 node 机器上会发生什么？这暴露了池共享的什么本质？
2. 宽松模式下 `kick_pool` 只改 `wake_cpu` 而不改 `cpus_allowed`。调度器哪个机制会尊重 wake_cpu？（调度器分册 `select_task_rq` 的 want_affine 路径。）被迁出 pod 后谁发现、谁把 worker 送回去？
3. `wq_unbound_cpumask` 热更新后，一个自续 work 永远留在旧池（旧 mask 的 CPU 上）。若旧 mask 的 CPU 全部下线，这个 work 的下一次执行会怎样？结合 `POOL_DISASSOCIATED` 的 unbind 机制推演。
4. 为什么 `queue_work_node` 只支持 unbound wq？若强行对 per-cpu wq 用"node 的第一个 CPU"，与直接 `queue_work_on(cpu)` 相比丢了什么语义？
5. 设计：一台 2-node × 4-die × 每 die 独立 LLC 的服务器跑存储压缩（数据来自各 node）。给出 affn_scope、affn_strict、max_active 的初始建议，并说明用什么观测验证/修正。

---

> 下一章：[使用规范与死锁分析](04-使用规范与死锁分析.md) —— WQ_* 标志的正确打开方式、system_* 家族选型、五类经典死锁的逐例剖析与十条军规。
