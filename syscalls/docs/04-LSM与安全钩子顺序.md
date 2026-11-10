# LSM 与安全钩子的顺序（Linux 6.6）

> 阅读目标：把安全机制放进**系统调用的时间轴**——入口处的 seccomp（调用号级、最前）、实现内部的 LSM 钩子（对象级）、以及 LSM 框架自身的注册顺序模型（`security_hook_heads`/blob/`LSM_ORDER_*`）。前置：[02-seccomp](02-seccomp.md)、[01-系统调用入口与分发](01-系统调用入口与分发.md)。

---

## 一、系统调用入口的安全时间轴（kernel/entry/common.c，已核对 6.6）

```
syscall_enter_from_user_mode_work(regs, syscall) 逐项顺序：
  ① ptrace（SYSCALL_WORK_SYSCALL_TRACE/EMU）
     tracer 可改参数/模拟调用
  ② seccomp——注释原样核对："Do seccomp after ptrace,
     to catch any tracer changes"（tracer 改完再过滤）
     __secure_computing() → SECCOMP_RET_{KILL,TRAP,ERRNO,TRACE,
     USER_NOTIF,LOG,ALLOW}
  ③ syscall = syscall_get_nr(current, regs)  ← 重读调用号
     （ptrace/seccomp 都可能改写过）
  ④ trace_sys_enter（tracepoint）
  ⑤ syscall_enter_audit
  ⑥ 真正执行 → do_syscall_x64（本轮的安全"下半场"在实现内部：LSM）
```

**分野**：seccomp 在**调用号层**短路（根本不让它执行）；LSM 在**实现内部的对象访问点**逐个把关（如 `do_dentry_open → security_file_open`）。seccomp 看不见 `open("/etc/shadow")` 的路径语义（只能裸比对 args 指针指向的字符串），LSM 看得见 file/inode/superblock 的标签。

## 二、LSM 框架：钩子链与 blob（security/security.c + include/linux/lsm_hooks.h，已核对）

```c
struct security_hook_heads security_hook_heads __ro_after_init;  /* 每类钩子一条链 */
static struct lsm_blob_sizes blob_sizes;   /* 各模块 per-object 私有区的合计 */
struct lsm_hook_list {                     /* 链节点 */
	struct list_head list;
	struct list_head *head;   /* 挂到哪个钩子头 */
	union security_list_options hook;  /* 回调 */
	char *lsm;                /* 归属模块名（审计用） */
} ;
```

调用侧（`security_file_open(f)`）：`call_int_hook(file_open, file)` 遍历该头链表——**任一返回非零即短路**（fail-closed）；逐模块顺序 = 注册顺序。

**blob 机制**：模块声明 `lsm_blob_sizes{lbs_cred/lbs_inode/…}`（landlock 的例子见 [landlock 执行路径](../../LSM/landlock/docs/04-执行路径与ABI演进.md)），`lsm_set_blob_sizes` 汇总各尺寸 → cred/inode/file 等对象尾部预留"模块专属格"；`landlock_cred(d->security + offset)` 按模块偏移取格——**LSM 之间共用宿主对象而不互踩**。

## 三、注册顺序模型（已核对 security.c）

```
启动：security_init → ordered_lsm_init：
  builtin_lsm_order = CONFIG_LSM（编译期默认串）
  boot 参数 lsm=…（chosen_lsm_order）可覆盖；lsm.debug 查看决策
  排序规则：
    LSM_ORDER_FIRST   恒最先（capability——所有判定的地基）
    LSM_ORDER_MUTABLE 按 CONFIG_LSM/lsm= 串序
    LSM_ORDER_LAST    恒最后（如 BPF/landlock 这类"观察/自约束"位）
  exclusive 独占槽：SELinux/AppArmor/Smack 互斥选一为主力
观测：cat /sys/kernel/security/lsm（security/inode.c 创建，0444）
      → "capability,lockdown,yama,integrity,landlock" 之类的生效序
```

**顺序的语义**：`capable(CAP_*)` 检查（capability LSM）总是先于标签判定（SELinux 类）——DAC 与 capability 失败时根本轮不到 MAC；反过来 audit 信息会带 `lsm=` 名字标明是哪个模块拒绝的。

## 四、三层安全机制对照

| | seccomp | LSM（selinux/apparmor/…） | landlock |
| :--- | :--- | :--- | :--- |
| 时间轴 | 入口（实现前） | 实现内部（对象访问点） | 同 LSM（框架内一员） |
| 粒度 | 调用号+裸参数 | 对象标签/类型 | 路径规则 |
| 配置者 | 应用自己（写代码） | 管理员（策略） | 应用自己 |
| 特权 | 无需 CAP | 管理员特权 | 无需 CAP |
| 能力 | 短路整个调用 | 拒特定对象操作 | 收紧自身文件访问 |
| 逃逸口 | 多（ioctl 大杂烩） | 取决于策略完备性 | fd 携带语义（best-effort） |

组合即纵深：容器常见 `seccomp（白名单 300 调用）+ LSM（宿主标签）+ landlock（应用内自限）` 三层同时生效。

## 五、观测与实战

```
cat /sys/kernel/security/lsm            # 生效 LSM 及顺序
auditd 的 AVC 拒绝记录（selinux）/ dmesg apparmor="DENIED"
perf probe security_file_open           # 钩子热点
bpftrace 'kprobe:__secure_computing'    # seccomp 触发频率
写驱动/新 LSM：security_add_hooks(lsm_hook_list[], count, "name")
  —— 仅 6.6 主线允许内核内静态注册（eBPF LSM 是动态挂 BPF 程序的
  另一条路，见 trace 分册）
```

## 六、思考题

1. seccomp 放在 ptrace 之后的设计意图？（提示：tracer 修改参数的"最后仲裁"该是谁。）
2. `syscall_get_nr` 重读调用号发生在 ptrace/seccomp 之后——mips 的 `__NR_syscall`（间接调用）语义如何被正确处理？（对照 [01-入口分发](01-系统调用入口与分发.md)。）
3. LSM 的 fail-closed 短路顺序对审计有什么副作用？（哪个模块先拒绝，后面的模块就"看不见"这次访问——策略调试的盲区。）
4. blob 偏移若在运行时重算会发生什么？（对照 `__ro_after_init` 的写保护。）
5. landlock 被排在 LSM_ORDER_LAST 一类意味着什么？（提示：谁先拒绝，audit 记谁的名字。）

---

> 回看：[02-seccomp](02-seccomp.md)｜[landlock 执行路径](../../LSM/landlock/docs/04-执行路径与ABI演进.md)｜[namespace 全景](../../process-mgmt/docs/06-namespace全景.md)
