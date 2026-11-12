# 三巨头：KASAN / KFENCE / KCSAN（Linux 6.6）

> 阅读目标：盘点内存/Race 检测的三个重型武器——**KASAN**（Address SANitizer：shadow memory 全量插桩 + 三模式 generic/sw_tags/hw_tags）、**KFENCE**（采样式 guard page 检测——"low-overhead...production environments" Kconfig 原话）、**KCSAN**（Concurrent SANitizer：watchpoint 采样检测数据竞态）。理解它们的**共同设计哲学**（ instrumentation 替代断言）与**分工矩阵**（检测什么 × 开销多大 × 生产可用与否），以及与 [debug-adversarial](debug-adversarial.md) 传统刑具的互补关系。全部对照 v6.6（mm/kasan/kasan.h 644 行、mm/kfence/kfence.h 145 行、kernel/kcsan/kcsan.h 142 行、lib/Kconfig.kfence）。前置：[debug-adversarial](debug-adversarial.md)（传统刑具——本章为"插桩型"的延伸）。

---

## 一、定位：插桩 vs 检查——与传统刑具的分野

```
传统刑具（debug-adversarial 五件）：
  在"操作时刻"做检查——list_add 校验、might_sleep 断言、lockdep 图
  → 检查点是手工选定的，覆盖有限

三巨头（本章）：
  编译器/硬件级"插桩"——每次内存访问都过一遍检测逻辑
  → 覆盖全面但开销大（KASAN ~2x）/ 采样折中（KFENCE/KCSAN ~0.1%）
```

## 二、KASAN：shadow memory 全量检测

### 2.1 三模式（mm/kasan/kasan.h:26-36）

```c
enum kasan_mode {
	KASAN_MODE_SYNC,    /* 同步：每次访问都检查 */
	KASAN_MODE_ASYNC,   /* 异步：硬件标记，延迟报告 */
	KASAN_MODE_ASYMM,   /* 混合：常 async + 精确 sync */
};
```

| 模式 | CONFIG | shadow 表示 | 硬件依赖 |
| :--- | :--- | :--- | :--- |
| generic | KASAN_GENERIC | 每字节 1 shadow byte（0=可访问/1-7=前 N 字节可访问/负值=各种不可访问） | 无 |
| sw_tags | KASAN_SW_TAGS | 8 字节 granule 1 tag byte（匹配标签） | 无 |
| hw_tags | KASAN_HW_TAGS | ARM64 MTE（Memory Tagging Extension） | ARMv8.5 MTE |

### 2.2 shadow memory 的映射（generic 模式）

```
每个内存地址 addr 的 shadow：
  shadow_addr = (addr >> KASAN_SHADOW_SCALE_SHIFT) + KASAN_SHADOW_OFFSET
  /* SCALE_SHIFT 通常 = 3（8 字节 → 1 shadow byte） */

shadow byte 语义：
  0x00 = 8 字节全部可访问
  0x01~0x07 = 前 N 字节可访问（部分粒度）
  0xFE = use-after-free（KASAN_FREE）
  0xFB = redzone（KASAN_REDZONE）

★KASAN_GRANULE_SIZE = 1 << SCALE_SHIFT（kasan.h:126）
  ——检测精度受此粒度约束（generic 8 字节/hw_tags 16 字节 MTE granule）
```

### 2.3 检测原理

```
读/写操作被编译器改写为：
  if (shadow_byte < 0 || shadow_byte < offset_in_granule)
      → 报告（kasan_report）

编译器插桩点：__asan_load1/2/4/8/16、__asan_store1/2/4/8/16
（每个对应大小的访存都有独立的检查函数——跟踪到具体指令）
```

### 2.4 KASAN_TAG 标签族（kasan.h:143-147）

```c
#define KASAN_PAGE_FREE      KASAN_TAG_INVALID   /* 释放后页 */
#define KASAN_PAGE_REDZONE   KASAN_TAG_INVALID   /* 页边红区 */
#define KASAN_SLAB_REDZONE   KASAN_TAG_INVALID   /* slab 边红区 */
#define KASAN_SLAB_FREE      KASAN_TAG_INVALID   /* 释放后 slab 对象 */
#define KASAN_VMALLOC_INVALID KASAN_TAG_INVALID  /* vmalloc 红区 */
```

## 三、KFENCE：采样式 guard page 检测

### 3.1 设计哲学（lib/Kconfig.kfence:2-8 原话）

```
"KFENCE is a low-overhead sampling-based detector of heap
 out-of-bounds access, use-after-free, and invalid-free errors.
 KFENCE is designed to have negligible cost to permit enabling
 it in production environments."

"KFENCE is not a substitute for explicit testing with tools
 such as KASAN. KFENCE can detect a subset of bugs that KASAN
 can detect, albeit at very different performance profiles.
 If you can afford to use KASAN, continue using KASAN, for
 example in test environments. If your kernel targets production
 use, and cannot enable KASAN due to its cost, consider using
 KFENCE."
```

### 3.2 机制：采样分配 + guard page

```
Kconfig 参数：
  KFENCE_SAMPLE_INTERVAL = 100ms（默认）
  KFENCE_NUM_OBJECTS = 255（默认，1~65535）

池布局（mm/kfence/core.c）：
  __kfence_pool（连续内存区，2 页/对象——1 页对象 + 前后 guard page）
  每次采样（interval 到期）从 slab 拿下一个分配 → 放入池内
  ★guard page 是**PTE 级保护**（PROT_NONE）——越界访问立即 page fault

检测：
  越界 → guard page fault → kfence_handle_page_fault
  UAF → 释放后对象标记 UNUSED + canary 校验
  canary（kfence.h:20-22）：
    KFENCE_CANARY_PATTERN_U8(addr) = 0xAA ^ (addr & 0x7)
    ★每字节模式与地址低 3 位相关——检测"相似常量污染"的概率最大化
```

