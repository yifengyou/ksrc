# 文件系统级加密 (fscrypt)

## 简介

**fscrypt** 是一个库，文件系统可接入该库以支持文件和目录的透明加密。

> **注意**：本文档中的“fscrypt”指内核层面的实现（位于 `fs/crypto/`），而非用户态工具 `fscrypt <https://github.com/google/fscrypt>`_。本文档仅涵盖内核部分。如需了解如何使用加密的命令行示例，请参阅用户态工具 `fscrypt` 的文档。此外，建议使用 fscrypt 用户态工具或其他现有用户态工具（如 `fscryptctl <https://github.com/google/fscryptctl>`_ 或 Android 的文件基于加密密钥管理系统 `_），而不是直接使用内核 API。使用现有工具可降低引入自身安全漏洞的风险。（尽管如此，为求完整起见，本文档仍涵盖内核 API。）

与 dm-crypt 不同，fscrypt 工作在文件系统层面而非块设备层面。这使得它能够为不同的文件使用不同的密钥，并在同一文件系统上存在未加密的文件。这在多用户系统中非常有用，因为每个用户的静态数据需要在密码学上与其他用户隔离。然而，除文件名外，fscrypt 并不加密文件系统元数据。（仅加密文件名和文件内容，其他属性不加密）

与 eCryptfs（一种堆叠式文件系统）不同，fscrypt 直接集成到受支持的文件系统中——目前包括 ext4、F2FS 和 UBIFS。这使得加密文件的读写无需在页面缓存中同时缓存解密和加密后的页面，从而几乎将内存使用量减半，使其与未加密文件相当。同样，所需的 dentry 和 inode 数量也减少了一半。eCryptfs 还将加密文件名限制为 143 字节，导致应用程序兼容性问题；而 fscrypt 允许使用完整的 255 字节（NAME_MAX）。最后，与 eCryptfs 不同，fscrypt API 可由非特权用户使用，无需挂载任何东西。

fscrypt 不支持就地加密文件。相反，它支持将空目录标记为加密。之后，当用户态提供密钥后，在该目录树中创建的所有普通文件、目录和符号链接都将自动加密。

## 威胁模型

### 离线攻击

只要用户态选择了强加密密钥，fscrypt 就能在块设备内容发生单次瞬时永久离线泄露的情况下，保护文件内容和文件名的机密性。fscrypt 不保护非文件名元数据的机密性，例如文件大小、文件权限、时间戳和扩展属性。此外，文件中空洞（逻辑上全为零的未分配块）的存在和位置也不受保护。

如果攻击者能够在授权用户访问文件系统之前对文件系统执行离线篡改，则不能保证 fscrypt 能保护机密性或真实性。

### 在线攻击

fscrypt（以及一般的存储加密）只能提供有限甚至没有的保护，以应对在线攻击。详细说明如下：

#### 侧信道攻击

fscrypt 对侧信道攻击（如计时攻击或电磁攻击）的抵抗力取决于底层 Linux 加密 API 算法或内联加密硬件的抵抗力。如果使用易受攻击的算法（例如基于表格的 AES 实现），攻击者可能对在线系统发起侧信道攻击。侧信道攻击也可能针对消费解密数据的应用程序发起。

侧信道攻击（Side-Channel Attack）是一种**不直接破解加密算法本身，而是通过观察系统“运行时的物理表现”来窃取秘密信息**的攻击方式。

简单来说：
> **传统黑客**试图用数学方法把锁（加密算法）撬开；
> **侧信道黑客**则是在你开锁的时候，盯着你的动作、听你钥匙转动的声音、或者看你用了多大力气，从而推断出钥匙的密码。

---

为什么会有“侧信道”？

在计算机中，任何物理操作都会留下“痕迹”。当你进行加密或解密时（比如 fscrypt 在后台帮你加密文件），CPU 需要执行指令、访问内存、消耗电力。这些过程会产生物理特征：
1.  **时间**：某些操作比另一些快（计时攻击）。
2.  **功耗**：处理不同数据时，芯片耗电不一样（功耗分析）。
3.  **电磁波**：芯片工作时发出的电磁信号强弱不同（电磁攻击）。
4.  **声音/热量**：风扇转速、发热程度等。

攻击者不需要知道密钥是什么，他们只需要收集这些**侧面的线索**，结合统计学知识，就能反推出密钥。

---

场景一：基于表格的 AES 实现（Cache Timing Attack / 缓存计时攻击）

这是最经典的例子。为了加速计算，AES 算法通常会使用“查找表”（S-boxes）。
*   **正常情况**：程序根据密钥和明文，去查表得到结果。
*   **漏洞点**：如果 CPU 的缓存（Cache）里已经有了这个表的数据，读取速度就**极快**；如果没有，就需要从内存读取，速度就**慢**。
*   **攻击过程**：
    1.  攻击者发送大量不同的加密请求给服务器。
    2.  攻击者精确测量服务器返回每个请求的**耗时**。
    3.  发现：当密钥的某一位是 `0` 时，耗时短；是 `1` 时，耗时长。
    4.  通过统计成千上万次的时间差异，攻击者就能一步步猜出完整的密钥。

> **通俗比喻**：
> 就像你在图书馆找书。
> *   如果你要找的书就在手边的书架上（缓存命中），你拿起来只要 1 秒。
> *   如果你要找的书在仓库里（缓存未命中），你需要跑过去拿，要 10 秒。
> *   攻击者站在门口看你的动作：如果你每次翻到“红皮书”都很快，说明“红皮书”大概率是你常看的（或者是密钥的一部分），从而推断出你的阅读习惯（密钥）。

场景二：针对消费解密数据的应用程序

文本后半部分提到：“侧信道攻击也可能针对消费解密数据的应用程序发起。”

*   **场景**：用户正在浏览一个加密的网页（比如网银或私人相册），浏览器在后台解密数据并显示出来。
*   **攻击**：攻击者可能通过监控用户设备的**功耗变化**或**内存访问模式**，判断用户正在查看什么内容，甚至提取出解密的明文。
*   **举例**：
    *   假设你的电脑在解密“银行账号A”时，CPU 的电压波动有一个特定的波形。
    *   攻击者通过精密仪器捕捉到这个波形，就能知道你正在查看账号A，而不是账号B。

---


侧信道攻击就是**通过“偷看”计算机干活时的样子（快慢、耗电、发热），而不是直接破解它的逻辑，来偷走密码。**



#### 未经授权的访问

添加加密密钥后，fscrypt 不会隐藏同一系统上其他用户对明文文件内容或文件名的访问。相反，应使用现有的访问控制机制（如文件模式位、POSIX ACL、LSM 或命名空间）来实现此目的。（原因如下：虽然添加了密钥，但从系统本身的角度来看，数据的机密性并非由加密的数学特性保护，而是仅由内核的正确性保护。因此，任何特定的加密访问控制检查都只是通过内核 *代码* 强制执行，这与已有的广泛访问控制机制在很大程度上是冗余的。）

**fscrypt 的加密机制本身不负责“谁可以访问文件”，它只负责“把数据锁起来”。一旦系统允许你解密（即验证通过），数据在内存中就是明文，此时谁能看到它，取决于操作系统的传统权限规则（如 chmod、sudo 等）。**

#### 内核内存泄露

如果攻击者能够入侵系统并读取任意内存（例如通过物理攻击或利用内核安全漏洞），则可以破坏当前使用的所有加密密钥。

不过，fscrypt 允许从内核中移除加密密钥，这可以防止后续泄露。

更详细地说，`FS_IOC_REMOVE_ENCRYPTION_KEY` ioctl（或 `FS_IOC_REMOVE_ENCRYPTION_KEY_ALL_USERS` ioctl）可以从内核内存中清除主加密密钥。如果这样做，它将尝试驱逐所有使用该密钥“解锁”的已缓存 inode，从而清除其每文件密钥，并使它们再次显示为“锁定”，即以密文形式存在。

然而，这些 ioctl 存在一些局限性：
- 正在使用的文件的每文件密钥 *不会* 被移除或清除。因此，为了达到最大效果，用户在移除主密钥之前应关闭相关的加密文件和目录，并终止工作目录位于受影响加密目录中的所有进程。
- 内核无法神奇地清除用户态可能持有的主密钥副本。因此，用户态也必须清除其制作的所有主密钥副本；通常应在调用 `FS_IOC_ADD_ENCRYPTION_KEY` 后立即完成，而不要等待 `FS_IOC_REMOVE_ENCRYPTION_KEY`。当然，这也适用于密钥层次结构中的所有更高级别。用户态还应遵循其他安全措施，例如使用 `mlock()` 锁定包含密钥的内存，以防止其被交换出去。
- 一般来说，内核 VFS 缓存中的解密内容和文件名会被释放但不会被清除。因此，即使相应的密钥已被清除，其中一部分仍可能从已释放的内存中恢复。为解决这一问题，您可以在内核配置中设置 `CONFIG_PAGE_POISONING=y`，并在内核命令行中添加 `page_poison=1`。但这会带来性能成本。
- 秘密密钥可能仍存在于 CPU 寄存器或其他未明确考虑的地方。

#### v1 策略的局限性

v1 加密策略在应对在线攻击方面存在一些弱点：
- 没有验证所提供的主密钥是否正确。因此，恶意用户可以临时将错误的密钥与另一个他们只有只读访问权限的用户的加密文件关联起来。由于文件系统缓存，即使用户拥有正确的密钥在其自己的密钥环中，其他用户访问这些文件时也会使用该错误密钥。这违反了“只读访问”的含义。
- 每文件密钥的泄露也会导致其派生的主密钥泄露。
- 非 root 用户无法安全地移除加密密钥。

上述所有问题在 v2 加密策略中都已得到修复。出于这一原因以及其他原因，建议在所有新的加密目录上使用 v2 加密策略。

## 密钥层次结构

### 主密钥

每个加密目录树都由一个**主密钥**保护。主密钥长度可达 64 字节，且必须至少等于所使用的文件和目录加密模式的安全强度。例如，如果使用任何 AES-256 模式，主密钥必须至少为 256 位，即 32 字节。如果主密钥用于 v1 加密策略且使用了 AES-256-XTS，则有更严格的要求；此类密钥必须为 64 字节。

要“解锁”加密目录树，用户态必须提供适当的主密钥。可以有任意数量的主密钥，每个主密钥可以保护任意数量的目录树，分布在任意数量的文件系统上。

主密钥必须是真实的加密密钥，即与相同长度的随机字节串不可区分。这意味着用户**不得**直接将密码用作主密钥、用零填充较短的密钥或重复较短的密钥。如果用户态犯下此类错误，将无法保证安全性，因为加密证明和分析将不再适用。

相反，用户应使用加密安全的随机数生成器生成主密钥，或使用 KDF（密钥派生函数）。内核不进行任何密钥拉伸；因此，如果用户态从低熵秘密（如口令）派生密钥，则必须使用为此目的设计的 KDF，例如 scrypt、PBKDF2 或 Argon2。

### 密钥派生函数

除了一种例外情况，fscrypt 从不直接使用主密钥进行加密。相反，它们仅作为 KDF（密钥派生函数）的输入来派生实际密钥。

特定主密钥使用的 KDF 取决于该密钥是用于 v1 加密策略还是 v2 加密策略。用户**不得**将同一密钥同时用于 v1 和 v2 加密策略。（目前没有已知针对这种特定密钥重用情况的现实世界攻击，但由于加密证明和分析将不再适用，因此无法保证其安全性。）

对于 v1 加密策略，KDF 仅支持派生每文件加密密钥。它通过将主密钥用 AES-128-ECB 加密来实现，使用文件的 16 字节 nonce 作为 AES 密钥。生成的密文用作派生密钥。如果密文过长，则截断至所需长度。

对于 v2 加密策略，KDF 是 HKDF-SHA512。主密钥作为“输入密钥材料”传递，不使用盐值，并为每个待派生的不同密钥使用不同的“应用特定信息字符串”。例如，当派生每文件加密密钥时，应用特定信息字符串是前缀为 "fscrypt\0" 和上下文字节的文件 nonce。不同类型的派生密钥使用不同的上下文字节。

HKDF-SHA512 优于原始的基于 AES-128-ECB 的 KDF，因为 HKDF 更灵活、不可逆，并能均匀分布来自主密钥的熵。HKDF 也是标准化的，并被其他软件广泛使用，而基于 AES-128-ECB 的 KDF 则是临时的。

### 每文件加密密钥

由于每个主密钥可以保护许多文件，因此有必要“调整”每个文件的加密方式，使得两个文件中的相同明文不会映射到相同的密文，反之亦然。在大多数情况下，fscrypt 通过派生每文件密钥来实现这一点。当创建新的加密 inode（普通文件、目录或符号链接）时，fscrypt 会随机生成一个 16 字节的 nonce 并将其存储在 inode 的加密 xattr 中。然后，它使用 KDF（见上文“密钥派生函数”）从主密钥和 nonce 派生文件的密钥。

选择密钥派生而非密钥包装是因为包装密钥需要更大的 xattr，不太可能适合文件系统 inode 表中的行内存储，并且似乎没有密钥包装的任何显著优势。特别是，目前不需要支持使用多个替代主密钥解锁文件或支持轮换主密钥。相反，主密钥可以在用户态进行包装，例如 `fscrypt <https://github.com/google/fscrypt>`_ 工具所做的那样。

