# ALTERNATIVE 深挖：boot 期的批量 text_poke（Linux 6.6）

> 阅读目标：吃透 x86 运行时改码的**静态声明层**——`struct alt_instr` 的 **ft_flags 双域**（16 位 cpuid + 16 位 flags——头注释 "[31... flags ...16][15... CPUID feature bit ...0]...can be extended"）；★汇编标号体系（661-665）与**负数 .skip 填充戏法**（oldinstr 按最长 replacement 自垫 NOP）；`ALT_NOT` 反相补丁与 `ALTERNATIVE_TERNARY` 借 `X86_FEATURE_ALWAYS` 实现；★`apply_alternatives` 主循环（双取反 XOR 判定式 / insn_buff 的 0x90 垫 / `apply_relocation` 的 RIP 重定位 / KASAN 禁用的 LA57 假阳注释 / 扫描序敏感性）；`optimize_nops` 的"跳转+NOP 串压成单 NOP"与 noinline 刷 I$ 论证；boot 五步序列与 **paravirt→fineibt→retpoline→alternatives 的次序协议**；模块 finalize 链。全部对照 v6.6 当轮核对。前置：[19-text_poke_bp](19-text_poke_bp家族.md)｜[23-Spectre 缓解矩阵](23-Spectre缓解矩阵.md)。

---

## 一、分层：声明层 → 决策层 → 执行层

```
声明层（编译期）：宏在 .altinstructions 表里落一条 alt_instr
                 （旧指令位置/替换体位置/特征位/长度），代码本体未动
决策层（boot 期）：apply_alternatives 扫表——boot_cpu_has(bit) ?
                 + ALT_FLAG_NOT 的反相语义 → 补丁与否
执行层：text_poke_early（boot 期专用——其他 CPU 未跑，无需 stop_machine）
        ──运行期的改码走 [19 章] 的 text_poke_bp（int3 断点法）

邻居关系：static_key/jump_label 用自己的 .jump_table + text_poke_bp
        （可逆、运行期翻转）；static_call 用 .static_call_sites。
        ALTERNATIVE 是三者中唯一 **boot 期一次性、不可逆** 的。
```

## 二、数据结构与宏族（include/asm/alternative.h）

### 2.1 struct alt_instr（:73-86）

```c
struct alt_instr {
	s32 instr_offset;	/* original instruction */
	s32 repl_offset;	/* offset to replacement instruction */
	union {
		struct {
			u32 cpuid: 16;	/* CPUID bit set for replacement */
			u32 flags: 16;	/* patching control flags */
		};
		u32 ft_flags;
	};
	u8  instrlen;		/* length of original instruction */
	u8  replacementlen;	/* length of new instruction */
} __packed;
```

`ALT_FLAG_NOT (1 << 0)` / `ALT_NOT(feature)`（:11-12）——flags 高半域的第一位：**特征不存在时才补**（[23 章] CALL_DEPTH_ACCOUNT 的 ALTERNATIVE "" → ASM_INCREMENT 双态即此族）。

### 2.2 汇编标号体系与填充戏法（:158-198 附近）

```
661: ── oldinstr 开始 ──
     （OLDINSTR 的负数 .skip：
      ".skip -(((" alt_rlen(num) ")-(" alt_slen ")) > 0) * (" alt_rlen(num) ")"
      ★若 replacement 比 old 长，按差值垫 NOP——汇编期求 max！）
662: ── oldinstr 结束 ──
663: （end marker，alt_total_slen 的锚）
664/665: replacement 的起止（按编号 6641/6651、6642/6652 …支持三路）

两段落表：
  .altinstructions（"a"）← ALTINSTR_ENTRY（五元组）
  .altinstr_replacement（"ax"）← ALTINSTR_REPLACEMENT（真身，可执行）
```

### 2.3 变体族（:210-247）

| 宏 | 语义 | 手法 |
| :--- | :--- | :--- |
| `ALTERNATIVE(old, new, ft)` | 有特征→替换 | 单条目 |
| `ALTERNATIVE_2/3` | 多特征多替换体 | 多条目+`alt_max_short` 的汇编期 max 填充 |
| ★`ALTERNATIVE_TERNARY(old, ft, yes, no)` | "If @feature is set, patch in @newinstr_yes, otherwise @newinstr_no" | 借 `X86_FEATURE_ALWAYS`（cpufeatures.h:103 "Always-present feature"）给 no 分支兜底 |

