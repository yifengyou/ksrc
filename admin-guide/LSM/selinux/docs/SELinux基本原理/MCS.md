# MCS

# MCS - Multi-Category Security（多类别安全）

## 概念

核心概念：什么是 MCS？

MCS (Multi-Category Security) 是 SELinux 在 **TE (Type Enforcement)** 基础之上扩展的一种**标签隔离机制**。它通过给进程和资源打上“类别（Category）”标签，实现同一类型（Type）内的细粒度隔离。

从程序员的角度来看，如果说 TE 解决了“什么类型的进程能访问什么类型的资源”的问题，那么 MCS 解决的就是“**同类型的进程中，谁只能访问自己的数据，谁能访问共享数据**”的问题。

它的核心思想是：**在类型（Type）相同的前提下，利用集合论（子集关系）来控制访问权限。**

### 1. 为什么需要 MCS？

在纯 TE 模型中，所有运行在 `httpd_t` 域的 Apache 进程，理论上都可以访问所有标记为 `httpd_sys_content_t` 的文件。
*   **场景问题**：如果你在一台服务器上托管了 100 个不同的网站（虚拟主机），你希望网站 A 的进程只能读取网站 A 的文件，绝对不能读取网站 B 的文件。
*   **TE 的局限**：如果给每个网站创建一个新的 Type（如 `httpd_siteA_t`, `httpd_siteB_t`），策略文件会爆炸式增长，管理极其困难。
*   **MCS 的方案**：所有网站进程都运行在 `httpd_t` 域，所有网站文件都标记为 `httpd_sys_content_t` 类型。**区别在于附加的“类别（Category）”标签**。

### 2. MCS 的标签结构

在 SELinux 上下文中，MCS 标签通常出现在用户和角色之后，类型之前或之中（取决于具体配置，通常是 `s0:cXX` 形式）。

一个完整的 SELinux 上下文示例：
`system_u:system_r:httpd_t:s0:c100,c200`

*   `system_u`: 用户 (User)
*   `system_r`: 角色 (Role)
*   `httpd_t`: 类型 (Type) - **这是 TE 控制的层级**
*   `s0`: 敏感度级别 (Level/Sensitivity) - 通常用于 MLS (Multi-Level Security)，在普通 MCS 中常固定为 `s0`。
*   `c100,c200`: **类别 (Categories)** - **这是 MCS 控制的核心**。

**类别表示法**：
*   单个类别：`c100`
*   范围/集合：`c100.c200` (表示从 100 到 200 的所有类别)
*   组合：`c100,c205` (表示类别 100 和 205 的集合)

### 3. MCS 的访问规则（集合论）

MCS 的权限检查逻辑基于**集合包含关系**，而不是像 TE 那样的显式 `allow` 规则。

假设：
*   **主体 (Subject)** 进程 P 的标签：`httpd_t:s0:c100` (拥有集合 {100})
*   **客体 (Object)** 文件 F 的标签：`httpd_sys_content_t:s0:c100` (拥有集合 {100})

**访问判定逻辑**：
1.  **读操作 (Read)**：主体的类别集合必须 **包含 (Superset)** 客体的类别集合。
    *   `{100}` ⊇ `{100}` -> **允许**
    *   如果文件是 `c100,c200`，进程只有 `c100` -> `{100}` ⊇ `{100, 200}`? **False** -> **拒绝** (进程没资格读属于 200 的数据)。

2.  **写操作 (Write)**：主体的类别集合必须 **等于 (Equal)** 客体的类别集合（严格模式下），或者是客体的超集（取决于具体策略配置，通常要求精确匹配以防止污染）。
    *   在标准的 Docker/容器化场景中，通常要求 **完全匹配** 才能写入，或者主体必须拥有客体所有的类别。

3.  **默认隔离**：
    *   进程 A (`c100`) 无法访问 文件 B (`c200`)。即使它们都是 `httpd_t` 和 `httpd_sys_content_t` 类型。
    *   这就是**同类型隔离**。

---

## 6.6.0 内核 selinux MCS 示例

在 Linux 6.6.0 内核中，MCS 的实现并不是独立于 TE 的另一套代码路径，而是**深度集成在 TE 的权限检查流程中**。

当 `avc_has_perm` 进行权限检查时，它不仅比较 `ssid` (Source ID) 和 `tsid` (Target ID) 对应的类型，还会提取这两个 SID 中包含的 **MLS/MCS 级别信息 (Level)** 并进行集合运算。

### 1. 核心调用链路 (Call Path)

MCS 的检查点位于 TE 查找之后，最终决策之前。

1.  **VFS/LSM**: `security_file_open` -> `selinux_file_open` (与 TE 相同)。
2.  **AVC 入口**: `avc_has_perm`。
3.  **策略计算**: `security_compute_av_flags`。
4.  **MCS 关键检查**: `mls_context_isvalid` (加载时检查) 和 **`mls_dominate` / `mls_level_eq`** (运行时检查)。
    *   在 6.6 内核中，具体的逻辑主要在 `security/selinux/ss/mls.c` 和 `services.c` 中。
    *   如果在策略中启用了 `mls_constrained` 或 `mls_validated`，MCS 检查会成为 `av_decision` 生成的一部分。

