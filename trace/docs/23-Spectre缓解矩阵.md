# Spectre 缓解矩阵：x86 推测执行防御全景（Linux 6.6）

> 阅读目标：把散在 [19-text_poke](19-text_poke_bp家族.md)/[21-sync_core](21-sync_core.md)/[22-CALL_DEPTH_ACCOUNT](22-CALL_DEPTH_ACCOUNT.md) 的安全线收进**一张全景矩阵**——**13 个漏洞 sysfs 文件**（cpu.c:583-598 总表）；spectre_v2 的**七种模式**（NONE/RETPOLINE/LFENCE/EIBRS/EIBRS_LFENCE/EIBRS_RETPOLINE/IBRS——nospec-branch.h:462-470）与 **12 个 cmdline 选项**（bugs.c:1367-1382 原表）；retbleed **六法**（含 Stuffing）与 SRSO **五策**（safe-ret 默认——"Mitigation: safe RET"）；GDS 的 microcode/AVX-disable 双路；cpufeatures 位系（RETPOLINE/RETHUNK/UNRET/ENTRY_IBPB/CALL_DEPTH/SRSO——cpufeatures.h:297-313）；用户态 prctl（PR_SPEC_INDIRECT_BRANCH/PR_SPEC_STORE_BYPASS）。全部对照 v6.6 当轮核对。前置：[22-CALL_DEPTH_ACCOUNT](22-CALL_DEPTH_ACCOUNT.md)。

---

## 一、问题家族的共性

```
推测执行侧信道的四步剧本：
  攻击者训练预测器 → 受害进程的推测路径吃了预测 →
  推测执行触碰秘密并留下微架构痕迹（cache/端口）→ 侧信道读出

防御的分层（对应本章各节）：
  ①预测器换法：retpoline/eIBRS（间接分支不走预测器）
  ②预测器消毒：IBPB/UNRET/SRSO safe-ret（训练过就清洗）
  ③RSB 管理：RETHUNK/STUFF/call-depth（深度追踪+填充）
  ④缓冲清洗：VERW（MDS/GDS——出口清缓冲区）
  ⑤调度隔离：L1D flush/SMT 关（核边界类）
```

## 二、观测基准：13 个漏洞文件（drivers/base/cpu.c:583-598）

```
/sys/devices/system/cpu/vulnerabilities/
  meltdown  spectre_v1  spectre_v2  spec_store_bypass  l1tf  mds
  tsx_async_abort  itlb_multihit  srbds  mmio_stale_data
  retbleed  spec_rstack_overflow  gather_data_sampling
```

每个文件输出三态之一：`Not affected` / `Vulnerable[: 补充]` / `Mitigation: <手法>`。**这张表就是矩阵的行索引**——下面各节逐行展开手法。

## 三、spectre_v2：七模式与十二选项

### 3.1 模式枚举（include/asm/nospec-branch.h:462-470）

```c
enum spectre_v2_mitigation {
	SPECTRE_V2_NONE,	SPECTRE_V2_RETPOLINE,	SPECTRE_V2_LFENCE,
	SPECTRE_V2_EIBRS,	SPECTRE_V2_EIBRS_RETPOLINE,
	SPECTRE_V2_EIBRS_LFENCE,	SPECTRE_V2_IBRS,
};
```

### 3.2 sysfs 字符串表（bugs.c:1355-1362 原表）

```
NONE            → "Vulnerable"
RETPOLINE       → "Mitigation: Retpolines"
LFENCE          → "Mitigation: LFENCE"
EIBRS           → "Mitigation: Enhanced / Automatic IBRS"
EIBRS_LFENCE    → "Mitigation: Enhanced / Automatic IBRS + LFENCE"
EIBRS_RETPOLINE → "Mitigation: Enhanced / Automatic IBRS + Retpolines"
IBRS            → "Mitigation: IBRS"
```

### 3.3 cmdline 选项（bugs.c:1367-1382 mitigation_options 原表）

```
off / on / auto / retpoline / retpoline,amd / retpoline,lfence /
retpoline,generic / eibrs / eibrs,lfence / eibrs,retpoline / ibrs
```

### 3.4 模式 ↔ CPU 速配

