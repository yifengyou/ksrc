# kernel_read_file：内核读文件基础设施（Linux 6.6）

> 阅读目标：吃透"把文件读进内核内存"的统一管道 `kernel_read_file` 家族——模块加载（`finit_module`）、固件加载、kexec 镜像/initramfs、IMA 策略与 X.509 证书**四方共用的四变体 API**；理解 ID 枚举的 X-macro 双表技巧与 `LOADING_*` 平行枚举的**零漂移复用**、`deny_write_access` 的写排斥（与二进制执行同源的 `ETXTBSY` 机制）、读前/读后**双 LSM 门**的配对契约、以及 IMA `read_idmap` 的豁免语义。全部对照 v6.6（fs/kernel_read_file.c、include/linux/kernel_read_file.h、security/security.c、security/integrity/ima/ima_main.c、kernel/module/main.c）。前置：[01-模块布局与加载器](01-模块布局与加载器.md)。

---

## 一、定位：四个子系统的共同管道

```
          finit_module(fd)         request_firmware          kexec_file_load       IMA 策略写入
                │                        │                        │                    │
                ▼                        ▼                        ▼                    ▼
      kernel_read_file(f,...,     kernel_read_file_from_    kernel_read_file_from_  kernel_read_file(f,...
        READING_MODULE)           path_initns(...,            fd(..., READING_       0, ..., READING_POLICY)
                                   READING_FIRMWARE)           KEXEC_IMAGE)               │
                │                        │                   ...KEXEC_INITRAMFS)        ▼
                └────────────────────────┴────────────────────────┴─────────────► kernel_read_file()
                                                            （fs/kernel_read_file.c:35）
```

头文件的设计宣言（kernel_read_file.h:8 原话）：

```c
/* This is a list of *what* is being read, not *how* nor *where*. */
```

——ID 只回答"读的是什么"（LSM 检查的最小语义），"怎么读/从哪读"由四个变体函数各自表达。

## 二、ID 枚举：一个 X-macro，两张表，三个消费者

```c
#define __kernel_read_file_id(id) \
	id(UNKNOWN, unknown)            \
	id(FIRMWARE, firmware)          \
	id(MODULE, kernel-module)       \
	id(KEXEC_IMAGE, kexec-image)    \
	id(KEXEC_INITRAMFS, kexec-initramfs) \
	id(POLICY, security-policy)     \
	id(X509_CERTIFICATE, x509-certificate) \
	id(MAX_ID, )

#define __fid_enumify(ENUM, dummy) READING_ ## ENUM,     /* → enum kernel_read_file_id */
#define __fid_stringify(dummy, str) #str,                 /* → kernel_read_file_str[]  */
```

枚举（`READING_UNKNOWN..READING_MAX_ID`）与字符串表（`"unknown".."x509-certificate"`）**由同一宏展开**——新增 ID 永远不会枚举与字符串漂移。更妙的是 security.h:239-241 的姊妹枚举：

```c
enum kernel_load_data_id {
	__kernel_read_file_id(__data_id_enumify)   /* LOADING_* 平行枚举 */
};
```

`LOADING_FIRMWARE/LOADING_MODULE/...` 复用**同一个** `__kernel_read_file_id` 宏——`READING_*`（读文件）与 `LOADING_*`（收数据，无 file 结构的场景，如固件 sysfs fallback 的用户态注入）两族 ID 保持机械同步。

## 三、核心 kernel_read_file 逐段（krf.c:35-123）

### 3.1 入场三连防御

```c
if (!S_ISREG(file_inode(file)->i_mode))          /* :48 必须是普通文件 */
	return -EINVAL;
ret = deny_write_access(file);                   /* :51 写排斥锁（见 3.3） */
i_size = i_size_read(file_inode(file));
if (i_size <= 0)  → -EINVAL                      /* :56-58 空文件 */
if (i_size > SSIZE_MAX) → -EFBIG                 /* :61-64 注释原话：
	"The file is too big for sane activities."（:60） */
if (!file_size && offset == 0 && i_size > buf_size)  /* :66-68 整读装不下 */
	→ -EFBIG
```

### 3.2 双 LSM 门与 whole_file 契约

```c
whole_file = (offset == 0 && i_size <= buf_size);       /* :71 */
ret = security_kernel_read_file(file, id, whole_file);  /* :72 读前门 */
...
ret = security_kernel_post_read_file(file, *buf, i_size, id);  /* :110 读后门（内容检查）*/
```

