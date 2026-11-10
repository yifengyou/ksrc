# memory 控制器（Linux 6.6）

> 阅读目标：掌握 v2 memory 控制器接口族与 page_counter 的四道闸门（min/low/high/max 行为各不相同）；吃透两层记账（folio → obj_cgroup → mem_cgroup）与 per-CPU stock 批处理；理解 memcg 直接回收、swap 记账、组内 OOM 的选人规则；用 memory.stat/events 做容器内存排障。

---

## 一、接口文件族

| 文件 | 语义 |
| :--- | :--- |
| `memory.current` | 当前记账总量（anon+file+kernel+sock+swap） |
| `memory.min` | **硬保护**：低于此值的页回收器拒绝触碰（即使全局 OOM 也保） |
| `memory.low` | **软保护**：尽量不回收，忙时可越线 |
| `memory.high` | **节流闸**：越线不 OOM，而是"罚时 + 催收"（第五节） |
| `memory.max` | **硬上限**：越线走回收，回收不济走组内 OOM |
| `memory.reclaim` | 6.6：写接口——**主动**要求本组回收 N 字节（同步预降载） |
| `memory.oom.group` | 0/1：组内 OOM 时把**整组**进程纳入候选（防数据面半死不活） |
| `memory.events` | 事件计数（低/高/max/oom/oom_kill/oom_group_kill/swap_*） |
| `memory.swap.current` / `swap.max` / `swap.high` | swap 记账与独立限额 |
| `memory.stat` | 全量流水（下文逐列） |
| `zswap.max` | 组级 zswap 池上限（6.6 已有，联动 mm 分册的 zswap） |

`memory.events` 的九列（`enum memcg_memory_event`，已核对源码）：`low / high / max / oom / oom_kill / oom_group_kill / swap_high / swap_max / swap_fail`。

---

## 二、两层记账：folio → obj_cgroup → mem_cgroup

```
匿名/文件页                 内核对象（slab）
─────────────              ─────────────
folio ──► obj_cgroup ──► mem_cgroup
（page->memcg_data 指针）   （页的记账挂 objcg；
                            slab 的记账挂"对象所属 objcg"）
```

* **为什么要中间层 obj_cgroup**：一个 obj_cgroup 永远属于一个 mem_cgroup，但**生命周期可以比 memcg 的"成员资格"长**——kmem 时代记账与释放异步（RCU 窗口），直接挂 memcg 会让 memcg 的释放等待全量对象归还。objcg 把等待圈在薄对象上。
* **folio 的记账**（`__mem_cgroup_charge`）：首次映射时挂到当前任务的 objcg；批量挂 LRU 时把 objcg 引用折算给 memcg 的 LRU 计数。`folio_memcg()` 的读取在 6.6 恒经 memcg_data 编码（MEMCG_DATA_... 标志位区分 folio/slab/objcg 情形）。
* **slab 记账**：每对象挂 objcg 的 state 数组（`NR_KMEM_...` 按页折算）。6.6 已无"每 memcg 一套 kmalloc cache"（5.9 统一）——只有一份 slab cache，记账走对象级计数。
* **per-CPU stock（`memcg_stock_pcp`）**：charge/uncharge 的原子加减是最热路径，批量囤 32 页在本地股池，跨 memcg 边界才 drain——把原子操作摊薄一个量级。

---

## 三、charge 主链与四道闸门

```
__mem_cgroup_charge(folio, mm, gfp)
 └─ try_charge(memcg, gfp, nr_pages)
      for (retries = MAX_RECLAIM_RETRIES; ;) {
          page_counter_try_charge(&memcg->memory, nr, &over)   /* page_counter
                                                                   沿层级向上传播 */
          ├─ 未超 max → 成功（顺手检查 high：超了挂"欠账" memcg_nr_pages_over_high）
          └─ 超了：
               ① memory.max 沿祖先链检查——先回收本组：try_to_free_mem_cgroup_pages
                  （memcg direct reclaim：扫本组的 LRU——anon 换出/file 丢弃）
               ② swap 限额满（MEMCG_SWAP_MAX 事件）也拦
               ③ 回收反复不济 → mem_cgroup_out_of_memory
                     ├─ memcg_oom_reap（异步清失效原子上下文的映射）
                     ├─ oom_badness：组内按"本组记账占用"排选受害者
                     │   （+ oom_group 时用 css_task_iter 遍历全组）
                     └─ MEMCG_OOM / MEMCG_OOM_KILL 记事件
      }
```

