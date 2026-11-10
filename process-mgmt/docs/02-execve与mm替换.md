# execve 与 mm 替换（Linux 6.6）

> 阅读目标：exec 是进程生命周期里最"暴力"的一步——**PID 不变、task_struct 不变，但灵魂（地址空间、映像、凭证、信号表、fd 语义）被整体置换**。本章沿 v6.6 真实调用链走完全程：`execve` → `do_execveat_common` → `alloc_bprm`（先造一个**全新的 mm** 预热栈区）→ `copy_strings`（argv/envp 跨越新旧两个 mm 的搬运）→ `exec_binprm`（binfmt 链轮询、`#!` 脚本的解释器拼接、递归深度限制）→ `load_elf_binary`（ELF 头校验、PT_LOAD 映射与 load_bias 对齐技巧、`begin_new_exec` 的"不可回退点"事务语义、`de_thread` 的 PID 顶替、`start_thread` 的寄存器重置）；最后理清失败语义（为什么过了某个点进程只能死、不能报 errno）与观测手段。所有代码均已对照 v6.6。

---

## 一、入口链：execve 到 bprm 的诞生

### 1.1 调用链总览

```
execve(path, argv, envp)                        fs/exec.c SYSCALL_DEFINE3
 └─ do_execve → do_execveat_common(AT_FDCWD, ...)
execveat(fd, path, argv, envp, flags)           SYSCALL_DEFINE5（AT_EMPTY_PATH 支持）
 └─ do_execveat → do_execveat_common(fd, ...)
kernel_execve(filename, argv, envp)             内核内部（kthread→用户程序，如 modprobe/init）
 └─ 直接组装 bprm（不走用户指针，用 copy_strings_kernel）

do_execveat_common(fd, filename, argv, envp, flags)
 ├─ PF_NPROC_EXCEEDED 且 NPROC 仍超限 → -EAGAIN     （把 setuid 的失败挪到 exec 报告）
 ├─ bprm = alloc_bprm(fd, filename)                 ★ 新 mm 在这里诞生（§1.2）
 ├─ count(argv, MAX_ARG_STRINGS) / count(envp)      （E2BIG 的第一道检查：条目数）
 ├─ bprm_stack_limits(bprm)                         （栈空间上限 → bprm->argmin）
 ├─ copy_string_kernel(bprm->filename, bprm)        （文件名上栈，记 bprm->exec）
 ├─ copy_strings(bprm->envc, envp, bprm)            （环境先上栈）
 ├─ copy_strings(bprm->argc, argv, bprm)            （参数后上栈——顺序有意义，见思考题）
 ├─ argc==0 → 补一个空串 argv[0]=""
 └─ bprm_execve(bprm, fd, filename, flags)
      ├─ prepare_bprm_creds（cred_guard_mutex + prepare_exec_creds）
      ├─ check_unsafe_exec（LSM_UNSAFE_PTRACE/NO_NEW_PRIVS/SHARE）
      ├─ do_open_execat（打开文件，deny_write_access）
      ├─ sched_exec()                               （exec 是最后一次"搬家"机会：求负载均衡
      │                                               把自己迁到目标 CPU，吃新映像的 cache 局部性）
      └─ exec_binprm(bprm)                          ★ 主战场（§2、§3）
```

### 1.2 alloc_bprm 与 bprm_mm_init：先造新房子再谈搬进（fs/exec.c:1512/368）

`struct linux_binprm`（`include/linux/binfmts.h`）是整个 exec 的"事务上下文"：新 mm、栈位置游标 `p`、256 字节文件头缓存 `buf`、解释器信息、`cred`、`point_of_no_return`……而它做的第一件重活是**提前把新地址空间建好**：

```c
static int bprm_mm_init(struct linux_binprm *bprm)
{
	bprm->mm = mm = mm_alloc();          /* 全新 mm_struct（mm_cachep slab）*/
	task_lock(current->group_leader);
	bprm->rlim_stack = current->signal->rlim[RLIMIT_STACK];  /* 先快照栈上限 */
	task_unlock(current->group_leader);
	__bprm_mm_init(bprm);
}

static int __bprm_mm_init(struct linux_binprm *bprm)   /* CONFIG_MMU 版本 */
{
	vma = vm_area_alloc(mm);
	vma->vm_end  = STACK_TOP_MAX;        /* 架构支持的最大栈顶（先高后低：稍后再挪）*/
	vma->vm_start = vma->vm_end - PAGE_SIZE;
	vm_flags_init(vma, VM_SOFTDIRTY | VM_STACK_FLAGS | VM_STACK_INCOMPLETE_SETUP);
	                                 /* ★ INCOMPLETE_SETUP：此刻它还不是合法栈，
	                                    防止任何 fault 路径误认 */
	insert_vm_struct(mm, vma);
	mm->stack_vm = mm->total_vm = 1;
	bprm->p = vma->vm_end - sizeof(void *);   /* 栈游标从最高处起步，向下生长 */
}
```

