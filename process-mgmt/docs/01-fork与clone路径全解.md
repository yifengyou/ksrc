# fork 与 clone 路径全解（Linux 6.6）

> 阅读目标：吃透从系统调用入口到新任务首次上 CPU 的完整链路——fork/vfork/clone/clone3 四个系统调用的参数与 `CLONE_*` 标志矩阵（每个标志到底共享什么、有什么语义陷阱）；`kernel_clone()`（6.6 已从老内核的 `_do_fork` 更名）→ `copy_process()` 的**逐阶段**剖析（任务结构与内核栈分配、凭证复制、调度器初始化、mm/fs/files/sighand 的共享与复制、架构相关的 `copy_thread`、`alloc_pid` 的多层 idr 分配）；6.6 `dup_mmap` 已经改用 **maple tree** 遍历（不是老的 vma 链表/红黑树）这一关键事实；最后理清 pid/tgid/pgid/sid 的数据结构基础，为 namespace 章节铺路。所有函数名与调用顺序均已对照 v6.6 源码逐一核对。

---

## 一、系统调用家族与 CLONE_* 标志矩阵

### 1.1 四个入口，一个主干

用户态创建任务的系统调用有四个（`kernel/fork.c`，6.6）：

```c
SYSCALL_DEFINE0(fork)      /* 无参数：.exit_signal = SIGCHLD，其余全空 */
{
	struct kernel_clone_args args = { .exit_signal = SIGCHLD, };
	return kernel_clone(&args);
}

SYSCALL_DEFINE0(vfork)     /* fork + CLONE_VM|CLONE_VFORK：父进程睡到子进程 exec/exit */
{
	struct kernel_clone_args args = {
		.flags      = CLONE_VFORK | CLONE_VM,
		.exit_signal = SIGCHLD,
	};
	return kernel_clone(&args);
}

/* clone：参数顺序随架构不同（CONFIG_CLONE_BACKWARDS*），x86_64 为：*/
SYSCALL_DEFINE5(clone, unsigned long, clone_flags, unsigned long, newsp,
		 int __user *, parent_tidptr,
		 int __user *, child_tidptr, unsigned long, tls)

SYSCALL_DEFINE2(clone3, struct clone_args __user *, uargs, size_t, size)
	/* 结构体传参，按 size 版本化：VER0=64B(tls 止)/VER1=80B(set_tid_size 止)/VER2=88B(cgroup 止) */
```

所有入口最终汇入 **`kernel_clone(struct kernel_clone_args *args)`**（`kernel/fork.c:2868`）。注意：老资料里的 `_do_fork()` 在 5.13 已更名为 `kernel_clone()`，5.10 时代的 `do_fork()` 则更早消失——本章一律以 6.6 的 `kernel_clone` 为准。

内核内部也走同一条路：`kernel_thread()`（kthread，多加 `CLONE_VM|CLONE_UNTRACED` 与 `args->kthread=1`）、`user_mode_thread()`（启动早期创建"要回用户态"的任务，如 init）、`create_io_thread()`（io_uring 的 IORING_WRK_WORKER 线程）、`fork_idle()`（每 CPU 的 idle 任务，直接传 `&init_struct_pid` 跳过 `alloc_pid`）。这些差异全部被编码进一个参数结构（`include/linux/sched/task.h`）：

```c
struct kernel_clone_args {
	u64 flags;             /* CLONE_* 组合（64 位，容纳 clone3 的新标志）*/
	int __user *pidfd;     /* CLONE_PIDFD：pidfd 写回地址 */
	int __user *child_tid; /* CLONE_CHILD_SETTID / CLONE_CHILD_CLEARTID */
	int __user *parent_tid;/* CLONE_PARENT_SETTID */
	const char *name;      /* 内核内部命名（kthread）*/
	int exit_signal;       /* 退出时发给 real_parent 的信号（fork=SIGCHLD）*/
	u32 kthread:1;         /* PF_KTHREAD */
	u32 io_thread:1;       /* PF_IO_WORKER */
	u32 user_worker:1;     /* io_uring 用户 worker：屏蔽非致命信号 */
	u32 no_files:1;        /* 不要 fd 表（kthread 用）*/
	unsigned long stack;   /* 子任务用户栈顶（clone 的 newsp）*/
	unsigned long stack_size;
	unsigned long tls;     /* CLONE_SETTLS */
	pid_t *set_tid;        /* clone3：指定各层 pid namespace 的 PID（CRIU）*/
	size_t set_tid_size;
	int cgroup;            /* CLONE_INTO_CGROUP */
	int (*fn)(void *);     /* 内核线程函数；用户 fork 为 NULL */
	void *fn_arg;
	...
};
```

### 1.2 CLONE_* 标志矩阵（include/uapi/linux/sched.h，6.6 真实值）

