# KCSAN encoding：把一次访问压进一个原子字（Linux 6.6）

> 阅读目标：吃透 KCSAN（Kernel Concurrency SANitizer）的**watchpoint 编码**——如何把一次访问的 `(addr, size, is_write)` 三元组压进**单个 long**，让"查命中"与"消费"都退化为单原子操作；`INVALID=0`/`CONSUMED=1` 的哨兵妙用；槽=PFN 取模的全局哈希表与防回绕 padding；★假阳性两条件论证（encoding.h:31-43 注释原文——"Both these are assumed to be very unlikely"）与 report 侧的 `matching_access` 复核兜底；watcher/racer 双剧本与 `diff = old ^ new` 的值变化推断；限流三闸（skip 计数/delay 随机化/报告去重）；reorder_access 模拟编译器重排。全部对照 v6.6（kernel/kcsan/——★当轮考证：KCSAN 在 6.6 位于 kernel/ 非 mm/）。前置：[sanitizers](sanitizers.md)。

---

## 一、检测模型：watchpoint 不是断点也不是 shadow

```
KASAN 思路：每字节元数据 → 访问时被动检查（错了才炸）
KCSAN 思路：主动设局 —— "我在看这块内存，谁敢动它？"

剧本（三方：watcher / racer / 仲裁值）：
  watcher：为地址 A 设观察点 → delay（给对手时间）→ 重读 A 比对
  racer：  访问 A 时命中观察点 → 消费它 + 留下自己的档案
  值变化： 无人消费但值变了 → "unknown origin"（非插桩访问，如设备 DMA）
```

**encoding 的使命**：整个"设局/查局/拆局"高频发生在每个插桩访问上——必须把"一次访问"压进**一个自然对齐的原子字**（`atomic_long_t`），所有状态迁移用单条原子指令完成。

## 二、位布局：一个 long 装下三元组（kernel/kcsan/encoding.h）

```c
#define SLOT_RANGE PAGE_SIZE
#define INVALID_WATCHPOINT  0
#define CONSUMED_WATCHPOINT 1

#define MAX_ENCODABLE_SIZE (SLOT_RANGE * (1 + KCSAN_CHECK_ADJACENT))  /* 8K */
#define WATCHPOINT_SIZE_BITS bits_per(MAX_ENCODABLE_SIZE)             /* 14 */
#define WATCHPOINT_ADDR_BITS (BITS_PER_LONG-1 - WATCHPOINT_SIZE_BITS) /* 49 */
```

4K 页 64 位机上的布局（`WATCHPOINT_ADDR_MASK`/`SIZE_MASK`/`WRITE_MASK` 互斥完备，有 static_assert 兜底）：

```
63    62................49  48.................................0
+-----+-------------------+------------------------------------+
| 写? |  size（14 bit）    |  addr（低 49 bit，掩码后的地址）      |
+-----+-------------------+------------------------------------+
```

★**哨兵值的妙用**：`INVALID=0`、`CONSUMED=1`——合法编码的 addr 字段 ≥ PAGE_SIZE（`check_encodable` 强制 `addr >= PAGE_SIZE`，注释原话："While we can encode addrs<PAGE_SIZE, avoid crashing with a NULL pointer deref inside KCSAN"），因此 0 和 1 永远不与合法编码撞车，"槽空"与"槽已被消费"各占一个特殊值。

### 假阳性的两条件论证（encoding.h:31-43 注释原文）

```
 * This encoding for addresses discards the upper (1 for is-write + SIZE_BITS);
 * however, most 64-bit architectures do not use the full 64-bit address space.
 * Also, in order for a false positive to be observable 2 things need to happen:
 *
 *	1. different addresses but with the same encoded address race;
 *	2. and both map onto the same watchpoint slots;
 *
 * Both these are assumed to be very unlikely. However, in case it still
 * happens, the report logic will filter out the false positive (see report.c).
```

——丢高位换来单字编码，风险 = "同编码**且**同槽"两个低概率事件相乘；即便发生，report 侧还有 `matching_access` 复核兜底（§四）。

## 三、槽寻址：PFN 取模的哈希表

```c
static __always_inline int watchpoint_slot(unsigned long addr)
{
	return (addr / PAGE_SIZE) % CONFIG_KCSAN_NUM_WATCHPOINTS;
}
```