为什么先建新 mm？因为 **`copy_strings` 的目的地在"还没上任"的新 mm 里**——此刻进程仍跑在旧地址空间（argv/envp 指针是旧 mm 里的），但字符串要拷进新 mm 的栈区。**一次 exec 自始至终有两个 mm 并存**：`current->mm`（旧，正在执行系统调用的那个）与 `bprm->mm`（新，只有一页栈 vma 的空壳）。直到 `begin_new_exec → exec_mmap` 才正式交接。

### 1.3 copy_strings：跨越两个 mm 的搬运（fs/exec.c:523）

```c
/* 源码注释原文：'copy_strings()' copies argument/environment strings
   from the old processes's memory to the new process's stack. */
static int copy_strings(int argc, struct user_arg_ptr argv, struct linux_binprm *bprm)
{
	while (argc-- > 0) {
		str = get_user_arg_ptr(argv, argc);     /* 源：旧 mm 的用户指针 */
		len = strnlen_user(str, MAX_ARG_STRLEN);/* 单串上限 128KB（32 页）*/
		if (!valid_arg_len(bprm, len))          /* E2BIG */
			goto out;
		pos = bprm->p;  str += len;  bprm->p -= len;
		if (bprm->p < bprm->argmin)             /* 越过 bprm_stack_limits 算的下界 */
			goto out;
		while (len > 0) {                       /* 从串尾向前、逐页搬运 */
			if (fatal_signal_pending(current))  /* OOM/被杀：可随时中止 */
				return -ERESTARTNOHAND;
			cond_resched();
			page = get_arg_page(bprm, pos, 1);  /* 目的页：在 bprm->mm 里
			                                        分配并 pin（get_user_page）*/
			kaddr = kmap_local_page(page);
			copy_from_user(kaddr + offset, str - bytes_to_copy, bytes_to_copy);
			                                    /* 源在旧 mm（copy_from_user 按
			                                       current->mm 翻译），目的在新 mm 页 */
		}
	}
}
```

三个细节值得刻在脑子里：

- **为什么"从后往前"**（`str += len; bprm->p -= len` 且逐串 `argc--` 从最后一个参数开始）：栈游标 `bprm->p` 单调下降，拷贝顺序恰好使**内存布局与 argv 数组顺序一致**（argv[0] 在最高地址）。
- **上限公式**（`bprm_stack_limits`）：`min(_STK_LIM*3/4, rlim_stack/4)`，再抬到至少 `ARG_MAX`（32 页），还要预扣 argv/envp **指针数组**的空间——"参数过长"的 E2BIG 在这里提前报，而不是等程序跑起来栈溢出。
- **每页之间 `cond_resched()` + fatal signal 检查**：拷贝几 MB 环境变量（CI 系统的真实场景）是可中断的慢路径。

---

## 二、exec_binprm 与 binfmt 链

### 2.1 search_binary_handler：formats 链轮询（fs/exec.c:1717）

```c
static int search_binary_handler(struct linux_binprm *bprm)
{
	prepare_binprm(bprm);       /* 重读 256B 文件头到 bprm->buf（解释器轮次里
	                               这是新文件的头）+ bprm_fill_uid（suid/sgid 判定）*/
	security_bprm_check(bprm);  /* LSM：SELinux/AppArmor 的 exec 策略在此拦截 */
 retry:
	read_lock(&binfmt_lock);
	list_for_each_entry(fmt, &formats, lh) {   /* binfmt_elf、binfmt_script、
	                                              binfmt_misc、binfmt_flat… */
		retval = fmt->load_binary(bprm);       /* 逐个试载 */
		if (bprm->point_of_no_return || (retval != -ENOEXEC))
			return retval;                     /* 认领成功 或 已过不可回退点：
			                                       立即返回，不再轮询 */
	}
	read_unlock(&binfmt_lock);
	if (need_retry) {          /* 全链 -ENOEXEC 且文件头不可打印：
	                               尝试 request_module("binfmt-%04x", magic)——
	                               binfmt_misc 的按需自动注册 */
		...
		goto retry;
	}
}
```

`formats` 链是全局注册表（`register_binfmt`），遍历时 `try_module_get` 防模块中途卸载。**`-ENOEXEC` 是"不是我"的暗号**：binfmt_elf 对非 ELF 头、binfmt_script 对非 `#!` 开头都返回它，链继续走。`binfmt_misc`（`/proc/sys/fs/binfmt_misc/register`）让用户态按魔数/扩展名注册处理规则（QEMU 用户态模拟 `qemu-aarch64` 的经典用法就靠它）。