---

### 2. 详细代码路径与逻辑分析 (基于 6.6.0)

#### 第一步：上下文解析与 SID 映射

当进程和文件被访问时，它们的上下文字符串（如 `s0:c100`）已经被解析并映射为内部的 `struct level_datum` 或包含在 `struct sidtab_entry` 中。

在 `security/selinux/ss/sidtab.c` 中，SID 不仅存储类型索引，还存储关联的安全级别（Level）。

```c
// security/selinux/ss/sidtab.h (简化概念)
struct sidtab_entry {
    struct context ctx; // 包含 user, role, type, 和 level (MCS 信息)
    // ...
};

struct context {
    u32 user;
    u32 role;
    u32 type;
    struct mls_level level; // 关键点：存储 s0:c100,c200 等信息
};

struct mls_level {
    u32 sens;       // 敏感度 (s0, s1...)
    u32 cat;        // 类别位图 (实际实现通常是位图或指针到类别集合)
    // 在 6.6 中，类别通常由 ebitmap 结构高效存储
    struct ebitmap cat; 
};
```

#### 第二步：权限计算中的 MCS 介入 (`security_compute_av_flags`)

这是核心逻辑所在。在计算出 TE 允许的权限后，内核必须检查 MCS 约束。

```c
// security/selinux/ss/services.c (Linux 6.6 逻辑流)
int security_compute_av_flags(...) 
{
    // ... 1. 先进行 TE 查找 (avtab_search_node) 获取基础允许权限 ...
    struct avtab_node *te_node = avtab_search_node(...);
    u32 allowed_perms = te_node ? te_node->datum.data : 0;

    if (!allowed_perms)
        return 0; // TE 已经拒绝，无需检查 MCS

    // ... 2. 获取源和目标的 MLS/MCS 级别 ...
    struct context *sctx = sidtab_get_entry(sidtab, ssid);
    struct context *tctx = sidtab_get_entry(sidtab, tsid);
    
    struct mls_level *slevel = &sctx->level;
    struct mls_level *tlevel = &tctx->level;

    // 3. 检查策略是否对该类操作启用了 MLS/MCS 约束
    // policydb->policycap 中可能有相关标志，或者在 class 定义中有 constraints
    struct constraint_node *constraints = policydb->policy_constraints[tclass];
    
    u32 final_allowed = allowed_perms;

    // 遍历约束条件 (Constraints)
    for (struct constraint_node *c = constraints; c; c = c->next) {
        if (c->expr.flags & CELLS_MLS) { // 如果约束涉及 MLS/MCS
            // 执行表达式求值，例如：m1 dom m2 (m1 dominates m2)
            if (!constraint_expr_eval(state, sctx, tctx, c->expr)) {
                // 如果 MCS 检查失败，从允许掩码中剔除相应权限
                final_allowed &= ~c->permissions; 
            }
        }
    }
    
    avd->allowed = final_allowed;
    return 0;
}
```

#### 第三步：核心集合运算 (`mls.c`)

实际的集合比较逻辑在 `security/selinux/ss/mls.c` 中实现。对于 MCS，主要关注类别（Categories）的位图操作。

**场景：读操作 (Read) - 要求主体支配 (Dominate) 客体**
即：主体的类别集合 $\supseteq$ 客体的类别集合。

```c
// security/selinux/ss/mls.c
// 检查 slevel 是否 "dominate" (包含) tlevel
// 返回 1 表示真 (允许)，0 表示假 (拒绝)
int mls_level_dominate(const struct mls_level *slevel, const struct mls_level *tlevel)
{
    // 1. 检查敏感度 (Sensitivity)，通常 MCS 中都是 s0，这一步通常通过
    if (slevel->sens < tlevel->sens)
        return 0;

    // 2. 检查类别 (Categories) - 核心位图操作，每个类别一个bit
    // ebitmap_contains 检查 slevel->cat 是否包含 tlevel->cat 的所有位
    // 这是一个高效的位图操作 (Bitmap operation)
    if (!ebitmap_contains(&slevel->cat, &tlevel->cat))
        return 0;

    return 1;
}
```

**场景：写操作 (Write) - 通常要求相等 (Equal)**
防止低安全级别的进程污染高安全级别的文件，或者不同隔离域的进程互相污染。

```c
// security/selinux/ss/mls.c
int mls_level_eq(const struct mls_level *slevel, const struct mls_level *tlevel)
{
    if (slevel->sens != tlevel->sens)
        return 0;
    
    // 检查两个位图是否完全一致
    if (!ebitmap_equal(&slevel->cat, &tlevel->cat))
        return 0;

    return 1;
}
```

#### 第四步：约束表达式 (Constraint Expressions)