### DIRECT_KEY 策略

Adiantum 加密模式（见“加密模式和用法”）适用于内容和文件名加密，并接受长 IV——足够长以容纳 8 字节的逻辑块号和 16 字节的每文件 nonce。此外，每个 Adiantum 密钥的开销大于 AES-256-XTS 密钥。

因此，为了提高性能并节省内存，对于 Adiantum，支持“直接密钥”配置。当用户通过在 fscrypt 策略中设置 `FSCRYPT_POLICY_FLAG_DIRECT_KEY` 启用此功能时，不使用每文件加密密钥。相反，每当加密任何数据（内容或文件名）时，都会将文件的 16 字节 nonce 包含在 IV 中。此外：
- 对于 v1 加密策略，加密直接使用主密钥完成。因此，用户**不得**将同一主密钥用于任何其他目的，即使是其他 v1 策略。
- 对于 v2 加密策略，加密使用通过 KDF 派生的每模式密钥完成。用户可以将同一主密钥用于其他 v2 加密策略。

### IV_INO_LBLK_64 策略

当在 fscrypt 策略中设置了 `FSCRYPT_POLICY_FLAG_IV_INO_LBLK_64` 时，加密密钥从主密钥、加密模式编号和文件系统 UUID 派生。这通常导致受同一主密钥保护的所有文件共享单个内容加密密钥和单个文件名加密密钥。为了仍然以不同方式加密不同文件的数据，IV 中包含 inode 号。因此，缩小文件系统可能不被允许。

此格式专为符合 UFS 标准的内联加密硬件优化，后者每个 I/O 请求仅支持 64 位 IV，且可能只有少量密钥槽。

### IV_INO_LBLK_32 策略

IV_INO_LBLK_32 策略的工作原理类似于 IV_INO_LBLK_64，不同的是对于 IV_INO_LBLK_32，inode 号使用 SipHash-2-4 进行哈希（其中 SipHash 密钥从主密钥派生），并与文件逻辑块号模 2^32 相加，生成 32 位 IV。

此格式专为符合 eMMC v5.2 标准的内联加密硬件优化，后者每个 I/O 请求仅支持 32 位 IV，且可能只有少量密钥槽。此格式会导致一定程度的 IV 重用，因此仅在因硬件限制而有必要时使用。

### 密钥标识符

对于用于 v2 加密策略的主密钥，还会使用 KDF 派生唯一的 16 字节“密钥标识符”。该值以明文存储，因为需要它来可靠地识别密钥本身。

### 目录哈希密钥

对于使用基于明文文件名的密钥化 dirhash 索引的目录，KDF 还用于为每个目录派生一个 128 位的 SipHash-2-4 密钥以哈希文件名。这类似于派生每文件加密密钥，只是使用了不同的 KDF 上下文。目前，只有大小写折叠（“大小写不敏感”）的加密目录使用这种风格的哈希。