### 2.2 binfmt_script：`#!` 的解释器拼接（fs/binfmt_script.c）

`load_script()` 解析 `#!/usr/bin/env python3 -u` 这一行，然后把 argv 重排为：

```
原： [脚本路径, arg1, arg2, ...]
新： [解释器名, (可选解释器参数), 脚本路径, arg1, arg2, ...]
      └── remove_arg_zero 摘掉旧 argv[0] 后逆序逐个 copy_string_kernel 塞回
```

同时 `bprm_change_interp(i_name)`（**进程名/comm 与 /proc/PID/exe 最终指向解释器**，脚本路径只是参数）并把打开的解释器文件放进 `bprm->interpreter`——然后 `return 0`，把控制权交回 `exec_binprm` 的**外层循环**：

```c
static int exec_binprm(struct linux_binprm *bprm)
{
	for (depth = 0;; depth++) {
		if (depth > 5)
			return -ELOOP;                  /* ★ 递归深度限制：最多 5 层解释器嵌套
			                                      （#!a → #!b → …）。防 /etc/binfmt
			                                      注册成环导致死循环 */
		ret = search_binary_handler(bprm);
		if (ret < 0 || !bprm->interpreter)
			break;                          /* 无解释器：载入完成 */
		exec = bprm->file;                  /* 有解释器：换主角，再来一轮 */
		bprm->file = bprm->interpreter;
		bprm->interpreter = NULL;
		fput(exec);                         /* 旧（脚本/主程序）文件关闭
		                                       ——注意：此刻若已过 point_of_no_return，
	                                       文件描述与 deny_write_access 的事务
	                                       语义见 §3.3 */
	}
	audit_bprm(bprm);
	trace_sched_process_exec(current, old_pid, bprm);  /* perf/ebpf 的 exec 事件 */
	ptrace_event(PTRACE_EVENT_EXEC, old_vpid);
}
```

ELF 的 `PT_INTERP`（动态链接器 `/lib64/ld-linux-x86-64.so.2`）走的是**同一条循环**：`load_elf_binary` 发现 PT_INTERP 后 `open_exec` 解释器并塞进 `bprm->interpreter`……不对——ELF 的 interp 是在 `load_elf_binary` **内部**直接加载并映射的（见 §3.5），`bprm->interpreter` 路径主要服务 script/misc。两者语义不同：脚本是"换文件重来"，ELF interp 是"同一次加载里把 loader 也映射进来"。

---

## 三、load_elf_binary：主战场（fs/binfmt_elf.c:823）

### 3.1 前置校验与 PT_INTERP

```c
	if (memcmp(elf_ex->e_ident, ELFMAG, SELFMAG) != 0)  goto out;  /* \x7fELF */
	if (elf_ex->e_type != ET_EXEC && elf_ex->e_type != ET_DYN) goto out; /* 静态/PIE */
	if (!elf_check_arch(elf_ex))  goto out;         /* 架构匹配（交叉执行在此被拒）*/
	elf_phdata = load_elf_phdrs(elf_ex, bprm->file); /* 读整个程序头表 */

	for (i = 0; i < e_phnum; i++) {
		if (p_type == PT_INTERP) {         /* 有解释器：读路径、open_exec、
		                                          would_dump、读其 ELF 头 */
			elf_interpreter = ...;
			interpreter = open_exec(elf_interpreter);
		}
		switch (p_type) {
		case PT_GNU_STACK:                  /* 栈可执行位：EXSTACK_ENABLE_X/DISABLE_X */
		case PT_LOPROC..PT_HIPROC:          /* 架构特定段（如 arm64 的
		                                          BTI/PAC 约束经由 arch_elf_pt_proc）*/
		}
	}
	parse_elf_properties(...);             /* GNU_PROPERTY：x86 CET/IBT、arm64 BTI */
	arch_check_elf(...);                   /* 架构还来得及拒绝（返回 errno 给调用者）*/
```

### 3.2 begin_new_exec：不可回退点（fs/exec.c:1246）

5.8 对 exec 做了安全性重构（此前失败路径可能留下"半 exec"进程），核心就是 `begin_new_exec` 把所有**不可撤销**的动作集中到一个函数里，并显式标记事务状态：