**page_counter 的层级传播**：`memory.max` 的检查沿"本组 → 祖先"逐级（每级都记 events），quota 世界里父子同时设限取最紧的命中。四道闸门的行为差异是治理核心：

| 闸门 | 越线行为 | 谁触发 | 典型用途 |
| :--- | :--- | :--- | :--- |
| `min` | 回收拒绝触碰这段内存 | 全局/组回收 | 不可降级的服务（etcd 之类） |
| `low` | 回收"最后"才动它（软优先级） | 回收排序 | 一般服务的优先保障 |
| `high` | **罚时**：按超额比例 `calculate_high_delay()` 睡眠 + 主动回收（`mem_cgroup_handle_over_high`，罚时上限 `MEMCG_MAX_HIGH_DELAY_JIFFIES` 封顶） | 分配者自己 | 反压（backpressure）：让写入方减速而非死亡 |
| `max` | 回收 → 组内 OOM | 分配者自己 | 硬隔离（防内存泄漏打穿宿主） |

> 💡 `high` 是 v2 最被低估的旋钮：**它是"限速器"不是"断路器"**。日志服务写爆时选 high=慢下来，选 max=被 OOM。罚时公式的输入是超额页数与 `high` 距离，输出 jiffies——越界越多罚越重，天然反压曲线。

---

## 四、memory.stat 逐列精读（排障主战场）

```
anon / file                    匿名与文件页总量（charge 侧分类）
kernel                         内核栈/页表等不可移动记账
kernel_stack / pagetables      细分（6.6 已拆出）
slab / slab_reclaimable / slab_unreclaimable
percpu / sock / vmalloc        其余内核内存
shmem                          共享内存（file 计数内的特殊类）
zswap / zswapped               zswap 压缩存储（联动 mm 分册）
file_mapped                    已映射文件页
anon_thp / file_thp            大页细分
pgfault / pgmajfault           缺页与**_MAJOR** 缺页（读盘次数——颠簸第一指标）
pgrefill / pgscan / pgsteal    回收扫描与回收比（steal/scan = 回收效率）
pgactivate / pgdeactivate      活跃↔不活跃 LRU 流动
workingset_refault_anon/file   ★再次访问曾被回收的页（"看似空闲却慢"的元凶）
workingset_activate_*          refault 中被证明"该留"的部分
workingset_nodereclaim_*       提前回收避免的 refault
nr_slab_reclaimable 等         瞬时值（其余为累计计数器）
```

**读法范式**：`memory.current` 告诉你"占多少"，`memory.stat` 告诉你"占的是什么、发生过什么"——`pgmajfault` 高 + `workingset_refault` 高 = 组的活跃集大于限额（该加限额或降负载），而不是"内存泄漏"。

---

## 五、组内 OOM 的细节

* **候选范围**：`mem_cgroup_out_of_memory` 只在本组（含子组，可配）选受害者——`oom_badness` 用**本组记账**（`mem_cgroup_usage`）而非全局观点排序；与全局 OOM（mm 分册 ras 部分）互不干扰。
* **`memory.oom.group=1`**：把整个 cgroup 视作一个受害者单元（css_task_iter 遍历整组发 SIGKILL）——服务由多进程协同时，"只杀最大者"常留下半瘫状态（监听器死了、worker 活着），oom_group 换取干净重启。对应事件 `oom_group_kill`。
* **与进程退出联动**：受害者的内存记账随页释放回落——OOM 后 `memory.events` 的 `max`/`oom_kill` 是容器监控的第一告警源。

---

## 六、swap 记账与 memory.swap.events

swap 的 charge 在换出时发生（页离开 memcg 的 LRU 进入 swap cache 记到 `memory.swap.current`），换入时回冲。`swap.max` 独立限额（`swap_fail` 事件 = 想 swap 无额度）；`swap.high` 走同样的"罚时"逻辑（`swap_high` 事件）。**total = memory + swap 由 `memory.max` 统一控制、swap 再单设细闸**是 v2 的双层设计（v1 的 memsw 混合语义已废）。

---

## 七、实战排障流

