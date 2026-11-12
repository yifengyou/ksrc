# tnum：三值数——eBPF 验证器的数学底座（Linux 6.6）

> 阅读目标：吃透 tnum（tracked/tristate number）的**知识传播代数**——每个 bit 独立地处于"已知 0/已知 1/未知 x"三态，算术与逻辑运算如何位级精确地传播不确定性（加法的进位携带 `chi`、乘法的部分积分解）；理解 `tnum_range` 与 `tnum_in` 的**过逼近诚实注释**（sound 优先于 precise 的验证器哲学）；掌握 verifier 的三层消费链：`var_off`（tnum）→ `__update_reg64_bounds` 的符号位技巧 → `deduce_bounds` 的符号跨界互推。全部对照 v6.6（include/linux/tnum.h、kernel/bpf/tnum.c——★6.6 中 tnum 实现仅此两文件，`lib/test_tnum.c` 不存在）。前置：[10-eBPF 验证器入门](10-eBPF验证器入门.md)。

---

## 一、是什么：位级知识的三态表示（tnum.h:1-7 头注释）

```c
/* tnum: tracked (or tristate) numbers
 * A tnum tracks knowledge about the bits of a value.  Each bit can be either
 * known (0 or 1), or unknown (x).  Arithmetic operations on tnums will
 * propagate the unknown bits such that the tnum result represents all the
 * possible results for possible values of the operands. */

struct tnum {                    /* tnum.h:14-17 */
	u64 value;
	u64 mask;
};
```

**核心不变式**：`value & mask == 0`——mask 位为 1 表示"该位未知"，mask 位为 0 时 value 的对应位才是确定知识。三个极点：

```c
const struct tnum tnum_unknown = { .value = 0, .mask = -1 };  /* tnum.c:14 全未知 */
struct tnum tnum_const(u64 v)  { return TNUM(v, 0); }         /* :16-19 全已知 */
```

与 scheduler 分册 [14-schedstat](../../scheduler/docs/14-调度器统计纵深schedstat.md) 的 SCHED_INFO"时间戳账"对照：那是**数值累计**的代数，tnum 是**知识传播**的代数——verifier 不算结果是多少，只算"能证明到什么程度"。

## 二、构造器：tnum_range 的过逼近诚实（tnum.c:21-35）

```c
struct tnum tnum_range(u64 min, u64 max)
{
	u64 chi = min ^ max, delta;
	u8 bits = fls64(chi);
	if (bits > 63)
		return tnum_unknown;       /* 1ULL << 64 UB 防护 */
	delta = (1ULL << bits) - 1;    /* 最高差异位以下全部标未知 */
	return TNUM(min & ~delta, delta);
}
```

头文件的**诚实声明**（tnum.h:24-28 原话）：*"Could include values outside the range of [@min, @max]. For example tnum_range(0, 2) is represented by {0, 1, 2, **3**}, rather than the intended set of {0, 1, 2}."*——最小/最大只异或在第 3 位（0b10），该位以下全涂抹成 x，结果**包含本不在区间内的 3**。这就是验证器的哲学：**sound 优先于 precise**（宁可包含多余值，绝不漏掉真实值——漏掉=放行越界）。

## 三、算术运算：进位携带的位技巧

### 3.1 加法（tnum.c:62-72）

```c
struct tnum tnum_add(struct tnum a, struct tnum b)
{
	u64 sm, sv, sigma, chi, mu;

	sm = a.mask + b.mask;
	sv = a.value + b.value;
	sigma = sm + sv;           /* Σ = (m_a+m_b) + (v_a+v_b) */
	chi = sigma ^ sv;          /* ★进位携带集合：
	                              Σ 与 v 的差 = 与未知数求和引入的新进位 */
	mu = chi | a.mask | b.mask;   /* 新未知 = 携带 ∪ 原未知 */
	return TNUM(sv & ~mu, mu);
}
```

手推一例：`{v=0, m=0b11} + {v=0, m=0b11}`（两个 0-3 未知数）→ sv=0, sm=0b110, sigma=0b110, chi=0b110, mu=0b111 → `{v=0, m=0b111}`：和∈{0..6}⊂{0..7} ✓（真值域恰是 0-6，tnum 给 0-7——又一次精确性让位于健全性）。

### 3.2 减法（tnum.c:74-84）

```c
	dv = a.value - b.value;
	alpha = dv + a.mask;
	beta  = dv - a.mask;       /* 值 ± 自身未知度 */
	chi = alpha ^ beta;        /* 借位不确定性集合 */
	mu = chi | a.mask | b.mask;
```

### 3.3 乘法（tnum.c:114-141）：部分积 + 学术出处

```c
/* 注释原话（:114-121）："Generate partial products by multiplying
 * each bit in the multiplier (tnum a) with the multiplicand (tnum b)...
 * decompose each partial product into two tnums, consisting of the
 * value-sum (acc_v) and the mask-sum (acc_m)...
 * The following paper explains the algorithm in more detail:
 * https://arxiv.org/abs/2105.05398." */
```