`whole_file` 标志是**配对契约**（security.c:3067-3069 注释原话）：post_read_file "must be paired with a prior call to security_kernel_read_file() **that indicated this hook would also be called**"——只有整读（非分块）才保证读后门被调，LSM 依赖此标志决定是否在 pre 阶段做全部工作。

### 3.3 deny_write_access：与 exec 同源的写排斥（include/linux/fs.h:2728-2732）

```c
static inline int deny_write_access(struct file *file)
{
	struct inode *inode = file_inode(file);
	return atomic_dec_unless_positive(&inode->i_writecount) ? 0 : -ETXTBSY;
}
```

`i_writecount` 的**符号即锁**：正数=活跃写者，负数=内核读者持有。与二进制执行（fs/exec.c:938 对 ELF 同样调 deny_write_access——经典 `ETXTBSY`）和 fs-verity 共用同一 inode 机制——**内核读你就要下写锁，防止读一半被改**。读毕 `allow_write_access`（krf.c:122）归还。

### 3.4 读循环与短读防御

```c
if (!*buf)
	*buf = allocated = vmalloc(i_size);            /* :80 需要时分配 */
pos = offset; copied = 0;
while (copied < buf_size) {
	wanted = min_t(size_t, buf_size - copied, i_size - pos);
	bytes = kernel_read(file, *buf + copied, wanted, &pos);
	if (bytes == 0) break;                          /* EOF */
	copied += bytes;
}
if (whole_file) {
	if (pos != i_size) → -EIO                       /* :105-107 整读但没读够=短读 */
	ret = security_kernel_post_read_file(...);
}
```

错误路径只释放**自己分配的**缓冲（`if (allocated)`，:114-117）——调用者预分配的 buf（`request_firmware_into_buf` 场景）所有权归调用者。

## 四、四个变体：三种"从哪读"

| 变体 | 打开方式 | 语义 | 主消费者 |
| :--- | :--- | :--- | :--- |
| `kernel_read_file`（:35） | 已有 `struct file *` | 基元 | module 的 `init_module_from_file`（main.c:3123→:3129）、loadpin（:302）、IMA 策略（ima_fs.c:288） |
| `..._from_path`（:127） | `filp_open(O_RDONLY)`（:137） | **调用者的** mount namespace 与凭证 | 少数路径内使用者 |
| `..._from_path_initns`（:147） | `task_lock(&init_task); get_fs_root(init_task.fs); file_open_root`（krf.c:159-163） | **init 的**根——不受调用者 nsview 影响 | 固件加载（fw main.c:548，[driver-model/04](../../driver-model/docs/04-firmware加载与request_firmware生命周期.md) 的 :547 注释回响） |
| `..._from_fd`（:174） | `fdget`（:178）+ `FMODE_READ` 检查（:181，失败 -EBADF） | 用户态递来的 fd | kexec_file.c:199（KEXEC_IMAGE）/:220（KEXEC_INITRAMFS）、finit_module 链 |

module 侧的完整链（已核对 6.6）：`SYSCALL_DEFINE3(finit_module)`（main.c:3169）→ `may_init_module` → `fdget(fd)`（:3185）→ **`idempotent_init_module`**（:3186——并发去重层，**v6.5 引入**（commit 9b9879fc0327，merge-base 实证；详见 [07 章](07-idempotent模块加载.md)））→ `init_module_from_file`（:3123）→ `kernel_read_file(f, 0, &buf, INT_MAX, NULL, READING_MODULE)`（:3129）——之后才进入 module/01 章的十四段流水线与 module/04 章的签名验证。

## 五、双 LSM 门的分发与 IMA 直连（security.c:3048-3085）

```c
int security_kernel_read_file(struct file *file, enum kernel_read_file_id id, bool contents)
{
	ret = call_int_hook(kernel_read_file, 0, file, id, contents);  /* LSM 链 */
	if (ret) return ret;
	return ima_read_file(file, id, contents);                      /* ★IMA 直连（不走链）*/
}
/* post 同构：call_int_hook(kernel_post_read_file) → ima_post_read_file */
```

IMA 被硬编码追加在 LSM 链之后（integrity 子系统不在 `security_hook_heads` 里）——同一模式的姊妹族：`security_kernel_load_data → ima_load_data`。

**IMA 侧的映射与豁免**（ima_main.c:767-773/:797，已核对）：