### 3.3 开销对比

| 检测器 | 开销 | 覆盖 | 生产可用 |
| :--- | :--- | :--- | :--- |
| KASAN generic | **~2x**（每次访存都查 shadow） | 100% | ✗（太慢） |
| KASAN hw_tags | ~1.1x（硬件 MTE） | 100%（MTE 范围） | △（ARM64 only） |
| **KFENCE** | **~0.1%**（仅采样分配） | 极低（1/interval × 255 个活跃对象） | ✓ |

## 四、KCSAN：数据竞态采样检测

### 4.1 机制：watchpoint + 延迟

```
核心思路：
  ① 按采样概率选一个内存地址 → 设置 watchpoint（原子编码）
  ② 在地址旁 delay 一小段（kcsan_udelay_task）
  ③ 检查期间如果另一 CPU 也访问了该地址 → 报告数据竞态

kcsan.h:20-21：
  #define KCSAN_CHECK_ADJACENT 1     /* 检查相邻 watchpoint 数 */
  #define NUM_SLOTS (1 + 2*KCSAN_CHECK_ADJACENT)  /* 共 3 slot */
```

### 4.2 检测语义

```
不是"检测到就一定错误"——KCSAN 报告的是"数据竞态"（两个访问
并发且至少一个是写且无同步保护）。竞态本身不一定是 bug——
但内核的正确性规范要求：所有跨 CPU 共享访问都应有明确的
同步或标注（READ_ONCE/WRITE_ONCE/data_race()）

误报控制：
  permissive.h 的白名单——已知安全的竞态（如统计计数器的
  non-atomic 更新）通过 kcsan_disable/enable 对跳过
```

## 五、分工矩阵

| 检测器 | 检测什么 | 插桩方式 | 开销 | 生产可用 | 误报率 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| KASAN | UAF/OOB/invalid-free | 编译器插桩 + shadow | ~2x | ✗ | 极低 |
| KFENCE | UAF/OOB/invalid-free | guard page + 采样分配 | ~0.1% | ✓ | 极低 |
| KCSAN | 数据竞态 | watchpoint + 采样地址 | ~1-3% | △ | 中（竞态≠bug） |

**选型口诀**：测试环境上 KASAN（全量）；生产环境上 KFENCE（采样兜底）；追竞态上 KCSAN（但要处理误报）。

## 六、与传统刑具的互补

| 维度 | 传统刑具（debug-adversarial） | 三巨头（本章） |
| :--- | :--- | :--- |
| 检测时机 | 操作时刻（手工选点） | 每次访存（编译器/硬件插桩） |
| 开销来源 | 额外检查代码 | shadow 查找/guard page/watchpoint |
| 检测范围 | 特定不变式（锁序/链表/上下文） | 通用内存操作 |
| 组合 | lockdep + DEBUG_LIST 互补 | KASAN/KFENCE/KCSAN 互补 |
| 典型组合 | CI 内核全开 | 测试 KASAN + 生产 KFENCE |

## 七、观测实战

```bash
# 1) KASAN 报告识别
dmesg | grep -A 20 "BUG: KASAN:"
#   输出含访问地址/shadow 值/调用栈（分配与释放两份）

# 2) KFENCE 的采样率控制
cat /sys/module/kfence/parameters/sample_interval
echo 10 > /sys/module/kfence/parameters/sample_interval  # 10ms——更密集

# 3) KCSAN 报告
dmesg | grep -A 15 "BUG: KCSAN:"
#   两个访问点的完整调用栈

# 4) 典型 CI 内核配置
CONFIG_KASAN=y
CONFIG_KASAN_GENERIC=y       # 或 HW_TAGS（ARM64）
CONFIG_KFENCE=y
CONFIG_KCSAN=y
#   + CONFIG_DEBUG_* 全家桶（见 debug-adversarial 章）
```

## 八、思考题

1. KASAN generic 模式的 shadow 是每 8 字节 1 shadow byte——为什么不是每字节？（提示：shadow 内存开销 = 1/8 vs 1/1——全量 shadow 的内存代价。）
2. KFENCE 的 guard page 是 PTE 级保护——为什么不用 trap flag？（提示：PTE 的 PROT_NONE 由硬件 MMU 检查——零开销；trap flag 需要单步。）
3. KCSAN 报告的"竞态"不一定是 bug——为什么仍然值得默认开？（提示：竞态暗示缺少明确同步——"unknown sync" 是维护债。）
4. KASAN hw_tags 依赖 ARM64 MTE——x86 上无法使用。这如何影响跨架构 CI 策略？（提示：x86 CI 用 generic（慢但全量）/ARM64 CI 用 hw_tags（快）。）
5. KFENCE 的 canary pattern（0xAA ^ addr&7）为什么不用固定值？（提示：固定值可以被"合理"数据碰巧覆盖——变值提高检测概率。）
6. KASAN + KFENCE 同时开会怎样？（提示：KASAN 的 shadow 覆盖一切（包括 KFENCE 的池）；KFENCE 的 guard page 触发的 fault 由 KASAN 先拦截——报告可能双份。）

---

> 回看：[debug-adversarial](debug-adversarial.md)（传统五件刑具——本章为插桩型的延伸）｜[locking/README](README.md)（锁族谱）｜[seqlock](seqlock.md)（KCSAN 检测的竞态对象之一）
