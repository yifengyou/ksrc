# CALL_DEPTH_ACCOUNT：retbleed 缓解的调用深度追踪（Linux 6.6）

> 阅读目标：吃透 ftrace 汇编中反复出现的 `CALL_DEPTH_ACCOUNT` 宏（[18 章](18-ftrace汇编层DIRECT分派.md)已提及）——它是 Intel Skylake retbleed（CVE-2022-29900/29901）的**深度追踪缓解**：per-CPU 的 `X86_call_depth` 计数器（`sarq $5` 的算术移位技巧——每 32 次 call 计一次深度），`RET` 翻译为 `jmp __x86_return_skl`（**避免预测器训练**），深度溢出时 `RESET_CALL_DEPTH` 清零并填充 RSB。理解这个**观测基础设施里的安全基础设施**（ftrace 汇编为什么要插 CALL_DEPTH_ACCOUNT）。全部对照 v6.6（arch/x86/include/asm/nospec-branch.h:270-390、arch/x86/kernel/callthunks.c）。前置：[18-ftrace 汇编层](18-ftrace汇编层DIRECT分派.md)。

---

## 一、问题：Intel Skylake 的 RET 预测漏洞

```
RET 指令依赖 RSB（Return Stack Buffer）预测返回地址
Skylake 系列：RSB 预测器可被"预训练"（retbleed——2022 年披露）
  攻击者用大量 call+ret 在 RSB 中植入预测
  → 被害进程的 RET 跳到错误地址 → 副信道读取内核数据

缓解策略对比：
  ① jmp thunk（AMD unret）：把 RET 翻译为 jmp——弃用 RSB
     → Skylake 上 jmp 也走预测器——不够
  ② call depth tracking（Intel skl）：只追踪"深度溢出"——
     正常路径零开销，溢出时一次性处理
```

## 二、CALL_DEPTH_ACCOUNT 宏（nospec-branch.h:325-330）

```asm
.macro CALL_DEPTH_ACCOUNT
#ifdef CONFIG_CALL_DEPTH_TRACKING
	ALTERNATIVE "",                         \
		    __stringify(ASM_INCREMENT_CALL_DEPTH), \
		    X86_FEATURE_CALL_DEPTH
#endif
.endm
```

★**运行时可裁剪**：`ALTERNATIVE` 的 runtime patching——CPU 无 `X86_FEATURE_CALL_DEPTH` 时展开为空（零开销）；有此特征时替换为 `ASM_INCREMENT_CALL_DEPTH`。

## 三、计数器：X86_call_depth 与 sarq 技巧（:96-99）

```asm
#define INCREMENT_CALL_DEPTH                       \
	sarq $5, %gs:pcpu_hot + X86_call_depth;   \
	CALL_THUNKS_DEBUG_INC_CALLS

#define ASM_INCREMENT_CALL_DEPTH                  \
	sarq $5, PER_CPU_VAR(pcpu_hot + X86_call_depth);  \
	CALL_THUNKS_DEBUG_INC_CALLS
```

★**`sarq $5` 的算术移位**：每次 call 后对 per-CPU 计数器做 `>>= 5`（即 `/32`）：
- 初始值设为 `-1 << (5 × max_depth)` 的大负数
- 每次 `sarq $5` 把计数器向右移 5 位
- **溢出到 -1（全 1）时 = 深度耗尽**——32 次 call 耗 5 位

这比 `inc` + 比较**少一条指令**（一条 sarq 同时做计数和比较）——观测/安全路径的极致微优化。

## 四、三组配套宏（:80-100）

```asm
/* 清零（深度重置——入口/返回到用户态时）*/
#define RESET_CALL_DEPTH                          \
	movl    $0, PER_CPU_VAR(pcpu_hot + X86_call_depth);

/* 重置并预置额度 */
#define CREDIT_CALL_DEPTH                         \
	movq    $-1, PER_CPU_VAR(pcpu_hot + X86_call_depth);

/* 每次 CALL 后递增（sarq 的移位即递增）*/
#define INCREMENT_CALL_DEPTH                      \
	sarq $5, %gs:pcpu_hot + X86_call_depth;
```

## 五、ftrace 汇编中的插入点（回扣 [18 章](18-ftrace汇编层DIRECT分派.md)）

```asm
/* arch/x86/kernel/ftrace_64.S */
SYM_FUNC_START(ftrace_caller)
	/* save_mcount_regs fills in first two parameters */
	save_mcount_regs

	CALL_DEPTH_ACCOUNT        /* ★ftrace_caller 里的深度追踪 */

	...（ftrace_call 主体）...

	restore_mcount_regs
	RET
```

