# exit 与僵尸孤儿（Linux 6.6）

> 阅读目标：退出一半在路上，一半在别人手里——本章前半逐段拆解 `do_exit()` 的真实顺序（6.6 的 `exit_signals` 置 `PF_EXITING` 在**很早**的位置，此后信号快速路径只认 SIGKILL；`exit_mm` 的 lazy TLB 转身；`do_task_dead` 的"不归路"与栈的延迟释放），后半讲**僵尸的诞生与消亡**：`exit_notify` 的通知协议（含 ptrace 的两阶段退出）、`wait4/waitid → wait_task_zombie → release_task` 的收尸流水线、父进程先死时的 `forget_original_parent → find_new_reaper` 三级领养、SA_NOCLDWAIT/SIG_IGN 的自动收尸、容器 init 的特殊性。所有顺序均已对照 v6.6 的 kernel/exit.c 逐行核对。

---

## 一、退出入口：谁在调 do_exit

```
exit(status)              SYSCALL_DEFINE1(exit)      → do_exit((status&0xff)<<8)
exit_group(status)  ★常用  SYSCALL_DEFINE1(exit_group) → do_group_exit(...) → do_exit
main() return             glibc crt 调 exit_group      （exit 只杀本线程！）
致命信号默认动作            get_signal → do_group_exit  （组退出）
kthread fn 返回            kthread 框架 → do_exit
make_task_dead(signr)      内核 oops 自清理路径（限制次数，见 ras 分册）
```

`do_group_exit()`（kernel/exit.c:998）处理组语义：已置 `SIGNAL_GROUP_EXIT` 则沿用 `group_exit_code`（并发退出只认第一个码）；正被 mt-exec 占用（`group_exec_task` 非空，第 2 章 §5.2 的场景）则以 0 退出让 exec 继续；否则置组退出标志并 `zap_other_threads()` 广播 SIGKILL，最后 `do_exit(exit_code)`。

`do_exit` 的 code 编码约定：`exit(1)` → 传入 `(1 & 0xff) << 8 = 0x100`；被信号杀死 → 低 7 位信号号（`code & 0x7f`），coredump 再加 `0x80`。父进程 `WEXITSTATUS/WTERMSIG/WCOREDUMP` 解的正是这个布局。

---

## 二、do_exit 逐段（kernel/exit.c:809，6.6 真实顺序）

### 2.1 全景流程图

```
do_exit(code)
 ├─ ① synchronize_group_exit：quick_threads--；归零者置 SIGNAL_GROUP_EXIT
 │     （此函数与 6.4 的 quick_threads 优化配套：无并发竞态地决定组退出码）
 ├─ ② kcov/kmsan_task_exit；coredump_task_exit；ptrace_event(PTRACE_EVENT_EXIT)
 ├─ ③ io_uring_files_cancel()                    /* 等待/取消与 fd 绑定的 ring */
 ├─ ④ exit_signals(tsk)                          /* ★ 置 PF_EXITING（§2.2）*/
 ├─ ⑤ sync_mm_rss；group_dead = atomic_dec_and_test(&tsk->signal->live)
 │     group_dead 时：is_global_init→panic("Attempted to kill init!")；
 │     hrtimer_cancel(real_timer)+exit_itimers；setmax_mm_hiwater_rss
 ├─ ⑥ acct_collect/taskstats_exit                /* 进程记账（BSD acct/taskstats）*/
 ├─ ⑦ exit_mm()                                  /* ★ 地址空间退役（§2.3）*/
 ├─ ⑧ exit_sem → exit_shm → exit_files → exit_fs /* SYSV UNDO/共享内存/fd 表/fs_struct */
 │     group_dead 时 disassociate_ctty(1)         /* 释放控制终端 */
 ├─ ⑨ exit_task_namespaces → exit_task_work      /* task_work_run：跑完全部挂起工作
 │                                                  （io_uring 的 TWA 清理在此收尾）*/
 ├─ ⑩ exit_thread（架构：x86 释放 IO bitmap/ds）
 ├─ ⑪ perf_event_exit_task                       /* perf 计数器冲账给父进程——
 │                                                  必须在 exit_notify 之前，父进程
 │                                                  被唤醒前完成 */
 ├─ ⑫ sched_autogroup_exit_task → cgroup_exit → flush_ptrace_hw_breakpoint
 ├─ ⑬ exit_tasks_rcu_start（tasks-RCU 计数）
 ├─ ⑭ exit_notify(tsk, group_dead)               /* ★ EXIT_ZOMBIE 诞生（§三）*/
 ├─ ⑮ proc_exit_connector；debug_check_no_locks_held；exit_io_context 等零碎
 ├─ ⑯ exit_task_stack_account；check_stack_usage()  /* DEBUG_STACK_USAGE：报最小余量 */
 ├─ ⑰ preempt_disable()                          /* ★ 此后到 do_task_dead 不允许
 │                                                  抢占/睡眠 */
 ├─ ⑱ exit_rcu()；exit_tasks_rcu_finish()；lockdep_free_task()
 └─ ⑲ do_task_dead()                             /* ★ 不归路（§2.4）*/
```

