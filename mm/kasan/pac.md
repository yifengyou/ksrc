# PAC：指针认证——地址位元数据三部曲之三（Linux 6.6）

> 阅读目标：完成地址位元数据三部曲的收官——**签名**方案（MTE=检查数据访问/LAM=容忍元数据/PAC=控制流完整性）：把返回地址等指针签上密钥 MAC，篡改一字节即失配（ROP/JOP 防御）。密钥体系（**5 键** × 128 位分 KEYLO/KEYHI 寄存器对 + SCTLR 四 En 位 + ★"inherited upon fork(), and reinitialised upon exec\*()" 注释 + 内核自签名单键 APIA）；★**位域共存**（`ptrauth_user_pac_mask() = GENMASK_ULL(54, vabits_actual)` 与头注释 "This is dependent on TBI0/TBI1 being enabled, or bits 63:56 would also apply"——MTE 占 byte 7 时 PAC 退守 bit 54 以下）；uapi 三 prctl（RESET=54 / SET_ENABLED=60 / GET_ENABLED=61——`sctlr_user` 基建与 MTE TCF0 同款）；内核自签名（`CONFIG_ARM64_PTR_AUTH_KERNEL` → `-mbranch-protection=pac-ret` 的三档编译旗标）。★当轮考证：v6.6 的文件名是 **pointer_auth.h/c**（"ptrauth.h" 系后续版本改名）。全部对照 v6.6 当轮核对。前置：[MTE 与 LAM](mte-lam.md)｜[tagged_addr prctl](tagged-addr-prctl.md)。

---

## 一、三部曲收束：签名与前两者本质不同

```
MTE   —— 标签：数据访问的硬件检查（内存安全辅助）
LAM   —— 掩码：不检查任何东西（把高位让给用户元数据）
PAC   —— 签名：指针本身就是防伪码（控制流完整性）

PAC 的运作：
  paciasp   —— 函数序言：用 APIA 键给 LR（返回地址）签一个 MAC，
                写进指针的空闲高位（PAC 字段）
  autiasp   —— 尾声：验签；不匹配 → 指针变成非法地址 → 取指即 fault
  攻击面    —— ROP 链里的每个伪造返回地址都无法通过 autiasp
```

## 二、密钥体系（arch/arm64/include/asm/pointer_auth.h）

```c
/* "Each key is a 128-bit quantity which is split across a pair of
   64-bit registers (Lo and Hi)." */
struct ptrauth_key { unsigned long lo, hi; };

/* "We give each process its own keys, which are shared by all threads.
   The keys are inherited upon fork(), and reinitialised upon exec*()." */
struct ptrauth_keys_user {
	struct ptrauth_key apia;   /* 指令 A——编译器签返回地址的默认键 */
	struct ptrauth_key apib;   /* 指令 B */
	struct ptrauth_key apda;   /* 数据 A */
	struct ptrauth_key apdb;   /* 数据 B */
	struct ptrauth_key apga;   /* 泛型（PACGA 指令——软件自定义签名） */
};
```

- 安装宏 `__ptrauth_key_install_nosync(k, v)`：写 `SYS_k_KEYLO_EL1/KEYHI_EL1` 寄存器对
- 硬件使能：`ptrauth_enable()` 置 **SCTLR_ELx_ENIA/ENIB/ENDA/ENDB** 四位（每键一位总闸）
- **内核侧自签名**（`CONFIG_ARM64_PTR_AUTH_KERNEL`）：`struct ptrauth_keys_kernel { struct ptrauth_key apia; }`——**单键**（内核只签自己的返回地址，用 APIA）

## 三、★位域共存：PAC 住哪几位（pointer_auth.h:13-17）

```c
/*
 * The EL0/EL1 pointer bits used by a pointer authentication code.
 * This is dependent on TBI0/TBI1 being enabled, or bits 63:56 would also apply.
 */
#define ptrauth_user_pac_mask()		GENMASK_ULL(54, vabits_actual)
#define ptrauth_kernel_pac_mask()	GENMASK_ULL(63, vabits_actual)
```

★与 MTE 的分账（三部曲的位图收束）：