## 加密模式和用法

fscrypt 允许指定一个用于文件内容的加密模式和一个用于文件名的加密模式。不同的目录树可以使用不同的加密模式。

### 支持的模式

目前支持以下加密模式对：
- 内容：AES-256-XTS，文件名：AES-256-CTS-CBC
- 内容：AES-256-XTS，文件名：AES-256-HCTR2
- 内容：Adiantum，文件名：Adiantum
- 内容：AES-128-CBC-ESSIV，文件名：AES-128-CTS-CBC
- 内容：SM4-XTS，文件名：SM4-CTS-CBC

由于处理密文扩展的难度，目前不支持认证加密模式。因此，内容加密使用 `XTS 模式 <https://en.wikipedia.org/wiki/Disk_encryption_theory#XTS>`_ 或 `CBC-ESSIV 模式 <https://en.wikipedia.org/wiki/Disk_encryption_theory#Encrypted_salt-sector_initialization_vector_(ESSIV)>`_ 的分组密码，或宽分组密码。文件名加密使用 `CTS-CBC 模式 <https://en.wikipedia.org/wiki/Ciphertext_stealing>`_ 的分组密码或宽分组密码。

(AES-256-XTS, AES-256-CTS-CBC) 对是推荐的默认值。这也是唯一*保证*在内核支持 fscrypt 时就始终可用的选项；见“内核配置选项”。

(AES-256-XTS, AES-256-HCTR2) 对也是一个不错的选择，它将文件名加密升级为使用宽分组密码。（*宽分组密码*，也称为可调整超级伪随机置换，具有改变一位即可打乱整个结果的性质。）如“文件名加密”所述，宽分组密码是该问题域的理想模式，尽管 CTS-CBC 是备选方案中“最不差”的选择。有关 HCTR2 的更多信息，请参阅《HCTR2 论文》。

在缺乏 AES 硬件加速导致 AES 过慢的系统上，推荐使用 Adiantum。Adiantum 是一种宽分组密码，使用 XChaCha12 和 AES-256 作为其底层组件。大部分工作由 XChaCha12 完成，当缺乏 AES 加速时，它比 AES 快得多。有关 Adiantum 的更多信息，请参阅《Adiantum 论文》。

(AES-128-CBC-ESSIV, AES-128-CBC-CTS) 对的添加是为了尝试为缺乏 CPU 中 AES 指令但拥有支持 AES-CBC（不支持 AES-XTS）的非内联加密引擎（如 CAAM 或 CESA）的系统提供更高效的选项。此选项已过时。事实证明，在 CPU 上执行 AES 实际上更快。此外，Adiantum 更快，推荐在这些系统上使用。

其余的模式对是“民族自豪密码”：
- (SM4-XTS, SM4-CTS-CBC)

一般而言，这些密码本身并不“坏”，但与 AES 和 ChaCha 等常用选择相比，它们受到的安全审查较少。它们也没有带来太多新东西。建议在必须使用这些密码的地方才使用它们。

### 内核配置选项

启用 fscrypt 支持 (`CONFIG_FS_ENCRYPTION`) 会自动拉入使用 AES-256-XTS 和 AES-256-CTS-CBC 加密所需的加密 API 的基本支持。为了获得最佳性能，强烈建议启用任何可用的平台特定 kconfig 选项，以提供您希望使用的算法的加速。支持任何“非默认”加密模式通常还需要额外的 kconfig 选项。

下面按加密模式列出一些相关选项。请注意，未列出的加速选项可能适用于您的平台；请参考 kconfig 菜单。文件内容加密也可以配置为使用内联加密硬件代替内核加密 API（见“内联加密支持”）；在这种情况下，文件内容模式不需要在内核加密 API 中支持，但文件名模式仍然需要。

- **AES-256-XTS 和 AES-256-CTS-CBC** - 推荐：
    - arm64: `CONFIG_CRYPTO_AES_ARM64_CE_BLK`
    - x86: `CONFIG_CRYPTO_AES_NI_INTEL`
- **AES-256-HCTR2** - 必需：
    - `CONFIG_CRYPTO_HCTR2`
    - 推荐：
        - arm64: `CONFIG_CRYPTO_AES_ARM64_CE_BLK`
        - arm64: `CONFIG_CRYPTO_POLYVAL_ARM64_CE`
        - x86: `CONFIG_CRYPTO_AES_NI_INTEL`
        - x86: `CONFIG_CRYPTO_POLYVAL_CLMUL_NI`
- **Adiantum** - 必需：
    - `CONFIG_CRYPTO_ADIANTUM`
    - 推荐：
        - arm32: `CONFIG_CRYPTO_CHACHA20_NEON`, `CONFIG_CRYPTO_NHPOLY1305_NEON`
        - arm64: `CONFIG_CRYPTO_CHACHA20_NEON`, `CONFIG_CRYPTO_NHPOLY1305_NEON`
        - x86: `CONFIG_CRYPTO_CHACHA20_X86_64`, `CONFIG_CRYPTO_NHPOLY1305_SSE2`, `CONFIG_CRYPTO_NHPOLY1305_AVX2`
- **AES-128-CBC-ESSIV 和 AES-128-CTS-CBC**:
    - 必需：`CONFIG_CRYPTO_ESSIV`, `CONFIG_CRYPTO_SHA256` 或其他 SHA-256 实现
    - 推荐：AES-CBC 加速

fscrypt 还使用 HMAC-SHA512 进行密钥派生，因此建议启用 SHA-512 加速：
- **SHA-512** - 推荐：
    - arm64: `CONFIG_CRYPTO_SHA512_ARM64_CE`
    - x86: `CONFIG_CRYPTO_SHA512_SSSE3`

### 内容加密

对于文件内容，每个文件系统块独立加密。从 Linux 内核 5.5 开始，支持加密块大小小于系统页大小的文件系统。

每个块的 IV 设置为文件内的逻辑块号（小端数），例外情况如下：
- 使用 CBC 模式加密时，也使用 ESSIV。具体来说，每个 IV 都用 AES-256 加密，其中 AES-256 密钥是文件数据加密密钥的 SHA-256 哈希。
- 使用 `DIRECT_KEY 策略`_ 时，文件的 nonce 附加到 IV 末尾。目前仅允许与 Adiantum 加密模式一起使用。
- 使用 `IV_INO_LBLK_64 策略`_ 时，逻辑块号限制为 32 位，放置在 IV 的 0-31 位。inode 号（也限制为 32 位）放置在 32-63 位。
- 使用 `IV_INO_LBLK_32 策略`_ 时，逻辑块号限制为 32 位，放置在 IV 的 0-31 位。然后对 inode 号进行哈希并模 2^32 相加。

请注意，由于文件逻辑块号包含在 IV 中，文件系统必须确保块永远不会在加密文件内移动，例如通过“collapse range”或“insert range”。

### 文件名加密

对于文件名，每个完整文件名一次性加密。由于需要保留对高效目录查找的支持以及长达 255 字节的文件名，目录中的每个文件名都使用相同的 IV。

然而，每个加密目录仍然使用唯一的密钥，或者对于 `DIRECT_KEY 策略`_ 包含文件的 nonce，对于 `IV_INO_LBLK_64 策略`_ 包含 inode 号。因此，IV 重用仅限于单个目录内。

使用 CTS-CBC 时，IV 重用意味着当明文文件名共享至少与密码块大小一样长的公共前缀（AES 为 16 字节）时，对应的加密文件名也将共享公共前缀。这是不可取的。Adiantum 和 HCTR2 没有这个弱点，因为它们是宽分组加密模式。

所有支持的文件名加密模式接受任何 >= 16 字节的明文长度；不需要密码块对齐。但是，短于 16 字节的文件名在加密前会用 NUL 填充到 16 字节。此外，为了减少通过密文泄露文件名长度，所有文件名都会被 NUL 填充到下一个 4、8、16 或 32 字节边界（可配置）。推荐 32，因为这提供了最好的机密性，代价是目录项消耗稍多的空间。请注意，由于 NUL (`\\0`) 在其他地方不是文件名中的有效字符，因此填充永远不会产生重复的明文。