### 2.2 exit_signals：PF_EXITING 的三重意义（kernel/signal.c）

```c
void exit_signals(struct task_struct *tsk)
{
	cgroup_threadgroup_change_begin(tsk);
	if (thread_group_empty(tsk) || (tsk->signal->flags & SIGNAL_GROUP_EXIT)) {
		tsk->flags |= PF_EXITING;      /* 快路径：单线程直接置位 */
		return;
	}
	spin_lock_irq(&tsk->sighand->siglock);
	tsk->flags |= PF_EXITING;
	/* 此后本任务对组播信号"隐身"：wants_signal() 会跳过 PF_EXITING 任务 */
	unblocked = tsk->blocked; signotset(&unblocked);
	retarget_shared_pending(tsk, &unblocked);  /* 组待决信号里本该给我的，
	                                              转投给组内还活着的兄弟 */
	if (JOBCTL_STOP_PENDING && task_participate_group_stop(tsk))
		group_stop = CLD_STOPPED;      /* 我退出导致组停止计数凑齐 → 补发通知 */
	...
}
```

`PF_EXITING` 置位后的连锁：`wants_signal()` 不再选它、`de_thread` 等待它、调度器的 `try_to_wake_up` 对它只剩 SIGKILL 能推进（`signal_pending_state` 对 PF_EXITING 任务强制放行致命信号）。**注意 6.6 里这一步在 do_exit 的很前面**——很多资料画在退出尾部是错的，置位早正是为了让"退出漫长清理期间"仍能被 OOM killer/用户 SIGKILL 打断推进。

### 2.3 exit_mm：最后一眼地址空间（kernel/exit.c:535）

```c
static void exit_mm(void)
{
	mm = current->mm;
	exit_mm_release(current, mm);    /* futex_exit_release + mm_release：
	                                  CLONE_CHILD_CLEARTID 清 0+futex wake、
	                                  vfork complete（fork 章 §五）*/
	if (!mm) return;                 /* kthread：无 mm 直接走 */
	sync_mm_rss(mm);
	mmgrab_lazy_tlb(mm);             /* ★ 为"继续借用"拿 mm_count 引用
	                                  （context_switch 的 lazy TLB 机制，
	                                  见调度器分册 04 §5.1）*/
	task_lock(current);
	smp_mb__after_spinlock();        /* membarrier 语义屏障 */
	local_irq_disable();
	current->mm = NULL;              /* ★ 我不再是用户任务 */
	enter_lazy_tlb(mm, current);     /* 借自己的旧 mm 当 active_mm：
	                                  避免立刻换 CR3/TTBR0 */
	task_unlock(current);
	mm_update_next_owner(mm);        /* memcg：owner 移交给同 mm 的其他任务
	                                  （children→siblings→全表搜索三级）*/
	mmput(mm);                       /* mm_users--；归零 → exit_mmap 拆地址空间 */
	if (test_thread_flag(TIF_MEMDIE))
		exit_oom_victim();           /* 我是被 OOM 选中的：解除内存压力记账 */
}
```

