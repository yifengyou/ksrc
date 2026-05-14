# MLS - Multi-Level Security（多级安全）

## 概念

核心概念：什么是 MLS？

**MLS (Multi-Level Security，多级安全)** 是 SELinux 中基于 **敏感度（Sensitivity）** 和 **范畴（Category）** 的强制访问控制模型。它主要用于处理具有不同保密级别的数据（如：绝密、机密、秘密、公开），确保信息只能从低安全级别流向高安全级别（或同级），防止高密级信息泄露给低密级主体。

从程序员的角度来看，如果说 TE (Type Enforcement) 是基于“角色/功能”的白名单（例如：Web服务器只能访问网页文件），那么 **MLS 就是基于“数据保密等级”的围栏**。

它的核心思想遵循 **Bell-LaPadula 模型** 的两个主要规则：
1.  **简单安全属性 (No Read Up, NRU)**：主体不能读取高于其安全级别的客体。（防止低密级用户看高密级文件）。
2.  **星号属性 (No Write Down, NWD)**：主体不能写入低于其安全级别的客体。（防止高密级用户无意或恶意将机密数据泄露到低密级文件中）。

在 SELinux 的 MLS 模型中，安全上下文（Security Context）不仅包含类型（Type），还包含一个 **范围（Range）**，格式通常为：`sensitivity:category`。

*   **敏感度 (Sensitivity)**：表示保密等级，通常是有序的层级。例如：`s0` (最低/公开), `s1`, `s2` ... `s15` (最高/绝密)。
*   **范畴 (Category)**：表示 compartments（隔间），用于在同一敏感度下进行逻辑隔离，通常是无序的集合。例如：`c0`, `c1`, `c0.c5` (表示 c0 到 c5 的范围), `c0,c2` (表示离散的集合)。

**完整的安全上下文示例**：
*   用户进程：`user_u:user_r:user_t:s0:c0.c10` (可以访问 s0 级别，且范畴在 c0 到 c10 之间的数据)
*   绝密文件：`system_u:object_r:secret_file_t:s15:c0.c5`

**关键点**：即使 TE 规则允许 `user_t` 访问 `secret_file_t` 类型，如果 MLS 级别不匹配（例如用户是 `s0`，文件是 `s15`），内核依然会拒绝访问。MLS 检查发生在 TE 检查之后（或者说作为权限计算的一部分），拥有“一票否决权”。

---

## 6.6.0 内核 SELinux MLS 示例

在 Linux 6.6.0 内核中，MLS 的逻辑紧密集成在权限计算引擎中。与 TE 主要依赖 `avtab` 查找不同，MLS 的核心在于对 **安全级别（Level）** 的比较运算。

现代内核将 MLS 的策略规则编译成高效的查找表（MLS 规则表），并在 `security/selinux/ss/services.c` 中进行快速比对。

### 1. 核心调用链路 (Call Path)

当一个进程尝试访问资源时，MLS 检查嵌入在标准的权限计算流程中：

1.  **入口**: `avc_has_perm()` (同 TE 流程)
2.  **策略计算**: `security_compute_av_flags()`
3.  **MLS 验证**: `mls_context_isvalid()` (加载策略时验证) -> **`mls_compute_sid()`** (动态计算) -> **`mls_allowed()`** (核心检查)
4.  **决策合并**: 将 MLS 的检查结果（允许/拒绝）与 TE 的 `avtab` 查找结果进行逻辑“与”运算。

---

### 2. 详细代码路径与逻辑分析 (基于 6.6.0)

#### 第一步：上下文结构中的 MLS 信息

在 6.6 内核中，安全标识符 (SID) 映射到具体的上下文结构 `struct context` (定义在 `security/selinux/ss/context.h`)。

```c
// security/selinux/ss/context.h
struct context {
    u32 user;    // 用户 ID
    u32 role;    // 角色 ID
    u32 type;    // 类型 ID (TE 用)
    struct mls_level range; // MLS 范围 (核心所在)
};

// security/selinux/ss/mls_types.h
struct mls_level {
    u32 sens;           // 敏感度 (s0, s1...)
    u32 cat;[BITMAP];   // 范畴位图 (哪些 c0, c1... 被启用)
};
```

#### 第二步：触发 MLS 检查 (`security_compute_av_flags`)

当 `avc_has_perm` 发现缓存未命中，调用 `security_compute_av_flags` 时，MLS 检查随之启动。