## 三、★apply_alternatives 主循环（arch/x86/kernel/alternative.c:398-471）

```c
void __init_or_module noinline apply_alternatives(struct alt_instr *start,
						  struct alt_instr *end)
{
	kasan_disable_current();   /* LA57/KASAN 假阳注释（:407-415 原文）：
	    "KASAN becomes confused seeing partial LA57 conversion and
	     triggers a false-positive out-of-bound report" */

	for (a = start; a < end; a++) {
		/* ★判定式——双取反的 XOR：
		   特征存在 且 无 NOT 旗 → 补
		   特征不存在 且 有 NOT 旗 → 补（反相）
		   注释原文："Patch if either: - feature is present
		   - feature not present but ALT_FLAG_NOT is set to mean,
		     patch if feature is *NOT* present." */
		if (!boot_cpu_has(a->cpuid) == !(a->flags & ALT_FLAG_NOT)) {
			optimize_nops(instr, a->instrlen);   /* 不补也顺手压 NOP */
			continue;
		}

		memcpy(insn_buff, replacement, a->replacementlen);
		for (; insn_buff_sz < a->instrlen; insn_buff_sz++)
			insn_buff[insn_buff_sz] = 0x90;    /* 0x90 垫到等长 */
		apply_relocation(insn_buff, a->instrlen, instr,
				 replacement, a->replacementlen);  /* RIP 相对重定位 */
		text_poke_early(instr, insn_buff, insn_buff_sz);
	}
	kasan_enable_current();
}
```

三个工程细节：
- **扫描序敏感**（头注释 :417-424 原文）："The scan order should be from start to end. A later scanned alternative code can overwrite previously scanned alternative code. Some kernel functions (e.g. memcpy, memset, etc) use this order to patch code. **So be careful if you want to change the scan order**"——同一指令可有多层 alt（先补短的再补长的）
- **重定位**：replacement 里的 RIP 相对寻址在落地到新地址时需按差值修正（`apply_relocation`）
- **0x90 单字节 NOP** 垫到与 old 等长——表内 instrlen 是合同

## 四、optimize_nops：把"跳转+NOP 串"压成单 NOP（:205-254）

```
头注释原文："Optimize a sequence of NOPs, possibly preceded by an
 unconditional jump to the end of the NOP sequence into a single NOP."

机制：__optimize_nops 逐指令扫描——
  遇 JMP8/JMP32 → 记 target（短跳到 NOP 串尾的跳转）
  遇 NOP 串 → skip_nops 到串尾，若尾==target：
    整段（含跳转）用 add_nop 换成一条最优多字节 NOP
★ optimize_nops 的 noinline 有讲究——注释原文：
  "noinline" to cause control flow change and thus invalidate I$
  and cause refetch after modification."
```

不补的特征分支也调它（§三）——**没吃补丁的代码白捡一次 NOP 瘦身**（编译器的保守 NOP 串被换成最优编码）。

## 五、boot 序列与次序协议（alternative_instructions，:1566-1620）

```
① int3_selftest()               （先验 [19 章] 的 int3 机制可用）
② stop_nmi()                    注释原文："The patching is not fully atomic,
                                  so try to avoid local interruptions that
                                  might execute the to be patched code.
                                  Other CPUs are not running."
   ★MCE 特意不停（注释原文）："MCEs only happen when something got corrupted
                                  ...Ignoring it is worse than an unlikely
                                  patching race."（权衡：损坏比竞态严重）
③ paravirt_set_cap + apply_paravirt    （间接调用→直接调用）
④ __apply_fineibt               （kCFI/FineIBT 签名改写）
⑤ apply_retpolines              注释原文："must be done before alternatives
                                  since those can rewrite the retpoline thunks"
⑥ apply_returns → apply_alternatives → ...（各表依序清完）

次序协议的三步论证（:1597-1610 原文节选）："1. set (artificial) features
 depending on used paravirt functions which can later influence
 alternative patching  2. apply paravirt patching ...  3. apply
 alternative patching ... Doing paravirt patching after alternative
 patching would clobber the optimization of the custom code with a
 function call again."
```