一个精妙细节：退出任务把**自己的旧 mm 借为 active_mm**（`enter_lazy_tlb(mm, current)`）——后续调度切到 kthread 时无需换页表，直到真正切到别的用户任务才 `mmdrop_lazy_tlb`。`mmput` 若是最后一个引用（常见单线程进程），`__mmput → exit_mmap` 把全部 VMA、页表、匿名页释放（`mm/pagefualt/pagefault.md` 与 mm 分册的 munmap 路径同源）。**多线程组里 leader 先死也没关系**：mm_users 由兄弟持有；`mm_update_next_owner` 只关心 memcg 归属。

### 2.4 do_task_dead：schedule 之后没有回头路

```c
void __noreturn do_task_dead(void)
{
	set_special_state(TASK_DEAD);   /* TASK_DEAD=0x80，且 preempt_count 已禁 */
	current->flags |= PF_NOFREEZE;   /* freezer 别再管我 */
	__schedule(SM_NONE);             /* 最后一次调度：把自己永久换下 */
	BUG();                           /* 若"回来"了，说明状态机已坏 */
	for (;;) cpu_relax();
}
```

`__schedule` 拿到 `prev_state == TASK_DEAD` 后：`deactivate_task` 出队 → `context_switch` 换人 → **`finish_task_switch`**（调度器分册 04 §5.3 已详述）里：

```c
	if (unlikely(prev_state == TASK_DEAD)) {
		if (prev->sched_class->task_dead)
			prev->sched_class->task_dead(prev);   /* fair: task_dead_fair
			                                          （cgroup 带宽/统计收尾）*/
		/* Task is done with its stack. */
		put_task_stack(prev);                     /* 内核栈引用 -1
			                                          （stack_refcount）*/
		put_task_struct_rcu_user(prev);           /* rcu_users 两份之一在此归零；
			                                          双零触发 call_rcu */
	}
```

**为什么栈不能立即释放**：`finish_task_switch` 返回后，短暂的窗口内我们**仍站在 prev 的栈上**（switch_to 的最后一个动作之后才真正切换）；且 kprobe/rethook 等可能还引用着栈帧。所以栈释放要么延迟 RCU、要么（VMAP_STACK）先进 per-CPU 缓存。task_struct 本体同样走 RCU：`delayed_put_task_struct`（kernel/exit.c:218）回调里 `kprobe_flush_task → rethook_flush_task → perf_event_delayed_put → trace_sched_process_free → put_task_struct → __put_task_struct → free_task`（slab 归还 task_struct_cachep）。

> 💡 观测佐证：`perf record -e sched:sched_process_exit` 的事件在 ⑭ exit_notify 附近发出（`trace_sched_process_exit(tsk)`），而 `sched:sched_process_free` 在 RCU 宽限期后的 `delayed_put_task_struct` 里——**两个事件的时间差就是 grace period 的长度**，可用 bpftrace 同 pid 打戳验证。

---

## 三、exit_notify：EXIT_ZOMBIE 的诞生（kernel/exit.c:727）