符号链接目标被视为一种文件名，并以与目录条目中的文件名相同的方式加密，只是 IV 重用不是问题，因为每个符号链接都有自己的 inode。

## 用户 API

### 设置加密策略

#### FS_IOC_SET_ENCRYPTION_POLICY

`FS_IOC_SET_ENCRYPTION_POLICY` ioctl 在空目录上设置加密策略，或验证目录或普通文件是否已经具有指定的加密策略。它接收指向 `struct fscrypt_policy_v1` 或 `struct fscrypt_policy_v2` 的指针，定义如下：

```c
#define FSCRYPT_POLICY_V1 0
#define FSCRYPT_KEY_DESCRIPTOR_SIZE 8
struct fscrypt_policy_v1 {
    __u8 version;
    __u8 contents_encryption_mode;
    __u8 filenames_encryption_mode;
    __u8 flags;
    __u8 master_key_descriptor[FSCRYPT_KEY_DESCRIPTOR_SIZE];
};
#define fscrypt_policy fscrypt_policy_v1

#define FSCRYPT_POLICY_V2 2
#define FSCRYPT_KEY_IDENTIFIER_SIZE 16
struct fscrypt_policy_v2 {
    __u8 version;
    __u8 contents_encryption_mode;
    __u8 filenames_encryption_mode;
    __u8 flags;
    __u8 __reserved[4];
    __u8 master_key_identifier[FSCRYPT_KEY_IDENTIFIER_SIZE];
};
```

必须按以下方式初始化此结构：
- 如果使用 `struct fscrypt_policy_v1`，则 `version` 必须为 `FSCRYPT_POLICY_V1` (0)；如果使用 `struct fscrypt_policy_v2`，则为 `FSCRYPT_POLICY_V2` (2)。（注意：我们将原始策略版本称为“v1”，尽管其版本代码实际上是 0。）对于新的加密目录，请使用 v2 策略。
- `contents_encryption_mode` 和 `filenames_encryption_mode` 必须设置为 `<linux/fscrypt.h>` 中的常量，以标识要使用的加密模式。如果不确定，对于 `contents_encryption_mode` 使用 `FSCRYPT_MODE_AES_256_XTS` (1)，对于 `filenames_encryption_mode` 使用 `FSCRYPT_MODE_AES_256_CTS` (4)。详细信息请参见“加密模式和用法”。
    - v1 加密策略仅支持三种模式组合：(FSCRYPT_MODE_AES_256_XTS, FSCRYPT_MODE_AES_256_CTS), (FSCRYPT_MODE_AES_128_CBC, FSCRYPT_MODE_AES_128_CTS), 和 (FSCRYPT_MODE_ADIANTUM, FSCRYPT_MODE_ADIANTUM)。
    - v2 策略支持“支持的模式”中记录的所有组合。
- `flags` 包含来自 `<linux/fscrypt.h>` 的可选标志：
    - `FSCRYPT_POLICY_FLAGS_PAD_*`: 加密文件名时要使用的 NUL 填充量。如果不确定，使用 `FSCRYPT_POLICY_FLAGS_PAD_32` (0x3)。
    - `FSCRYPT_POLICY_FLAG_DIRECT_KEY`: 见“DIRECT_KEY 策略”。
    - `FSCRYPT_POLICY_FLAG_IV_INO_LBLK_64`: 见“IV_INO_LBLK_64 策略”。
    - `FSCRYPT_POLICY_FLAG_IV_INO_LBLK_32`: 见“IV_INO_LBLK_32 策略”。
    - v1 加密策略仅支持 PAD_* 和 DIRECT_KEY 标志。其他标志仅由 v2 加密策略支持。
    - DIRECT_KEY、IV_INO_LBLK_64 和 IV_INO_LBLK_32 标志互斥。
- 对于 v2 加密策略，`__reserved` 必须清零。
- 对于 v1 加密策略，`master_key_descriptor` 指定如何在密钥环中找到主密钥；见“添加密钥”。由用户态为每个主密钥选择唯一的 `master_key_descriptor`。e4crypt 和 fscrypt 工具使用 `SHA-512(SHA-512(master_key))` 的前 8 个字节，但不强制要求使用此特定方案。此外，在执行 `FS_IOC_SET_ENCRYPTION_POLICY` 时，主密钥不必已在密钥环中。但在加密目录中创建任何文件之前，必须添加它。

对于 v2 加密策略，`master_key_descriptor` 已被 `master_key_identifier` 替换，后者更长且不能任意选择。相反，必须先使用 `FS_IOC_ADD_ENCRYPTION_KEY`_ 添加密钥。然后，必须将在 `struct fscrypt_add_key_arg` 中内核返回的 `key_spec.u.identifier` 用作 `struct fscrypt_policy_v2` 中的 `master_key_identifier`。

如果文件尚未加密，则 `FS_IOC_SET_ENCRYPTION_POLICY` 验证该文件是否为空目录。如果是，则将指定的加密策略分配给该目录，将其转换为加密目录。之后，并按“添加密钥”所述提供相应的主密钥后，在该目录中创建的所有普通文件、目录（递归）和符号链接都将加密，继承相同的加密策略。目录条目的文件名也将被加密。

或者，如果文件已经加密，则 `FS_IOC_SET_ENCRYPTION_POLICY` 验证指定的加密策略是否与实际的完全匹配。如果匹配，则 ioctl 返回 0。否则，它将失败并返回 EEXIST。这对普通文件和目录（包括非空目录）都有效。

当为目录分配 v2 加密策略时，还要求指定密钥已由当前用户添加，或者调用者在初始用户命名空间中具有 CAP_FOWNER 能力。（这是为了防止用户使用其他用户的密钥加密自己的数据。）在 `FS_IOC_SET_ENCRYPTION_POLICY` 执行期间，密钥必须保持添加状态。但是，如果新加密目录不需要立即访问，则密钥可以随后立即移除。

注意，ext4 文件系统不允许加密根目录，即使它是空的。想要用一个密钥加密整个文件系统的用户应考虑使用 dm-crypt。

`FS_IOC_SET_ENCRYPTION_POLICY` 可能失败并出现以下错误：
- `EACCES`: 文件不属于进程的 uid，且进程在映射了文件所有者 uid 的命名空间中不具有 CAP_FOWNER 能力
- `EEXIST`: 文件已使用不同于指定策略的加密策略加密
- `EINVAL`: 指定了无效的加密策略（无效的版本、模式或标志；或设置了保留位）；或指定了 v1 加密策略，但目录启用了 casefold 标志（casefolding 与 v1 策略不兼容）
- `ENOKEY`: 指定了 v2 加密策略，但未添加具有指定 `master_key_identifier` 的密钥，且进程在初始用户命名空间中不具有 CAP_FOWNER 能力
- `ENOTDIR`: 文件未加密且为普通文件，而非目录
- `ENOTEMPTY`: 文件未加密且为非空目录
- `ENOTTY`: 此类文件系统未实现加密
- `EOPNOTSUPP`: 内核未配置支持文件系统的加密，或文件系统超级块未启用加密。（例如，要在 ext4 文件系统上使用加密，必须在内核配置中启用 `CONFIG_FS_ENCRYPTION`，并使用 `tune2fs -O encrypt` 或 `mkfs.ext4 -O encrypt` 启用超级块的 "encrypt" 特性标志。）
- `EPERM`: 此目录可能无法加密，例如因为它是 ext4 文件系统的根目录
- `EROFS`: 文件系统为只读

### 获取加密策略

有两个 ioctl 可用于获取文件的加密策略：
- `FS_IOC_GET_ENCRYPTION_POLICY_EX`
- `FS_IOC_GET_ENCRYPTION_POLICY`

扩展版 (_EX) 更通用，推荐在可能时使用。然而，在旧的内核中仅提供原始 ioctl。应用程序应尝试扩展版，如果失败并返回 ENOTTY，则回退到原始版本。

#### FS_IOC_GET_ENCRYPTION_POLICY_EX