```c
// security/selinux/ss/services.c
int security_compute_av_flags(const struct selinux_state *state,
                              u32 ssid, u32 tsid, u16 tclass, u32 requested,
                              struct av_decision *avd)
{
    struct context *scontext = NULL; // 主体上下文
    struct context *tcontext = NULL; // 客体上下文
    // ... 获取上下文指针 ...

    // 1. 先进行基础的 TE 查找 (avtab_search)
    // ... (参考之前 TE 部分的代码) ...
    u32 allowed = te_avtab_search(...); 

    // 2. 如果启用了 MLS，执行 MLS 检查
    if (policydb->mls) {
        u32 mls_allowed;
        
        // 核心调用：检查主体范围是否允许访问客体范围
        // 这实现了 "No Read Up" 和 "No Write Down" 逻辑
        mls_allowed = mls_compute_av(state, policydb, scontext, tcontext, tclass, requested);
        
        // 3. 合并结果：必须同时满足 TE 和 MLS
        allowed &= mls_allowed;
        
        // 如果 MLS 拒绝，即使 TE 允许，最终结果也是 0 (拒绝)
        if (!allowed) {
             // 可选：记录详细的 MLS 拒绝原因到审计日志
        }
    }
    
    avd->allowed = allowed;
    return 0;
}
```

#### 第三步：核心 MLS 逻辑 (`mls_compute_av`)

这是 MLS 的心脏，位于 `security/selinux/ss/mls.c`。它根据 Bell-LaPadula 模型执行比较。

```c
// security/selinux/ss/mls.c
u32 mls_compute_av(struct policydb *policydb,
                   struct context *scontext, // 主体 (进程)
                   struct context *tcontext, // 客体 (文件)
                   uint16_t tclass,          // 类别 (文件、目录等)
                   uint32_t requested)       // 请求的权限 (读、写)
{
    struct mls_level *slevel = &scontext->range.level[0]; // 主体当前级别
    struct mls_level *tlevel = &tcontext->range.level[0]; // 客体级别
    u32 permitted = 0;

    // 遍历请求的权限位 (读、写、执行等)
    for (int i = 0; i < 32; i++) {
        if (!(requested & (1 << i))) continue;
        
        int perm_class = i + 1; 
        // 获取该类别下该权限对应的 MLS 规则 (读规则、写规则等)
        // 策略中定义了比如：file__read 需要满足什么 MLS 条件
        struct mls_rule *rule = get_mls_rule(policydb, tclass, perm_class);
        
        if (rule) {
            // 执行具体的比较逻辑
            if (mls_level_dom(rule->l1, slevel) && // 主体级别 支配/等于 规则要求的最低级别
                mls_level_dom(tlevel, rule->l2)) { // 客体级别 支配/等于 规则要求的最高级别
                
                permitted |= (1 << i); // 允许该权限
            }
        }
    }
    return permitted;
}
```

**关键的比较函数 (`mls_level_dom`)**：
这个函数实现了数学上的偏序关系判断。

```c
// security/selinux/ss/mls.c
// 判断 level1 是否 "支配" (dominates) level2
// 即：level1.sens >= level2.sens  AND  level2.categories 是 level1.categories 的子集
static inline int mls_level_dom(const struct mls_level *l1, const struct mls_level *l2)
{
    // 1. 检查敏感度 (Sensitivity)
    // l1 的敏感度必须 >= l2 的敏感度
    if (l1->sens < l2->sens)
        return 0;

    // 2. 检查范畴 (Categories)
    // l2 的所有范畴必须包含在 l1 中 (l1 必须拥有 l2 的所有 category)
    // 使用位图操作高效检查子集关系
    // ebitmap_contains(&l1->cat, &l2->cat)
    if (!ebitmap_contains(&l1->cat, &l2->cat))
        return 0;

    return 1;
}
```

#### 第四步：具体场景分析 (读 vs 写)

策略文件 (`policy.conf`) 中定义了不同操作对应的 MLS 约束。

**场景 A：读文件 (Read)**
*   **规则逻辑**: `mlsconstrain file read (l1 dom l2);`
    *   `l1`: 主体级别 (Subject)
    *   `l2`: 客体级别 (Object)
    *   `dom`: 支配关系 (>=)
*   **含义**: 主体的级别必须 **高于或等于** 客体的级别。
*   **代码行为**: `mls_level_dom(slevel, tlevel)` 必须为真。
*   **结果**: 如果用户是 `s0`，文件是 `s5`，`0 >= 5` 为假 -> **拒绝 (No Read Up)**。