按乘数 a 的每个 bit 生成部分积：确定 1 位 → 加 b 的 mask 进 acc_m；未知位 → 加 `b.value|b.mask`（最坏情形）；确定 0 位 → 零贡献——移位累加 64 轮。**内核源码引用 arXiv 论文**是罕见的景象（2105.05398）。

## 四、逻辑与位移：三个公式，三种精度（tnum.c:86-120）

```c
struct tnum tnum_and(struct tnum a, struct tnum b)   /* :86 */
{
	alpha = a.value | a.mask;      /* a 的"可能全 1"上界 */
	beta  = b.value | b.mask;
	v = a.value & b.value;         /* 双方确定 1 才确定 1 */
	return TNUM(v, alpha & beta & ~v);
	/* ★AND 最精确：0&x=0、x&0=0、1&x=x——确定 0 能"消毒"未知位 */
}

struct tnum tnum_or(struct tnum a, struct tnum b)    /* :96 */
{
	v = a.value | b.value;
	mu = a.mask | b.mask;
	return TNUM(v, mu & ~v);
	/* ★OR 居中：1|x=1 能消毒，但 x|x=x 的传播只并 mask（不做
	   chi 分析——OR 不产生跨位进位，位独立） */
}

struct tnum tnum_xor(struct tnum a, struct tnum b)   /* :105 */
{
	v = a.value ^ b.value;
	mu = a.mask | b.mask;
	return TNUM(v & ~mu, mu);
	/* ★XOR 最保守：任何一方的 x 都传染结果位，无消毒能力 */
}
```

位移三兄弟（:37-60）：`lshift/rshift` 平移 value 与 mask；`arshift` 处理**算术右移的符号位**——`(s64)value >> min_shift` 连 mask 一起算术移（注释 :49-53 原话解释负值时最小移位产生更大负偏移），32 位指令按 `(u32)(s32)` 换算。

## 五、verifier 的三层消费链：从 tnum 到界

`bpf_reg_state`（bpf_verifier.h:143/:149-152）：

```c
struct tnum var_off;         /* :143 ★唯一事实源（真相在这） */
s64 smin_value; smax_value;  /* :149-150 signed 界（派生缓存） */
u64 umin_value; umax_value;  /* :151-152 unsigned 界（派生缓存） */
```

**第一层：tnum → 界**（verifier.c `__update_reg64_bounds`，符号位技巧）：

```c
/* min signed is max(sign bit) | min(other bits) */
reg->smin_value = max_t(s64, reg->smin_value,
		reg->var_off.value | (reg->var_off.mask & S64_MIN));
/* max signed is min(sign bit) | max(other bits) */
reg->smax_value = min_t(s64, reg->smax_value,
		reg->var_off.value | (reg->var_off.mask & S64_MAX));
reg->umin_value = max(reg->umin_value, reg->var_off.value);
reg->umax_value = min(reg->umax_value,
		      reg->var_off.value | reg->var_off.mask);
```

读法：smin 取"符号位最坏=1（负）、其余位取已知下界"；umax 取"所有未知位全填 1"。**从位知识直接榨出最紧界**。

**第二层：signed ↔ unsigned 互推**（`__reg64_deduce_bounds`）——注释原话（:2112-2116 附近）：*"Learn sign from signed bounds. If we cannot cross the sign boundary, then signed and unsigned bounds are the same"*——例如 `-3 ≤ x ≤ -1`（signed）⇒ `0xff...fd ≤ x ≤ 0xff...ff`（unsigned）；跨界时只推安全侧。

**第三层：总装 `reg_bounds_sync`（:2178）**——还要把 64 位视图与 32 位子寄存器视图（ALU32 指令的世界）互相校准：`var64_off = tnum_intersect(reg->var_off, ...)`（:2168-2171）。

★**架构语义**：`var_off` 是唯一权威（single source of truth），四个界是**可重建的缓存**——任何 ALU 指令改写 var_off 后调 `reg_bounds_sync` 重算。这与 scheduler 的 rq 时钟三兄弟（[02 章](../../scheduler/docs/02-核心数据结构.md)）同构：一个真相源 + 派生视图。

**哪些运算有 tnum 传播**——`adjust_scalar_min_max_vals`（:12823）的分派表（:12900-12971，已核对）：

| 指令 | var_off 处理 | 界处理 |
| :--- | :--- | :--- |
| ADD/SUB/MUL/AND/OR/XOR | `tnum_add/sub/mul/and/or/xor` 精确传播 | 同步算界 |
| LSH/RSH/ARSH | 移位数已知才传播（越界 UB → unknown） | bounds 族 |
| **DIV/MOD（default 分支 :12969-12971）** | **`mark_reg_unknown`——整个 tnum 擦成 {0,-1}** | 全未知 |

除法没有位级封闭公式（思考题 6），verifier 的选择是**最保守的全面放弃**——代价是指令序列中一次 DIV 之后，前面辛苦积累的全部位知识清零。