| 标志 | 值 | 共享/创建什么 | 主要使用者 | 语义风险 |
| :--- | :--- | :--- | :--- | :--- |
| `CLONE_VM` | 0x100 | 共享整个地址空间（`mmget` 引用 +1，不复制） | pthread、vfork | 子进程写内存父进程立即可见；exit 时不能自动 `mmput` 对方还在用的 mm |
| `CLONE_FS` | 0x200 | 共享 `fs_struct`（cwd/root/umask） | pthread | chdir 影响彼此；与 `CLONE_NEWNS`/`CLONE_NEWUSER` 互斥 |
| `CLONE_FILES` | 0x400 | 共享打开文件表（`atomic_inc(&files->count)`） | pthread | close 影响彼此；exec 的 CLOEXEC 需先 `unshare_files` |
| `CLONE_SIGHAND` | 0x800 | 共享信号处理表（`refcount_inc(&sighand->count)`） | pthread | 必须伴随 CLONE_VM；信号处理函数改动全组生效 |
| `CLONE_PIDFD` | 0x1000 | 父进程拿到子进程的 pidfd | pidfd_send_signal 等 | 与 CLONE_DETACHED/CLONE_THREAD 互斥；legacy clone 里复用 parent_tidptr 传回 |
| `CLONE_PTRACE` | 0x2000 | 子进程出生即被父进程的 tracer 追踪 | 调试器 | 一般无人用；ptrace_event 才是常规通知 |
| `CLONE_VFORK` | 0x4000 | 父进程挂起直到子进程 `mm_release`（exec/exit） | vfork、glibc posix_spawn | 父进程不可中断地睡（killable）；子进程"借用"父地址空间期间父进程冻结 |
| `CLONE_PARENT` | 0x8000 | 与调用者**同父**（real_parent 复用） | 作业系统 | 若调用者是容器 init（SIGNAL_UNKILLABLE）会被拒绝，防多根进程树 |
| `CLONE_THREAD` | 0x10000 | 加入同一线程组（tgid 复用、共享 sighand） | pthread | 必须同时 CLONE_SIGHAND（进而必须 CLONE_VM）；exit_signal 恒 -1（不给父进程发 SIGCHLD） |
| `CLONE_NEWNS` 等 8 个 | 0x20000~0x40000000/0x80 | 创建新 namespace（mnt/cgroup/uts/ipc/user/pid/net/time） | 容器运行时 | 需对应 CAP；CLONE_NEWUSER 与 CLONE_FS 互斥；CLONE_THREAD 禁止同时跨 user/pid ns |
| `CLONE_SYSVSEM` | 0x40000 | 共享 SysV 信号量 UNDO 列表 | pthread | — |
| `CLONE_SETTLS` | 0x80000 | 设置子线程 TLS 基址（glibc `tls` 参数） | pthread | 用户栈+TLS 是线程库自己管理的 |
| `CLONE_PARENT_SETTID` | 0x100000 | 父进程地址写入子 tid | pthread | futex 依赖 |
| `CLONE_CHILD_CLEARTID` | 0x200000 | 子进程退出时清 0 并 futex-wake 该地址 | pthread | `mm_release()` 实现——线程库 join 的基石 |
| `CLONE_CHILD_SETTID` | 0x1000000 | 子进程自身地址写入 tid | pthread | — |
| `CLONE_UNTRACED` | 0x800000 | 禁止 tracer 强制追踪（内核内部用） | kernel_thread | — |
| `CLONE_IO` | 0x80000000 | 共享 I/O 调度上下文（io_context） | 罕见 | — |
| `CLONE_CLEAR_SIGHAND` | 0x100000000 | clone3 专属：把信号处理重置为 SIG_DFL | 沙箱 | 与 CLONE_SIGHAND 互斥 |
| `CLONE_INTO_CGROUP` | 0x200000000 | clone3 专属：直接落指定 cgroup | 容器 | — |
| `CSIGNAL` | 0xff | 低 8 位：exit_signal | fork/clone | clone3 已禁用（用独立字段） |

> 📌 **"进程 vs 线程"在内核里只差一组标志**：`clone(CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|CLONE_SYSVSEM|CLONE_SETTLS|CLONE_PARENT_SETTID|CLONE_CHILD_CLEARTID|CLONE_CHILD_SETTID, ...)` 就是 `pthread_create`。task_struct 本身不分进程线程——所谓线程，就是**共享了 mm/fs/files/sighand、且 tgid 相同**的任务。

### 1.3 标志组合的硬约束（copy_process 的第一道门）

`copy_process()` 开头是一串显式合法性检查（`kernel/fork.c:2240`），违反即 `-EINVAL`：

```c
if ((clone_flags & (CLONE_NEWNS|CLONE_FS)) == (CLONE_NEWNS|CLONE_FS))
	return ERR_PTR(-EINVAL);          /* 新 mount ns 不许共享 fs */
if ((clone_flags & (CLONE_NEWUSER|CLONE_FS)) == (CLONE_NEWUSER|CLONE_FS))
	return ERR_PTR(-EINVAL);          /* 新 user ns 同理 */
if ((clone_flags & CLONE_THREAD) && !(clone_flags & CLONE_SIGHAND))
	return ERR_PTR(-EINVAL);          /* 线程组必须共享信号处理 */
if ((clone_flags & CLONE_SIGHAND) && !(clone_flags & CLONE_VM))
	return ERR_PTR(-EINVAL);          /* 共享 sighand 必须共享 VM */
if ((clone_flags & CLONE_PARENT) && current->signal->flags & SIGNAL_UNKILLABLE)
	return ERR_PTR(-EINVAL);          /* init 不许造"兄弟姐妹" */
if (clone_flags & CLONE_THREAD) {
	if ((clone_flags & (CLONE_NEWUSER | CLONE_NEWPID)) ||
	    (task_active_pid_ns(current) != nsp->pid_ns_for_children))
		return ERR_PTR(-EINVAL);      /* 线程组不得跨 user/pid ns */
}
```

其中"SIGHAND ⇒ VM"这条注释值得背下来：**共享信号处理却不共享地址空间**是不允许的，因为信号处理函数指针指向的代码必须在双方地址空间里含义一致。

随后是一个精巧的**信号时序协议**：fork 期间到达的"多播信号"（发给进程组的 SIGINT 等）要么在 fork 前投递完，要么延迟到 fork 后。实现是把调用者挂进 `current->signal->multiprocess` 哈希链（`delayed` 节点），`recalc_sigpending()` 后若自己已有未决信号则直接 `-ERESTARTNOINTR` 重启系统调用——保证孩子出生瞬间信号状态是干净的（线程组 leader 出生时继承 `delayed.signal`，见 §4.3）。

---

## 二、kernel_clone 与 copy_process 的骨架

### 2.1 kernel_clone 主干（kernel/fork.c:2868）

```
kernel_clone(args)
 ├─ CLONE_PIDFD ∧ CLONE_PARENT_SETTID ∧ 两指针相同 → -EINVAL
 ├─ 选 ptrace 事件类型：CLONE_VFORK→PTRACE_EVENT_VFORK；
 │   exit_signal≠SIGCHLD→PTRACE_EVENT_CLONE；否则 PTRACE_EVENT_FORK
 ├─ p = copy_process(NULL, trace, NUMA_NO_NODE, args)   ★ 主战场（§2.2~§4）
 ├─ trace_sched_process_fork(current, p)                 /* 在唤醒前发：子指针
 │                                                          可能很快失效 */
 ├─ pid = get_task_pid(p, PIDTYPE_PID); nr = pid_vnr(pid) /* 调用者 ns 里的编号 */
 ├─ CLONE_PARENT_SETTID → put_user(nr, args->parent_tid)
 ├─ CLONE_VFORK → p->vfork_done = &vfork（栈上 completion）+ get_task_struct(p)
 ├─ LRU_GEN 且 !CLONE_VM → lru_gen_add_mm(p->mm)          /* MGLRU 链表登记 */
 ├─ wake_up_new_task(p)                                   /* ★ 首次调度（见 §5）*/
 ├─ trace → ptrace_event_pid(trace, pid)
 ├─ CLONE_VFORK → wait_for_vfork_done(p, &vfork)          /* 父进程睡到
 │                                                          PTRACE_EVENT_VFORK_DONE */
 └─ return nr                                             /* 父进程返回子 pid */
```