```c
const int read_idmap[READING_MAX_ID] = {
	[READING_FIRMWARE] = FIRMWARE_CHECK,
	[READING_MODULE] = MODULE_CHECK,
	[READING_KEXEC_IMAGE] = KEXEC_KERNEL_CHECK,
	[READING_KEXEC_INITRAMFS] = KEXEC_INITRAMFS_CHECK,
	[READING_POLICY] = POLICY_CHECK
};
...
int ima_post_read_file(...)
{
	/* permit signed certs */
	if (!file && read_id == READING_X509_CERTIFICATE)   /* :797 X509 无 file 豁免 */
		return 0;
```

`READING_X509_CERTIFICATE`（digsig.c:201，系统密钥环装证书）**无 read_idmap 映射**——签名链上游的证书本身免 IMA 度量（否则鸡生蛋）。

## 六、观测实战

```bash
# 1) 谁在读什么（tracepoint：无专用 TP；用 LSM 挂钩观测）
#    ima policy 打开 func=MODULE_CHECK 等即可在 ascii_runtime_measurements 看到每次加载
cat /sys/kernel/security/ima/ascii_runtime_measurements | tail

# 2) 排障速用
dmesg | grep -i 'ETXTBSY'           # 写排斥命中：文件正被写时内核要读
# 模块读失败的统计（CONFIG_MODULE_STATS，debugfs）：
ls /sys/kernel/debug/modules/        # failed_kreads/failed_decompress/
                                     # invalid_kread_bytes... 与汇总 stats 文件
                                     # （main.c:3362 建目录；stats.c:421-426 计数）

# 3) loadpin（把所有 READING_* 钉死在指定只读文件系统）：
#    开关：/proc/sys/kernel/loadpin/enforce（loadpin.c:57 注册）
#    信任根 = 内核自身所在且只读的文件系统（:71-89 "read-only block" 语义；
#    可选 CONFIG_SECURITY_LOADPIN_VERITY 以 dm-verity 摘要扩展信任根 :278）

# 4) kdump 语境：kexec -l 走 kexec_file_load 系统调用才会过 kernel_read_file
#    （老 kexec_load 传内存指针、绕过本管道——见 ras/kdump 分册的签名对照）
```

## 七、思考题

1. `deny_write_access` 用 `atomic_dec_unless_positive` 而非一把专门锁——多读者并发时 i_writecount 可以到多负？这与 `request_firmware` 的五级路径搜索组合时，哪一级可能撞 `ETXTBSY`？（提示：updates 目录正在被包管理器写入。）
2. `from_path_initns` 为什么要 `task_lock(&init_task)` 而不是直接读 `init_task.fs->root`？（提示：fs 结构的引用计数与并发换根；对比 firmware 章的 `override_creds`——ns 与 creds 是两件事。）
3. `whole_file=false` 时 post 门不调——一个 LSM 若在 pre 门里只记了"即将读"而把内容检查押在 post，会发生什么？（提示：chunked 读的内容检查责任转移给谁？）
4. `read_idmap` 缺 `READING_X509_CERTIFICATE` 映射是疏忽还是设计？（从 :797 的 "permit signed certs" 注释与信任链的根出发。）
5. `LOADING_*` 与 `READING_*` 复用一个 X-macro——如果未来加 `id(BPF_BYTECODE, bpf-bytecode)`，哪三张表自动同步？代价是什么？（提示：uapi 稳定性——这是 kernel 内部枚举，用户态可见吗？）
6. `finit_module` 的 `idempotent_init_module` 层（main.c:3186）在 `kernel_read_file` **之前**还是之后介入？重复 insmod 同一 .ko 时第二次调用还会走到 READING_MODULE 门吗？（提示：等待者路径 `wait_for_completion`。）

---

> 回看：[01-模块布局与加载器](01-模块布局与加载器.md)（READING_MODULE 的下游流水线）｜[04-模块签名链](04-模块签名链.md)（读入之后的验证）｜[driver-model/04-firmware 加载](../../driver-model/docs/04-firmware加载与request_firmware生命周期.md)（from_path_initns 的主顾）｜[ras/kdump/01-kexec 加载与跳转](../../ras/kdump/docs/01-kexec加载与跳转.md)（KEXEC_IMAGE 双路径的分叉点）｜[init/05-async 异步基础设施](../../init/docs/05-async异步基础设施.md)（kmod 死锁警戒线的另一侧）