```c
static void exit_notify(struct task_struct *tsk, int group_dead)
{
	LIST_HEAD(dead);
	write_lock_irq(&tasklist_lock);
	forget_original_parent(tsk, &dead);          /* 先把我的孩子安顿好（§四）*/
	if (group_dead)
		kill_orphaned_pgrp(tsk->group_leader, NULL);  /* POSIX 孤儿进程组：
	                                                     有 stopped 作业 → SIGHUP+SIGCONT */
	tsk->exit_state = EXIT_ZOMBIE;               /* ★ 此刻起我是一个僵尸 */
	if (unlikely(tsk->ptrace)) {                  /* —— 被追踪中：通知 tracer —— */
		int sig = thread_group_leader(tsk) && thread_group_empty(tsk)
			  && !ptrace_reparented(tsk) ? tsk->exit_signal : SIGCHLD;
		autoreap = do_notify_parent(tsk, sig);   /* 通知方是 tsk->parent（=tracer）*/
	} else if (thread_group_leader(tsk)) {
		autoreap = thread_group_empty(tsk) &&
			do_notify_parent(tsk, tsk->exit_signal);  /* ★ 正常路径：
		                                                  给 real_parent 发 SIGCHLD */
	} else {
		autoreap = true;                          /* 非 leader 线程：谁也不通知
		                                              （僵尸 leader 会代整组收场）*/
	}
	if (autoreap) {                               /* 父进程 SIG_IGN/SA_NOCLDWAIT：
		                                              直接跳过僵尸阶段 */
		tsk->exit_state = EXIT_DEAD;
		list_add(&tsk->ptrace_entry, &dead);
	}
	if (unlikely(tsk->signal->notify_count < 0))  /* mt-exec：de_thread 在等我死 */
		wake_up_process(tsk->signal->group_exec_task);
	write_unlock_irq(&tasklist_lock);
	list_for_each_entry_safe(p, n, &dead, ptrace_entry) {
		list_del_init(&p->ptrace_entry);
		release_task(p);                          /* 立即自收（autoreap 与孤儿zap）*/
	}
}
```

三个关键语义：

- **EXIT_ZOMBIE ≠ 进程还在**：任务早已在 `do_task_dead` 永久下 CPU。僵尸只是 task_struct 的一个墓碑状态——`exit_state=EXIT_ZOMBIE`、地址空间已拆、fd 已关，**唯一保留的是 PID 编号、退出码、累计资源用量**（`getrusage` 的数据源），等待父进程 `wait()` 取走。
- **do_notify_parent 的返回值决定僵尸命运**（kernel/signal.c）：父进程对 SIGCHLD 设了 `SIG_IGN` 或 `SA_NOCLDWAIT` → 返回 true → `autoreap`：**连僵尸都不当**，当场 `release_task`（POSIX 语义：父进程明确说"我不收尸，你自便"）。
- **ptrace 的两阶段通知**：tracee 死时 `tsk->parent` 是 **tracer**（被 PTRACE_ATTACH 期间 `parent` 被替换为 tracer，`real_parent` 不变），第一份 SIGCHLD 发给 tracer；tracer `wait4` 收走或 `PTRACE_DETACH`/死亡后（`__ptrace_unlink` 恢复 `parent=real_parent`），`wait_task_zombie` 的 `EXIT_TRACE` 分支向**真实父进程**补发第二次通知。真实父进程全程只见第二次——这就是"被 gdb 追踪过的进程，gdb 退出后父进程还能 wait 到它"的机制（第 5 章展开 ptrace 全貌）。

---

## 四、父进程先死：forget_original_parent 与孤儿领养

### 4.1 为什么孤儿必须被领养

若孤儿直接挂到不存在的父节点上，它们死后将**永远无人有权 release_task**——僵尸在 PID idr、进程链表里永久占位，PID 空间被蛀空。所以 do_exit 的 exit_notify **第一步不是通知自己的父**，而是 `forget_original_parent` 安顿自己的孩子（顺序很重要：先当孝子再当尸体）。

### 4.2 三级领养（kernel/exit.c:688/621/583）