### 2.2 copy_process：总体阶段图

`copy_process()` 约 500 行，可切成六个阶段（顺序严格对应 6.6 源码）：

```
阶段① 合法性检查 + 信号时序协议（§1.3）
阶段② dup_task_struct：分配 task_struct + 内核栈 + 基础字段复位
      → copy_creds → RLIMIT_NPROC / nr_threads≥max_threads 配额检查
阶段③ 一大段"清零式"初始化（cputime/rss/psi/lockdep/…）
      → sched_fork（调度器初始化，TASK_NEW 状态）
      → perf_event_init_task / audit_alloc / security_task_alloc
阶段④ copy_* 家族：semundo→files→fs→sighand→signal→mm→namespaces→io→thread
      → alloc_pid（4 层 idr）→ CLONE_PIDFD 的 pidfd 创建
阶段⑤ tasklist_lock 写锁下的"挂接"：父子关系、PID 哈希、
      children/tasks/thread_group 链表、child_reaper 判定
      —— 从"pid_allocated 检查 + fatal_signal_pending 检查"之后
         不再有失败路径（注释原文：No more failure paths after this point）
阶段⑥ 解锁后的通知：sched_post_fork / cgroup_post_fork /
      trace_task_newtask / uprobe_copy_process ...
```

### 2.3 dup_task_struct：一身一栈（kernel/fork.c:1103）

```c
static struct task_struct *dup_task_struct(struct task_struct *orig, int node)
{
	tsk = alloc_task_struct_node(node);     /* task_struct_cachep（SLAB_ACCOUNT：
	                                             每个任务给 memcg 记账）*/
	arch_dup_task_struct(tsk, orig);        /* 弱实现是 *dst = *src；
	                                             x86 自己的实现还要复制 FPU 状态 */
	alloc_thread_stack_node(tsk, node);     /* 内核栈，见下 */
	refcount_set(&tsk->stack_refcount, 1);
	account_kernel_stack(tsk, 1);           /* memcg 记账（VMAP_STACK 按 vmalloc 页）*/
	scs_prepare(tsk, node);                 /* shadow call stack（arm64）*/
	tsk->seccomp.filter = NULL;             /* 延后到持 sighand 锁的 copy_seccomp */
	setup_thread_stack(tsk, orig);
	clear_user_return_notifier(tsk);
	clear_tsk_need_resched(tsk);
	set_task_stack_end_magic(tsk);          /* 栈底写 0x57AC6E9D（"stack ac"）*/
	...
	refcount_set(&tsk->rcu_users, 2);       /* 1 份给用户可见状态（被 reap 时归零），
	                                             1 份给调度器（task_dead 时归零）*/
	refcount_set(&tsk->usage, 1);
}
```

三个值得展开的点：

- **内核栈的分配三种模式**（同文件 §182~430）：`CONFIG_VMAP_STACK`（x86_64/arm64 默认开）走 vmalloc——虚拟连续物理离散，配 guard page 防溢出，且释放走 RCU + per-CPU `cached_stacks[2]` 缓存（`vfree` 会触发 TLB 刷新，缓存能省掉大部分）；`THREAD_SIZE >= PAGE_SIZE` 直接 buddy 分配整页；否则（小内存配置）用 `thread_stack_cache` slab。16K 的 THREAD_SIZE 是 x86_64/arm64 常规值。
- **`set_task_stack_end_magic`**：在栈的最低地址写魔数。栈向下生长，一旦越过栈底（比如深递归），内核在下一次检查（`object_is_on_stack`/溢出检测路径，以及调度器 `__switch_to` 相关检查）发现魔数被改写即报告 overflow——这是成本最低的栈溢出探测器。
- **两个引用计数**（`rcu_users=2, usage=1`）：`usage` 是常规任务引用（持有 task_struct 指针者 +1）；`rcu_users` 分两份——用户空间可见状态（进程表里的那个"进程"，等 `release_task` 后归零）与调度器（等 `finish_task_switch` 里看到 `TASK_DEAD` 后 `put_task_struct_rcu_user` 归零）。**两份都归零才触发 `call_rcu(delayed_put_task_struct)` 释放**——第 3 章会看到这条释放链的全貌。

### 2.4 copy_creds 与配额检查（kernel/cred.c）

```c
int copy_creds(struct task_struct *p, unsigned long clone_flags)
{
	if (!p->cred->thread_keyring && clone_flags & CLONE_THREAD) {
		p->real_cred = get_cred(p->cred);  /* 线程：完全共享凭证 */
		get_cred(p->cred);
		inc_rlimit_ucounts(task_ucounts(p), UCOUNT_RLIMIT_NPROC, 1);
		return 0;
	}
	new = prepare_creds();                  /* 否则复制一份（RCU 防护）*/
	if (clone_flags & CLONE_NEWUSER) {      /* 新 user ns：装新 uid_map 语义 */
		create_user_ns(new);
		set_cred_ucounts(new);
	}
	p->cred = p->real_cred = get_cred(new);
	inc_rlimit_ucounts(task_ucounts(p), UCOUNT_RLIMIT_NPROC, 1);
}
```

紧接着是两道 early 配额（`copy_process` 内）：

- **RLIMIT_NPROC**：`is_rlimit_overlimit(task_ucounts(p), UCOUNT_RLIMIT_NPROC, rlimit(RLIMIT_NPROC))` 超限时，除非是 root 身份（`p->real_cred->user != INIT_USER` 为否）或持 `CAP_SYS_RESOURCE/CAP_SYS_ADMIN`，否则 `-EAGAIN`。注意 6.6 的计数已改为 **ucounts**（per-user-namespace 层级计数），不再是老的纯 `user->processes`——所以容器里 UID 冲突不会互相顶配额。
- **`nr_threads >= max_threads`**：全局线程数上限，`-EAGAIN`。注释坦承多线程并发 fork 时这个检查有竞态（"只在防 root fork 炸弹"）。

