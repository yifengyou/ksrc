# TE -  Type Enforcement（类型强制）

## 概念

核心概念：什么是 TE？

TE 机制通过给系统中的每个进程（主体）和资源（客体，如文件、端口、socket等）打上“类型标签”，然后定义规则：“什么类型的进程”允许对“什么类型的资源”执行“什么操作”。

从程序员的角度来看，SELinux 的 **TE (Type Enforcement, 类型强制)** 模型可以理解为一个在操作系统内核中实现的、基于标签的、白名单式的权限检查系统。它通过在传统的 Linux 权限（DAC）之上增加一层强制访问控制（MAC），来限制进程（主体）对资源（客体）的访问。

它的核心思想是：**默认拒绝一切，除非有明确的规则允许。**

在传统的 Linux 权限（DAC, Discretionary Access Control）中，权限取决于文件的拥有者（Owner）、组（Group）和其他人（Others）的读/写/执行位。只要你是 root，或者文件所有者，通常就能为所欲为。

而在 SELinux 的 TE 模型中，权限取决于标签（Label）：

1. 域（Domain）：运行中的进程被打上的类型标签。例如，Web 服务器进程 httpd 运行在 httpd_t 域中。
2. 类型（Type）：静态资源（文件、目录、端口）被打上的类型标签。例如，Web 网页文件标记为 httpd_sys_content_t。
3. 规则（Rule）：策略文件中定义的允许语句。例如：“允许 httpd_t 域的进程读取 httpd_sys_content_t 类型的文件”。

关键点：即使是 root 用户运行的进程，如果它的域（Domain）没有被策略允许访问某个文件类型，它也会被内核拒绝访问。

## 6.6.0 内核selinux TE示例

在 Linux 6.6.0 内核中，SELinux 的实现逻辑与你提供的伪代码核心思想一致（基于类型强制 TE），但在代码结构、函数命名、数据结构访问方式以及执行路径上已经发生了显著变化。

现代内核（特别是 5.10+ 到 6.x）对 SELinux 进行了大量的重构，包括将部分逻辑移至 `security/selinux` 目录下的独立模块，优化了权限检查缓存（AVC），并改变了安全上下文（security context）的存储和访问方式。

以下是基于 **Linux 6.6.0** 源码的详细实现路径和代码逻辑分析。

### 1. 核心调用链路 (Call Path)

当 Apache (`httpd_t`) 进程调用 `open()` 系统时，内核的执行路径如下：

1.  **VFS 层**: `do_open()` -> `do_dentry_open()` -> `security_file_open()`
2.  **LSM 框架层**: `security_file_open()` 调用所有注册的 LSM 钩子。
3.  **SELinux 钩子**: `selinux_file_open()` (位于 `security/selinux/hooks.c`)
4.  **权限检查核心**: `file_permission()` -> `inode_permission()` -> `avc_has_perm()`
5.  **策略查找**: `avc_has_perm_noaudit()` -> `avc_compute_av()` -> `avtab_search()`

---

### 2. 详细代码路径与逻辑分析 (基于 6.6.0)

#### 第一步：触发钩子 (`security_file_open`)

在 `fs/open.c` 或 `fs/namei.c` 中，文件打开流程会调用安全钩子：

```c
// fs/namei.c (简化)
int do_dentry_open(struct file *f, ...) {
    // ...
    error = security_file_open(f, cred);
    // ...
}
```

在 `include/linux/security.h` 中，`security_file_open` 定义为内联函数，它遍历 LSM 链表：

```c
// include/linux/security.h
static inline int security_file_open(struct file *file, const struct cred *cred)
{
    return call_int_hook(file_open, 0, file, cred);
}
```

#### 第二步：SELinux 具体实现 (`selinux_file_open`)

进入 `security/selinux/hooks.c`。在 6.6 内核中，这个函数不仅检查 `open` 权限，通常还会结合 `ioctl` 等权限，但核心是调用通用的权限检查函数。

```c
// security/selinux/hooks.c (Linux 6.6)
static int selinux_file_open(struct file *file, const struct cred *cred)
{
    struct file_security_struct *fsec = selinux_file(file);
    struct inode_security_struct *isec = selinux_inode(file->f_path.dentry->d_inode);
    // 注意：6.6 中不再直接通过 task->security_context 访问，而是通过 cred (credentials)
    struct task_security_struct *tsec = selinux_cred(cred); 
    u32 perms = file_to_av(file); // 将 open 标志 (O_RDONLY 等) 转换为 SELinux 权限位

    // 关键调用：执行实际的 TE 检查
    return avc_has_perm(&selinux_state, // 全局状态
                        tsec->sid,      // 源 SID (Subject Identity, 即 httpd_t)
                        isec->sid,      // 目标 SID (Object Identity, 即 httpd_sys_content_t)
                        SECCLASS_FILE,  // 类别 (FILE_CLASS)
                        perms,          // 请求的权限掩码
                        &file_audit_data(file));
}
```

**关键变化点**：
*   **上下文获取**：旧代码伪代码中用 `task->security_context`。在现代内核中，进程的安全上下文绑定在 `struct cred` (凭证) 上，通过 `selinux_cred(cred)` 获取 `task_security_struct`，进而拿到 `sid` (Security ID)。
*   **文件上下文**：文件的安全上下文存储在 `struct inode` 的 `i_security` 字段中（由 `selinux_inode` 宏获取），对应 `inode_security_struct`。

#### 第三步：AVC 缓存检查 (`avc_has_perm`)

这是性能优化的关键。内核不会每次都去查庞大的 AVTAB 树，而是先查 **AVC (Access Vector Cache)**。

