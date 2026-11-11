# eBPF 验证器入门：verifier 视角的 map 与程序约束（Linux 6.6）

> 阅读目标：从 verifier（`kernel/bpf/verifier.c`，6.6 中 ~2 万行）的实现级视角理解——`bpf_check()` 静态分析器如何**穷举所有路径**证明程序安全、状态树与 `branches` 计数的回溯协议、**复杂度上限**的三层设计（路径爆炸防护）、循环的真实政策（有界循环对 CAP_BPF 开放）、以及开发者最常遇到的 verifier 拒绝消息的**真实原文**（本章所有报错字符串均逐字核对自 6.6 源码，无一杜撰）。前置：[06-perf 与 BPF 观测](06-perf与BPF观测.md)（eBPF 使用面）。

---

## 一、验证器是什么：加载期静态分析

```
bpf(BPF_PROG_LOAD) → bpf_check()（verifier.c:19659）
  "bpf_check() is a static code analyzer that walks eBPF program
   instruction by instruction and updates register/stack state.
   All paths of conditional branches are analyzed until 'bpf_exit' insn."
   ——verifier.c:44-46 注释原样

两遍结构（:44-53 注释）：
  第一遍 DFS：确认程序是 DAG——拒绝超长（> BPF_MAXINSNS）、
    拒绝 back-edge 循环（对非特权）、拒绝不可达指令、拒绝畸形跳转
  第二遍：从第一条指令做全路径下降，逐条更新寄存器/栈状态

验证产物：一段被证明"不可能越界读写、不可能泄露内核指针、
  不可能死循环"的指令集 → 经 JIT 后方可运行
★ eBPF 与内核模块(KO)的本质区别：KO 靠签名/信任，
  eBPF 靠数学证明——证明不了就拒绝（宁可误杀）
```

## 二、核心结构：env 与 state（include/linux/bpf_verifier.h，已核对 6.6）

```c
struct bpf_verifier_env {           /* bpf_verifier.h:573 */
	u32 insn_idx;	u32 prev_insn_idx;
	struct bpf_prog *prog;             /* 被验证的 eBPF 程序 */
	const struct bpf_verifier_ops *ops;
	struct bpf_verifier_stack_elem *head; /* 待处理状态栈 */
	int stack_size;
	struct bpf_verifier_state *cur_state;
	struct bpf_verifier_state_list **explored_states; /* 剪枝优化 */
	struct bpf_map *used_maps[MAX_USED_MAPS];  /* ★上限 64 个 map（:495） */
	struct btf_mod_pair used_btfs[MAX_USED_BTFS];      /* :586，上限 64 */
	u32 used_map_cnt;	u32 used_btf_cnt;
	u32 id_gen;         /* 生成唯一 reg ID */
	bool allow_ptr_leaks, bpf_capable;   /* 特权判定贯穿全程 */
	...
};

struct bpf_verifier_state {         /* :319 */
	struct bpf_func_state *frame[MAX_CALL_FRAMES]; /* ★调用栈，最多 8 帧（:316） */
	struct bpf_verifier_state *parent;  /* ★状态树的父指针 */
	u32 branches;   /* 未探索分支数，见下方协议 */
	u32 insn_idx;	u32 curframe;
	struct bpf_active_lock active_lock; /* bpf_spin_lock 持有状态 */
	bool speculative;
	...
};
```

**`branches` 回溯协议**（:323-355 注释原样含树形图）：if 分叉时 push 一个子状态到栈（branches=1）、fallthrough 走另一个（branches=1）、父状态记 branches=2；到达 `bpf_exit` 时 `update_branch_counts()` 沿 `parent` 链递减——**全树 branches 归零 = 所有路径验证通过**。`pop_stack()` 后从栈顶恢复继续探索下一个分叉。

## 三、验证循环与剪枝（已核对 6.6）

```
bpf_check → do_check_common(:19018) → do_check(:16430)：
  逐条指令模拟：
    regs[] 状态追踪——R0-R10 各自的类型+范围+id
    指令语义驱动类型迁移（例：BPF_MOV64_REG(R1,R5) 则 R5 类型拷给 R1，
    :58-60 注释原样）
  is_state_visited(:16097)：剪枝——
    当前状态与已探索状态"等价"（states_equal :15807）→ 该路径已被
    证明安全，直接跳过 ★这是 verifier 不指数爆炸的关键
  states_maybe_looping(:15992)：快速预检（比 states_equal 便宜），
    命中即进入死循环判定
  pop_stack(:1797) / push_stack(:1826)：DFS 回溯
```

**对源发现（2026-11-12 轮）**：`verifier.c:56` 注释仍写 *"the length of the analysis is limited to 64k insn"*，但 6.6 实际常量 `BPF_COMPLEXITY_LIMIT_INSNS` 已是 **1000000**（include/linux/bpf.h:1723，定义处注释原话 *"yes. 1M insns"*）——**头注释滞后于代码演进**，读源码时以常量为准。

## 四、复杂度上限：路径爆炸的防护（已核对 6.6）