- **全局**槽表 `watchpoints[CONFIG_KCSAN_NUM_WATCHPOINTS + NUM_SLOTS-1]`（core.c:103，`atomic_long_t`，默认 64+2 个）——不按 CPU 分片，自然负载均衡
- **+2 的防回绕 padding**：一次访问要查**本页与左右邻页**（`KCSAN_CHECK_ADJACENT=1` → `NUM_SLOTS=3`，kcsan.h:17-18——跨页边界访问会落在邻页的槽里），`slot±1` 扫描时尾部多出的 2 个槽让索引**无需模运算回绕分支**
- 快扫 `find_watchpoint`（core.c:114-146）：3 槽逐个 `atomic_long_read` → decode → `expect_write` 方向过滤 → `matching_access` 区间重叠判定（`addr1 <= end2 && addr2 <= end1`——不是等值比较，**支持重叠区间**）

## 四、双剧本与双半报告

### 1. watcher：设局与值变化推断（core.c kcsan_setup_watchpoint）

```
insert_watchpoint（cmpxchg 置入编码）
→ 读旧值 old（read_instrumented_memory——注释原话："we don't use
   READ_ONCE(), since all we need for race detection is to observe 2
   different values"；且 arm64 的 READ_ONCE 会展开成不能未对齐的复杂指令）
→ delay_access（in_task() ? udelay_task(80) : udelay_interrupt(20)，可随机化）
→ 重读 new，diff = old ^ new（access_mask 非零则先 & access_mask）
→ consume_watchpoint：
    消费失败（racer 抢先消费了）→ known-origin 报告（两半齐了）
    消费成功但 diff≠0          → unknown-origin 报告
```

★`MAYBE→TRUE/FALSE` 映射规则（core.c:666-682）：`access_mask != 0` → FALSE（注释："races on ~access_mask bits are expected"——掩外位竞争是设计内行为）；`size > 8 || is_assert` → TRUE（"Always assume a value-change"——大对象无法值比对，宁报勿漏）。

### 2. racer：消费与留证（core.c kcsan_found_watchpoint:455）

```
注释原话："Let's try to keep the race-window between here and finally
 consuming the watchpoint below as small as possible"
→ try_consume_watchpoint：cmpxchg（旧值=精确的编码值 → CONSUMED）
   —— 编码本身就是 cmpxchg 的比较基准：任何字段变了都消费失败
→ 消费成功：kcsan_report_set_info(...) 把自己的 (ptr,size,type,ip,栈)
   存进 other_infos[slot]，等 watcher 来取
→ 消费失败：只加 REPORT_RACES 计数（"The other thread may not print
   any diagnostics, as it has already removed the watchpoint"）
```

### 3. 双半合流与假阳性过滤（report.c）

- `other_infos[CONFIG_KCSAN_NUM_WATCHPOINTS + NUM_SLOTS-1]`（:71）——**按槽索引**，注释原话："To never block any producers of struct other_info, we need as many elements as we have watchpoints (upper bound on concurrent races to report)"
- `prepare_report_consumer`（:603-618）：自旋等生产者填 `ai.size`（`cpu_relax` 循环）→ ★**假阳性过滤器**：`WARN_ON(!matching_access(other_info->ai.ptr & WATCHPOINT_ADDR_MASK, ..., ai->ptr & WATCHPOINT_ADDR_MASK, ...))`——报告前用掩码地址复核两访问**确实重叠**，兑现 encoding.h 注释的承诺（"the report logic will filter out the false positive"）
- `struct other_info` 头注释（:44-45）："communicated from other racing thread to thread that set up the watchpoint, which then prints the complete report **atomically**"；传 `task` 指针的代价（:49-63）：不能 get/put_task_struct（"would result in deadlock if we want to use KCSAN on the allocators"）——**把生产者线程原地困住**直到消费完成

### 4. 快慢路径分工（core.c:441-453 注译）

```
"the fast-path (including check_access) functions should all be
 inlinable by the instrumentation functions. The slow-path
 (kcsan_found_watchpoint, kcsan_setup_watchpoint) are non-inlinable --
 note that, we prefix these with 'kcsan_' to ensure they can be filtered
 from the stacktrace, as well as give them unique names for the UACCESS
 whitelist of objtool."
```

——kcsan_ 前缀的双重用途：stacktrace 过滤 + objtool UACCESS 白名单。

## 五、限流三闸（lib/Kconfig.kcsan）

| 闸 | 机制 | 默认 | 备注 |
| :--- | :--- | :--- | :--- |
| **skip** | per-CPU `kcsan_skip` 递减计数器（`this_cpu_dec_return(...) >= 0` 则跳过，core.c:280），慢路径 `reset_kcsan_skip` 重置——LCG 随机（:292-304 注释："Numerical Recipes" 的线性同余） | `KCSAN_SKIP_WATCH=4000` + RANDOMIZE=y | "one in KCSAN_SKIP_WATCH per-CPU memory operations are used to set up a watchpoint" |
| **delay** | `kcsan_udelay_task/interrupt` + `CONFIG_KCSAN_DELAY_RANDOMIZE` | 80 / 20 ms + y | **module_param 0644 运行时可调**（core.c:40-41） |
| **report** | `report_times[]` 按 (frame1, frame2) 对去重 | `REPORT_ONCE_IN_MS=3000` | 固定数组（一页）——注释原话："to avoid deadlock, report_times cannot be dynamically resized with krealloc"（要能拿 KCSAN 调试分配器自己） |