### 2.5 sched_fork：调度器视角的新生（kernel/sched/core.c）

```c
int sched_fork(unsigned long clone_flags, struct task_struct *p)
{
	__sched_fork(clone_flags, p);      /* 清零 se/vruntime/vlag、slice=sysctl_sched_base_slice、
	                                      RT/DL 子结构、stats、numa、mm_cid */
	p->__state = TASK_NEW;             /* ★ 0x800：谁也不能跑它、唤醒不了它
	                                      ——直到 wake_up_new_task 亲手入队 */
	p->prio = current->normal_prio;    /* 不继承 PI 提升后的 prio（防优先级泄漏）*/
	uclamp_fork(p);
	if (unlikely(p->sched_reset_on_fork)) { /* SCHED_RESET_ON_FORK：RT/DL 降为
	                                      CFS、负 nice 拉回 0 */
		...
	}
	if (dl_prio(p->prio))
		return -EAGAIN;               /* reset_on_fork 后仍是 DL → 拒绝 */
	else if (rt_prio(p->prio))
		p->sched_class = &rt_sched_class;
	else
		p->sched_class = &fair_sched_class;   /* 绝大多数任务 */
	init_entity_runnable_average(&p->se);
	...
}
```

要点：**vruntime 归零而非继承**（否则 fork 循环攻击者可以靠孩子不断"年轻化"抢占）；**`TASK_NEW` 是一道闸门**——`try_to_wake_up` 的状态掩码不含它，新任务在 `wake_up_new_task()` 之前对整个调度器不可见，杜绝"半初始化任务被信号唤醒"的竞态。调度类选择、`sched_cgroup_fork`（把任务放进正确 cgroup 的运行队列）与 `sched_post_fork`（含 core scheduling 的 cookie 继承）的细节，见调度器分册第 2、4 章，此处不展开。

---

## 三、copy_* 家族：共享还是复制

### 3.1 copy_fs / copy_files / copy_sighand / copy_signal

```c
static int copy_fs(unsigned long clone_flags, struct task_struct *tsk)
{
	if (clone_flags & CLONE_FS) {
		spin_lock(&fs->lock);
		if (fs->in_exec) { /* 组内有人正在 exec：fs 正在被换，不能加引用 */
			spin_unlock(&fs->lock);
			return -EAGAIN;
		}
		fs->users++;
		return 0;
	}
	tsk->fs = copy_fs_struct(fs);   /* 否则深拷贝（新 cwd/root 引用）*/
}
```

- `copy_files`：`CLONE_FILES` 时仅 `atomic_inc(&oldf->count)`；否则 `dup_fd()` 复制整个 fd 表（fdtable 数组 + `open_fds` 位图；已打开的 `struct file` 只加引用，不复制）。注意 `args->no_files`（kthread）直接 `tsk->files = NULL`——内核线程默认没有 fd 表。
- `copy_sighand`：`CLONE_SIGHAND` 时 `refcount_inc(&current->sighand->count)`；否则 `kmalloc` 新 `sighand_struct` 并 `memcpy` 动作表。**共享与复制的分界就是信号处理函数改动的可见性**（第 4 章展开）。
- `copy_signal`：`CLONE_THREAD` 时共享 `signal_struct`（`refcount_inc(&sig->sigcnt)`）；否则 `kmem_cache_alloc(signal_cachep)` 新建——里面装的是**线程组级**状态：共享待决信号集、RLIMIT 数组、cputime 累计、tty、`has_child_subreaper` 等。进程的资源限制（`rlim[]`）挂在 signal 而非 task 上，这就是"同组线程 getrlimit 看到同一份"的原因。

### 3.2 copy_mm：共享 vs dup_mm（kernel/fork.c:1708）

```c
static int copy_mm(unsigned long clone_flags, struct task_struct *tsk)
{
	tsk->mm = NULL; tsk->active_mm = NULL;
	oldmm = current->mm;
	if (!oldmm)
		return 0;                    /* 内核线程：无 mm，运行时借 active_mm（lazy TLB）*/
	if (clone_flags & CLONE_VM) {
		mmget(oldmm);                /* 只加 mm_users，一个字节都不复制 */
		mm = oldmm;
	} else {
		mm = dup_mm(tsk, current->mm);  /* 完整复制：新 mm_struct + 新 pgd + 逐 VMA 复制 */
	}
	tsk->mm = mm; tsk->active_mm = mm;
	sched_mm_cid_fork(tsk);
}
```

`dup_mm()`（`kernel/fork.c:1671`）三步：`allocate_mm()`（`mm_cachep` slab）→ `memcpy(mm, oldmm, sizeof(*mm))`（把 brk/mmap_base/代码数据边界等**布局字段**先整体继承）→ `mm_init()` 重置动态状态（`mt_init_flags(&mm->mm_mt, MM_MT_FLAGS)` 初始化 **maple tree**、`mm_users=mm_count=1`、`pgd_alloc()`、`init_new_context()`、rss_stat percpu 计数器、user_ns 引用）→ `dup_mmap()` 复制 VMA 与页表。

### 3.3 dup_mmap：6.6 的 maple tree 遍历与 COW（kernel/fork.c:646）

> ⚠️ **常见资料错误**：大量教科书/博客仍在讲"fork 沿 `mm->mmap` 链表遍历 VMA、复制 `vm_next/vm_prev`、插入 `mm_rb` 红黑树"。vma 链表与 `mm_rb` 树在 6.1~6.4 间已被**移除**，替换为一棵 maple tree（`mm->mm_mt`）。6.6 的 `dup_mmap` 用 `VMA_ITERATOR` 遍历 + `vma_iter_bulk_store` 批量建树——既没有链表也没有 rbtree 插入。