```
① current 贴 max 且 oom_kill 增长 → 真限额不足或泄漏：
   memory.stat 看 anon（涨=泄漏/缓存未释放）vs slab（涨=内核对象泄漏）vs file（可控）
② current 不高但服务慢 → 看 pgmajfault/workingset_refault：
   活跃集 > 实际可用 → 加限额 或 memory.reclaim 预热/降载
③ 延迟毛刺但无 OOM → high 罚时：events 的 high 列在涨（throttle 型反压）
④ 容器 RSS ≠ memory.current → stat 的 file/kernel/sock 部分不在 RSS 里；
   反之 RSS 里的共享页只按第一个 touch 者粗粒度记账——两种"对不上"方向相反
⑤ 主动工具：echo 256M > memory.reclaim（同步回收实验）；
   drgn/crash 下 mem_cgroup 的 per-node LRU 现场排查
```

---

## 八、机制侧骨架（衔接 mm 分册）

* **mem_cgroup 的主要字段**（mm/memcontrol.c）：`memory`/`swap`（page_counter 链头）、`info`（各 events 计数）、`vmpressure`（老 v1 通知的内核侧）、`nodeinfo[]`（per-node 的 `lruvec`——组的每节点 LRU 就挂这里，与 pgdat 的 lruvec 并行存在）、`omem`（objcg 计数）、`kmem`（slab 页计数）、`socket_pressure`、`mutex`/`spinlock`。
* **LRU 双轨**：页的 lruvec 取 `folio_lruvec(folio)` = folio 记账 memcg × 所在 node——全局回收（kswapd）与 memcg 回收共用同一套 isolate/shrink 代码，只是遍历起点不同（mm 分册 page/cache 章的 lru 机制在此复用）。
* **uncharge 家族**：`__mem_cgroup_uncharge`（页释放）、folio split 的逐子页分账、`mem_cgroup_uncharge_list`（回收批量）——与 charge 一样批量走 stock。
* **workingset**：组的 refault 统计复用全局 workingset 机制（`workingset_refault` 对 folio 的 memcg 记账）——"曾被回收又回来"的页按组归因，是 memory.stat 最有诊断力的列。
* **v1 映射**：`memory.limit_in_bytes` → `memory.max`；`memsw.limit_in_bytes` → 无对应（v2 拆成 memory+swap 两道）；`memory.soft_limit_in_bytes` → `memory.low`（语义近似但实现全新——v1 的 soft limit 是回收时的比较排序，v2 的 low 是保护位）。

## 九、排障案例二则

**案例 A："容器内存泄漏"**。`memory.current` 每日 +500MB，重启即回落。

```
1. memory.stat：增量几乎全在 file（页缓存）——不是泄漏，是文件页堆积
2. 但为何不回落？组内常驻进程持有少量 folio 的引用？错——
   file 页的"常驻"只因组的 LRU 老化太慢（低压力下 aging 不推进）
3. 处置：memory.high 设为 max 的 80%（罚时机制让写入方被反压并触发回收），
   或定时 echo 512M > memory.reclaim 主动修剪
```

**案例 B：oom_kill 波动**。每周随机一次组内 OOM，`memory.current` 峰值仅 85% max。

```
1. 高峰页缓存瞬间涌入（日志 flush 批）+ anon 峰值叠加 → 单周期内 try_charge
   回收追不上分配速度（direct reclaim 的扫描上限 MAX_RECLAIM_RETRIES 耗尽）
2. 处置：memory.high 兜住缓存增长节奏 + workingset_refault 确认活跃集大小
   → 若 refault 高说明限额本身偏低，加 max 而非调 high
```

---

## 九点五、per-CPU stock 的批处理细节

```
struct memcg_stock_pcp { cached[NR_OBJCGS?]（6.6: task_stock/obj_stock/irq_stock
                          三槽——task 的 NR_PAGES_BATCH=32、objcg 的 32 对象、
                          中断路径独立囤货防递归） }

charge 热路径：先看本地 stock 有余量 → 原子减 stock 即返回（零 memcg 全局结构接触）
stock 空 → 一次 charge_compound 批量从 page_counter 取 32 页入囤
跨界（换组/换 memcg）→ drain_local_stock（先冲销旧组的账）
drain_all_stock：跨 CPU 冲账（memory hotplug/限额变更时全局同步）
```