| 模式 | 谁用 | cpufeatures 位 | 代价 |
| :--- | :--- | :--- | :--- |
| RETPOLINE | Intel 较旧（无 eIBRS 微码） | `X86_FEATURE_RETPOLINE`（:299） | 间接调用 +开销 |
| LFENCE | AMD Zen（lfence 序列化） | `X86_FEATURE_RETPOLINE_LFENCE`（:300） | 比 retpoline 便宜 |
| EIBRS(+X) | 新 Intel（硬件 IBRS） | — | 近零（微码常开） |
| IBRS | 显式选（`spectre_v2=ibrs`） | — | 重（Spectre v2+retbleed 双防时） |

## 四、返回侧两兄弟：retbleed 与 SRSO

### 4.1 retbleed（bugs.c:898-968）

```c
enum retbleed_mitigation {
	RETBLEED_MITIGATION_NONE, UNRET, IBPB, IBRS, EIBRS, STUFF,
};
```

sysfs 字符串（:917-924 原表）："Mitigation: untrained return thunk" / "Mitigation: IBPB" / "Mitigation: IBRS" / "Mitigation: Enhanced IBRS" / **"Mitigation: Stuffing"**。cmdline：`retbleed=off/auto/unret/ibrs`（early_param :968）。

- **UNRET**：AMD 的 untrain 序列（`X86_FEATURE_UNRET` :302——train 了就反训练）
- **STUFF**：RSB 填充——[22 章](22-CALL_DEPTH_ACCOUNT.md)的 call-depth 即 Intel 的深度追踪版（`X86_FEATURE_CALL_DEPTH` :306）
- **IBRS/EIBRS**：Intel 的微码路线（`retbleed=ibrs` 时深防御）

### 4.2 SRSO / Inception（bugs.c:2362-2401——v6.6 新入）

```c
enum srso_mitigation_cmd {
	SRSO_CMD_OFF, SRSO_CMD_MICROCODE, SRSO_CMD_SAFE_RET,
	SRSO_CMD_IBPB, SRSO_CMD_IBPB_ON_VMEXIT,
};
```

- 默认 **`SRSO_CMD_SAFE_RET`**（:2379）；cmdline `spec_rstack_overflow=off/microcode/safe-ret/ibpb/ibpb-vmexit`（early_param :2401）
- sysfs 字符串（:2370-2376 原表）："Mitigation: microcode" / **"Mitigation: safe RET"** / "Mitigation: IBPB" / **"Mitigation: IBPB on VMEXIT only"**
- **ibpb-vmexit** 是虚拟化特化：宿主只在意 guest 逃逸方向的泄露，出口一次 IBPB 即可——省下 guest 内切换的成本
- cpufeatures：`X86_FEATURE_SRSO`（:312）与 `SRSO_ALIAS`（:313——别名路径反训练）

## 五、缓冲清洗族：GDS 与 MDS 的 VERW

GDS（gather_data_sampling，bugs.c:663-690）：

```c
enum gds_mitigations { OFF, UCODE_NEEDED, FORCE, FULL, FULL_LOCKED, HYPERVISOR };
/* sysfs 字符串（:680-687 原表）：
   "Vulnerable" / "Vulnerable: No microcode" /
   "Mitigation: AVX disabled, no microcode" / "Mitigation: Microcode" /
   "Mitigation: Microcode (locked)" / "Unknown: Dependent on hypervisor status" */
```

cmdline `gather_data_sampling=off/force`（early_param :803）；微码修复后（RDCL_NO? 否——MCU 位）在**返回用户态时 VERW 清缓冲**（与 MDS 同款手法）。`CONFIG_GDS_FORCE_MITIGATION`（:674-677）可强制 AVX 禁用路线。MDS 族（mds= :282 / mmio_stale_data= :477）同用 VERW + 入口/出口 IBPB（`X86_FEATURE_ENTRY_IBPB` :297——"Issue an IBPB on kernel entry"）。

## 六、用户态与进程粒度

```
prctl(PR_GET/SET_SPECULATION_CTRL, PR_SPEC_*, ...)：
  PR_SPEC_STORE_BYPASS（bugs.c:2127/:2206——SSB 的 per-process 关闭）
  PR_SPEC_INDIRECT_BRANCH（:2129/:2208——IBPB 的 per-process 控制）
seccomp 场景自动开（不可信代码的默认防御）
spectre_v2_user 的 stibp/ibpb 两轨（bugs.c:1101-1104 的
  spectre_v2_user_stibp / spectre_v2_user_ibpb）
```