```c
static __latent_entropy int dup_mmap(struct mm_struct *mm, struct mm_struct *oldmm)
{
	VMA_ITERATOR(old_vmi, oldmm, 0);      /* 遍历旧树的游标 */
	VMA_ITERATOR(vmi, mm, 0);             /* 填充新树的游标 */
	uprobe_start_dup_mmap();
	mmap_write_lock_killable(oldmm);
	flush_cache_dup_mm(oldmm);            /* 架构缓存维护（VIPT 别名等）*/
	mmap_write_lock_nested(mm, SINGLE_DEPTH_NESTING); /* 新树未挂上：无死锁可能 */
	dup_mm_exe_file(mm, oldmm);           /* exe_file 引用 + deny_write_access */
	vma_iter_bulk_alloc(&vmi, oldmm->map_count);  /* 预分配 maple 节点（避免中途 OOM）*/
	mt_clear_in_rcu(vmi.mas.tree);
	for_each_vma(old_vmi, mpnt) {         /* ★ 按 maple tree 顺序遍历旧 VMA */
		vma_start_write(mpnt);
		if (mpnt->vm_flags & VM_DONTCOPY) {  /* VSYSCALL 页等：子进程不继承 */
			vm_stat_account(mm, mpnt->vm_flags, -vma_pages(mpnt));
			continue;
		}
		if (fatal_signal_pending(current))   /* OOM 杀到一半：尽快终止复制 */
			goto loop_out;
		if (mpnt->vm_flags & VM_ACCOUNT)     /* RLIMIT_AS 记账转移 */
			security_vm_enough_memory_mm(oldmm, len);
		tmp = vm_area_dup(mpnt);             /* slab 分配新 vma（复制全部字段）*/
		vma_dup_policy(mpnt, tmp);           /* mempolicy 复制 */
		tmp->vm_mm = mm;
		dup_userfaultfd(tmp, &uf);
		if (tmp->vm_flags & VM_WIPEONFORK)   /* MADV_WIPEONFORK：子进程清零该区 */
			tmp->anon_vma = NULL;
		else if (anon_vma_fork(tmp, mpnt))   /* 反向映射：新 vma 链到同一 anon_vma 树 */
			goto fail_nomem_anon_vma_fork;
		vm_flags_clear(tmp, VM_LOCKED_MASK); /* mlock 状态不继承（语义上是新进程）*/
		if (file) {
			get_file(file);
			i_mmap_lock_write(mapping);
			vma_interval_tree_insert_after(tmp, mpnt, &mapping->i_mmap);
			i_mmap_unlock_write(mapping);    /* 文件映射：挂入 page cache 的反向映射 */
		}
		vma_iter_bulk_store(&vmi, tmp);      /* ★ 写入新 maple tree（批量）*/
		mm->map_count++;
		copy_page_range(tmp, mpnt);          /* ★ 页表级 COW（下面详解）*/
		if (tmp->vm_ops && tmp->vm_ops->open)
			tmp->vm_ops->open(tmp);
	}
	arch_dup_mmap(oldmm, mm);               /* 架构钩子：x86 记 LAM、arm64 记 ASID 等 */
	mt_set_in_rcu(vmi.mas.tree);
	flush_tlb_mm(oldmm);                    /* 父进程 PTE 已写保护：冲掉旧可写 TLB 项 */
}
```

**`copy_page_range()` 做的事**（`mm/memory.c`）：自顶向下 `pgd→p4d→pud→pmd→pte` 遍历**父进程已有页表的部分**；对每个存在的 present PTE，若 VMA 可写，把**父子两份 PTE 都改成只读**并标记 COW。这就是 fork 最大的成本来源之一：

- 复杂度 **O(已触碰的 VMA 数 × 已分配页的页表项数)**——一个 RSS 数 GB 的进程，fork 要把每个 PTE 都写一遍，还要 `flush_tlb_mm`。这是 Redis/Java 之类大进程"fork 瞬间卡顿百毫秒"的经典成因（哪怕一页都不真的复制数据）。
- 真正的数据复制推迟到**写时**：任一方首次写触发 page fault，fault 处理函数发现 COW 标记，分配新页复制内容（`mm/pagefualt/pagefault.md` 详解 do_wp_page）。
- 6.6 已支持 `clone3` 的 `CLONE_VM` 场景完全跳过这些；而普通 fork 的替代方案（写时复制整个 mm、父子共享页表 `COPY_MM_...` 的方案）仍在演进中，主线尚未落地的需以 6.6 现状为准。

### 3.4 copy_namespaces / copy_io / copy_thread

`copy_namespaces()`（`kernel/nsproxy.c`）：无 CLONE_NEW* 标志时 `get_nsproxy()` 共享（引用计数）；有则 `create_new_namespaces()` 按标志逐个 unshare/share。`nsproxy` 把 uts/ipc/mnt/pid/cgroup/net/time ns 打包——第 6 章（namespace 专题，见分册规划）展开。

`copy_io()`：`CLONE_IO` 才共享 `io_context`（CFQ 时代的遗产，如今多级 blk-mq 下意义很小）；否则 `p->io_context = NULL`——首次块 IO 时 `get_io_context()` 惰性创建。

**`copy_thread()`** 是架构差异的分水岭（本节核对 x86 与 arm64 两个主流架构）：

```c
/* arch/x86/kernel/process.c，6.6 精简 */
int copy_thread(struct task_struct *p, const struct kernel_clone_args *args)
{
	childregs = task_pt_regs(p);            /* 新栈顶端的 pt_regs */
	frame = &container_of(childregs, struct fork_frame, regs)->frame;
	frame->ret_addr = (unsigned long)ret_from_fork_asm;  /* ★ 返回地址造假 */
	p->thread.sp = (unsigned long)fork_frame;            /* 内核栈起点 */
	fpu_clone(p, clone_flags, ...);         /* FPU 状态：父子按需复制（XSAVE）*/
	if (unlikely(p->flags & PF_KTHREAD)) {  /* —— 分岔点 A：内核线程 —— */
		memset(childregs, 0, sizeof(struct pt_regs));
		kthread_frame_init(frame, args->fn, args->fn_arg); /* fn/arg 塞进被调用者
		                                                       保存寄存器槽位 */
		return 0;
	}
	frame->bx = 0;                          /* rbx=0：子进程 fork 返回值 0 的来源 */
	*childregs = *current_pt_regs();        /* 整份 pt_regs 复制（ip=系统调用下一条）*/
	childregs->ax = 0;                      /* rax=0：fork(2) 在子进程返回 0 */
	if (sp)
		childregs->sp = sp;             /* clone 的 newsp：换用户栈 */
	if (unlikely(args->fn)) { ... }         /* —— 分岔点 B：user_mode_thread：
		                                       内核栈帧进内核函数，但终将回用户态 */
	if (clone_flags & CLONE_SETTLS)
		set_new_tls(p, tls);            /* 写 fsbase（x86_64 的 TLS 基址）*/
}
```