这个"本地 32 页零钱"设计把 charge 的均摊成本压到接近纯原子加减——6.6 又把 stock 拆成 task/obj/irq 三个子槽（`struct obj_stock`），分别服务任务上下文、对象记账、中断上下文（防 charge → 中断 → 再 charge 的递归污染囤货归属）。

## 九点六、events 与 swap.events 全表（include/linux/memcontrol.h 已核对枚举）

```
memory.events：  low / high / max / oom / oom_kill / oom_group_kill
memory.swap.events： swap_high / swap_max / swap_fail
```

注意 `low`/`high` 也是事件（不是只有 max 才响）——监控上三档告警（low=保障被触、high=反压生效、max=硬限触碰）各配不同严重级别，比只盯 oom_kill 提前一个身位。

## 九点七、THP 与尾页的记账特例

* THP（2MB 匿名大页）一次 charge 512 页：`memory.max` 贴边的组配 THP 会看到"一次越界 2MB"的粗粒度抖动——排障时先确认 `memory.stat` 的 `anon_thp` 占比再谈限额精度。
* 大页的"软"代价之外还有 split 路径：`split_huge_page` 把一个 folio 拆散时**记账不动**（split 是记账无关的结构操作），但 KSM 合并页恢复独占时会产生 uncharge/charge 往返。
* 尾页（tail page）：6.6 的 `page->memcg_data` 只存头页，尾页经 folio 头间接寻址——用户态工具读 `/proc/kpageflags` 一族时注意大页的计数语义差异。

## 九点八、mem_cgroup 全字段精读（补路线图指定素材）

```c
struct mem_cgroup {                     /* mm/memcontrol.c, 6.6 关键字段 */
	struct cgroup_subsys_state css;  /* 宿主插槽（第 1 章协议）*/
	struct page_counter memory;      /* 主账本（含 min/low/high/max 全链）*/
	struct page_counter swap;        /* swap 账本（两本独立记账）*/
	struct page_counter kmem;        /* 内核内存（slab 等）——memory 的子账本 */

	struct obj_cgroup __rcu *objcg;  /* 当前组的对象记账锚（第 3 节）*/
	struct memcg_vmstats vmstats;    /* 全部累计计数器（memory.stat 的数据源，
	                                    NR_VM_NODE_STAT_ITEM 级全量）*/
	struct memcg_vmstats_percpu __percpu *vmstats_percpu;  /* per-CPU 侧写 */

	struct mem_cgroup_events events; /* events 文件的计数器（九枚举）*/

	struct lruvec nodeinfo[0];       /* per-node 的 lruvec 柔性数组——
	                                    组自己的 LRU（每节点一份），与
	                                    pgdat 的全局 lruvec 并行 */
	atomic_t moving_account;         /* 迁移记账时的同步屏障 */
	spinlock_t        move_lock;
	...
};
```

三个阅读点：① `vmstats_percpu` → `vmstats` 的上卷与 cgroup rstat 联动（第 1 章）——memory.stat 的读也是"读时付费"；② nodeinfo 柔性数组按 node 数开——memcg 的 LRU 是**每 memcg × 每 node** 的二维矩阵（回收遍历的复杂度来源）；③ `moving_account`/`move_lock` 服务 v1 时代 `memory.move_charge_at_immigrate` 的记账迁移（v2 已无此接口，字段为兼容保留）。

## 十、思考题

1. `min` 与 `low` 的差别在回收器代码里是哪两处判断？（提示：`mem_cgroup_protected` 的返回值 MIN/LOW/0 三态如何影响 `shrink_node` 的优先级。）
2. high 的罚时为什么封顶 `MEMCG_MAX_HIGH_DELAY_JIFFIES` 而不线性无界？（提示：分配方持锁/持资源的场景，罚时过长的连锁反应。）
3. obj_cgroup 比直接挂 memcg 多一层间接。哪个具体的异步窗口让"等待归还"必须圈在 objcg？（对照 RCU 与 slab 释放路径。）
4. `memory.reclaim` 与 `memory.high` 都能压低占用，二者的触发主体与时机有什么本质区别？
5. 设计：Redis 容器（数据集 8G、可容忍换出、绝不可 OOM）与日志容器（可被反压）的 min/low/high/max/swap.max 五元配置，并说明每项的对立面风险。

---

> 下一章：[io 控制器与 cpuset](04-io控制器与cpuset.md) —— io.cost/io.latency/io.max/io.weight 四把刀与 cpuset 的 partition 语义。