```
forget_original_parent(father, dead)
 ├─ exit_ptrace(father, dead)：我是 tracer？先给我的 tracee 断链
 │     （PT_EXITKILL 的直接补 SIGKILL；僵尸 tracee 转移给真实父并通知）
 ├─ find_child_reaper(father, dead)：
 │     我就是本 pid ns 的 child_reaper（容器/PID 1）？
 │       → 找我线程组里的活线程接班 child_reaper
 │       → 一个都没有？zap_pid_ns_processes(pid_ns)：
 │         ns 里全部进程收 SIGKILL —— pid ns 的生命周期终点
 └─ find_new_reaper(father, child_reaper)：
       ① 同线程组还活着的人（"annoying 兄弟线程接管"——源码注释的戏称，
          最常见：主线程死、worker 线程领走全部孩子）
       ② 沿 real_parent 向上找第一个 is_child_subreaper 的祖先
          （prctl(PR_SET_CHILD_SUBREAPER,1)——systemd/tmux 这类服务管理器
          "领养服务孙进程"的机制；靠 fork 时继承的 has_child_subreaper
          标志免每次遍历）
       ③ 兜底：本 pid ns 的 init（child_reaper）
   然后遍历 father->children 的每个孩子的每个线程：
       real_parent = reaper;  !ptrace 的同时 parent = real_parent
       pdeath_signal → group_send_sig_info（PR_SET_PDEATHSIG 语义：
       "父死则子死"——注意领养时立刻兑现，所以它经常在孤儿化瞬间触发）
   !same_thread_group(reaper, father) 时 reparent_leader 每个孩子：
       exit_signal = SIGCHLD
       已是僵尸且线程组空 → do_notify_parent(新父)：
         新父若 SIG_IGN → EXIT_DEAD 进 dead 名单（当场释放）
         否则新父收到一份迟来的 SIGCHLD（"兄终弟及通知"——僵尸孙辈
         被新父收编，init/supreaper 因此能替烂尾父进程收尸）
   最后 list_splice_tail_init(&father->children, &reaper->children)
```

`reparent_leader` 里那句 `p->exit_signal = SIGCHLD;`（注释 *"We don't want people slaying init."*）补的洞：孩子可能被 clone 出来带任意 exit_signal（clone3 允许），若领养后保持原值，init 可能收到奇怪的信号。

### 4.3 孤儿的"双阶段"

孤儿的一生有两个被收割的机会：**(a) 生父死亡时**——`reparent_leader` 只对**已死**的孩子发通知（活的等它们自己死时通知新父）；**(b) 孤儿自己死亡时**——`exit_notify` 的 `do_notify_parent` 找的是**当时的** real_parent（即领养者）。若领养者是 `SA_NOCLDWAIT` 的 systemd，孤儿连僵尸都不当——这就是"容器里进程死了不产生 defunct"的正常情形；反过来，**容器 init 若从不 wait**，所有被领养的孤儿僵尸都会挂在 PID 1 下堆积——僵尸堆积两大成因之二（成因之一：普通父进程 fork 后不 wait 就长期存活）。

---

## 五、收尸协议：wait 家族与 release_task

### 5.1 do_wait：等在 wait_chldexit 上

`wait4/waitpid/waitid/wait3` 全部汇入 `kernel_waitid → do_wait`（kernel/exit.c:1585）：

```
do_wait(wo)
 ├─ 挂 wait_queue 到 current->signal->wait_chldexit   /* ★ 组级等待队列：
 │                                                      任何线程 wait、任何
 │                                                      线程的孩子死都会唤醒
 │                                                      （child_wait_callback）*/
 ├─ repeat:
 │    read_lock(tasklist_lock)
 │    遍历本组每个线程的 children 链（do_wait_thread）+ 各自的 ptraced 链
 │      → wait_consider_task：eligibility（wo_pid 匹配/wo_type）后按状态分流：
 │          EXIT_DEAD  → 跳过（正被别人收尸）
 │          EXIT_TRACE → 只对 ptrace 查询者可见
 │          EXIT_ZOMBIE→ wait_task_zombie ★
 │          活着       → wait_task_stopped（WUNTRACED/WSTOPPED）
 │                        / wait_task_continued（WCONTINUED）
 │    read_unlock
 │    无收获且 !WNOHANG → schedule() 睡在 chldexit 队列上，醒来 goto repeat
 └─ __set_current_state(TASK_RUNNING) 返回
```

唤醒方：`do_notify_parent` 里的 `__wake_up_parent`（SIGCHLD 投递时）。

### 5.2 wait_task_zombie：唯一收割者（kernel/exit.c:1099）