```
bit 63......56 55.....54 ...... VA_top ..... 0
用户态（TBI 开）：[ MTE 标签 ][ PAC  ][  实地址  ]
用户态（TBI 关）：[   PAC（扩到 63） ][ 实地址 ]
内核态指针      ：[   PAC（恒到 63） ][ 实地址 ]   ← 内核指针无标签
```

——头注释一句话讲透依赖关系：**TBI0/TBI1 开启时 byte 7 归 MTE，PAC 只得 bits 54:vabits；关则 PAC 独占到 63**。内核指针无标签，PAC 恒用全高位。

## 四、uapi 三 prctl（uapi prctl.h:225-262 + kernel/sys.c:2626-2645 + asm/processor.h:382-389）

| prctl | 值 | 语义 | 实现（pointer_auth.c，113 行） |
| :--- | :--- | :--- | :--- |
| `PR_PAC_RESET_KEYS` | 54 | 重置选定键（arg 位图：`PR_PAC_APIAKEY`1<<0 … `APGAKEY`1<<4；arg=0 全重置） | `ptrauth_prctl_reset_keys`（:11-55）——逐键 `get_random_bytes` 后安装 |
| `PR_PAC_SET_ENABLED_KEYS` | 60 | 按键开关检查（keys 位图选、enabled 位图定） | `ptrauth_set_enabled_keys`（:67-90）——★写 `thread.sctlr_user` 的 EnIA/IB/DA/DB 位（与 MTE 的 TCF0 同一基建，切换时生效） |
| `PR_PAC_GET_ENABLED_KEYS` | 61 | 回读当前使能集 | `ptrauth_get_enabled_keys`（:92-113）——从 `sctlr_user` 读 |

验证式细节：`if ((keys & ~PR_PAC_ENABLED_KEYS_MASK) || (enabled & ~keys)) return -EINVAL;`——**enabled 只能是 keys 的子集**；`arg_to_enxx_mask` 把五位键名翻成四个 SCTLR 位（APGA 无总闸——泛型键不需要 En 位）。

调用链：**通用 kernel/sys.c 的 prctl 大 switch**（:2626/:2633/:2640）→ `PAC_RESET_KEYS` 等宏（asm/processor.h:382-389 的薄包装）→ arm64 实现。

## 五、线程生命周期

```
arch_setup_new_exec（process.c:632）→ ptrauth_thread_init_user（:122-128）
  ① ptrauth_keys_init_user：五键全 get_random_bytes + install   ★exec 重置
  ② ptrauth_set_enabled_keys(current, MASK, MASK)               ★全键默认开
__switch_to → ptrauth_thread_switch_user(tsk)                    每次切换重装键
suspend 出口 → ptrauth_suspend_exit() = install_user(keys)       唤醒后补装
```

fork 不重置（注释的 "inherited upon fork()"）——子进程继承父键，**同键才能验彼此签的指针**（线程共享键的同款理由）。

## 六、内核自签名：编译器与汇编两侧

```
arch/arm64/Makefile:64-76 的三档：
  CONFIG_ARM64_BTI_KERNEL    → -mbranch-protection=pac-ret+bti   （签名+跳转目标）
  CONFIG_ARM64_PTR_AUTH_KERNEL → -mbranch-protection=pac-ret
    （无 CC_HAS_BRANCH_PROT_PAC_RET 时回退 -msign-return-address=non-leaf）
  否则                       → -mbranch-protection=none

汇编侧（asm/asm_pointer_auth.h）：
  ptrauth_keys_install_kernel_nosync / ptrauth_keys_install_kernel
  —— entry.S 等手写汇编路径安装内核 APIA 键（与 C 侧同一把）
KVM 侧（asm/kvm_ptrauth.h）：进出 guest 时的键保存/恢复——
  宿主/访客各持各键，切换即换装
```

★内核为什么单键：内核只签**自己的返回地址**（一种用途一种键）；数据键/泛型键是用户态的玩法。

## 七、ptrace 与 HWCAP：调试器通道与能力位