```c
int begin_new_exec(struct linux_binprm * bprm)
{
	bprm_creds_from_file(bprm);          /* 计算 suid/sgid 新凭证（LSM 参与判定）*/
	bprm->point_of_no_return = true;     /* ★★ 从这一行起，任何失败都不再返回
	                                            errno——只能 force_sig 致死（§5）*/
	de_thread(me);                       /* 杀光线程组其他成员；非 leader exec 则
	                                        顶替 leader 的 PID（§5.2）*/
	io_uring_task_cancel();              /* 取消本任务所有 io_uring 请求（fd 要换了）*/
	unshare_files();                     /* fd 表若被 CLONE_FILES 共享：先复制一份
	                                        （否则关 fd 会影响别人）*/
	set_mm_exe_file(bprm->mm, bprm->file); /* 新 mm 的 exe_file（/proc/PID/exe 的来源）
	                                        + deny_write_access（不许再写正在执行的文件）*/
	would_dump(bprm, bprm->file);        /* 不可读的映像 → 强制 dumpable=0 */
	exec_mmap(bprm->mm);                 /* ★ 交接地址空间：activate_mm(新) +
	                                        mmput(旧)（§3.3）*/
	bprm->mm = NULL;                     /* 所有权移交 current，防 free_bprm 误放 */
	exec_task_namespaces();              /* CLONE_NEWPID 语义：nsproxy 切换 */
	exit_itimers(me); flush_itimer_signals();
	unshare_sighand(me);                 /* sighand 若共享（pthread）则复制一份 */
	me->flags &= ~(PF_RANDOMIZE | PF_FORKNOEXEC | ...);  /* ★ 摘掉 PF_FORKNOEXEC */
	flush_thread();                      /* 架构级线程状态清洗（x86: 清 TLS/FPU 残留）*/
	do_close_on_exec(me->files);         /* CLOEXEC fd 在此批量关闭 */
	...
	set_dumpable(current->mm, SUID_DUMP_USER 或 suid_dumpable);
	__set_task_comm(me, kbasename(bprm->filename), true);
	WRITE_ONCE(me->self_exec_id, me->self_exec_id + 1);  /* 父进程靠它识别
	                                                       "孩子 exec 过了"（§5.1）*/
	flush_signal_handlers(me, 0);        /* 自定义 handler 全部重置为 SIG_DFL */
	commit_creds(bprm->cred);            /* suid 凭证生效 */
}
```

**事务性体现在顺序上**：先 `de_thread`（线程组单一化——此后失败也无需再收拾多线程）、再换 exe_file、再 `exec_mmap`（mm 交接）、再清信号表、最后 `commit_creds`。每一步都设计成"要么已生效要么不再回退"——过了 `point_of_no_return` 后的错误只能走 `force_fatal_sig(SIGSEGV)` 让进程死掉（`bprm_execve` 的 `out:` 标签），而不是把一个"没有映像却还活着"的进程交还给用户态。

### 3.3 exec_mmap：old_mm 的死亡时刻（fs/exec.c:979）

```c
static int exec_mmap(struct mm_struct *mm)
{
	old_mm = current->mm;
	exec_mm_release(tsk, old_mm);        /* futex_exec_release + mm_release：
	                                        vfork 父进程在此被 complete 唤醒！*/
	sync_mm_rss(old_mm);
	down_write_killable(&tsk->signal->exec_update_lock); /* 与 /proc/PID/maps 读者互斥 */
	mmap_read_lock(old_mm);
	task_lock(tsk);
	membarrier_exec_mmap(mm);            /* membarrier 语义切换 */
	local_irq_disable();
	tsk->active_mm = mm; tsk->mm = mm;   /* ★ 交接点：current 换上新 mm */
	activate_mm(active_mm, mm);          /* 架构页表基址加载（x86: write_cr3
	                                        + 保留内核半区映射；arm64: TTBR0 + 新 ASID）*/
	lru_gen_add_mm(mm); lru_gen_use_mm(mm);
	mm_update_next_owner(old_mm);        /* memcg：owner 若是本任务则移交（fork §已述）*/
	mmput(old_mm);                       /* ★ 旧 mm 引用 -1。最后一个引用（常见情形）
	                                        在此触发 mmput → exit_mmap → 整个旧地址空间
	                                        （所有 VMA、页表、匿名页、文件页映射）被拆除 */
	mmdrop_lazy_tlb(active_mm);          /*（无 old_mm 的 kthread 分支）*/
}
```

注意 `exec_mm_release` 里的 `complete_vfork_done`：**vfork 的父进程醒来时机就是子进程 exec 的这一刻**（`mm_release` 同时处理 `CLONE_CHILD_CLEARTID` 的 futex wake——pthread 语义同样在 exec 时触发，这是 POSIX "exec 销毁线程库状态"的内核支点）。

### 3.4 PT_LOAD 映射循环与 load_bias（binfmt_elf.c:1033）