`FS_IOC_GET_ENCRYPTION_POLICY_EX` ioctl 检索目录或普通文件的加密策略（如果有）。除了能够打开文件外，不需要其他权限。它接收指向 `struct fscrypt_get_policy_ex_arg` 的指针，定义如下：

```c
struct fscrypt_get_policy_ex_arg {
    __u64 policy_size; /* input/output */
    union {
        __u8 version;
        struct fscrypt_policy_v1 v1;
        struct fscrypt_policy_v2 v2;
    } policy; /* output */
};
```

调用者必须将 `policy_size` 初始化为策略结构可用的大小，即 `sizeof(arg.policy)`。

成功时，策略结构返回在 `policy` 中，其实际大小返回在 `policy_size` 中。应检查 `policy.version` 以确定返回的策略版本。注意，“v1”策略的版本代码实际上是 0 (`FSCRYPT_POLICY_V1`)。

`FS_IOC_GET_ENCRYPTION_POLICY_EX` 可能失败并出现以下错误：
- `EINVAL`: 文件已加密，但使用了 unrecognized 加密策略版本
- `ENODATA`: 文件未加密
- `ENOTTY`: 此类文件系统未实现加密，或此内核太旧而不支持 `FS_IOC_GET_ENCRYPTION_POLICY_EX`（请尝试 `FS_IOC_GET_ENCRYPTION_POLICY`）
- `EOPNOTSUPP`: 内核未配置支持此文件系统的加密，或文件系统超级块未启用加密
- `EOVERFLOW`: 文件已加密并使用认可的加密策略版本，但策略结构不适合提供的缓冲区

注意：如果您只需要知道文件是否已加密，在大多数文件系统上也可以使用 `FS_IOC_GETFLAGS` ioctl 并检查 `FS_ENCRYPT_FL`，或使用 `statx()` 系统调用并检查 `stx_attributes` 中的 `STATX_ATTR_ENCRYPTED`。

#### FS_IOC_GET_ENCRYPTION_POLICY

`FS_IOC_GET_ENCRYPTION_POLICY` ioctl 也可以检索目录或普通文件的加密策略（如果有）。然而，与 `FS_IOC_GET_ENCRYPTION_POLICY_EX`_ 不同，`FS_IOC_GET_ENCRYPTION_POLICY` 仅支持原始策略版本。它直接接收指向 `struct fscrypt_policy_v1` 的指针，而不是 `struct fscrypt_get_policy_ex_arg`。

`FS_IOC_GET_ENCRYPTION_POLICY` 的错误代码与 `FS_IOC_GET_ENCRYPTION_POLICY_EX` 相同，但 `FS_IOC_GET_ENCRYPTION_POLICY` 还会在文件使用较新的加密策略版本加密时返回 `EINVAL`。

### 获取文件系统范围的盐

某些文件系统（如 ext4 和 F2FS）还支持已弃用的 ioctl `FS_IOC_GET_ENCRYPTION_PWSALT`。此 ioctl 检索存储在文件系统超级块中的随机生成的 16 字节值。此值旨在用作从口令或其他低熵用户凭证派生加密密钥时的盐。

`FS_IOC_GET_ENCRYPTION_PWSALT` 已弃用。相反，建议在用户态生成和管理任何需要的盐。

### 获取文件的加密 nonce

自 Linux v5.7 起，支持 ioctl `FS_IOC_GET_ENCRYPTION_NONCE`。在加密文件和目录上，它获取 inode 的 16 字节 nonce。在未加密的文件和目录上，它将失败并返回 ENODATA。

此 ioctl 可用于验证加密是否正确执行的自动化测试。对于 fscrypt 的正常使用并不需要。

### 添加密钥

#### FS_IOC_ADD_ENCRYPTION_KEY

`FS_IOC_ADD_ENCRYPTION_KEY` ioctl 向文件系统添加主加密密钥，使文件系统中所有使用该密钥加密的文件看起来“已解锁”，即以明文形式存在。它可以在目标文件系统上的任何文件或目录上执行，但建议使用文件系统的根目录。它接收指向 `struct fscrypt_add_key_arg` 的指针，定义如下：

```c
struct fscrypt_add_key_arg {
    struct fscrypt_key_specifier key_spec;
    __u32 raw_size;
    __u32 key_id;
    __u32 __reserved[8];
    __u8 raw[];
};
#define FSCRYPT_KEY_SPEC_TYPE_DESCRIPTOR 1
#define FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER 2
struct fscrypt_key_specifier {
    __u32 type; /* one of FSCRYPT_KEY_SPEC_TYPE_* */
    __u32 __reserved;
    union {
        __u8 __reserved[32]; /* reserve some extra space */
        __u8 descriptor[FSCRYPT_KEY_DESCRIPTOR_SIZE];
        __u8 identifier[FSCRYPT_KEY_IDENTIFIER_SIZE];
    } u;
};
struct fscrypt_provisioning_key_payload {
    __u32 type;
    __u32 __reserved;
    __u8 raw[];
};
```

必须将 `struct fscrypt_add_key_arg` 清零，然后按以下方式初始化：
- 如果要添加的密钥用于 v1 加密策略，则 `key_spec.type` 必须包含 `FSCRYPT_KEY_SPEC_TYPE_DESCRIPTOR`，且 `key_spec.u.descriptor` 必须包含要添加的密钥的描述符，对应于 `struct fscrypt_policy_v1` 中 `master_key_descriptor` 字段的值。要添加此类型的密钥，调用进程必须在初始用户命名空间中具有 CAP_SYS_ADMIN 能力。
- 或者，如果要添加的密钥用于 v2 加密策略，则 `key_spec.type` 必须包含 `FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER`，且 `key_spec.u.identifier` 是一个*输出*字段，内核将用密钥的加密哈希填充它。要添加此类型的密钥，调用进程不需要任何特权。但是，可以添加的密钥数量受用户密钥环服务的配额限制（见 `Documentation/security/keys/core.rst`）。
- `raw_size` 必须是提供的 `raw` 密钥的大小（以字节为单位）。或者，如果 `key_id` 不为零，则此字段必须为 0，因为在这种情况下，大小由指定的 Linux 密钥环密钥隐含。
- `key_id` 为 0 表示直接在 `raw` 字段中给出原始密钥。否则，`key_id` 是类型为 "fscrypt-provisioning" 的 Linux 密钥环密钥的 ID，其负载为 `struct fscrypt_provisioning_key_payload`，其 `raw` 字段包含原始密钥，且其 `type` 字段与 `key_spec.type` 匹配。由于 `raw` 是可变长度的，此密钥负载的总大小必须为 `sizeof(struct fscrypt_provisioning_key_payload)` 加上原始密钥大小。进程必须对此密钥具有搜索权限。
- 大多数用户应将其留为 0 并直接指定原始密钥。支持指定 Linux 密钥环密钥主要是为了允许在文件系统卸载并重新挂载后重新添加密钥，而无需将原始密钥存储在用户态内存中。
- `raw` 是一个可变长度字段，必须包含实际密钥，长度为 `raw_size` 字节。或者，如果 `key_id` 不为零，则此字段未使用。

对于 v2 策略密钥，内核跟踪添加密钥的用户（通过有效用户 ID 标识），并仅允许该用户（或“root”，如果他们使用 `FS_IOC_REMOVE_ENCRYPTION_KEY_ALL_USERS`_）移除密钥。

但是，如果另一个用户添加了密钥，可能希望防止该用户意外移除它。因此，`FS_IOC_ADD_ENCRYPTION_KEY` 也可用于再次添加 v2 策略密钥，即使它已被其他用户添加。在这种情况下，`FS_IOC_ADD_ENCRYPTION_KEY` 仅为当前用户安装对该密钥的声明，而不会实际再次添加密钥（但仍需提供原始密钥作为知识证明）。

如果密钥或对密钥的声明已添加或已存在，则 `FS_IOC_ADD_ENCRYPTION_KEY` 返回 0。