## 六、合并与比较：状态树的语言

```c
/* tnum_intersect（tnum.c:144）：两个约束的合取 */
struct tnum tnum_intersect(struct tnum a, struct tnum b)
{  /* 头注释（tnum.h:52）："Return a tnum representing numbers
      satisfying both @a and @b" */
	v = a.value & b.value;
	m = a.mask & b.mask;
	return TNUM(v, m);
}
```

★**算术上不健全的用法**：若 a 某位已知 0、b 同位已知 1，交出"该位已知 0"——但 `{0}` ∩ `{1}` 应为空集！verifier 仍用它（:13823 MOV 语义合并、:2168 reg_bounds_sync 的 64/32 视图合取），因为**两条路径必有一条不可达**（先前分支已判定互斥）——上下文排除了空集情形。工具语义 ≠ 代数语义的典型案例。

**tnum_in（:167）**：子集检查 `b ⊆ a`——先 `b.mask & ~a.mask` 非零则 false（b 有 a 没有的未知位），再比 value。头注释的**陷阱警告**（tnum.h:83-87 原话）：*"using tnum_range() as @a requires extra cautions as tnum_in() may* `tnum_in(tnum_range(0, 2), tnum_const(3)) == true`"——range(0,2) 包含幻影值 3，子集检查被骗。verifier 的用法（:9264/:14530/:14627）：spin_lock/timer/kptr 的"偏移必须落在范围内"判定——正是 10 章 `"R%d doesn't have constant offset"` 报错族的判定内核。

## 七、ALU32 三兄弟与观测（tnum.c:201-213）

```c
struct tnum tnum_subreg(struct tnum a)          /* :201 取低 32 位视图 */
{ return tnum_cast(a, 4); }
struct tnum tnum_clear_subreg(struct tnum a)    /* :206 高 32 位归零 */
{ return tnum_lshift(tnum_rshift(a, 32), 32); }
struct tnum tnum_const_subreg(struct tnum a, u32 value)  /* :211 低 32 写常量 */
{ return tnum_or(tnum_clear_subreg(a), tnum_const(value)); }
```

BPF_ALU32 指令把寄存器低半当完整目标——三兄弟支撑 32 位世界的"读写分离"（高半保留 64 位知识、低半按 32 位语义更新）。

**观测**：verifier 日志里每个寄存器状态中的 `var_off` 就是 `tnum_strn`（tnum.c:175-180）的产物——格式 `"(%#llx; %#llx)"`（value; mask 十六进制）；`tnum_sbin`（:181-199）输出 64 位二进制串，未知位显示 `x`：

```
R1_w=invP(id=0, var_off=(0x0; 0xffffffff), ...)  ← tnum_strn
R2=inv(var_off=0x1; 0xfffffffffffffffe)          ← 低 1 位确定，其余未知
bpftool prog load 失败时：完整 var_off 演化链即
  "这条路上验证器对你的值知道多少"的全过程录像
```

## 八、思考题

1. `tnum_add` 的 `chi = sigma ^ sv` 为什么恰好是"携带集合"？用 `{1,0}+{1,0}`（两未知最低位）手推：chi 能捕获"未知+未知"和"未知+已知"产生的进位吗？（提示：sm 把两个 mask 当作最坏全 1 求和。）
2. `tnum_range(0,2)` 返回 `{0, mask=3}`（含幻影 3）——如果改成精确区间表示（min/max 对），代价是什么？（对比：位级运算 O(1) vs 区间运算的笛卡尔积爆炸。）
3. AND/OR/XOR 三公式里只有 AND 的 mask 计算用了 `alpha & beta & ~v` 三项——OR 的 mask 为什么不含进位分析？（提示：OR 的位独立性；AND 的"确定 0 消毒"需要同时看两边的上界。）
4. `tnum_intersect` 在空集情形返回非空 tnum——verifier 为什么敢用？（从 :13823 的 MOV 语境反推：两条来源路径的互斥性由谁保证？）
5. `__update_reg64_bounds` 里 smin 的公式为什么是 `value | (mask & S64_MIN)` 而不是 `value | mask`？（提示：符号位未知时最坏是负——但其余未知位要取最小值 0 才是最负。）
6. 假设写一个 `tnum_div`（除法）——为什么 6.6 没有实现？（提示：商的位级传播没有封闭公式；BPF 指令集 ALU 有 DIV——verifier 的处理见 §五分派表的 default 分支：`mark_reg_unknown` 全面放弃，积累的位知识清零。）

---

> 回看：[10-eBPF 验证器入门](10-eBPF验证器入门.md)（tnum 的消费者）｜[09-debugfs 全景索引](09-debugfs全景索引.md)｜[scheduler/14-调度器统计纵深](../../scheduler/docs/14-调度器统计纵深schedstat.md)（另一套"账本代数"的对照）｜[scheduler/02-核心数据结构](../../scheduler/docs/02-核心数据结构.md)（时钟三兄弟 vs var_off 单一真相源）