```
for 每个 PT_LOAD:
    elf_prot = make_prot(p_flags)          /* PF_R/W → PROT_* */
    elf_flags = MAP_PRIVATE
    if 非首个 LOAD:            elf_flags |= MAP_FIXED           /* 基址已定，精确落位 */
    elif ET_EXEC（非 PIE）:      elf_flags |= MAP_FIXED_NOREPLACE /* 落在链接地址，撞了就失败 */
    elif ET_DYN（PIE）:
        if 有 interp: load_bias = ELF_ET_DYN_BASE + arch_mmap_rnd()  /* 主程序：传统 PIE 基址+随机 */
        else:          load_bias = 0                      /* loader 本身（ld.so 直接运行）：
                                                             让内核自由选址 */
        load_bias &= ~(maximum_alignment(phdrs)-1);       /* 对齐到最大 p_align（2MB 大页友好）*/
        load_bias = ELF_PAGESTART(load_bias - vaddr);     /* ★ 对齐技巧：先减首个 vaddr
                                                             再页对齐，保证"文件内偏移"与
                                                             "运行时偏移"差恒定 */
        total_size = total_mapping_size(...)              /* 所有 LOAD 的整体跨度：
                                                             首次 elf_map 一次性把
                                                             [base, base+total) 保留，
                                                             防 ld.so 被别的映射插队 */
    error = elf_map(bprm->file, load_bias + vaddr, elf_ppnt, elf_prot,
                    elf_flags, total_size);                /* vm_mmap 包装 */
    记录 start_code/end_code/start_data/end_data/elf_bss/elf_brk 与 phdr_addr
之后：
    set_brk(elf_bss, elf_brk, bss_prot);                  /* BSS：匿名零页扩展段 */
    padzero(elf_bss);                                     /* .bss 尾页清零 */
    interpreter: load_elf_interp(...) → elf_entry = interp 入口 + 重定位量
    set_binfmt(&elf_format);                              /* mm->binfmt = elf（coredump 路径用）*/
    ARCH_SETUP_ADDITIONAL_PAGES(...)                      /* vvar/vdso 重映射（§4.3）*/
    create_elf_tables(...)                                /* auxv（§4.4）*/
    mm->start_stack = bprm->p;  ...code/data 边界写回 mm...
    START_THREAD(elf_ex, regs, elf_entry, bprm->p);       /* ★ 寄存器点火（§3.6）*/
```

**为什么 PIE 的第一映射要 `total_size` 整体保留再细分**：ELF 的多个 LOAD 段虚拟连续（text/data/bss 相邻）。若逐段 `MAP_FIXED`，中间可能被并发的其他映射（另一个线程——虽然 exec 已经 de_thread，但 vDSO/brk 分配也可能）抢占形成碎片；一次性保留后逐段落位则保证整体性（`total_mapping_size` 只对"连续或重叠"的 LOAD 有意义，注释明确 ia64 式大洞 ET_EXEC 不适用此法）。

### 3.5 动态链接的两段式

`PT_INTERP` 存在时（几乎所有用户程序）：`load_elf_binary` 先映射**主程序**的 LOAD 段（基址 `ELF_ET_DYN_BASE + 随机`），再 `load_elf_interp` 把 **ld.so** 映射到 `mmap_base` 附近的随机地址，入口寄存器指向 **ld.so 的 e_entry**——`_start`（不是 main！）先跑，ld.so 依 AT_PHDR/AT_BASE 完成重定位与库装载后跳主程序。所以"execve 加载了一个文件"在动态链接世界里实际是**内核映射两份映像 + 用户态 loader 完成剩下的一切**。glibc 的 `rtld` 甚至接管 AT_RANDOM 传来的随机数做 canary 与 ASLR 种子。

### 3.6 START_THREAD：寄存器的临门一脚（架构对比）

```c
/* x86_64（arch/x86/kernel/process_64.c）：整个 pt_regs 就地重写 */
start_thread_common(regs, new_ip=elf_entry, new_sp=bprm->p, __USER_CS, __USER_DS, 0):
	loadsegment(fs, 0); load_gs_index(0);
	regs->ip = new_ip;   regs->sp = new_sp;
	regs->cs = __USER_CS; regs->ss = __USER_DS;
	regs->flags = X86_EFLAGS_IF;          /* 开中断、清其它标志 */
/* 返回路径：load_elf_binary return → ... → 系统调用出口。此时 pt_regs 已是
   "新程序入口"的现场——execve 系统调用"永不返回"到调用者，而是直接进入新程序 */

/* arm64（arch/arm64/include/asm/processor.h）：pt_regs 清零重写 */
start_thread_common(regs, pc): memset(regs, 0, sizeof(*regs)); regs->pc = pc;
start_thread: regs->pstate = PSR_MODE_EL0t;   /* EL0 用户态 */
              spectre_v4_enable_task_mitigation(current);
              regs->sp = sp;
/* ELF_PLAT_INIT：x86 清 bx/cx/dx/si/di/bp 等调用者保存寄存器；
   arm64 只有 (regs->regs[0] = 0)——x0 是启动约定的 argc 语义起点 */
```

