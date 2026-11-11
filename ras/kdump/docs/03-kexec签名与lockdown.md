# kexec 签名与 lockdown 联动（Linux 6.6）

> 定位：[01-kexec加载与跳转](01-kexec加载与跳转.md)（file 模式的加载链）与 [module 签名链](../../../module/docs/04-模块签名链.md)（lockdown 体系）的**交叉章**——kexec_file_load 的签名验证全链、老 kexec_load 在 lockdown 下的封禁逻辑、以及 IMA 评估作为第三通道的豁免语义。前置：01 章、module/04。

---

## 一、为什么 kexec 必须管签名

```
kernel/kexec.c:210-218 原句（老 kexec_load 路径的封锁点）：
  "kexec can be used to circumvent module loading restrictions,
   so prevent loading in that case"
  → security_locked_down(LOCKDOWN_KEXEC) 非 0 即拒绝
推理链：攻击者 root → kexec 换一个无签名验证的内核 → 新内核
  随意 insmod——绕过整个模块信任链。因此 lockdown 体系里
  KEXEC 与 MODULE_SIGNATURE 同级敏感（include/linux/security.h
  的 LOCKDOWN_* 枚举族）
```

## 二、kexec_file_load 的签名链（kernel/kexec_file.c，已核对 6.6）

```
KEXEC_SIG（kernel/Kconfig.kexec:46——"Verify kernel signature
  during kexec_file_load() syscall"）：
  语义（help 原文浓缩）：无签名仍可加载（除非 FORCE），
  ★但"有签名可查则必须查对"——存在签名且验签失败即拒
KEXEC_SIG_FORCE（:60）：强制（无签名=拒）
验证链：
  kimage_validate_signature
    → kexec_image_verify_sig（:146）→ image->fops->verify_sig
      ——验证是"镜像格式 loader"的回调（bzImage/Image/ELF 各自实现）
    → x86 bzImage：kexec_kernel_verify_pe_sig（:126-142）：
       verify_pefile_signature(kernel, len,
         VERIFY_USE_SECONDARY_KEYRING,
         VERIFYING_KEXEC_PE_SIGNATURE)     ← 用途限定枚举
       ★-ENOKEY 时回退 VERIFY_USE_PLATFORM_KEYRING
         （INTEGRITY_PLATFORM_KEYRING——UEFI 平台密钥环）
用途枚举（include/linux/verification.h:30-35）：VERIFYING_
  MODULE_SIGNATURE 与 VERIFYING_KEXEC_PE_SIGNATURE 相邻——
  同一密钥环、不同用途标识（防签名跨域挪用——module/04 的
  同款设计）
```

## 三、三通道互动（:170-180 原句结构，已核对）

```
KEXEC_SIG 验签失败后的判定：
  if (!ima_appraise_signature(READING_KEXEC_IMAGE) &&
      security_locked_down(LOCKDOWN_KEXEC))
      return -EPERM;
语义（源码注释原样）："If IMA is guaranteed to appraise a
  signature on the kexec image, permit it even if the kernel is
  otherwise locked down."
  ★IMA 评估通道可豁免 lockdown：ima_appraise=签名 且策略覆盖
  READING_KEXEC_IMAGE 时，kexec 内核镜像的信任判给 IMA
三通道矩阵：
  ① KEXEC_SIG_FORCE：编译期强制（最硬）
  ② lockdown（LOCKDOWN_KEXEC）：运行期总闸（integrity 级起）
  ③ IMA appraisal：替代信任源（有 IMA 签名即可放行）
```

## 四、crash 内核（kdump 场景）的签名现实

```
capture kernel 也要过同一验证（kexec_file_load -p 路径无豁免）
——但发行版通常自带已签 capture 内核（kexec-tools 的
  /usr/lib/kernel/vmlinuz* 同源签名）
排障：kexec -s -p 失败 "Kernel signature verification failed"：
  ① 镜像无签名（自编译内核没签）→ sign-file 或换发行版镜像
  ② -ENOKEY → 证书不在 secondary/platform 环（对照 module/04
    的密钥环体系）
  ③ lockdown=none 下仍失败 → KEXEC_SIG_FORCE 编译进了
```

## 五、观测与速查

```
cat /sys/kernel/security/lockdown      # 方括号=当前级（kexec 受
                                        integrity 级约束）
dmesg | grep -E 'kexec|Lockdown'       # 两种拒绝的区分：
  "signature verification failed"（KEXEC_SIG 链）
  "Lockdown: kexec: ... restricted"（lockdown 闸）
kexec -s -l vmlinuz                    # -s 即走 file 模式（签名
                                        可验）；不带 -s 的老接口
                                        在 lockdown 下直接 EPERM
```

## 六、思考题

1. 老 kexec_load 为何被 lockdown **整体封禁**而非"只封未签名"？（提示：老接口镜像格式任意、无统一签名载体——无处挂签名。）
2. "有签名可查则必须查对"（KEXEC_SIG 非 FORCE 语义）防什么攻击？（提示：攻击者附带一个**故意错的**签名骗降级？还是防"签名存在却跳过验证"的实现漏洞？）
3. IMA 豁免通道的信任模型差在哪？（对照：KEXEC_SIG 验的是"镜像=密钥环信任的发布者"，IMA 验的是"本机的 IMA 策略认可此文件"。）
4. VERIFYING_KEXEC_PE_SIGNATURE 的"PE"从何而来？（提示：EFI stub 后 bzImage 即 PE/COFF——[init 分册启动章] 的 MZ 头。）
5. 若 crash 内核未签名且 FORCE 开启——panic 时还有 vmcore 吗？（推演 kdump 预备的失效面。）

---

> 回看：[01-kexec加载与跳转](01-kexec加载与跳转.md)｜[02-crashkernel预留与elfcorehdr](02-crashkernel预留与elfcorehdr.md)｜[module/04-模块签名链](../../../module/docs/04-模块签名链.md)｜[syscalls/04-LSM](../../../syscalls/docs/04-LSM与安全钩子顺序.md)