```c
// security/selinux/avc.c
int avc_has_perm(const struct selinux_state *state,
                 u32 ssid, u32 tsid, u16 tclass, u32 requested,
                 struct avc_audit_data *ad)
{
    struct avc_entry *node;
    struct avc_av_entry *av_entry;
    u32 denied;

    // 1. 尝试从哈希表 (AVC Cache) 中快速查找
    node = avc_lookup(state->avc, ssid, tsid, tclass, requested);
    
    if (node) {
        // 缓存命中
        av_entry = &node->ae.avd;
        denied = requested & ~av_entry->allowed;
        if (!denied)
            return 0; // 允许
        
        // 如果缓存明确记录为拒绝，且不需要审计，直接返回错误
        if ((av_entry->auditallow & requested) == 0 && 
            (av_entry->dontaudit & requested) == 0)
             return -EACCES;
    }

    // 2. 缓存未命中或需要重新计算，调用慢速路径
    return avc_has_perm_noaudit(state, ssid, tsid, tclass, requested, ad, 0);
}
```

#### 第四步：策略引擎查找 (`avc_compute_av` -> `avtab_search`)

如果缓存未命中，内核必须查询加载到内存中的策略数据库（Policy Database）。这发生在 `security/selinux/ss/services.c` (SS = Security Server)。

```c
// security/selinux/ss/services.c
static int avc_has_perm_noaudit(...) 
{
    struct av_decision avd;
    // ...
    // 调用核心策略计算函数
    rc = security_compute_av_flags(state, ssid, tsid, tclass, requested, &avd);
    // ...
}

// security/selinux/ss/services.c
int security_compute_av_flags(const struct selinux_state *state,
                              u32 ssid, u32 tsid, u16 tclass, u32 requested,
                              struct av_decision *avd)
{
    struct policydb *policydb;
    struct sidtab *sidtab;
    // ... 获取当前策略指针 ...
    
    // 核心中的核心：在 AVTAB 中查找
    // 这里的逻辑对应你伪代码中的 avtab_search
    symtab_datum_t *sdatum = sidtab_search(sidtab, ssid);
    symtab_datum_t *tdatum = sidtab_search(sidtab, tsid);
    
    // 构造查询键 (内部实现)
    // key.source_type = sdatum->value; (即 httpd_t 的 integer ID)
    // key.target_type = tdatum->value; (即 httpd_sys_content_t 的 integer ID)
    // key.target_class = tclass;
    
    struct avtab_key key;
    key.source_type = sdatum->value;
    key.target_type = tdatum->value;
    key.target_class = tclass;
    key.specified = AVTAB_ALLOWED; // 我们找的是允许规则

    // 在红黑树或哈希表实现的 AVTAB 中查找
    struct avtab_node *node = avtab_search_node(&policydb->te_avtab, &key);
    
    if (node) {
        avd->allowed = node->datum.data & requested;
        // 处理其他权限位 (auditallow, dontaudit 等)
    } else {
        avd->allowed = 0; // 默认拒绝
    }
    
    // ... 处理默认规则和布尔值 (booleans) ...
    
    return 0;
}
```

#### 第五步：底层数据结构 (`avtab_search_node`)

在 `security/selinux/ss/avtab.c` 中，实际的数据结构查找发生。6.6 内核中的 `avtab` 通常是一个哈希表数组，每个桶包含一个链表或更复杂的结构（取决于编译配置，有时为了性能使用更高效的结构）。

```c
// security/selinux/ss/avtab.c
struct avtab_node *avtab_search_node(struct avtab *h, struct avtab_key *k)
{
    u32 hash;
    struct avtab_node *cur;

    // 计算哈希值
    hash = avtab_hash(k, h->nslot);
    
    // 遍历对应槽位的链表
    for (cur = h->htable[hash]; cur; cur = cur->next) {
        if (cur->key.source_type == k->source_type &&
            cur->key.target_type == k->target_type &&
            cur->key.target_class == k->target_class &&
            (cur->key.specified & k->specified)) {
            
            // 找到匹配项
            // 注意：这里可能还需要检查 key.specified 的具体位 (AVTAB_ALLOWED, AVTAB_AUDITALLOW 等)
            if (k->specified == cur->key.specified) 
                return cur;
        }
    }
    return NULL;
}
```

### 4. 结论：完整的 TE 检查流程图 (6.6.0)

1.  **System Call**: `open()` 触发。
2.  **VFS**: `do_dentry_open` 调用 `security_file_open`。
3.  **LSM Hook**: `selinux_file_open` (in `hooks.c`)。
    *   提取 `ssid` (from `cred`), `tsid` (from `inode`), `tclass` (`FILE`), `perms`。
4.  **AVC Layer**: `avc_has_perm` (in `avc.c`)。
    *   **Hit**: 检查缓存中的 `allowed` 位。如果有，返回 0；如果明确拒绝且非 dontaudit，返回 -EACCES。
    *   **Miss**: 调用 `avc_has_perm_noaudit`。
5.  **Policy Engine**: `security_compute_av_flags` (in `services.c`)。
    *   锁定当前策略 (RCU read lock)。
    *   调用 `avtab_search_node` 在 `policydb->te_avtab` 中查找 `(ssid, tsid, tclass, ALLOWED)`。
    *   应用 Boolean 条件过滤。
    *   生成 `av_decision` 结构（包含 allowed, auditallow, dontaudit 位）。
6.  **Cache Update**: 将结果写入 AVC 缓存，供下次快速访问。
7.  **Result**: 根据计算出的权限位决定返回 0 还是 -EACCES，并决定是否发送审计消息到 `auditd`。