`FS_IOC_ADD_ENCRYPTION_KEY` 可能失败并出现以下错误：
- `EACCES`: 指定了 `FSCRYPT_KEY_SPEC_TYPE_DESCRIPTOR`，但调用者在初始用户命名空间中不具有 CAP_SYS_ADMIN 能力；或原始密钥由 Linux 密钥 ID 指定，但进程缺乏对该密钥的搜索权限
- `EDQUOT`: 添加密钥将超出此用户的密钥配额
- `EINVAL`: 无效的密钥大小或密钥说明符类型，或设置了保留位
- `EKEYREJECTED`: 原始密钥由 Linux 密钥 ID 指定，但密钥类型不正确
- `ENOKEY`: 原始密钥由 Linux 密钥 ID 指定，但不存在具有该 ID 的密钥
- `ENOTTY`: 此类文件系统未实现加密
- `EOPNOTSUPP`: 内核未配置支持此文件系统的加密，或文件系统超级块未启用加密

#### 遗留方法

对于 v1 加密策略，也可以通过将主加密密钥添加到进程订阅的密钥环（例如会话密钥环，或如果用户密钥环链接到会话密钥环则添加到用户密钥环）来提供。

此方法已弃用（且不支持 v2 加密策略），原因有几个。首先，它不能与 `FS_IOC_REMOVE_ENCRYPTION_KEY`（见“移除密钥”）结合使用，因此要移除密钥，必须使用诸如 `keyctl_unlink()` 结合 `sync; echo 2 > /proc/sys/vm/drop_caches` 之类的变通方法。其次，它与加密文件锁定/解锁状态（即它们看起来是明文形式还是密文形式）是全局的这一事实不符。这种不匹配导致了大量混淆，以及在以不同 UID 运行的进程（如 `sudo` 命令）需要访问加密文件时出现了实际问题。

尽管如此，要将密钥添加到进程订阅的密钥环之一，可以使用 `add_key()` 系统调用（见：`Documentation/security/keys/core.rst`）。密钥类型必须为 "logon"；此类密钥保存在内核内存中，用户态无法读回。密钥描述符必须是 "fscrypt:" 后跟加密策略中设置的 `master_key_descriptor` 的 16 个小写十六进制表示。密钥负载必须符合以下结构：

```c
#define FSCRYPT_MAX_KEY_SIZE 64
struct fscrypt_key {
    __u32 mode;
    __u8 raw[FSCRYPT_MAX_KEY_SIZE];
    __u32 size;
};
```

`mode` 被忽略；只需将其设置为 0。实际密钥在 `raw` 中提供，`size` 指示其大小（字节）。也就是说，字节 `raw[0..size-1]`（含）是实际密钥。

密钥描述符前缀 "fscrypt:" 也可以替换为特定于文件系统的名称前缀，如 "ext4:"。但是，特定于文件系统的名称前缀已弃用，不应在新程序中使用。

### 移除密钥

有两个 ioctl 可用于移除由 `FS_IOC_ADD_ENCRYPTION_KEY`_ 添加的密钥：
- `FS_IOC_REMOVE_ENCRYPTION_KEY`
- `FS_IOC_REMOVE_ENCRYPTION_KEY_ALL_USERS`

这两个 ioctl 仅在非 root 用户添加或移除 v2 策略密钥的情况下有所不同。

这些 ioctl 不适用于通过遗留进程订阅密钥环机制添加的密钥。

在使用这些 ioctl 之前，请阅读“内核内存泄露”一节，了解这些 ioctl 的安全目标和局限性。

#### FS_IOC_REMOVE_ENCRYPTION_KEY

`FS_IOC_REMOVE_ENCRYPTION_KEY` ioctl 从文件系统移除对主加密密钥的声明，并可能移除密钥本身。它可以在目标文件系统上的任何文件或目录上执行，但建议使用文件系统的根目录。它接收指向 `struct fscrypt_remove_key_arg` 的指针，定义如下：

```c
struct fscrypt_remove_key_arg {
    struct fscrypt_key_specifier key_spec;
    #define FSCRYPT_KEY_REMOVAL_STATUS_FLAG_FILES_BUSY 0x00000001
    #define FSCRYPT_KEY_REMOVAL_STATUS_FLAG_OTHER_USERS 0x00000002
    __u32 removal_status_flags; /* output */
    __u32 __reserved[5];
};
```

必须将此结构清零，然后按以下方式初始化：
- 通过 `key_spec` 指定要移除的密钥：
    - 要移除用于 v1 加密策略的密钥，请将 `key_spec.type` 设置为 `FSCRYPT_KEY_SPEC_TYPE_DESCRIPTOR` 并填写 `key_spec.u.descriptor`。要移除此类型的密钥，调用进程必须在初始用户命名空间中具有 CAP_SYS_ADMIN 能力。
    - 要移除用于 v2 加密策略的密钥，请将 `key_spec.type` 设置为 `FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER` 并填写 `key_spec.u.identifier`。

对于 v2 策略密钥，此 ioctl 可由非 root 用户使用。但是，为了使这一点成为可能，它实际上只是移除了当前用户对密钥的声明，撤销了一次 `FS_IOC_ADD_ENCRYPTION_KEY` 调用。只有在所有声明都被移除后，密钥才会真正被移除。

例如，如果 `FS_IOC_ADD_ENCRYPTION_KEY` 是用 uid 1000 调用的，那么密钥将被 uid 1000 “声明”，`FS_IOC_REMOVE_ENCRYPTION_KEY` 将仅在 uid 1000 下成功。或者，如果 uid 1000 和 2000 都添加了密钥，那么对于每个 uid，`FS_IOC_REMOVE_ENCRYPTION_KEY` 只会移除他们自己的声明。只有当 *两者* 都被移除后，密钥才会真正被移除。（可以将其想象为删除可能有硬链接的文件。）

如果 `FS_IOC_REMOVE_ENCRYPTION_KEY` 真正移除了密钥，它将尝试“锁定”所有曾用该密钥解锁的文件。它不会锁定仍在使用的文件，因此预计此 ioctl 将与用户态配合使用，以确保没有任何文件仍处于打开状态。但是，如有必要，可以稍后再次执行此 ioctl 以重试锁定剩余文件。

如果密钥被移除（但可能仍有文件需要锁定）、用户对密钥的声明被移除，或者密钥已被移除但仍有文件需要锁定所以 ioctl 重试锁定它们，则 `FS_IOC_REMOVE_ENCRYPTION_KEY` 返回 0。在任何这些情况下，`removal_status_flags` 都会填充以下信息状态标志：
- `FSCRYPT_KEY_REMOVAL_STATUS_FLAG_FILES_BUSY`: 如果某些文件仍在使用中则设置。仅在仅移除了用户对密钥的声明的情况下不保证设置。
- `FSCRYPT_KEY_REMOVAL_STATUS_FLAG_OTHER_USERS`: 如果仅移除了用户对密钥的声明，而未移除密钥本身则设置。

`FS_IOC_REMOVE_ENCRYPTION_KEY` 可能失败并出现以下错误：
- `EACCES`: 指定了 `FSCRYPT_KEY_SPEC_TYPE_DESCRIPTOR` 密钥说明符类型，但调用者在初始用户命名空间中不具有 CAP_SYS_ADMIN 能力
- `EINVAL`: 无效的密钥说明符类型，或设置了保留位
- `ENOKEY`: 根本找不到密钥对象，即从未添加或已完全移除（包括所有文件已锁定）；或者，用户没有对密钥的声明（但其他人有）。
- `ENOTTY`: 此类文件系统未实现加密
- `EOPNOTSUPP`: 内核未配置支持此文件系统的加密，或文件系统超级块未启用加密

#### FS_IOC_REMOVE_ENCRYPTION_KEY_ALL_USERS

`FS_IOC_REMOVE_ENCRYPTION_KEY_ALL_USERS` 与 `FS_IOC_REMOVE_ENCRYPTION_KEY`_ 完全相同，只是对于 v2 策略密钥，ALL_USERS 版本的 ioctl 将移除所有用户对密钥的声明，而不仅仅是当前用户的。即，无论有多少用户添加了密钥，密钥本身总是会被移除。如果非 root 用户正在添加和移除密钥，这种差异才有意义。

因此，`FS_IOC_REMOVE_ENCRYPTION_KEY_ALL_USERS` 也需要“root”，即在初始用户命名空间中具有 CAP_SYS_ADMIN 能力。否则，它将失败并返回 EACCES。