| 限制 | 值 | 出处 | 含义 |
| :--- | :--- | :--- | :--- |
| `BPF_COMPLEXITY_LIMIT_JMP_SEQ` | 8192 | verifier.c:183 | 跳转序列长度上限 |
| `BPF_COMPLEXITY_LIMIT_STATES` | 64 | verifier.c:184 | 同时待处理状态上限 |
| `BPF_COMPLEXITY_LIMIT_INSNS` | 1000000 | include/linux/bpf.h:1723 | 总处理指令数上限 |
| `MAX_USED_MAPS` | 64 | bpf_verifier.h:495 | 单程序可访问 map 数 |
| `MAX_USED_BTFS` | 64 | bpf_verifier.h:496 | 单程序可关联 BTF 数 |
| `MAX_CALL_FRAMES` | 8 | bpf_verifier.h:316 | 函数调用深度 |

超限的处理在 `do_check()` 主循环（:16455）：

```c
if (++env->insn_processed > BPF_COMPLEXITY_LIMIT_INSNS) {
	verbose(env, "BPF program is too large. Processed %d insn\n",
		env->insn_processed);
	return -E2BIG;
}
```

这不是 bug，是 **verifier 自身的 DoS 防护**：恶意构造的上千个 if 分叉产生指数级路径数，必须在多项式时间内截止。同理 `check_max_stack_depth(:5710)` 递归核算子程序调用栈深（bpf_check :19779 调用）。

## 五、开发者视角：真实报错消息与机制（全部逐字核对）

```
"R%d invalid mem access '%s'"（:6514）
  —— 寄存器类型不满足访问要求：调 helper 后未判返回值、
     拿 SCALAR_VALUE 当指针用等。类型系统见第六节。

"invalid read from stack off %d+%d size %d"（:4644）
  —— 栈槽追踪：struct bpf_func_state 的 stack[].slot_type[8]
     按字节标记（bpf_verifier.h:231-234）：
     STACK_INVALID（未写）/ STACK_SPILL（寄存器溢出）
     STACK_MISC（写过数据）/ STACK_ZERO（写过常量 0）
     读未初始化槽即拒绝

"invalid unbounded variable-offset stack R%d"（:6401）
"invalid variable-offset stack R%d var_off=%s size=%d"（:6424）
  —— 指针算术：变量偏移不可证明落在界内。范围追踪靠
     umin/umax/smin/smax + var_off（tnum 三值逻辑，lib/tnum）

"invalid access to map value, value_size=%d off=%d size=%d"（:4874）
  —— map 访问越界（key 侧 :4870，只读 map 写侧 :4842
     "write into map forbidden"）

"R%d min value is negative, either use unsigned index or do a
 if (index >=0) check."（:4916）
  —— 经典新手错：有符号索引范围包含负值 → 越界不可排除

"back-edge from insn %d to %d"（:14765）
  —— 见下节循环政策
```

## 六、类型系统：enum bpf_reg_type 与 PTR_MAYBE_NULL（6.6 现状）

基础类型（`enum bpf_reg_type`，**定义在 include/linux/bpf.h**——bpf_verifier.h 顶部 `#include <linux/bpf.h> /* for enum bpf_reg_type */` 注明）：

```c
enum bpf_reg_type {
	NOT_INIT = 0,        /* 没写过 */
	SCALAR_VALUE,        /* 非指针 */
	PTR_TO_CTX,          /* 指向 bpf_context */
	CONST_PTR_TO_MAP,    /* 指向 struct bpf_map（map 指针本身不可算术） */
	PTR_TO_MAP_VALUE,    /* 指向 map 元素值 */
	PTR_TO_MAP_KEY,
	PTR_TO_STACK,        /* R10+偏移 */
	PTR_TO_PACKET_META, PTR_TO_PACKET, PTR_TO_PACKET_END, /* skb 族 */
	... PTR_TO_SOCKET / PTR_TO_TCP_SOCK / PTR_TO_BTF_ID ...
};
```

**6.6 关键演进**：`*_OR_NULL` 不再是独立枚举值——NULL 性改为类型标志位（v5.17 引入 `enum bpf_type_flag`，include/linux/bpf.h:572 起；v5.16 尚无、v5.17 出现 18 处引用——本仓 linux-stable 双标签实证）：

```c
PTR_MAYBE_NULL = BIT(0 + BPF_BASE_TYPE_BITS),   /* bpf.h:575 */
/* helper 原型侧组合使用： */
RET_PTR_TO_MAP_VALUE_OR_NULL = PTR_MAYBE_NULL | RET_PTR_TO_MAP_VALUE, /* :754 */
```

因此 `bpf_map_lookup_elem` 的返回值是 `PTR_TO_MAP_VALUE | PTR_MAYBE_NULL`——**必须显式 `if (!ptr)` 判空后**，verifier 才把标志位摘掉、允许解引用。

## 七、循环的真实政策：有界循环对 CAP_BPF 开放

`verifier.c:14760` 的 DFS 白名单揭示了确切政策（已核对 6.6）：