x86 的"两个返回值"魔术：父进程从 `kernel_clone` 正常 return 拿到子 pid；子进程**从未执行过 kernel_clone 的返回路径**——它第一次被调度时从 `ret_from_fork_asm` 开始，栈顶的 `inactive_task_frame` 已被伪造成"一次函数调用的现场"，`schedule_tail` 收尾后沿 `syscall_exit_to_user_mode` 返回用户态，此时 rax=0（`childregs->ax=0`）就是 fork 在子进程的返回值 0。

```c
/* arch/arm64/kernel/process.c，6.6 精简 */
int copy_thread(struct task_struct *p, const struct kernel_clone_args *args)
{
	memset(&p->thread.cpu_context, 0, sizeof(struct cpu_context));
	if (likely(!args->fn)) {               /* 普通用户 fork */
		*childregs = *current_pt_regs();
		childregs->regs[0] = 0;            /* x0=0：子进程返回值 0 */
		...
	} else {                               /* kthread / user_mode_thread */
		memset(childregs, 0, sizeof(struct pt_regs));
		childregs->pstate = PSR_MODE_EL1h | PSR_IL_BIT;  /* 误 ERET 即非法 */
		p->thread.cpu_context.x19 = (unsigned long)args->fn;   /* fn 存 x19 */
		p->thread.cpu_context.x20 = (unsigned long)args->fn_arg; /* arg 存 x20 */
	}
	p->thread.cpu_context.pc = (unsigned long)ret_from_fork;
	p->thread.cpu_context.sp = (unsigned long)childregs;
	p->thread.cpu_context.fp = (unsigned long)childregs->stackframe;
}
```

arm64 的差异是本质性的：它没有 x86 那种"伪造返回地址"的帧，而是经典的 **`cpu_context`（pc/sp/x19/x20）四件套**——`__switch_to` 恢复 `cpu_context` 后跳到 `ret_from_fork`；`ret_from_fork`（`arch/arm64/kernel/entry.S`）里 `cbz x19, 1f` 判断是否 kthread：是则 `blr x19` 调用内核线程函数（返回即 `do_exit`，由 kthread 框架保证不返回）。x86 则在 C 函数 `ret_from_fork()` 里判断 `fn` 非空则调用，且注释特别说明：**kthread 允许在成功 `kernel_execve()` 后"返回"到这里**——这就是 kthreadd 派生的任务最终变成用户进程（如 modprobe）的机制。

> 💡 **kthread 与用户任务的分岔点**就是 `copy_thread` 里那个 `args->fn`/`PF_KTHREAD` 判断：kthread 的 pt_regs 被清零（永远不会"返回用户态"），用户任务的 pt_regs 复制自父进程。`kernel_thread()` 创建时带 `CLONE_VM`（借用 kthreadd 的地址空间概念——反正 kthread 不用用户地址）且 `args->kthread=1`，随后 `copy_process` 里置 `PF_KTHREAD`。

---

## 四、alloc_pid 与 PID 数据结构

### 4.1 struct pid / upid（include/linux/pid.h）

```c
struct upid {              /* 每层 namespace 一个槽位（柔性数组存于 struct pid 尾部）*/
	int nr;                /* 该层 namespace 内的 PID 编号 */
	struct pid_namespace *ns;
};

struct pid {
	refcount_t count;
	unsigned int level;            /* 最高层 ns 的层级（根为 0）*/
	spinlock_t lock;
	struct hlist_head tasks[PIDTYPE_MAX]; /* 挂"用这个 pid 的任务"：
	                                        PID/TGID/PGID/SID 四种类型各一条链 */
	struct hlist_head inodes;      /* proc inode 关联 */
	wait_queue_head_t wait_pidfd;  /* pidfd 的等待队列（任务退出时唤醒 poll）*/
	struct rcu_head rcu;
	struct upid numbers[];         /* level+1 个，numbers[0]=最外层（init_pid_ns）*/
};
```

关键认知：**task_struct 里存的 pid/tgid/ppid/pgid/sid 全是 int 编号，而"编号 ↔ 任务"的翻译要通过 `struct pid`**。`p->thread_pid` 指向本任务的 `struct pid`；`p->pids[PIDTYPE_*]` 的 hlist_node 把任务挂进对应 `struct pid` 的 `tasks[]` 链。`find_task_by_vpid()` = vpid → `struct pid`（查当前 ns 层的 idr）→ `pid_task(pid, PIDTYPE_PID)`（查 tasks 链）。

`PIDTYPE_MAX` 为 4：`PIDTYPE_PID`（线程 id）、`PIDTYPE_TGID`（进程/线程组 id，只挂在 leader 上）、`PIDTYPE_PGID`（进程组，挂 leader）、`PIDTYPE_SID`（会话，挂 leader）。**线程组里只有 leader 挂 TGID/PGID/SID 三条链**——这就是"kill(pid) 只发给进程、getpid() 返回 tgid"的结构基础。

### 4.2 alloc_pid：多层 idr 的自上而下分配（kernel/pid.c）