### 获取密钥状态

#### FS_IOC_GET_ENCRYPTION_KEY_STATUS

`FS_IOC_GET_ENCRYPTION_KEY_STATUS` ioctl 检索主加密密钥的状态。它可以在目标文件系统上的任何文件或目录上执行，但建议使用文件系统的根目录。它接收指向 `struct fscrypt_get_key_status_arg` 的指针，定义如下：

```c
struct fscrypt_get_key_status_arg {
    /* input */
    struct fscrypt_key_specifier key_spec;
    __u32 __reserved[6];
    /* output */
    #define FSCRYPT_KEY_STATUS_ABSENT 1
    #define FSCRYPT_KEY_STATUS_PRESENT 2
    #define FSCRYPT_KEY_STATUS_INCOMPLETLY_REMOVED 3
    __u32 status;
    #define FSCRYPT_KEY_STATUS_FLAG_ADDED_BY_SELF 0x00000001
    __u32 status_flags;
    __u32 user_count;
    __u32 __out_reserved[13];
};
```

调用者必须将所有输入字段清零，然后填写 `key_spec`：
- 要获取 v1 加密策略密钥的状态，请将 `key_spec.type` 设置为 `FSCRYPT_KEY_SPEC_TYPE_DESCRIPTOR` 并填写 `key_spec.u.descriptor`。
- 要获取 v2 加密策略密钥的状态，请将 `key_spec.type` 设置为 `FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER` 并填写 `key_spec.u.identifier`。

成功时，返回 0，内核填充输出字段：
- `status` 指示密钥是缺失、存在还是部分移除。部分移除意味着主秘密已被移除，但某些文件仍在使用中；即，`FS_IOC_REMOVE_ENCRYPTION_KEY`_ 返回 0 但设置了信息状态标志 `FSCRYPT_KEY_REMOVAL_STATUS_FLAG_FILES_BUSY`。
- `status_flags` 可以包含以下标志：
    - `FSCRYPT_KEY_STATUS_FLAG_ADDED_BY_SELF` 表示密钥是由当前用户添加的。这仅对通过 `identifier` 而非 `descriptor` 标识的密钥设置。
- `user_count` 指定添加密钥的用户数量。这仅对通过 `identifier` 而非 `descriptor` 标识的密钥设置。

`FS_IOC_GET_ENCRYPTION_KEY_STATUS` 可能失败并出现以下错误：
- `EINVAL`: 无效的密钥说明符类型，或设置了保留位
- `ENOTTY`: 此类文件系统未实现加密
- `EOPNOTSUPP`: 内核未配置支持此文件系统的加密，或文件系统超级块未启用加密

除其他用例外，`FS_IOC_GET_ENCRYPTION_KEY_STATUS` 可用于确定在提示用户输入派生密钥所需的口令之前，是否需要为给定加密目录添加密钥。

`FS_IOC_GET_ENCRYPTION_KEY_STATUS` 只能获取文件系统级密钥环中的密钥状态，即由 `FS_IOC_ADD_ENCRYPTION_KEY`_ 和 `FS_IOC_REMOVE_ENCRYPTION_KEY`_ 管理的密钥环。它无法获取仅通过使用涉及进程订阅密钥环的遗留机制为 v1 加密策略添加的密钥的状态。

## 访问语义

### 拥有密钥时

拥有加密密钥时，加密的普通文件、目录和符号链接的行为与其未加密的 counterpart 非常相似——毕竟，加密应该是透明的。然而，细心的用户可能会注意到行为上的一些差异：
- 未加密的文件，或用不同加密策略（即不同密钥、模式或标志）加密的文件，无法重命名或链接到加密目录中；见“加密策略强制执行”。尝试这样做将失败并返回 EXDEV。但是，加密文件可以在加密目录内重命名，或链接到未加密目录。
    - 注意：“移动”未加密文件到加密目录（例如使用 `mv` 程序）在用户态是通过复制后删除实现的。请注意，原始未加密数据可能仍可从此处回收。最好从一开始就保持所有文件加密。可以使用 `shred` 程序覆盖源文件，但不能保证在所有文件系统和存储设备上有效。
- 仅在某些情况下支持加密文件上的 Direct I/O。详情见“Direct I/O 支持”。
- fallocate 操作 `FALLOC_FL_COLLAPSE_RANGE` 和 `FALLOC_FL_INSERT_RANGE` 不支持加密文件，并将失败并返回 EOPNOTSUPP。
- 不支持加密文件的在线碎片整理。`EXT4_IOC_MOVE_EXT` 和 `F2FS_IOC_MOVE_RANGE` ioctl 将失败并返回 EOPNOTSUPP。
- ext4 文件系统不支持加密普通文件的数据日志记录。它将回退到有序数据模式。
- DAX (Direct Access) 不支持加密文件。
- 加密符号链接的最大长度比未加密符号链接的最大长度少 2 字节。例如，在块大小为 4K 的 EXT4 文件系统上，未加密符号链接最长可达 4095 字节，而加密符号链接最长只能达到 4093 字节（两者均不包括终止 null）。

注意，mmap *是* 支持的。这是因为加密文件的页面缓存包含明文，而不是密文。

### 无密钥时

即使在添加加密密钥之前或移除加密密钥之后，也可以在加密的普通文件、目录和符号链接上执行某些文件系统操作：
- 可以读取文件元数据，例如使用 `stat()`。
- 可以列出目录，此时文件名将以从其密文派生的编码形式列出。当前的编码算法在“文件名哈希和编码”中描述。该算法可能会更改，但保证呈现的文件名长度不超过 NAME_MAX 字节，不包含 `/` 或 `\0` 字符，并且唯一标识目录条目。
    - `.` 和 `..` 目录条目是特殊的。它们始终存在，且不加密或不编码。
- 可以删除文件。即，非目录文件可以像往常一样使用 `unlink()` 删除，空目录可以像往常一样使用 `rmdir()` 删除。因此，`rm` 和 `rm -r` 将按预期工作。
- 可以读取和跟随符号链接目标，但它们将以加密形式呈现，类似于目录中的文件名。因此，它们不太可能指向任何有用的地方。

没有密钥时，无法打开或截断普通文件。尝试这样做将失败并返回 ENOKEY。这意味着任何需要文件描述符的普通文件操作（如 `read()`, `write()`, `mmap()`, `fallocate()` 和 `ioctl()`）也被禁止。

没有密钥时，任何类型的文件（包括目录）都无法创建或链接到加密目录中，加密目录中的名称也不能是重命名的源或目标，也无法在加密目录中创建 O_TMPFILE 临时文件。所有此类操作都将失败并返回 ENOKEY。

目前，在没有加密密钥的情况下无法备份和还原加密文件。这需要尚未实现的特殊 API。

## 加密策略强制执行

在目录上设置加密策略后，在该目录中创建的所有普通文件、目录和符号链接（递归）都将继承该加密策略。特殊文件——即命名管道、设备节点和 UNIX 域套接字——将不会被加密。

除了这些特殊文件外，加密目录树中不允许存在未加密的文件，或用不同加密策略加密的文件。尝试将此类文件链接或重命名到加密目录中将失败并返回 EXDEV。这在 ->lookup() 期间也会强制执行，以提供有限的保护，防止离线攻击试图禁用或降级已知位置（应用程序以后可能写入敏感数据）的加密。建议实施“验证启动”形式的系统利用这一点，在访问之前验证所有顶级加密策略。

## 内联加密支持

许多 newer 系统（尤其是移动 SoC）具有*内联加密硬件*，可以在数据往返存储设备时加密/解密数据。Linux 通过一组称为 *blk-crypto* 的块层扩展支持内联加密。blk-crypto 允许文件系统将加密上下文附加到 bios（I/O 请求）以指定如何在线加密/解密数据。有关 blk-crypto 的更多信息，请参阅 :ref:`Documentation/block/inline-encryption.rst <inline_encryption>`。

在支持的文件系统上（目前为 ext4 和 f2fs），fscrypt 可以使用 blk-crypto 代替内核加密 API 来加密/解密文件内容。要启用此功能，请在内核配置中设置 `CONFIG_FS_ENCRYPTION_INLINE_CRYPT=y`，并在挂载文件系统时指定 "inlinecrypt" 挂载选项。