```c
static int wait_task_zombie(struct wait_opts *wo, struct task_struct *p)
{
	/* WNOWAIT：只取状态不收尸（守尸权仍在，供多线程 wait 各取所需）*/
	if (wo->wo_flags & WNOWAIT) { ...取 status/rusage 后直接返回... }
	/* ★ cmpxchg 抢收割权：ZOMBIE → DEAD（正常）或 TRACE（ptrace_reparented 且
	   leader）。并发 wait 的其他线程在此被挡（cmpxchg 失败返回 0）*/
	state = (ptrace_reparented(p) && thread_group_leader(p)) ? EXIT_TRACE : EXIT_DEAD;
	if (cmpxchg(&p->exit_state, EXIT_ZOMBIE, state) != EXIT_ZOMBIE)
		return 0;
	read_unlock(&tasklist_lock);          /* 收割不持 tasklist 锁（下面可能慢）*/
	/* 资源账合并：把 p 与其整组（signal->cutime/cstime...孙辈累计）并入
	   父进程 signal_struct 的 c* 字段——ps 的 TIME 累计、getrusage(RUSAGE_CHILDREN)
	   的数据在此更新；psig->stats_lock + siglock 双锁保护 */
	if (state == EXIT_DEAD && thread_group_leader(p)) {
		thread_group_cputime_adjusted(p, &tgutime, &tgstime);
		write_seqlock(&psig->stats_lock);
		psig->cutime += tgutime + sig->cutime;   /* cutime 含"孙辈的孙辈"——
		                                           父进程先死时 reparent_leader
		                                           已把账转移给领养者 */
		...
	}
	status = (p->signal->flags & SIGNAL_GROUP_EXIT)
		? p->signal->group_exit_code : p->exit_code;  /* 组退出优先 */
	if (state == EXIT_TRACE) {           /* ptrace 僵尸：unlink + 二次通知（§三）*/
		ptrace_unlink(p);
		state = do_notify_parent(p, p->exit_signal) ? EXIT_DEAD : EXIT_ZOMBIE;
		p->exit_state = state;           /* tracer 收了一次，真实父还能再 wait
		                                   到（除非其父 SIG_IGN）*/
	}
	if (state == EXIT_DEAD)
		release_task(p);                 /* ★ 真正拆墓碑（§5.3）*/
	/* 填 waitid 的 infop：cause=CLD_EXITED(status>>8)/CLD_KILLED(0x7f)/
	   CLD_DUMPED(0x80) */
	return pid;
}
```

### 5.3 release_task：墓碑拆除流水线（kernel/exit.c:239）

```
release_task(p)
 ├─ dec_rlimit_ucounts(UCOUNT_RLIMIT_NPROC)   /* 配额归还（fork 章 §2.4 的镜像）*/
 ├─ cgroup_release(p)
 ├─ write_lock_irq(tasklist_lock)
 │   ├─ ptrace_release_task(p)
 │   ├─ __exit_signal(p)：                      /* 组级记账与摘链（kernel/exit.c:143）*/
 │   │    posix_cpu_timers_exit → tsk->sighand=NULL → __cleanup_sighand
 │   │    （引用归零才真释放 sighand_struct——CLONE_SIGHAND 共享者还在用）
 │   │    stats_lock 下：sig->utime/stime/min_flt/... += p 的值
 │   │      （线程的 CPU 时间死后并入组账——getrusage(RUSAGE_SELF) 的数据源）
 │   │    __unhash_process(p, group_dead)：nr_threads--; detach_pid(PID)；
 │   │      group_dead 再 detach TGID/PGID/SID、list_del_rcu(&p->tasks)
 │   │      （全局进程链）、list_del_init(&p->sibling)（父的 children 链）、
 │   │      process_counts--；list_del_rcu(thread_group/thread_node)
 │   └─ zap_leader：我是组里最后一个非 leader 线程且 leader 早成僵尸？
 │        do_notify_parent(leader) → 新通知或当场连锁 release_task(leader)
 │        （goto repeat——leader 的账也要有人收）
 ├─ write_unlock
 ├─ seccomp_filter_release；proc_flush_pid(thread_pid)  /* /proc 缓存失效 */
 ├─ put_pid(thread_pid)                        /* struct pid 引用归零 → kfree
 │                                                （idr 里已 detach）*/
 ├─ release_thread(p)（架构）
 └─ put_task_struct_rcu_user(p)                /* rcu_users 归零 → call_rcu
                                                  delayed_put_task_struct
                                                  → … → free_task（§2.4 已述）*/
```