★**为什么 ftrace 也要插**：ftrace_caller 里有一个 `call ftrace_stub`——这会推进调用深度。如果不追踪，ftrace 启用后的深度计数会**少计**——溢出检测失准。`CALL_DEPTH_ACCOUNT` 出现在所有含 call 的手写汇编路径（ftrace_64.S 的 :160、entry_64.S 的 :298/:1105 等）。

## 六、UNTRAIN_RET 的三选一（:291-300）

```asm
.macro UNTRAIN_RET
#if defined(CONFIG_CPU_UNRET_ENTRY) || defined(CONFIG_CPU_IBPB_ENTRY) || \
	defined(CONFIG_CALL_DEPTH_TRACKING) || defined(CONFIG_CPU_SRSO)
	VALIDATE_UNRET_END
	ALTERNATIVE_3 "",                                    \
		      CALL_UNTRAIN_RET, X86_FEATURE_UNRET,     \  /* AMD unret */
		      "call entry_ibpb", X86_FEATURE_ENTRY_IBPB, \  /* IBPB */
		      __stringify(RESET_CALL_DEPTH), X86_FEATURE_CALL_DEPTH
#endif  /* Intel skl 深度清零 */
.endm
```

★**三选一的 runtime 分派**：同一入口点，根据 CPU 特征运行时选择（ALTERNATIVE 的 text_poke 替换）——AMD 用 unret、Intel 用 IBPB 或 call depth 清零。

## 七、__x86_return_skl 与 RSB 填充

```
Intel Skylake 的 return thunk（__x86_return_skl）：
  不直接 RET——翻译为：
    ① 深度计数 sarq $5（CALL_DEPTH_ACCOUNT）
    ② RSB stuffing（填充 RSB 使预测器不可训练）
    ③ 最后才 RET（此时 RSB 已清——预测必须走正确路径）
```

## 八、观测实战

```bash
# 1) 当前 CPU 的缓解状态
cat /sys/kernel/debug/x86/retbleed* 2>/dev/null
cat /sys/kernel/security/x86/ibrs_enabled 2>/dev/null

# 2) dmesg 的缓解选择
dmesg | grep -i "retbleed\|call_depth\|skl"

# 3) callthunks 的调试
#   cmdline 加 debug-callthunks 可看到 patch 详情

# 4) 性能影响
#   CALL_DEPTH_ACCOUNT 的 sarq 在有 CALL_DEPTH 特征时 ~1 cycle
#   每次 call/ret 各加一条 → 微基准 ~3-5% 开销
```

## 九、思考题

1. `sarq $5`（>>32）——为什么 5 而不是 1？（提示：32 次才计一次——计数器位宽 64，每次 sarq 消耗 5 位 → 12 级深度 = 60 位 + 初始 4 位余量——用位宽换精确度。）
2. `ALTERNATIVE` 展开——没有 CALL_DEPTH 特征时 CALL_DEPTH_ACCOUNT 为空——这与 static_key 的 nop 化有何异同？（提示：ALTERNATIVE 是 boot 期一次性 text_poke（不可逆）；static_key 是运行时可翻转。）
3. ftrace 汇编中的 CALL_DEPTH_ACCOUNT 放在 `save_mcount_regs` **之后**——为什么不在之前？（提示：save_mcount_regs 里没有 call 指令——插太早白白计一次。）
4. `RESET_CALL_DEPTH` 清零而 `CREDIT_CALL_DEPTH` 写 -1——何时用哪个？（提示：RESET 用于"已知深度归零"（用户态入口）；CREDIT 用于"预支新额度"（分派新任务）。）
5. retbleed 影响的是 RET 预测——call depth tracking 为什么有效？（提示：Skylake 的 RSB 溢出时回退到 BTB 预测——深度溢出时主动填充 RSB → BTB 路径被堵。）
6. 这种"每条 call/ret 加一条指令"的方案——为什么不能在编译器层做？（提示：需要**per-CPU**计数——编译器不知道 per-CPU 寻址；且手写汇编（ftrace/entry）也需覆盖。）

---

> 回看：[18-ftrace 汇编层](18-ftrace汇编层DIRECT分派.md)（CALL_DEPTH_ACCOUNT 的首次出现）｜[19-text_poke_bp](19-text_poke_bp家族.md)（ALTERNATIVE 的底层机制——同样是 text_poke）｜[21-sync_core](21-sync_core.md)（RSB 重平衡——Spectre-RSB 的另一缓解）｜[trace/README](README.md)（观测基础设施的完整族谱）
