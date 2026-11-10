# landlock 执行路径与 ABI 演进（Linux 6.6）

> 定位：本篇补齐"怎么跑起来"与"版本演进"两个视角；数据结构与关系详见本分册 [数据结构和关系](数据结构和关系.md)、设计取舍见 [优缺点](优缺点.md)。全部事实对照 v6.6 源码核对。

---

## 一、三个系统调用：沙箱的自装流程（security/landlock/ruleset.c）

```
① landlock_create_ruleset(attr_p, size, flags)
   flags|LANDLOCK_CREATE_RULESET_VERSION → 只返回内核 ABI 版本
   （6.6 返回 3，见下节）探测-降级的标准姿势
   正常调用：attr.allowed_access 位图 → struct landlock_ruleset
   （逐层 access_masks[]，fs.root 空树起步）
② landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
     {parent_fd, allowed_access}, 0)
   path_beneath：以某文件/目录为根挂规则（rbtree 插入）
③ landlock_restrict_self(ruleset_fd, 0)
   ——关键一步：把 ruleset 绑定到 current 的凭据（见下）
```

`restrict_self` 之后规则**不可撤销**（随 cred 终身）；叠加调用会**追加层**（layered，见第四节）。

## 二、注册与存储：LSM blob（security/landlock/setup.c，已核对 6.6）

```c
struct lsm_blob_sizes landlock_blob_sizes = {
	.lbs_cred       = sizeof(struct landlock_cred_security),   /* 域指针 */
	.lbs_file       = sizeof(struct landlock_file_security),   /* open 时的域快照 */
	.lbs_inode      = sizeof(struct landlock_inode_security),  /* object 挂 rule 树 */
	.lbs_superblock = sizeof(struct landlock_superblock_security),
};
DEFINE_LSM(LANDLOCK_NAME) = { .name = "landlock", .init = landlock_init, .blobs = &landlock_blob_sizes };
landlock_init(): add_cred_hooks + add_ptrace_hooks + add_fs_hooks
                 → landlock_initialized = true
```

要点：landlock 的"域"存在 **cred blob** 里（`landlock_cred_security`）——沙箱边界跟凭据走：fork/exec 继承（cred 复制），setuid 也不丢；这是"自限制"语义的存储学。

## 三、执行：hook 到检查的完整路径（security/landlock/fs.c，已核对 6.6）

```
注册的 fs 钩子（LSM_HOOK_INIT 全清单核对）：
  挂载族：sb_mount / move_mount / sb_umount / sb_remount / sb_pivotroot
  路径族：path_link / path_rename / path_mkdir / path_mknod /
          path_symlink / path_unlink / path_rmdir / path_truncate
  文件族：file_open / file_truncate / file_alloc_security
  生命周期：inode_free_security / sb_delete

执行路径（以 open 为例）：
  hook_file_open → current_check_access_path(file->f_path,
      LANDLOCK_ACCESS_FS_{READ,WRITE,EXECUTE}_FILE)
    → 取 current 域（cred blob）→ 逐层（layer）走：
       landlock_find_rule(inode 的 rule 树, 层内 object)
       —— 命中且层内 allowed_access 含所需位 → 本层过
       —— 任何一层不满足 → -EACCES（fail-closed）
  精妙点：inode 的 rule 树挂在 lbs_inode blob 上（object 是
  "被规则盯上的 inode"集合，见数据结构篇的 landlock_object）
```

**file_truncate / path_truncate** 是 6.2（ABI 3）新增——`truncate(2)` 与 `ftruncate(2)` 分别从路径与已打开文件两个入口受控。

## 四、ABI 演进（include/uapi/linux/landlock.h 注释核对）

| ABI | 版本 | 新增权力 | 备注 |
| :--- | :--- | :--- | :--- |
| 1 | 5.13 | 基础 fs 权限族（execute/read/write/mknod 系列） | 无 truncate：旧内核下 open 后仍可 ftruncate |
| 2 | 5.19 | `LANDLOCK_ACCESS_FS_REFER`（跨目录硬链/reparent） | 未授予时跨目录 rename 被拒 |
| 3 | **6.2（6.6 即此）** | `LANDLOCK_ACCESS_FS_TRUNCATE` | uapi 注释"third version"原样核对 |
| 4 | 6.7（**不在 6.6**） | TCP bind/connect | 网络权力族——超出本分册基准，如实记录 |

探测模板：`create_ruleset(..., VERSION)` → 得 3 → 应用按 3 组装位图（向后兼容旧内核）。

## 五、设计决策速记（承 [优缺点](优缺点.md)）

- **无特权**：不需要 CAP，任何进程可自戴枷锁——与 seccomp BPF 同属"应用自约束"
- **只减不加**：规则只能收紧；多层叠加 = 交集（层间 AND）
- **best-effort 文件语义**：open 后的文件操作不再逐次检查（fd 是逃逸口，靠 access 快照与 truncate 钩子补）
- **ptrace 族的钩子**（cred/ptrace 模块）：限制被 ptrace/被读 /proc——横向防窥探

## 六、观测与实战

```
最小沙箱：sys_landlock 三连（见第一节）→ ls / 被拒=EACCES
调试：strace 看 landlock_* 返回；E2BIG=规则集 fd 已 attach
  EDOM=restrict 自身时 flags 非 0；EINVAL 多为位图超出 ABI
集成参考：systemd RestrictFileSystems=（v252+）、Firejail、
  Chrome/Flatpak 沙箱的现实落地
版本确认：/sys/kernel/security/lsm 里有 landlock（见
  [syscalls 分册 LSM 章](../../../../syscalls/docs/04-LSM与安全钩子顺序.md)）
```

## 七、思考题

1. 为什么 ruleset 必须先 add_rule 再 restrict_self？（对照"已生效域不可修改"的一致性代价。）
2. 域挂在 cred 而不是 task 上——推演 exec 一个 setuid 二进制后沙箱是否还在？（提示：cred 继承链。）
3. 多层 AND 语义下，"临时放宽"为什么不可能？（对照 stack：只能整体换 cred。）
4. `LANDLOCK_ACCESS_FS_REFER`（ABI 2）解决的跨目录 reparent 漏洞在 ABI 1 时代如何被绕过？（提示：rename 是天然的 reparent 路径。）
5. truncate 从 ABI 3 才受控——ABI 1/2 时代的真实逃逸路径？（open 后 ftruncate。）

---

> 回看：[基本原理](基本原理.md)｜[数据结构和关系](数据结构和关系.md)｜[优缺点](优缺点.md)