至此 PID 编号归还 idr、task_struct 进 RCU 释放队列、父进程的 children 链恢复干净——一次生命周期的账全部两清。

---

## 六、自动收尸与 init 的特殊性

### 6.1 两条自动收尸通道（do_notify_parent 内）

```c
	if (!tsk->ptrace && sig == SIGCHLD &&
	    (psig->action[SIGCHLD-1].sa.sa_handler == SIG_IGN ||
	     (psig->action[SIGCHLD-1].sa.sa_flags & SA_NOCLDWAIT))) {
		/* POSIX：自动收割，不留僵尸 */
		... autoreap = true; （内核态直接完成，父进程感知不到 SIGCHLD）
	}
```

`SIG_IGN` 与 `SA_NOCLDWAIT` 的细微差别（signal 章 §详述）：SIG_IGN 时代码不投递信号；SA_NOCLDWAIT 则**信号照发**（若同时有 handler），只是孩子不被留成僵尸。shell 的 `signal(SIGCHLD, SIG_IGN)` 技巧、Go runtime 的 `SIGCHLD=SIG_IGN`（fork/exec 执行外部命令）都吃这条快速通道——**代价是 wait 取不到子进程退出码**，只能靠 `$?` 之外的机制。

### 6.2 init 的特殊退出语义

- **全局 init 之死即 panic**：`do_exit` 里 `is_global_init(tsk) && group_dead → panic("Attempted to kill init!")`——内核无法想象没有 init 的世界（收尸兜底消失）。
- **SIGNAL_UNKILLABLE**（fork 章见其置位）：容器/PID-ns init 对"默认动作为致命"的信号免疫（`sig_task_ignored`：`handler == SIG_DFL` 且无 force 时直接忽略；全局 init 更连 `sig_kernel_only`（SIGKILL/SIGSTOP）都不收）。**容器 init 被 `kill -9` 杀不死是特性不是 bug**——要终结容器得从父 ns 发力（runtime 对 cgroup 的操作）或 init 自己装 handler 后自行处理。这正是许多"僵尸堆满容器"排障案例的根源：**PID 1 装了 SIGCHLD handler 却不 wait**（或对未知信号一律 reap 失败）。
- **pid ns 终结**：init 死 → `find_child_reaper → zap_pid_ns_processes` 全 ns 广播 SIGKILL → 全员 `do_group_exit`。ns 内的最后清场同样依赖收尸协议（子 ns 的 init 死后，其子进程的 real_parent 早已是它——`forget_original_parent` 在 ns 内无领养者时走 zap 路径）。

---

## 七、观测与排障

### 7.1 僵尸的观测

```
ps aux | awk '$8 ~ /Z/ || /defunct/'   # state Z / [进程名] <defunct>
cat /proc/PID/status | grep State       # State:  Z (zombie)
                                        # （僵尸仍有 /proc/PID/{status,stat}，
                                        #  但 maps/exe/cwd 等已消失——
                                        #  地址空间早拆了）
ls /proc/PID/task/                      # 僵尸 leader 若还有活线程：
                                        #  非 leader 不留僵尸，leader 会一直
                                        #  拖到最后一个线程死（delay_group_leader）
```

### 7.2 堆积归因三板斧

1. **父是谁**：`ps -o ppid= -p <僵尸pid>` → 看它 `wait` 行为：`strace -p <父pid> -e trace=wait4`、`/proc/<父pid>/syscall`。
2. **父是否 init/领养者**：孤儿会被领养到 subreaper 或 PID 1——`cat /proc/<僵尸>/status | grep PPid` 指向 PID 1 时，问题变成"容器 init 的收割策略"。
3. **量级与速率**：`perf trace -e sched:sched_process_exit -e sched:sched_process_free` 两事件差值 = 在途僵尸数；`/proc/slabinfo` 的 `task_struct` 活跃对象数 vs `nr_threads`（`/proc/loadavg` 第三列）——差值里就有僵尸占的坑。