对比第 1 章 `copy_thread` 的 pt_regs 复制：fork 是"复制现场"，exec 是"**伪造初始现场**"——一个刚出生（新程序）的任务从未执行过任何代码，寄存器组被精确设定为 ABI 入口状态（x86_64 SysV：rsp 指向 argc；arm64：sp→{argc,argv,envp,auxv}，由 ld.so/_start 拆）。

---

## 四、地址空间的一生一死（exec 视角）

### 4.1 新旧地址空间时间线

```
execve(2) 期间的两个 mm：
 ┌────────────────────────────────────────────────────────────────┐
 │ T0  bprm_mm_init：新 mm 诞生（1 个未完成栈 vma @ STACK_TOP_MAX）│
 │ T1  copy_strings：源=旧 mm 用户指针，目的=新 mm 栈页            │
 │     （此刻两 mm 并存：current->mm=旧，bprm->mm=新）            │
 │ T2  begin_new_exec→exec_mmap：current 换新 mm，activate_mm，   │
 │     mmput(旧) —— 旧 mm 若是最后引用：exit_mmap 拆光全部 VMA、  │
 │     释放页表与匿名页（[heap] 的 brk 空间、所有 mmap 区域随葬）  │
 │ T3  setup_arg_pages：栈 vma 从 STACK_TOP_MAX 平移（shift_arg_  │
 │     pages 重映射 PTE）到真实随机化栈顶 randomize_stack_top     │
 │     mm->arg_start/env_start 定格                               │
 │ T4  PT_LOAD/interp/vdso/auxv → START_THREAD：新程序上线        │
 └────────────────────────────────────────────────────────────────┘
```

`setup_arg_pages`（fs/exec.c:749）的平移：`stack_shift = vma->vm_end - stack_top`，然后 `mprotect_fixup` 定权限（`PT_GNU_STACK` 决定 `VM_EXEC`）、`shift_arg_pages` 把栈区连同已拷贝的 argv/envp 页**整体搬移**到最终位置——T1 的"先高后低再搬家"正是为了此刻能把整段平移而无需再次逐页 copy。

### 4.2 brk 的重置

`mm->brk = mm->start_brk = arch_randomize_brk(mm)`：**新程序的堆从零开始**（旧程序堆里的 malloc 状态全部作废——exec 后再访问旧堆指针是未定义行为）。loader 直接运行（`ld.so ./prog`）的 ET_DYN 特例：brk 被挪到 `ELF_ET_DYN_BASE`，避免与 mmap 区向上生长的堆打架（源码注释详细解释了这个 brk 区选择）。

### 4.3 vvar/vdso 的重映射（联动 timers 分册）

`ARCH_SETUP_ADDITIONAL_PAGES` 是每个架构的钩子；x86_64 为 `arch_setup_additional_pages → map_vdso_randomized(&vdso_image_64)`（`arch/x86/entry/vdso/vma.c:367`）——**每次 exec 重新映射一对 vvar+vdso vma**（随机化地址），时间子系统分册（`timers/docs/03-timekeeping与clock_gettime.md`）详解了 vvar 页里 clocksource/seqcount 的内容，此处只强调：新程序拿到的 vDSO 是**新映射**，且随 fork 继承、随 exec 重生。arm64 的 `__setup_additional_pages` 还要区分 sigreturn trampoline（M 前的 HVC 变体）。

### 4.4 auxv 关键项（create_elf_tables，binfmt_elf.c:260~）

| 项 | 传给用户态什么 | 谁消费 |
| :--- | :--- | :--- |
| `AT_PHDR/AT_PHENT/AT_PHNUM` | 运行时程序头表地址/表项大小/数目 | **ld.so**：遍历 PT_DYNAMIC 做重定位的锚点 |
| `AT_BASE` | 解释器（ld.so）装载基址 | ld.so 自举 |
| `AT_ENTRY` | 主程序入口 | ld.so 完成装载后跳转 |
| `AT_RANDOM` | 16 字节内核随机数 | glibc：stack canary 源 |
| `AT_EXECFN` | 文件名字符串地址（bprm->exec） | `program_invocation_name` |
| `AT_HWCAP/AT_HWCAP2` | CPU 特性位图 | ifunc 优化分发（AVX512 等） |
| `AT_SECURE` | suid/sgid 或 LSM 判定的"安全执行" | glibc 开启安全模式（忽略 LD_PRELOAD 等） |
| `AT_PAGESZ` | 页大小（4K/16K/64K） | malloc/对齐库 |
| `AT_UID/EUID/GID/EGID` | 凭证快照 | setuid 程序判断降权前身份 |
| `AT_EXECFD` | （binfmt_script 走 execfd 通道时）文件 fd | 解释器无法按路径打开时兜底 |