注意，"inlinecrypt" 挂载选项仅指定在可能时使用内联加密；它不强制使用。如果内联加密硬件缺乏所需的加密能力（例如，支持所需的加密算法和数据单元大小）且 blk-crypto-fallback 不可用，fscrypt 仍将回退到使用内核加密 API。（要使 blk-crypto-fallback 可用，必须在内核配置中启用 `CONFIG_BLK_INLINE_ENCRYPTION_FALLBACK=y`。）

目前，fscrypt 始终使用文件系统块大小（通常为 4096 字节）作为数据单元大小。因此，它只能使用支持该数据单元大小的内联加密硬件。

内联加密不影响磁盘格式上的密文或其他方面，因此用户可以自由地在是否使用 "inlinecrypt" 之间切换。

## Direct I/O 支持

要使加密文件上的 Direct I/O 正常工作，必须满足以下条件（除了未加密文件上 Direct I/O 的条件之外）：
* 文件必须使用内联加密。通常这意味着文件系统必须使用 `-o inlinecrypt` 挂载，并且必须有内联加密硬件。但是，也有软件回退可用。详情见“内联加密支持”。
* I/O 请求必须完全与文件系统块大小对齐。这意味着 I/O 定位的文件位置、所有 I/O 段的长度以及所有 I/O 缓冲区的内存地址必须是该值的倍数。注意，文件系统块大小可能大于块设备的逻辑块大小。

如果上述任一条件不满足，则加密文件上的 Direct I/O 将回退到缓冲 I/O。

## 实现细节

### 加密上下文

加密策略在磁盘上由 `struct fscrypt_context_v1` 或 `struct fscrypt_context_v2` 表示。由各个文件系统决定存储位置，但通常存储在隐藏的扩展属性中。它*不应*通过 getxattr() 和 setxattr() 等 xattr 相关系统调用暴露，因为加密 xattr 具有特殊语义。（特别是，如果加密策略被添加到或删除到除空目录之外的任何内容，将会造成很大混乱。）这些结构定义如下：

```c
#define FSCRYPT_FILE_NONCE_SIZE 16
#define FSCRYPT_KEY_DESCRIPTOR_SIZE 8
struct fscrypt_context_v1 {
    u8 version;
    u8 contents_encryption_mode;
    u8 filenames_encryption_mode;
    u8 flags;
    u8 master_key_descriptor[FSCRYPT_KEY_DESCRIPTOR_SIZE];
    u8 nonce[FSCRYPT_FILE_NONCE_SIZE];
};
#define FSCRYPT_KEY_IDENTIFIER_SIZE 16
struct fscrypt_context_v2 {
    u8 version;
    u8 contents_encryption_mode;
    u8 filenames_encryption_mode;
    u8 flags;
    u8 __reserved[4];
    u8 master_key_identifier[FSCRYPT_KEY_IDENTIFIER_SIZE];
    u8 nonce[FSCRYPT_FILE_NONCE_SIZE];
};
```

上下文结构与相应的策略结构（见“设置加密策略”）包含相同的信息，但上下文结构还包含 nonce。nonce 由内核随机生成，用作 KDF 输入或作为调整因子以使不同文件以不同方式加密；见“每文件加密密钥”和“DIRECT_KEY 策略”。

### 数据路径更改

当使用内联加密时，文件系统只需将加密上下文与 bios 关联，以指定块层或内联加密硬件如何加密/解密文件内容。

当不使用内联加密时，文件系统必须自己加密/解密文件内容，如下所述：

对于普通文件的读取路径 (->read_folio())，文件系统可以将密文读入页面缓存并在原地解密。必须持有 folio 锁直到解密完成，以防止 folio 过早对用户可见。

对于普通文件的写入路径 (->writepage())，文件系统不能在页面缓存中原地加密数据，因为必须保留缓存的明文。相反，文件系统必须加密到临时缓冲区或“弹跳页”，然后写入临时缓冲区。某些文件系统（如 UBIFS）无论如何都使用临时缓冲区。其他文件系统（如 ext4 和 F2FS）必须专门为加密分配弹跳页。

### 文件名哈希和编码

现代文件系统通过使用索引目录来加速目录查找。索引目录是按文件名哈希组织的树。当请求 ->lookup() 时，文件系统通常会哈希要查找的文件名，以便快速找到相应的目录条目（如果有的话）。

使用加密时，无论有无加密密钥，都必须支持和高效地进行查找。显然，哈希明文文件名不起作用，因为没有密钥就无法获得明文文件名。（哈希明文文件名还会使文件系统的 fsck 工具无法优化加密目录。）相反，文件系统哈希密文文件名，即实际存储在目录条目中的字节。当被要求用密钥执行 ->lookup() 时，文件系统只需加密用户提供的名称以获得密文。

没有密钥的查找更为复杂。原始密文可能包含 `\0` 和 `/` 字符，这些在文件名中是非法的。因此，readdir() 必须对密文进行 base64url 编码以呈现。对于大多数文件名，这没问题；在 ->lookup() 上，文件系统只需对用户提供的基础 64url 解码名称以返回原始密文。

但是，对于非常长的文件名，base64url 编码会导致文件名长度超过 NAME_MAX。为防止这种情况，readdir() 实际上以一种缩写形式呈现长文件名，该形式编码密文文件名的强“哈希”，以及目录查找所需的可选文件系统特定哈希。这允许文件系统仍然以高度置信度将 ->lookup() 中给出的文件名映射回 readdir() 之前列出的特定目录条目。有关更多详细信息，请参阅源代码中的 `struct fscrypt_nokey_name`。

注意，未来可能会更改文件名在无密钥时向用户态呈现的确切方式。它仅旨在作为一种暂时呈现有效文件名的方法，以便 `rm -r` 等命令在加密目录上按预期工作。

## 测试

要测试 fscrypt，请使用 xfstests，这是 Linux 的事实标准文件系统测试套件。首先，在相关文件系统上运行 "encrypt" 组中的所有测试。还可以使用 'inlinecrypt' 挂载选项运行测试，以测试内联加密支持的实现。例如，使用 `kvm-xfstests <https://github.com/tytso/xfstests-bld/blob/master/Documentation/kvm-quickstart.md>`_ 测试 ext4 和 f2fs 加密：

```bash
kvm-xfstests -c ext4,f2fs -g encrypt
kvm-xfstests -c ext4,f2fs -g encrypt -m inlinecrypt
```

UBIFS 加密也可以用这种方式测试，但应该在单独的命令中进行，并且 kvm-xfstests 需要一些时间来设置模拟的 UBI 卷：

```bash
kvm-xfstests -c ubifs -g encrypt
```

不应有任何测试失败。但是，如果内核加密 API 中未构建所需的算法，则使用非默认加密模式的测试（例如 generic/549 和 generic/550）将被跳过。此外，访问原始块设备的测试（例如 generic/399, generic/548, generic/549, generic/550）将在 UBIFS 上被跳过。

除了运行 "encrypt" 组测试外，对于 ext4 和 f2fs，还可以使用 "test_dummy_encryption" 挂载选项运行大多数 xfstests。此选项导致所有新文件自动使用虚拟密钥加密，而无需进行任何 API 调用。这更全面地测试了加密 I/O 路径。要使用 kvm-xfstests 执行此操作，请使用 "encrypt" 文件系统配置：

```bash
kvm-xfstests -c ext4/encrypt,f2fs/encrypt -g auto
kvm-xfstests -c ext4/encrypt,f2fs/encrypt -g auto -m inlinecrypt
```

因为这将运行比 "-g encrypt" 多得多的测试，所以运行时间要长得多；因此，还要考虑使用 `gce-xfstests <https://github.com/tytso/xfstests-bld/blob/master/Documentation/gce-xfstests.md>`_ 代替 kvm-xfstests：

```bash
gce-xfstests -c ext4/encrypt,f2fs/encrypt -g auto
gce-xfstests -c ext4/encrypt,f2fs/encrypt -g auto -m inlinecrypt
```