## 六、模块：finalize 链（arch/x86/kernel/module.c:330-342）

```
module_finalize：retpolines → returns → alternatives
  （"patch .altinstructions" 原注释）→ callthunks
模块的 .altinstructions 段在加载时重跑同一 apply_alternatives——
同一引擎两种时机（boot 全量 / 模块增量）
```

## 七、消费家族（谁在用这套机制）

| 消费者 | 用法 | 详见 |
| :--- | :--- | :--- |
| Spectre 矩阵各模式 | retpoline/lfence/IBRS 的运行时选定 | [23 章](23-Spectre缓解矩阵.md) |
| CALL_DEPTH_ACCOUNT | 深度追踪的 ALTERNATIVE 双态 | [22 章](22-CALL_DEPTH_ACCOUNT.md) |
| paravirt | 间接调用→直接序列 | 本章 §五 |
| memcpy/memset 优化 | 多层 alt 的扫描序叠加 | §三 |
| ALTERNATIVE_TERNARY | yes/no 双路（如 LA57 相关） | §二 |

## 八、观测实战

```bash
# 1) dynamic debug 开关（alternative.c 的 DPRINTK/DUMP_BYTES 全挂 dyndbg）
echo 'module alternative +p' > /sys/kernel/debug/dynamic_debug/control
dmesg | grep -E 'alt table|feat:|final_insn'   # 逐条补丁决策与前后字节

# 2) 与二进制对照
objdump -d vmlinux | grep -A2 '<memcpy'      # 看 .skip 垫的 NOP 痕迹
readelf -S vmlinux | grep altinstr           # .altinstructions/.altinstr_replacement

# 3) boot 序列日志
dmesg | grep -iE 'alternative|int3 selftest|retpoline|FineIBT'

# 4) 模块加载时的补丁（同 DPRINTK 通道）
dmesg | grep 'alt table' | tail
```

## 九、思考题

1. 负数 `.skip` 表达式 `-(((rl - sl) > 0) * rl)`——它实现了什么函数？为什么必须汇编期算？（提示：max(0, rl-sl) 的填充量；表内 instrlen 必须是**最长替换体的长度**才能原地容纳，而链接器不做代数。）
2. 判定式 `!boot_cpu_has(cpuid) == !(flags & ALT_FLAG_NOT)` 为什么不用两个显式 if？（提示：XOR 归一——"特征在且非反相"与"特征不在且反相"是同一真值表；两分支的后续动作完全相同。）
3. boot 期 text_poke_early 免 stop_machine 的前提是什么？谁在破坏这个前提？（提示："Other CPUs are not running"——SMP bringup 前的全窗口；NMI 已关而 MCE 特意留——损坏风险 > 竞态风险的价值排序。）
4. paravirt 必须先于 alternatives 的次序约束，反过来会发生什么？（提示：头注释 "clobber the optimization"——alternative 已把调用展开成内联序列，paravirt 再把（已不存在的）调用点换成直接调用=破坏。）
5. optimize_nops 连"没吃补丁"的分支也要跑——收益在哪？（提示：编译器为 alt 生成的 oldinstr 常带保守 NOP 串（等长垫）；压成单条最优 NOP 省取指带宽——Spectre 缓解时代 NOP 密度暴涨，这项收益被放大。）
6. ALTERNATIVE 的"不可逆"与 static_key 的"可逆"各自适合什么场景？（提示：CPU 特征永不消失→不可逆合法；tracepoint/BPF 尾调的开关要翻来翻去→可逆必须（jump_label 的表+text_poke_bp）。）

---

> 回看：[19-text_poke_bp](19-text_poke_bp家族.md)（执行层的 int3 断点法——运行期兄弟机制）｜[23-Spectre 缓解矩阵](23-Spectre缓解矩阵.md)（最大消费家族）｜[22-CALL_DEPTH_ACCOUNT](22-CALL_DEPTH_ACCOUNT.md)（ALTERNATIVE 双态的典型用户）｜[20-static_call](20-static_call.md)（邻居机制：调用点重定向）｜[trace/README](../README.md)（分册导读）