观测 auxv：`ldd --list` 之外，`getauxval(3)`/`AT_SYSINFO_EHDR`（vDSO 基址）最直接；或 `cat /proc/PID/auxv | xxd`（每项两个 long：type, val）。

---

## 五、失败语义与线程组

### 5.1 两个世界：errno 级失败 vs SIGKILL 级失败

exec 的失败语义有一条清晰的分界线——`bprm->point_of_no_return`（`begin_new_exec` 开头置位）：

```
┌─ 点之前 ───────────────────────────────────────────────┐
│ ENOENT/EACCES（do_open_execat）                        │
│ ENOEXEC（全 binfmt 链不认）                            │
│ E2BIG/EFAULT（count/copy_strings）                     │
│ ELOOP（解释器嵌套 >5）                                 │
│ ETXTBSY（目标正被写：deny_write_access）                │
│ LSM 拒绝（security_bprm_check/creds_for_exec）          │
│ ……execve 系统调用干干净净返回 -1+errno，进程毫发无损     │
└────────────────────────────────────────────────────────┘
┌─ 点之后 ───────────────────────────────────────────────┐
│ de_thread 已杀兄弟线程；mm 已换；cred 已换；旧映像已拆   │
│ 此时任何失败（如后续 elf_map 撞地址、setup_arg_pages     │
│ 溢出、auxv 上栈失败）都回不去了：                        │
│   bprm_execve 的 out: if (point_of_no_return &&         │
│       !fatal_signal_pending(current))                   │
│           force_fatal_sig(SIGSEGV);  ★ 内核替你收尸     │
│ 进程以 SIGSEGV 终结（父进程 wait 到 status<<8|11）——    │
│ 这不是 bug 而是设计：半残进程绝不能交还用户态            │
└────────────────────────────────────────────────────────┘
```

> ⚠️ 注意准确表述：**过了不可回退点的失败是 force_sig 一个致命 SIGSEGV**（若已有更早的致命信号则复用之），不是"返回 errno"。POSIX 对此的说法就是"exec 失败后进程不应继续存在时，实现可令其接收 SIGSEGV"。

### 5.2 de_thread 与"谁有权 exec"（fs/exec.c:1044）

常见认知："只有线程组 leader 才能 exec"。**6.6 的真实实现：任何线程都可以 exec**；非 leader exec 时会发生一场"身份顶替"：

```c
static int de_thread(struct task_struct *tsk)
{
	if (thread_group_empty(tsk)) goto no_thread_group;
	sig->group_exec_task = tsk;
	sig->notify_count = zap_other_threads(tsk);   /* SIGKILL 组内所有其他线程，
	                                                 返回"活着等他们退出的计数" */
	while (sig->notify_count) {                   /* TASK_KILLABLE 等兄弟们走完
	                                                 do_exit 的 __exit_signal */
		__set_current_state(TASK_KILLABLE); schedule();
	}
	if (!thread_group_leader(tsk)) {              /* ★ 我不是 leader：要顶替他 */
		leader = tsk->group_leader;
		for (;;) { ... /* 等leader 成为 EXIT_ZOMBIE（他也要先走 do_exit，
		                   notify_count=-1 让 exit_notify 唤醒我）*/ }
		tsk->start_time = leader->start_time;     /* ★ 连出生时间都继承：进程的
		                                             "年龄"以 leader 记 */
		exchange_tids(tsk, leader);               /* ★ 两个任务互换 thread_pid！
		                                             我拿到 leader 的 PID 编号，
		                                             他带走我的旧 tid */
		transfer_pid(leader, tsk, PIDTYPE_TGID/PGID/SID);  /* 组身份移交 */
		list_replace_rcu(&leader->tasks, &tsk->tasks);     /* 全局任务链占位 */
		list_replace_init(&leader->sibling, &tsk->sibling);/* 父进程的 children 链占位 */
		tsk->group_leader = tsk;  leader->group_leader = tsk;
		tsk->exit_signal = SIGCHLD;  leader->exit_signal = -1;
		leader->exit_state = EXIT_DEAD;
		release_task(leader);                     /* 老leader 静默收尸（不给父进程
		                                             发通知——他的身份已经是我）*/
	}
}
```

也就是说：`pthread_create` 出的线程调用 `execve` 是完全合法的——执行后**它变成（PID 意义上的）原 leader**，老 leader 作为"无名线程"被回收，父进程看到的子进程 PID 不变。`exit_notify` 里的 `notify_count < 0` 分支（唤醒 `group_exec_task`）正是与这条路径的握手点（第 3 章 §2 会再遇到它）。glibc 历史上因 TLS/栈归属问题 discourage 非 leader exec，那是用户态的顾虑，不是内核限制——这是"常见认知 vs 真实实现"的又一处偏差。

---

## 六、观测与调试