## 七、决策树速览（把矩阵收成一棵树）

```
间接分支侧（spectre_v2）：
  有 eIBRS 硬件？──是→ EIBRS(+LFENCE/Retpolines 组合)
  │否→ AMD？──是→ LFENCE
  │     否→ RETPOLINE（或显式 spectre_v2=ibrs 重防御）
返回侧（retbleed/SRSO）：
  AMD Zen？──retbleed=UNRET；SRSO=safe-ret（6.6 默认）
  Intel 旧？──RETHUNK+STUFF/call-depth（trace/22）
  Intel 新？──eIBRS 顺带覆盖（retbleed=EIBRS）
缓冲侧（MDS/GDS）：VERW 出口清洗 + microcode
进程侧：prctl 按进程收紧（seccomp 自动）
```

## 八、观测实战

```bash
# 1) 全套现状一行读
grep -r . /sys/devices/system/cpu/vulnerabilities/ | sed 's|.*/||'

# 2) 引导期选择日志
dmesg | grep -iE 'spectre_v2|retbleed|Spectre V2|SRSO|GDS|mmio'

# 3) 显式覆盖实验
#   spectre_v2=retpoline,gather_data_sampling=off,retbleed=auto,srso=safe-ret
#   （对比各模式的微基准：syscall 往返/间接调用密集负载）

# 4) 性能归因
perf stat -e br_inst_retired.near_call,indirect... workload   # 间接调用占比
#   retpoline 开销集中在间接调用密集的代码（如 BPF 分发/调度器跳表）

# 5) 关全部（基线测量）
#   mitigations=off —— bugs.c 的 cpu_mitigations_off() 总开关
```

## 九、思考题

1. `EIBRS_RETPOLINE` 与 `EIBRS_LFENCE` 为什么存在？（提示：eIBRS 防训练型 BTB 侧信道，但某些 CPU 上仍有残余向量——叠加 retpoline/lfence 是"硬件+软件双保险"；两张 sysfs 字符串的 "+" 后缀即此意。）
2. SRSO 的 `ibpb-vmexit` 只在 VM 出口打 IBPB——guest 内进程间泄露为什么不防？（提示：威胁模型定界：宿主关心 guest→host 方向；guest 内部隔离是 guest 内核自己的责任（自己的 mitigations）。防御按边界裁剪省成本。）
3. GDS 的 `Mitigation: AVX disabled` 为什么是"无微码时的强迫项"？（提示：GDS 经 AVX 收集跨行数据——禁 AVV 即断采集通道；代价是 AVX 应用性能崩——所以仅在 `CONFIG_GDS_FORCE_MITIGATION` 或 `gather_data_sampling=force`。）
4. call-depth（trace/22）为什么算 retbleed 的 STUFF 家族而非预测器消毒？（提示：它不洗预测器——它保证深度耗尽时 RSB 已被合法重填，使训练者的预测**不命中**受害者的推测窗口。）
5. `mitigations=off` 总开关为什么危险但在测量中必要？（提示：矩阵各行都关=基线；生产环境用它等于把 13 个文件里的 Mitigation 全换成 Vulnerable——正确性不坏但机密性裸奔。）
6. prctl 的 per-process SSB 控制（PR_SPEC_STORE_BYPASS）为什么默认 seccomp 场景才开？（提示：SSB 缓解代价在存储密集代码显著；只有跑不可信代码的进程（seccomp 沙箱）才有"值得关"的威胁面——其余进程按需 opt-in。）

---

> 回看：[22-CALL_DEPTH_ACCOUNT](22-CALL_DEPTH_ACCOUNT.md)（retbleed 的深度追踪详解——本章 §四的深挖版）｜[19-text_poke_bp](19-text_poke_bp家族.md)（ALTERNATIVE 的运行时补丁原语——矩阵各模式的落地机制）｜[21-sync_core](21-sync_core.md)（流水线/序列化语义）｜[trace/README](../README.md)（分册导读）