**场景 B：写文件 (Write)**
*   **规则逻辑**: `mlsconstrain file write (l1 domby l2);` (注意是 `domby`，即被支配)
    *   或者更常见的写法是限制流向：`(l1 eq l2)` 或 `(l2 dom l1)` 取决于具体策略配置，但在标准 Bell-LaPadula 中，写操作通常要求 **主体级别 <= 客体级别** (防止写向下)。
    *   实际上，SELinux 默认策略通常使用 `mlsconstrain file write (l1 dom l2);` 的变体或者结合 `role` 限制。
    *   最严格的 **No Write Down** 实现逻辑是：只有当 `tlevel` (客体) 支配 `slevel` (主体) 时，才允许写（即只能向更高或同级的地方写）。
*   **代码行为**: 检查 `mls_level_dom(tlevel, slevel)`。
*   **结果**: 如果用户是 `s10` (机密)，试图写入 `s0` (公开) 的文件。`s0` 不支配 `s10` -> **拒绝 (No Write Down)**。

#### 第五步：范围转换 (Range Transition)

在某些情况下，进程创建文件或改变自身级别时，需要计算新的 MLS 范围。这由 `mls_compute_sid` 处理。

```c
// security/selinux/ss/mls.c
int mls_compute_sid(struct policydb *policydb,
                    struct context *scontext,
                    struct context *tcontext,
                    uint16_t tclass,
                    uint32_t requested,
                    struct context *newcontext)
{
    // 根据策略中定义的 "range_transition" 规则
    // 例如：当 httpd_t (s0) 创建 tmp_t 文件时，自动标记为 (s0)
    // 或者：管理员进程 (s15) 降级运行某个工具时，将其范围限制为 (s5)
    
    struct range_trans *rtr;
    // 查找 range_trans 表
    rtr = find_range_trans(policydb, scontext->type, tcontext->type, tclass);
    
    if (rtr) {
        // 应用转换规则，设置 newcontext->range
        mls_level_set(&newcontext->range.level[0], &rtr->new_range);
    } else {
        // 默认继承或保持
        mls_level_set(&newcontext->range.level[0], &scontext->range.level[0]);
    }
    return 0;
}
```

### 4. 结论：完整的 MLS 检查流程图 (6.6.0)

1.  **系统调用**: 进程发起 `read()` 或 `write()`。
2.  **TE 预检**: 首先检查 Type Enforcement 规则 (httpd_t 能否读 httpd_sys_content_t?)。如果 TE 拒绝，直接返回，无需进行 MLS 计算。
3.  **MLS 触发**: 若 TE 允许，进入 `mls_compute_av`。
4.  **级别提取**: 从 `cred` (主体) 和 `inode` (客体) 中提取 `mls_level` (敏感度 + 范畴位图)。
5.  **规则匹配**: 根据操作类型 (读/写)，查找策略中定义的 `mlsconstrain` 规则。
    *   **读操作**: 检查 `Subject_Level >= Object_Level` (敏感度更高且范畴超集)。
    *   **写操作**: 检查 `Object_Level >= Subject_Level` (防止向下写)。
6.  **位图运算**: 使用 `ebitmap_contains` 快速判断范畴集合的包含关系。
7.  **决策合并**:
    *   `Final_Permission = TE_Allowed & MLS_Allowed`.
8.  **结果返回**:
    *   如果最终权限为空，返回 `-EACCES`。
    *   审计子系统记录拒绝原因（如果是 MLS 导致的拒绝，audit 日志中会明确显示 `mls` 相关的字段，如 `scontext` 和 `tcontext` 的级别差异）。

### 总结对比：TE vs MLS

| 特性 | TE (Type Enforcement) | MLS (Multi-Level Security) |
| :--- | :--- | :--- |
| **核心依据** | **类型标签** (Type/Domain) | **安全级别** (Sensitivity + Category) |
| **主要目的** | 隔离功能，防止程序越权 (如 Web 服不能改系统配置) | 隔离数据密级，防止信息泄露 (如 机密数据不能流向公开) |
| **规则逻辑** | 白名单：`allow domain type : class { perms }` | 偏序关系：`dominates`, `equals`, `in_range` |
| **数据结构** | AVTAB (哈希表/树) | EBitmap (范畴位图) + 敏感度整数 |
| **典型拒绝** | `type=httpd_t` 不允许访问 `type=shadow_t` | `level=s0` 不允许读取 `level=s15` |
| **优先级** | 基础访问控制 | 叠加在 TE 之上的额外强制约束 |

在 Linux 6.6.0 中，这两套机制在内核的 `security/selinux/ss/` 目录下紧密协作，共同构成了强大的强制访问控制系统。任何访问请求必须**同时**通过 TE 的类型检查和 MLS 的级别检查才能成功。