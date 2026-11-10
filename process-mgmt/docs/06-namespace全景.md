# namespace 全景：八类隔离域与 nsproxy（Linux 6.6）

> 阅读目标：建立 namespace 的**总图**——八类隔离域全景、`struct nsproxy` 的聚合模型、三种创建路径（clone/unshare/setns）、user namespace 的映射语义与能力降级、pid namespace 的 `for_children` 不对称、nsfs 的句柄协议。前置：[01-fork与clone路径全解](01-fork与clone路径全解.md)。容器语境的落点见 [cgroup 分册](../../cgroup/README.md)。

---

## 一、八类隔离域与聚合点 nsproxy（include/linux/nsproxy.h，已核对 6.6）

```c
struct nsproxy {
	refcount_t count;
	struct uts_namespace   *uts_ns;         /* 主机名/domainname */
	struct ipc_namespace   *ipc_ns;         /* SysV IPC/POSIX mqueue */
	struct mnt_namespace   *mnt_ns;         /* 挂载表（最复杂）*/
	struct pid_namespace   *pid_ns_for_children;   /* 子进程的 pid 视角 */
	struct net             *net_ns;         /* 整套网络栈实例 */
	struct time_namespace  *time_ns, *time_ns_for_children; /* 单调钟偏移 */
	struct cgroup_namespace *cgroup_ns;     /* cgroup 根视角 */
};
```

八域 = mnt/uts/ipc/pid/user/net/cgroup/time。**user namespace 不在 nsproxy 里**——它挂在 `cred`（task 的凭据），因为它的隔离对象不是"资源视图"而是"身份映射"。

clone 旗标（include/uapi/linux/sched.h，已核对）：

| flag | 值 | 域 |
| :--- | :--- | :--- |
| CLONE_NEWNS | 0x00020000 | mount |
| CLONE_NEWCGROUP | 0x02000000 | cgroup |
| CLONE_NEWUTS | 0x04000000 | uts |
| CLONE_NEWIPC | 0x08000000 | ipc |
| CLONE_NEWUSER | 0x10000000 | user |
| CLONE_NEWPID | 0x20000000 | pid |
| CLONE_NEWNET | 0x40000000 | net |
| CLONE_NEWTIME | 0x00000080 | time |

## 二、三条创建/进入路径（kernel/nsproxy.c）

```
① clone(CLONE_NEW*): copy_process → copy_namespaces →
   create_new_namespaces（逐域 copy_mnt_ns/copy_ipcs/copy_pid_ns…
   失败逐个回滚）——一次性换装整套 nsproxy
② unshare(CLONE_NEW*): 换自己的 nsproxy（不影响已存在的子进程）
③ setns(fd): 从 nsfs 句柄进入既有域——switch_task_namespaces
   一步换 proxy（fd 来自 /proc/<pid>/ns/<type>）
退场：exit_task_namespaces（引用计数归零逐域 put）
```

**`for_children` 的不对称**（pid/time 两个域）：任务自身"属于哪个 pid ns"在出生时烙定（`task_active_pid_ns`），换 `pid_ns_for_children` 只影响**之后 fork 的孩子**——setns 进新 pid ns 后 `getpid()` 不变，新 child 才看到 1 号视角。time ns 同理（出生时的偏移不可换）。

## 三、user namespace：身份映射（include/linux/user_namespace.h，已核对）

```c
struct user_namespace {
	struct uid_gid_map uid_map, gid_map, projid_map;  /* 三张映射表 */
	struct user_namespace *parent;  int level;        /* 层级深度 */
	kuid_t owner;  kgid_t group;                       /* 创建者（升级锚）*/
	struct ns_common ns;  ...
};
```

语义要点：
- **双向转换**：`make_kuid(ns, uid)` 把 ns 内 uid 翻成全局；无映射条目返回 `INVALID_UID`——文件属主显示 `nobody` 的根源
- **能力分域**：`ns_capable(ns, CAP_*)` 检查"在该 ns 里有能力"；创建新 userns 的进程在新域内自动获得**全套 capabilities**——容器的"root 幻觉"由此而来，而它对宿主全局的能力为零
- 写映射表（`/proc/<pid>/uid_map`）需满足"写入者在新 ns 有 CAP_SETUID 且遵守一次性写"规则

## 四、pid namespace 结构（include/linux/pid_namespace.h，已核对）

```c
struct pid_namespace {
	struct idr idr;                    /* pid → struct pid 的分配器 */
	unsigned int pid_allocated;        /* 末号分配水位（含哨兵）*/
	struct task_struct *child_reaper;  /* 本域 1 号进程（孤儿收养者）*/
	unsigned int level;                /* 嵌套深度（struct pid 按 level 数组存每层 nr）*/
	struct pid_namespace *parent;
	struct user_namespace *user_ns;    /* 该域由谁创建（能力判定基准）*/
	...
};
```

`struct pid` 的 `numbers[level]` 数组是嵌套的本质：一个任务在每一层 pid ns 里有独立的 nr——`kill` 跨域找进程就是沿 `upid` 反查。

## 五、nsfs：namespace 的统一句柄（fs/nsfs.c，已核对）

```
/proc/<pid>/ns/<type>  → nsfs inode（绑定 ns_common.inum）
句柄协议（ioctl，已核对）：
  NS_GET_USERNS     该域所属的 user ns（"谁开的"）
  NS_GET_PARENT     父域（user/pid 有层级；其余 EIO）
  NS_GET_NSTYPE     clone 旗标（type 反查）
  NS_GET_OWNER_UID  user ns 的属主（setns 权限判断）
持有 nsfs fd = 钉住该域不被回收（容器的"保活"机制）
```

## 六、观测与实战

```
lsns                                   # 系统全部 namespace 表
nsenter -t <pid> -n -m -p sh           # 进目标域执行
readlink /proc/self/ns/net             # net:[4026531956]（inum 即身份）
ls -l /proc/<pid>/ns/                  # 同一 inum = 同一域
unshare -Ur -n bash                    # user+net 新域的 root 幻觉实验
cat /proc/<pid>/uid_map                # 映射表（"0 1000 1"=域内0↔全局1000）
排障：容器内 chown 报 EPERM → 查 uid_map 是否含目标 uid；
      nobody 文件 → 映射缺失；跨域 kill 不达 → pid numbers 层级
```

## 七、思考题

1. 为什么 user ns 挂在 cred 而不是 nsproxy？（推论：nsproxy 换装不换身份——setns 换 mnt ns 不需要动 uid 映射。）
2. `create_new_namespaces` 半路失败为什么必须逐域回滚？（对照引用计数与半残 proxy 的后果。）
3. pid ns 里 `child_reaper` 死了会发生什么？（对照 [03-exit与僵尸孤儿](03-exit与僵尸孤儿.md) 的孤儿收养。）
4. 持有 `/proc/<pid>/ns/mnt` 的 fd 能阻止 mount ns 被回收，为什么 zygote 类长进程要刻意持有？（提示：容器 init 退出后 fd 是最后裁判。）
5. time ns 的偏移为何"出生即烙定"？（对照 vvar 页的映射时序——timers 分册的 vDSO 知识。）

---

> 回看：[01-fork与clone路径全解](01-fork与clone路径全解.md)｜[cgroup 分册](../../cgroup/README.md)｜[syscalls 分册 seccomp](../../syscalls/docs/02-seccomp.md)