```c
struct pid *alloc_pid(struct pid_namespace *ns, pid_t *set_tid, size_t set_tid_size)
{
	pid = kmem_cache_alloc(ns->pid_cachep, GFP_KERNEL); /* ★ 缓存大小随 ns 层级变化：
	                                                        浅 ns 的 pid 结构更小 */
	tmp = ns; pid->level = ns->level;
	for (i = ns->level; i >= 0; i--) {     /* 从最内层 ns 向外逐层要号 */
		if (set_tid_size) {                /* clone3 set_tid：CRIU 精确指定 */
			tid = set_tid[ns->level - i];
			...  /* 1 ≤ tid < pid_max、目标 ns 必须已有 child_reaper、
			        checkpoint_restore_ns_capable() 权限检查 */
		}
		idr_preload(GFP_KERNEL);
		spin_lock_irq(&pidmap_lock);
		if (tid)
			nr = idr_alloc(&tmp->idr, NULL, tid, tid + 1, GFP_ATOMIC);
		else {
			int pid_min = 1;
			if (idr_get_cursor(&tmp->idr) > RESERVED_PIDS)
				pid_min = RESERVED_PIDS;  /* ★ 1~299 保留给系统：PID 分配
				                              首轮用尽后从 300 起循环 */
			nr = idr_alloc_cyclic(&tmp->idr, NULL, pid_min, pid_max, GFP_ATOMIC);
		}
		spin_unlock_irq(&pidmap_lock); idr_preload_end();
		pid->numbers[i].nr = nr;
		pid->numbers[i].ns = tmp;
		tmp = tmp->parent;                 /* 走向更外层 */
	}
	/* 初始化 count/tasks/wait_pidfd 后：*/
	for (upid = pid->numbers + ns->level; upid >= pid->numbers; --upid) {
		idr_replace(&upid->ns->idr, pid, upid->nr); /* 先存 NULL 再替换：防 find_pid_ns
		                                              看到半初始化结构 */
		upid->ns->pid_allocated++;
	}
	return pid;
}
```

要点：**每层 pid namespace 有独立 idr，同一个 `struct pid` 在每层拿一个号**（容器内 PID 100 对宿主机可能是 34527——这就是两个视角）；分配用 radix 树（XArray/idr），`idr_alloc_cyclic` 从游标处环形分配（比老内核的位图+last_pid 更简单且免锁竞争）；1~299 为 RESERVED_PIDS。失败回滚要在 `pidmap_lock` 下逐层 `idr_remove`。`pid_max` 上限：64 位系统 `PID_MAX_LIMIT = 4194304`（`include/linux/threads.h`），默认 `PID_MAX_DEFAULT = 32768`；namespace 嵌套最多 `MAX_PID_NS_LEVEL = 32` 层。

### 4.3 挂接：copy_process 阶段⑤（tasklist_lock 写锁下）

拿到 pid 后，`copy_process` 在 `write_lock_irq(&tasklist_lock)` 下完成身份登记（这段任何一步都不再允许失败）：

```c
	p->pid = pid_nr(pid);                  /* 最外层编号（数值上等于全局视角）*/
	if (clone_flags & CLONE_THREAD) {
		p->group_leader = current->group_leader;   /* ★ tgid 复用：线程的 getpid()
	                                                   返回的是 leader 的 pid */
		p->tgid = current->tgid;
	} else {
		p->group_leader = p;
		p->tgid = p->pid;
	}
	/* CLONE_PARENT/CLONE_THREAD：real_parent 复用 current 的；否则 = current。
	   CLONE_THREAD 的 exit_signal = -1（线程死不发 SIGCHLD）*/
	...
	init_task_pid(p, PIDTYPE_PID, pid);
	if (thread_group_leader(p)) {
		init_task_pid(p, PIDTYPE_TGID, pid);
		init_task_pid(p, PIDTYPE_PGID, task_pgrp(current));  /* 继承进程组/会话 */
		init_task_pid(p, PIDTYPE_SID, task_session(current));
		if (is_child_reaper(pid)) {        /* 该 ns 的 1 号：当容器 init */
			ns_of_pid(pid)->child_reaper = p;
			p->signal->flags |= SIGNAL_UNKILLABLE;  /* init 免疫默认致命信号 */
		}
		p->signal->shared_pending.signal = delayed.signal;  /* fork 期间延迟的
		                                                       多播信号此刻生效 */
		list_add_tail(&p->sibling, &p->real_parent->children); /* 挂父的 children */
		list_add_tail_rcu(&p->tasks, &init_task.tasks);        /* 挂全局任务链 */
		attach_pid(p, PIDTYPE_TGID/PGID/SID);
	} else {
		current->signal->nr_threads++;
		atomic_inc(&current->signal->live);
		refcount_inc(&current->signal->sigcnt);
		task_join_group_stop(p);            /* 出生即加入进行中的组停止 */
		list_add_tail_rcu(&p->thread_group, &p->group_leader->thread_group);
	}
	attach_pid(p, PIDTYPE_PID);
	nr_threads++;
```

一个常被忽略的细节：**fork 出的子进程立刻继承父的 PGID/SID**（`task_pgrp(current)`），所以 shell 作业控制里"同一作业"的身份是跨 fork 传播的——只有 `setpgid/setsid` 主动改。

---

## 五、wake_up_new_task 与 vfork 契约

`kernel_clone` 拿到 `copy_process` 的产物后调 `wake_up_new_task(p)`：从 `TASK_NEW` 直接入队（`activate_task` + `check_preempt_curr`），绕过 `try_to_wake_up` 的常规状态机；新任务的首次运行从 `ret_from_fork*` 开始。**wake_up_new_task 的完整语义（`p->se.vlag` 的继承修正、负载均衡的入队钩子、sched_post_fork 与 core cookie 的交接）在调度器分册第 4 章 §五 已详述**，本章不重复——两条路径在 `context_switch` 处汇合。

**vfork 的 complete 契约**（本章独有）：`CLONE_VFORK` 时父进程在栈上建 `struct completion vfork`，`p->vfork_done = &vfork`，`get_task_struct(p)`（防子进程先死导致悬空指针），然后 `wait_for_vfork_done()` 以 `TASK_UNINTERRUPTIBLE|TASK_KILLABLE|TASK_FREEZABLE` 睡眠。**唤醒方**是子进程的 `mm_release()`（`kernel/fork.c:1616`）——它只在两个时机被调用：`exit_mm_release()`（退出）与 `exec_mm_release()`（exec 换地址空间）。`mm_release` 里：

```c
	if (tsk->clear_child_tid) {       /* CLONE_CHILD_CLEARTID 的 pthread 用 */
		if (atomic_read(&mm->mm_users) > 1) {
			put_user(0, tsk->clear_child_tid);        /* futex 约定：写 0 */
			do_futex(tsk->clear_child_tid, FUTEX_WAKE, 1, ...); /* 唤醒 join 者 */
		}
		tsk->clear_child_tid = NULL;
	}
	if (tsk->vfork_done)
		complete_vfork_done(tsk);     /* vfork 父进程放行 */
```