```c
} else if ((insn_state[w] & 0xF0) == DISCOVERED) {
	if (loop_ok && env->bpf_capable)
		return DONE_EXPLORING;      /* ★CAP_BPF：循环放行 */
	verbose_linfo(env, t, "%d: ", t);
	verbose_linfo(env, w, "%d: ", w);
	verbose(env, "back-edge from insn %d to %d\n", t, w);
	return -EINVAL;                 /* 非特权：拒绝 */
}
```

- **有界循环**（bounded loop）自 5.3 进入主线——本仓双标签实证：v5.2 的该处是无条件 `-EINVAL`，v5.3 起出现放行条件 `if (loop_ok && env->allow_ptr_leaks) return 0;`（特权门控后来从 `allow_ptr_leaks` 演变为 6.6 的 `env->bpf_capable`）；6.6 中 CAP_BPF 的程序可以写循环——verifier 追踪可证明的迭代上限（:7562 注释称之为 *"bounded loop simulation logic"*）
- **非特权程序**遇到 back-edge 一律 `-EINVAL`
- 另一条无循环写法：`bpf_loop()` helper（:9080 原型注释），verifier 会把小循环计数内联展开（`inline_bpf_loop`，:18852，注释注明是 *"inlined version of bpf_iter.c:bpf_loop"*）
- 兜底：即使放行的循环，`is_state_visited` 若发现状态完全复现（`states_maybe_looping`）即判死循环，`BPF_COMPLEXITY_LIMIT_INSNS` 最终截止

## 八、map 约束：verifier 视角

```
env->used_maps[] 记录程序触碰的全部 map（≤64）：
  · map_value 访问：PTR_TO_MAP_VALUE + 已证明界内的偏移
    （固定偏移直接过；变量偏移走范围追踪，:4916 那条报错常在此）
  · lookup 判空：PTR_MAYBE_NULL 标志（第六节）
  · 只读 map 写入：:4842 "write into map forbidden"
  · bpf_spin_lock/bpf_timer/kptr：必须固定偏移——
    "R%d doesn't have constant offset. bpf_spin_lock has to be at
     the constant offset"（:7189，timer :7263，kptr :7300）
  · 引用计数：acquire_reference_state/release_reference
    （bpf_verifier_env 前向声明 :194-196），程序退出时漏 release
    → 引用泄漏拒绝
  · BTF 依赖（used_btfs，bpf_verifier.h:586）：PTR_TO_BTF_ID 的
    类型核对。注意：libbpf 的 CO-RE 重定位发生在用户态，
    verifier 只看到重定后的真实类型——"verifier 不认识 relo"
```

## 九、观测与实战

```
bpftool prog load x.o /sys/fs/bpf/x     # 验证失败时打印完整日志
  日志逐条列出指令与其后的 regs/stack 状态（verbose() 产物）
/proc/sys/kernel/unprivileged_bpf_disabled  # 非特权 eBPF 总开关

程序长度上限（syscall.c:2586-2587，已核对 6.6）：
  attr->insn_cnt > (bpf_capable() ? BPF_COMPLEXITY_LIMIT_INSNS
                                  : BPF_MAXINSNS)
  ——特权 100 万条；非特权 4096 条（BPF_MAXINSNS，
    include/uapi/linux/bpf_common.h:54）。同一个 1M 常量兼任
    验证分析步数预算（:16455）——两层防护共用一个值

性能：verifier 是 eBPF 加载的主要延迟来源——多分支程序可轻易
  触达百万步分析上限（分支数指数于 if 数，与指令数是两回事）
定位工具：verifier 日志从出错指令反向引用源行号（verbose_linfo）
```

## 十、思考题

1. `branches` 的 2→1→0 递减为什么必须沿 `parent` 链做，而不能在 push/pop 时局部维护？（提示：DFS 回溯后栈顶状态的计数不变式。）
2. `is_state_visited` 剪枝用"等价"（states_equal）而非"相等"——若两个状态寄存器范围一个更宽一个更窄，何时可安全跳过？（提示：宽松状态已证明安全 ⟹ 严格状态也安全，方向不能反。）
3. `BPF_COMPLEXITY_LIMIT_INSNS=1M` 兜底与 `states_maybe_looping` 死循环判定是什么关系？为什么两个都需要？（提示：前者是资源截止，后者是语义判定。）
4. `PTR_MAYBE_NULL` 做成标志位而非独立枚举值，带来了什么正交性收益？（对照 6.0 之前 `PTR_TO_MAP_VALUE_OR_NULL`×`PTR_TO_SOCKET_OR_NULL`×… 的组合爆炸。）
5. 非特权程序为什么连有界循环都不给？（从 `loop_ok && env->bpf_capable` 反推非特权的攻击面。）
6. verifier 证明"安全"的谓词里为什么包含"不泄露内核指针"？（提示：allow_ptr_leaks 与spectre 缓解 bypass_spec_v1/v4 的关系。）

---

> 回看：[06-perf 与 BPF 观测](06-perf与BPF观测.md)｜[07-procfs 观测索引](07-procfs观测索引.md)｜[syscalls/07-seccomp 的 cBPF 校验](../../syscalls/docs/07-seccomp过滤器与BPF验证.md)｜[networking/07-XDP 数据面](../../networking/docs/07-XDP与eBPF数据面.md)