```
bpftrace -e 'tracepoint:sched:sched_process_exit { @exit[pid, comm] = nsecs }
  tracepoint:sched:sched_process_free /@exit[args->pid]/
  { printf("zombie %d (%s) lived %d ms\n", args->pid, comm,
           (nsecs-@exit[args->pid])/1000000); delete(@exit[args->pid]) }'
```

### 7.3 经典成因速查

| 现象 | 成因 | 处置 |
| :--- | :--- | :--- |
| 少量僵尸、父进程健在 | 父 fork 后不 wait（常见于脚本循环起子进程） | 父进程补 wait / `SIGCHLD=SIG_IGN` / `PR_SET_CHILD_SUBREAPER` 服务接管 |
| 僵尸全挂 PID 1 下（容器） | init 未 wait 被领养的孤儿 | init 循环 wait（systemd/tini/dumb-init 的工作原理）；或给业务进程装 subreaper |
| `kill -9` 杀不死容器 PID 1 | SIGNAL_UNKILLABLE 特性 | 从宿主侧 `kill`（宿主 ns 的 pid 视角）/ 停容器 |
| 僵尸"突然消失" | 父进程退出触发领养+reparent_leader 二次通知，新父收割 | 无需处置（这是机制自愈） |

---

## 八、思考题

1. **为什么 `TASK_DEAD` 的任务栈不能在 `do_task_dead` 里当场 kfree？给出至少两个独立的理由。** 提示：`finish_task_switch` 返回到谁栈上；kprobe/rethook 对栈上 trampoline 的引用；`CONFIG_VMAP_STACK` 下 per-CPU `cached_stacks` 缓存的意义。
2. **`exit_mm` 之后、`do_task_dead` 之前，任务仍会多次调用可能睡眠的函数（如 `mmput → exit_mmap`、`exit_task_work → task_work_run`）。此时 `current->mm == NULL`，`mmap_read_lock` 之类的用户地址空间操作如何不崩？** 提示：exit_mmap 锁的是**即将退役的 mm 对象**（不是"current->mm"）；lazy TLB 的 active_mm 借用；`context_switch` 对 `!next->mm` 的处理（调度器分册 04 §5.1）。
3. **`wait_task_zombie` 用 `cmpxchg(EXIT_ZOMBIE → EXIT_DEAD/EXIT_TRACE)` 抢收割权。若两个线程同时对同一僵尸 wait，输的那个返回什么？`WNOWAIT` 的存在解决了什么应用需求？** 提示：POSIX 允许多线程 wait 的语义（ECHILD/0）；`WNOHANG|WNOWAIT` 组合做"探测不收割"。
4. **`forget_original_parent` 为什么必须在 `tsk->exit_state = EXIT_ZOMBIE` **之前**跑？若顺序颠倒，race 出现在哪两个观察者之间？** 提示：领养遍历 `father->children` 时孩子的新父必须可见；`list_splice_tail_init` 原子地把孩子们过户——僵尸父的父（祖父）此刻 wait 谁？
5. **`reparent_leader` 对"活的孤儿"不发通知、对"僵尸孤儿"发通知。为什么活的要等它们自己死时再通知新父？（若领养瞬间就给新父发一份 SIGCHLD，会破坏什么？）** 提示：POSIX 信号语义——SIGCHLD 表示"状态变化"；领养不是状态变化。
6. **实验**：写一个 `N=10000` 次 fork 且子进程立即 exit 的压力程序，分别以 (a) 父循环 wait (b) 父 sleep 不 wait（结束后僵尸去哪了？）(c) 先 `signal(SIGCHLD, SIG_IGN)` 三种方式跑，用 7.2 的 bpftrace 观察僵尸存活时长分布与 `/proc/slabinfo` 的 task_struct 波动。提示：(b) 的终局由"父进程自己死 → 领养 → 二次通知"机制收尾——验证 zombie 的 PPid 是否真的变成了 1（或 subreaper）。

---

> 下一章：信号子系统（第 4 章，后续轮次补齐）—— SIGCHLD 的投递路径与 `do_notify_parent` 的另一半；`signal_pending` 如何在每次返回用户态时被检查。