```
# exec 前后地址空间对照（同一 PID，两次读）：
cat /proc/PID/maps > /tmp/before.exec; <触发 exec>; cat /proc/PID/maps > /tmp/after.exec
diff /tmp/before.exec /tmp/after.exec
# 典型差异：栈顶随机化新址、[heap] 消失→新 brk、旧 mmap 区（libc 等）全部消失、
# 新 text/data + ld.so + vvar/vdso 新址、[stack] 下方 argv/env 区并入栈 vma

readlink /proc/PID/exe       # 当前映像（脚本执行时指向解释器！）
readlink /proc/PID/exe.prev? # 不存在；想留旧 exe：dup fd（无 CLOEXEC）后 /proc/self/fd/N
cat /proc/PID/cmdline | tr '\0' ' '   # exec 后的 argv（脚本：解释器 argv 拼接过）
strace -f -e trace=execve ./prog      # 常见三种失败一眼定位：
                                      #   ENOENT（路径/解释器不存在）
                                      #   ENOEXEC（魔数无人认领）
                                      #   ETXTBSY（目标文件仍可写打开着）
perf trace -e sched:sched_process_exec     # exec 事件+文件名
cat /proc/sys/fs/suid_dumpable            # suid 映像是否允许 core dump（默认 0）
```

**corefile 陷阱**：core dump 语义上 dump 的是 `mm`（地址空间）而非"程序"——(a) suid/sgid 且 `suid_dumpable=0` 的映像**不产生 core**（安全考虑，per-映像 dumpable 在 begin_new_exec 定）；(b) 脚本崩溃时 core 的映像是解释器视角（exe_file=解释器，`/proc/PID/exe` 同理）；(c) 容器里 `core_pattern` 若是宿主机路径，core 会落在宿主 namespace 的文件系统——排查"core 不见了"先查 `cat /proc/sys/kernel/core_pattern` 与写权限。第 5 章（ptrace 与 coredump）将展开全链路。

---

## 七、思考题

1. **为什么 `copy_strings` 必须在"旧 mm 还活着、新 mm 还没上岗"的窗口里做？若推迟到 `exec_mmap` 之后（新 mm 已是 current->mm）会发生什么？** 提示：源指针 `str` 的翻译基于哪个 mm；`bprm->mm` 换成 `current->mm` 后 `get_arg_page` 与 `copy_from_user` 各自的地址空间。
2. **`exec_binprm` 的 depth 循环里，`bprm->file` 与 `bprm->interpreter` 换手时旧文件被 `fput`——若此时已过 `point_of_no_return`，这个 `fput` 失败了怎么办？（fput 会失败吗？）追问：`binfmt_script` 为什么把脚本文件读完头就放手，而不是持有到 exec 结束？** 提示：脚本内容通过 argv 传给解释器，内核不再需要它；`load_script` 里 `BINPRM_FLAGS_PATH_INACCESSIBLE` 的检查（`/dev/fd/N` 的 O_CLOEXEC 场景）。
3. **非 leader 线程 exec 的 `exchange_tids` 把两个任务的 `thread_pid` 对调。`ps` 与父进程的 wait4 在这个瞬间各自看到什么？为什么 `tsk->start_time` 必须继承 leader 的，否则会破坏什么用户可见语义？** 提示：进程"年龄"的唯一记录；`getrusage(RUSAGE_SELF)`/acct 计费的连续性（signal_struct 累计了所有兄弟的 cputime）。
4. **`load_elf_binary` 里 ET_EXEC 用 `MAP_FIXED_NOREPLACE`、ET_DYN 先 `total_size` 整体保留再逐段 `MAP_FIXED`。若没有前者（直接 MAP_FIXED），一个恶意 ELF 能构造出什么攻击？若没有后者，正常运行会碰到什么偶发故障？** 提示：覆盖 vDSO/vsyscall/其他已映射区的劫持；映射区间被 vDSO/brk 穿插导致 LOAD 段断开。
5. **实验**：写一个 `vfork`+`execve` 的最小 shell，用 strace 观察父进程在 `execve` 期间阻塞在哪个系统调用上（vfork 的 wait_for_vfork_done）；再改成 `posix_spawn`（glibc 用 CLONE_VM|CLONE_VFORK 的 clone3），对比两者与朴素 fork+exec 的 `sched_process_fork`/`sched_process_exec` 事件数量差。提示：vfork 路径**没有** fork 事件——`copy_process` 走的是同一个函数，思考"为什么 perf 里少了一次 fork"（答案在 CLONE_VM 的 copy_mm 上）。

---

> 下一章：[exit 与僵尸孤儿](03-exit与僵尸孤儿.md) —— do_exit 的 30 步、EXIT_ZOMBIE 的诞生、wait 收尸协议与孤儿领养。