```
uapi/asm/ptrace.h:258 —— NT_ARM_PACA_KEYS / NT_ARM_PACG_KEYS 两个 regset
  （调试器可读写被调试进程的四地址键/泛型键——gdb 调 PAC 程序的前提）
uapi/asm/hwcap.h:56-57 —— HWCAP_PACA (1<<30) / HWCAP_PACG (1UL<<31)
  （getauxval(AT_HWCAP) 的用户态检测位——地址认证/泛型认证分报）
```

## 八、观测实战

```bash
# 1) 硬件与配置
grep -oE 'paca|pacg' /proc/cpuinfo | sort -u       # HWCAP_PACA/PACG
grep -E 'CONFIG_ARM64_PTR_AUTH(_KERNEL)?=' /boot/config-$(uname -r)
dmesg | grep -i 'ptr auth\|pointer auth'           # "CPU features: PAC"

# 2) 用户态自检
#   getauxval(AT_HWCAP) & (HWCAP_PACA|HWCAP_PACG)
#   prctl(PR_PAC_GET_ENABLED_KEYS) —— 默认应全开（exec 时 init 置 MASK）

# 3) 攻防演示
#   关键比较：prctl(PR_PAC_SET_ENABLED_KEYS, PR_PAC_APIAKEY, 0)
#   （关 APIA 检查后旧二进制仍跑——autiasp 变 NOP 语义）vs 保持开启
#   gdb 的 NT_ARM_PACA_KEYS 读取：info registers? （需 gdb 支持）

# 4) 内核自签名的开销画像
#   pac-ret 开启的内核 vs none：syscall 密集负载对比（每函数序/尾一对指令）
```

## 九、思考题

1. PAC 的键为什么**fork 继承**而 MTE 的 TCF 模式 exec 才重置、PAC 键也 exec 重置——三种生命周期的差异各自服务什么？（提示：键=身份（子进程要验父辈签的栈上指针——继承是功能需要）；TCF=策略（新程序新策略）；exec 重置键=新程序的密钥学起点（旧程序的签名不该通过新程序的验签）。）
2. `ptrauth_user_pac_mask` 依赖 TBI——MTE 与 PAC 同时启用时 PAC 少了约 8 位，签名强度降多少？（提示：MAC 长度=空闲位数（48 位 VA+TBI 下约 7 位?——由 GENMASK(54,48) 得 7 位）——碰撞概率上升；PAC 设计上允许短 MAC（验签失败≠致命数据损坏，只是控制流拒走）。）
3. `enabled & ~keys` 的验证式为什么必要？（提示：SET_ENABLED(keys, enabled) 语义是"在 keys 圈定范围内按下 enabled 开关"——enabled 越界即调用者逻辑错误；不设此检会静默开启未授权键。）
4. APGA（泛型键）为什么没有 SCTLR En 位？（提示：地址四键的检查由指令隐式做（autiasp 等），需要硬件总闸；PACGA 是显式调用（软件自己算签名存任意处）——没有"硬件自动检查"就无需总闸。）
5. 用户键与内核键的安装宏都写 `SYS_APIAKEY_EL1` 一系寄存器——边界协议如何避免互相踩？（提示：对照 `ptrauth_keys_install_user`（切换时装用户键）与 `asm_pointer_auth.h` 的 `ptrauth_keys_install_kernel`（汇编路径装内核键）的调用时机——追 entry.S/`__switch_to` 里两宏的先后；KVM 的 kvm_ptrauth.h 保存/恢复是同款边界问题的第三现场。）
6. PAC 防的是 ROP 而非内存泄漏——与 MTE 组合的防御矩阵怎么画？（提示：PAC=控制流（去哪执行）；MTE=数据流（碰谁的数据）；LAM=两者都不管（纯元数据）——三者正交，同一位域分时复用。）

---

> 回看：[MTE 与 LAM](mte-lam.md)（三部曲之一二——位图分账的上游）｜[tagged_addr prctl](tagged-addr-prctl.md)（邻居 prctl 的实现层样例）｜[mte_sync_tags](mte-sync-tags.md)（标签侧的页生命周期）｜[KASAN boot 初始化](kasan-boot-init.md)（boot 期键初始化的邻接）｜[mm/README](../README.md)（分册导读）