MCS 的规则不是硬编码在 C 语言里的，而是定义在策略文件 (`policy.conf`) 的 **约束 (Constraints)** 中。内核只是解释执行这些约束。

在策略源码中，你会看到类似这样的定义（针对 `file` 类）：

```selinux
# 伪代码：策略文件中的约束定义
constraint file { read getattr open }
    (( l1 dom l2 ) or ( t1 == mlsfileread ) );
    # 含义：对于读操作，要么 主体级别(l1) 支配 客体级别(l2)，
    # 要么 类型具有特殊的免检属性 (mlsfileread)。

constraint file { write create unlink }
    (( l1 eq l2 ) or ( t1 == mlsfilewrite ) );
    # 含义：对于写操作，要么 主体级别(l1) 等于 客体级别(l2)。
```

在 6.6 内核的 `services.c` 中，`constraint_expr_eval` 函数会解析这些逻辑：
*   `l1 dom l2` -> 调用 `mls_level_dominate(slevel, tlevel)`
*   `l1 eq l2` -> 调用 `mls_level_eq(slevel, tlevel)`
*   `l1 incomp l2` -> 检查是否不兼容（用于禁止特定通信）

---

### 3. 实际应用案例：Docker 与 MCS

这是理解 MCS 最直观的场景。

1.  **启动容器 A**:
    *   Docker 守护进程请求创建一个隔离环境。
    *   SELinux 策略分配随机类别：`c100, c101`。
    *   容器主进程标签：`system_u:system_r:container_t:s0:c100,c101`。
    *   容器内文件标签：`system_u:object_r:container_file_t:s0:c100,c101`。

2.  **启动容器 B**:
    *   分配随机类别：`c102, c103`。
    *   容器主进程标签：`system_u:system_r:container_t:s0:c102,c103`。
    *   容器内文件标签：`system_u:object_r:container_file_t:s0:c102,c103`。

3.  **攻击尝试**:
    *   假设容器 A 被攻破，黑客试图读取 `/var/lib/docker/.../container_B/data`。
    *   **TE 检查**: 进程是 `container_t`，文件是 `container_file_t`。策略允许 `allow container_t container_file_t:file read`。**TE 通过**。
    *   **MCS 检查**:
        *   主体级别：`{c100, c101}`
        *   客体级别：`{c102, c103}`
        *   规则：读操作要求 `Subject >= Object` (Dominate)。
        *   判断：`{c100, c101}` 包含 `{c102, c103}` 吗？ **否**。
    *   **结果**: **内核拒绝访问 (-EACCES)**。审计日志显示 `denied { read } ... scontext=...:c100,c101 tcontext=...:c102,c103`。

---

### 4. 结论：完整的 MCS 检查流程图 (6.6.0)

1.  **System Call**: 进程发起文件访问。
2.  **TE 预检**: `avtab_search` 找到 `(Type_Subject, Type_Object, Class)` 的 `allow` 规则。
    *   若无 TE 规则 -> **拒绝** (流程结束)。
    *   若有 TE 规则 -> 获得 `base_perms`，进入 MCS 检查。
3.  **获取级别**: 从 `sidtab` 中提取 Subject 和 Object 的 `mls_level` (包含 `sens` 和 `cat` 位图)。
4.  **约束评估 (`constraint_expr_eval`)**:
    *   读取策略中针对该 `Class` 和 `Permission` 定义的约束表达式。
    *   执行位图运算 (`ebitmap_contains`, `ebitmap_equal`)。
    *   **读场景**: 检查 `Sub_Cat` ⊇ `Obj_Cat`。
    *   **写场景**: 检查 `Sub_Cat` == `Obj_Cat`。
5.  **决策合并**:
    *   若约束满足 -> 保留 `base_perms`。
    *   若约束失败 -> 将对应权限位从 `base_perms` 中清零。
6.  **最终结果**:
    *   若剩余权限包含请求的操作 -> **允许**。
    *   否则 -> **拒绝**，并记录审计日志。

### 总结对比

| 特性 | TE (Type Enforcement) | MCS (Multi-Category Security) |
| :--- | :--- | :--- |
| **核心对象** | 类型 (Type) | 类别 (Category) |
| **逻辑基础** | 查表 (Lookup Table / AVTAB) | 集合论 (Set Theory / Bitmap) |
| **主要解决的问题** | 不同功能间的隔离 (Web vs DB) | 同类功能实例间的隔离 (Site A vs Site B) |
| **策略复杂度** | 高 (需定义大量 allow 规则) | 低 (规则通用，标签动态分配) |
| **内核实现位置** | `avtab.c`, `services.c` (查找) | `mls.c`, `services.c` (位图运算) |
| **数据结构** | 哈希表/红黑树 (`avtab_node`) | 位图 (`ebitmap`) |
| **默认行为** | 默认拒绝，除非显式允许 | 在允许的类型基础上，默认拒绝跨类别访问 |

在 Linux 6.6.0 中，这两者是无缝融合的：**TE 决定了“能不能做这类事”，MCS 决定了“能不能做这件事里的这部分数据”。**