另：`KCSAN_INTERRUPT_WATCHER` **默认关**——help 原话："not all safe per-CPU access primitives and patterns may be accounted for, and therefore could result in false positives"；`REPORT_RACE_UNKNOWN_ORIGIN=y`（unknown-origin 报告可单独关）。

## 六、reorder_access：模拟编译器重排（core.c check_access 的第三岔口）

```
check_access 三岔口：
  ① 命中 watchpoint → kcsan_found_watchpoint
  ② should_watch（skip 闸放行）→ kcsan_setup_watchpoint
  ③ 都不是 → 若存在 reorder_access（CONFIG_KCSAN_WEAK_MEMORY）：
       把"上一条被记录的访问"当成本次访问 goto again 重查
       —— 模拟"编译器把前一条访问重排到后面"的效果
```

配套假阳性防护（:562-585 注译）：本 CPU 观察不到自己的重排，故 ① 重排访问与中断的竞争须 `local_irq_save`（`is_reorder_access → interrupt_watcher = false`）② 嵌套中断共享 ctx 会误触 scoped 检查 → `ctx->disable_scoped++`。reorder 读取的次序约束注释（:766-773）："We know that upon return, reorder_access is always invalidated by setting size to 0 via __tsan_func_exit(). Therefore we must read and check size after the other fields" + `barrier()`。

## 七、观测实战

```bash
# 1) 编译期
grep -E 'CONFIG_KCSAN(=|_)' /boot/config-$(uname -r)   # NUM_WATCHPOINTS/SKIP_WATCH/UDELAY_*

# 2) 运行时：单一 debugfs 文件（debugfs.c:271 的 debugfs_create_file("kcsan", 0644, ...)）
cat /sys/kernel/debug/kcsan          # 计数器快照（kcsan_counters[] 的 203 行循环导出）
#   关键计数：data_races / setup_watchpoints / used_watchpoints /
#            no_capacity（槽耗尽——该调大 NUM_WATCHPOINTS 或调小 SKIP_WATCH）/ races_unknown_origin

# 3) 运行时调延迟（module_param 0644）
ls /sys/module/kcsan/parameters/     # udelay_task / udelay_interrupt

# 4) 报告限流窗口
#   同一 (frame1, frame2) 对在 3000ms 内只报一次——report_times 固定一页
```

## 八、思考题

1. `CONSUMED_WATCHPOINT=1` 为什么是必要的第三态？（提示：racer 消费后槽里留什么？置回 INVALID 的话 watcher 无法区分"没人动过"与"有人动过但档案已留"——消费语义需要幂等可见性。）
2. `watchpoint_slot` 用 PFN 取模而非 hash_long——为什么够用？（提示：§二的假阳性两条件；同槽不同地址的碰撞由 matching_access 复核兜底 + 3 槽邻扫放大真实命中率。）
3. `encode_watchpoint` 的 cmpxchg 基准为什么不用版本号？（提示：编码本身就含 addr 字段——任何同槽异址的并发插入会因比较值不符而失败；版本号反而引入 ABA 之外的复杂性。）
4. watcher 的 `udelay` 在中断上下文为什么缩短到 20ms？（提示：中断里睡眠等价死锁；`kcsan_udelay_interrupt` 独立可调 + `INTERRUPT_WATCHER` 默认关——中断里被打断的 watcher 是假阳性重灾区。）
5. `read_instrumented_memory` 刻意不用 READ_ONCE——与"原子性无关"的论证链是什么？（提示：值变化检测只需要"观察到两个不同的值"，撕裂读最多造成漏报/误报 MAYBE→规则收敛；而 READ_ONCE 在 arm64 会展开成不允许未对齐的复杂指令。）
6. other_infos 为什么必须按槽数量配满而不能做成队列？（提示：报告路径自旋等待生产者——"To never block any producers"；槽即天然的去重键与容量上界。）

---

> 回看：[sanitizers](sanitizers.md)（KASAN/KCSAN/KFENCE 三巨头总览）｜[kcsan 姊妹：KASAN_VMALLOC](../mm/kasan/kasan-vmalloc.md)（另一个 sanitizer 的 vmalloc 深挖）｜[MTE 与 LAM](../mm/kasan/mte-lam.md)（地址位元数据——编码思想的硬件亲戚）｜[locking/README](README.md)（锁族谱）