也就是说 vfork 的语义内核并不强制"不许写内存"——它只保证**父进程睡到子进程 exec 或 exit 为止**；期间父子共享同一 mm（CLONE_VM），子进程乱写等于改父进程的内存，规范性全靠用户态自觉（这也是 vfork 被标记"危险"的原因）。`kthread_stop()` 也复用这个 completion 机制做同步。

---

## 六、性能与观测

### 6.1 fork 的成本解剖

| 开销项 | 量级 | 说明 |
| :--- | :--- | :--- |
| task_struct + 内核栈分配 | ~16K+ 栈，slab 缓存友好 | VMAP_STACK 下还可能 vmalloc |
| 页表 COW 复制 | **O(VMA 数 × 已分配页)** | 大 RSS 进程的主要成本；`copy_page_range` 全程持父 mmap_lock 写锁 |
| TLB 冲刷 | 1 次 `flush_tlb_mm`（父） | 写保护 PTE 后必须冲掉旧可写项 |
| maple tree 重建 | O(VMA 数) | 6.6 起 `vma_iter_bulk_alloc/store` 批量化，比逐个插入省 |
| fd 表 / creds / sighand 复制 | O(打开 fd 数) | dup_fd 每个文件只是引用 +1 |
| pid idr 分配 | 常数 | 每层 ns 一次 |

写时复制的触发：**父或子首次写 COW 页** → page fault → `do_wp_page` 复制页面。这既是 fork 便宜的根源（子进程立刻 exec 时几乎零页复制——vfork/posix_spawn 进一步连页表都不复制），也是"fork 炸弹 + 大内存"危险的根源（OOM 时子进程大面积写触发复制风暴）。

### 6.2 观测工具箱

```
/proc/sys/kernel/pid_max          # PID 空间上限（写之立即生效；>RESERVED_PIDS 起循环）
/proc/sys/kernel/threads-max      # 全局线程上限（boot 时按 totalram 估算：
                                   #   memsize / (THREAD_SIZE*8)，clamp 到 [20, 2^30]）
/proc/sys/kernel/threads-max 与 ulimit -u 的关系：
                                   # RLIMIT_NPROC per-uid（6.6 走 ucounts，按 user ns 分层）
/sys/fs/cgroup/<path>/pids.max    # pids cgroup：容器内进程数硬顶（fork 返回 -EAGAIN/
                                   # -ERESART... 场景先查这里）
/proc/loadavg 第四字段            # 最近 1 分钟创建的任务数（fork 速率速查）
/proc/slabinfo | grep task_struct # task_struct 存量（跟 nr_threads 对照）
```

- **`perf trace -e 'sched:sched_process_fork'`** 或 `perf record -e sched:sched_process_fork -e sched:sched_process_exec`：fork/exec 的时序与调用方栈。
- **`perf trace fork`**（对某命令）：直接量 fork 延迟分布。
- bpftrace 统计每秒 fork 速率与父进程：

```bash
bpftrace -e 'tracepoint:sched:sched_process_fork { @[comm] = count(); }'
```

- **排障三连**：fork 失败返回 EAGAIN → 依次查 `pids.max`（cgroup）、`ulimit -u`/ucounts（RLIMIT_NPROC）、`threads-max`；容器场景务必确认在**哪个 namespace 视角**下数进程（宿主 `ps` 与容器内 `ps` 的差值就是 ns 隔离的部分）。
- `pidfd`（`CLONE_PIDFD` 或 `pidfd_open()`）观测子进程退出：`poll()` 返回 `POLLIN` 即子进程已退——比 SIGCHLD+wait 轮询更适合现代运行时（`/proc/PID` 消失亦可作旁证）。

---

## 七、思考题

1. **为什么 `dup_task_struct` 里 `*dst = *src` 之后，紧接着要 `set_task_stack_end_magic` 且不能省略 `setup_thread_stack`？** 提示：新 task_struct 复制了父的 `thread_info`（`CONFIG_THREAD_INFO_IN_TASK`）与 `stack` 指针——若不重定向，新任务的 thread_info 仍指向**父的栈**；再想 STACK_END_MAGIC 探测的是谁的栈。
2. **`refcount_set(&tsk->rcu_users, 2)` 的两份引用分别由谁、在什么路径归零？** 若某个 ptrace 诊断工具长期持有 task 指针，会阻塞哪一份？提示：`release_task`（用户可见状态）与 `finish_task_switch` 的 `TASK_DEAD` 分支（调度器）；`usage` 与 `rcu_users` 是两个不同计数。
3. **`sched_fork` 把 vruntime 归零、slice 设为 `sysctl_sched_base_slice`——若改成"继承父的 vruntime"，攻击者如何占便宜？若 EEVDF 的 `vlag` 也归零，新任务的第一个切片会怎样被安排？** 提示：结合调度器分册第 3 章 `pick_eevdf` 的负 lag 优先；想想 `wake_up_new_task` 对 child 的 vlag 修正（`place_entity` 的 `ENQUEUE_INITIAL`）。
4. **6.6 的 `dup_mmap` 里 `mmap_write_lock_nested(mm, SINGLE_DEPTH_NESTING)` 为什么不是普通 `mmap_write_lock(mm)`？`vma_iter_bulk_alloc` 预分配 maple 节点解决了什么失败模式？** 提示：新 mm 还没被任何任务引用（无锁序问题）；中途分配失败会让"复制了一半的树"如何回滚。
5. **kthread 与用户任务在 `copy_thread` 的分岔点之后，各自第一次被调度时的执行起点有何不同？`ret_from_fork` 的 x86 版本为什么允许 `fn(fn_arg)` 返回后继续走 `syscall_exit_to_user_mode`？** 提示：`kernel_execve` 是谁调用的（kthreadd 的孩子）；返回用户态时 pt_regs 里的 ip 是谁填的。
6. **设计实验**：写一个分配 2GB 匿名内存并触碰一遍的程序，分别测量 fork 后 (a) 立即 exit (b) 立即 execve (c) 子进程写每一页 三种场景的耗时，用 `perf stat` 与 `/proc/PID/status` 的 VmRSS/Forks 解释差异来源。提示：COW 只在 (c) 触发；页表复制在三者中都已付出。

---

> 下一章：[execve 与 mm 替换](02-execve与mm替换.md) —— 一个进程如何在原地把"灵魂"（地址空间、凭证、信号表、映像）整体换掉，而 PID 纹丝不动。
