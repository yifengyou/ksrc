# 进程生命周期（process-mgmt）分册导读

> 基于 Linux 6.6 源码（`kernel/fork.c`，3554 行；`fs/exec.c`，2180 行；`kernel/exit.c`，1927 行；辅以 `kernel/pid.c`、`kernel/sched/core.c` 的 sched_fork、`fs/binfmt_elf.c`、`kernel/cred.c` 及 x86/arm64 的 `process.c`）。进程生命周期是内核的"主谓宾"：一个任务如何**生**（fork/clone）、如何**变身**（execve 换地址空间）、如何**死**（exit 与僵尸收割）——三条路径几乎贯穿 task_struct 的全部字段与调度器、内存管理、信号、文件系统四个子系统。

## 为什么值得深读

fork/exec/exit 是面试与生产排障的最高频主题之一，也是**错误重灾区**：老资料里的 `_do_fork`、vma 链表复制、`exit_signals` 在退出尾部等描述在 6.6 里均已过时。本分册的每一步都对照 v6.6 源码逐行核对，重点澄清了若干"常见认知 vs 真实实现"的偏差（dup_mmap 已改用 maple tree、非 leader 线程可以 exec、exit 失败语义的不可回退点等，详见各章）。理解这三条路径后，PID/线程组/namespace 的数据结构（`struct pid` 多层 `upid`）、ptrace、信号投递、OOM killer 的行为都会自然贯通。

## 学习路线

```
01 fork 与 clone 路径全解        生的全链：CLONE_* 矩阵、kernel_clone→copy_process
   ★ 必读                        逐阶段（dup_task_struct/sched_fork/copy_mm/
   （进程线程的一切              dup_mmap 的 maple tree 与 COW/copy_thread 的
    从这里开始）                  x86-arm64 分岔/alloc_pid 多层 idr）、PID 数据结构
        │
02 execve 与 mm 替换             变身：bprm 双 mm 时代、binfmt 链与 #! 拼接、
   ★ 必读                        load_elf_binary 与 load_bias、begin_new_exec 的
   （fork 的下半场）              不可回退点事务、de_thread 的 PID 顶替、auxv
        │
03 exit 与僵尸孤儿               死的全链：do_exit 三十步、EXIT_ZOMBIE 诞生、
   ★ 必读                        wait 收尸协议、release_task 流水线、
   （生产排障高频）               forget_original_parent 三级领养、容器 init 特殊性
        │
04 信号子系统                    交付：发送/递送双半场（complete_signal 目标选择
   ★ 必读                        算法、__send_signal_locked 位图+双队列）、
   （多线程怪象之源）             setup_rt_frame/sigreturn、STOP-CONT job control
        │
05 ptrace 与 coredump            交付：三种 stop 语义（signal-delivery-stop）、
                                  GDB 断点/单步循环还原、do_coredump 安全模型、
                                  core_pattern 格式符全表（按 6.6 逐 case 核对）
```

**状态**：全部 5 章已交付。04 章由后台子任务完成（其被取消前交付），05 章因该子任务中途取消由主会话接管补写。

每章结构：概述 → 机制详解（带真实源码与文件路径）→ ASCII 图 → 深挖难点 → 观测调试 → 思考题（附提示）。

## 核心数据结构速览

贯穿三章的五个主角（字段均以 6.6 为准，详见各章展开）：

| 结构 | 定义处 | 一句话职责 | 主要章节 |
| :--- | :--- | :--- | :--- |
| `task_struct` | include/linux/sched.h | 任务本体；进程/线程无本质差别，差别只在共享了多少 `signal_struct`/`mm_struct`/`files_struct`/`fs_struct` | 01/02/03 |
| `struct pid` + `upid[]` | include/linux/pid.h | **每层 pid namespace 一个编号**；`tasks[PIDTYPE_PID/TGID/PGID/SID]` 四条哈希链做"编号→任务"的反查 | 01 |
| `signal_struct` | include/linux/sched/signal.h | 线程组级状态：共享待决信号、rlim[]、cputime 累计、SIGNAL_* 组标志（组退出/停止/unkillable） | 01/03 |
| `struct linux_binprm` | include/linux/binfmts.h | exec 的事务上下文：新 mm、栈游标 `p`、`point_of_no_return`、cred、解释器轮换 | 02 |
| `exit_state`（task_struct 字段） | include/linux/sched.h | `EXIT_ZOMBIE`（可被收割的墓碑）与 `EXIT_DEAD`（收割中/自动收尸，不可再 wait 到） | 03 |

生命周期状态机的鸟瞰（进程视角）：

```
 fork()          execve()                    exit()               wait()
 ──────► TASK_NEW ──► RUNNING(新程序) ──► PF_EXITING清理 ──► EXIT_ZOMBIE ──► free
   copy_process     START_THREAD点火       do_task_dead永久下CPU  release_task     task_struct
   （TASK_NEW 仅存在于 wake_up_new_task 之前；此后状态机归调度器分册管）
```

## 常见误区澄清（本分册核对出的"认知 vs 6.6 真相"）

写作时逐条对照 v6.6 源码，以下偏差在旧资料/面经里出现频率极高：

| 常见说法 | 6.6 真实实现 | 详见 |
| :--- | :--- | :--- |
| "fork 的实现是 `_do_fork`" | 5.13 起改名 `kernel_clone(struct kernel_clone_args *)`，所有入口（含 kthread）共用它 | 01 章 §一/§二 |
| "dup_mmap 沿 mm->mmap 链表遍历、复制进 mm_rb 红黑树" | vma 链表与 rbtree 已在 6.1~6.4 被移除；6.6 用 `VMA_ITERATOR`（maple tree）遍历 + `vma_iter_bulk_alloc/store` 批量建新树 | 01 章 §3.3 |
| "fork 后子进程复制父进程 vruntime" | `sched_fork` 里 `vruntime/vlag` **清零**、slice 用 `sysctl_sched_base_slice`（防 fork 抢占攻击） | 01 章 §2.5 |
| "只有线程组 leader 才能 exec" | **任何线程**都可 exec；非 leader exec 时 `de_thread → exchange_tids` 顶替 leader 的 PID，老 leader 静默回收（`start_time` 都继承） | 02 章 §5.2 |
| "exec 失败就返回 errno" | 过了 `begin_new_exec` 的 `point_of_no_return` 后失败只会 `force_fatal_sig(SIGSEGV)` 致死——半 exec 进程绝不交还用户态 | 02 章 §5.1 |
| "exit_signals/PF_EXITING 在退出最后才置位" | 6.6 里它在 `do_exit` **很早**（io_uring 取消之后、exit_mm 之前），此后组播信号对它隐身、只认致命信号 | 03 章 §2.2 |
| "僵尸是还没释放 task_struct 的活进程" | 任务在 `do_task_dead → __schedule` 就永久下 CPU；僵尸只是 `EXIT_ZOMBIE` 墓碑（保留 pid/退出码/rusage，等父进程取） | 03 章 §三 |
| "PID 分配是位图" | 5.x 起为每层 ns 一个 **idr（radix tree）**，`idr_alloc_cyclic` 环形分配，1~299 保留（RESERVED_PIDS） | 01 章 §4.2 |

## 阅读约定

- **事实基准**：Linux 6.6（`/2T/kernel/linux-stable.git` 的 `v6.6` 标签）；所有函数名、调用顺序、CLONE_ 语义、结构体字段均经源码核对，"宁可少写不要编造"。
- **术语**：进程 = 线程组 leader 视角的 task_struct；线程 = 共享了 mm/fs/files/sighand 的任务；术语保留英文原文（real_parent、EXIT_ZOMBIE 等）。
- **交叉引用**：与 scheduler 分册重叠的内容（wake_up_new_task、context_switch、lazy TLB、finish_task_switch）一律引用不重复——建议先读 scheduler 02/04 两章再来。
- **每章末尾思考题**附提示，多数可直接用章内"观测"小节的工具验证。

## 与其他分册的联系

进程生命周期处在所有子系统的**交汇点**——task_struct 是内核最大的"总线"：

```
 fork() ──copy_mm──►  mm/maple tree/COW ──────────── mm 分册（pagefault/thp）
    │sched_fork                                             │page fault
    ▼                                                       ▼
 调度器（TASK_NEW→wake_up_new_task→context_switch）── scheduler 分册 02/04
    │                                                       ▲
 execve() ──► binfmt 链 ──► 新 mm + auxv ──► ld.so          │
    │                     └─ vvar/vDSO 重映射 ──── timers 分册 03
 exit() ──► EXIT_ZOMBIE ──► wait ──► release_task ──► RCU ── RCU 分册
    │              └─ SIGCHLD 投递 ───────────────── 信号章（本分册 04）
    └─ exit_task_work ──► io_uring 等的退出清理
```

| 分册 | 交汇点 | 从哪头看 |
| :--- | :--- | :--- |
| [scheduler 分册](../scheduler/README.md) | `sched_fork` 的 `__sched_fork`/TASK_NEW 状态（[02-核心数据结构](../scheduler/docs/02-核心数据结构.md)）；`wake_up_new_task` 与首次调度、lazy TLB 的 `mmgrab_lazy_tlb`、`finish_task_switch` 对 `TASK_DEAD` 的处理（[04-调度时机与上下文切换](../scheduler/docs/04-调度时机与上下文切换.md)）——本分册只引用不重复 | 调度侧：fork/exit 是任务队列成员变动的唯二入口 |
| [mm 分册](../mm/README.md) | fork 的页表 COW 复制（`copy_page_range`）与写时缺页（[pagefault](../mm/pagefualt/pagefault.md) 的 `do_wp_page`）；dup_mmap 重建的 maple tree（mm 增强章规划中）；exec 的栈 VMA 平移与 brk 重置 | mm 侧：fork/exec/exit 是地址空间生灭的三个入口 |
| [timers 分册](../timers/README.md) | exec 时 vvar/vDSO 的重新映射（[03-timekeeping与clock_gettime](../timers/docs/03-timekeeping与clock_gettime.md) 讲了 vvar 页内容）；`exit_itimers` 与 POSIX 定时器的退出清理 | timers 侧：每次 exec 都给新程序发一套新时钟页 |
| [RCU 分册](../RCU/README.md) | 僵尸的最终释放链 `call_rcu(delayed_put_task_struct)`；`rcu_users` 双引用设计；`list_del_rcu` 的进程链摘除 | RCU 侧：task_struct 是 RCU 保护引用的教科书案例 |
| [ras/OOM 分册](../ras/oom/README.md) | `exit_mm` 的 `TIF_MEMDIE → exit_oom_victim`；fork 的 `fatal_signal_pending` 短路点；OOM 选中任务在 do_exit 期间的推进保证 | OOM 侧：选杀之后的"等它真死"依赖本章的退出路径 |
| [workqueue 分册](../workqueue/README.md) | kthread 的诞生（`kernel_thread` → copy_process 的 `PF_KTHREAD` 分岔）与 kthread 框架的退出（fn 返回 → do_exit） | workqueue 侧：kworker 是 fork 路径的内核内部客户 |

## 源码索引（v6.6）

| 文件 | 行数 | 本分册覆盖内容 |
| :--- | :--- | :--- |
| `kernel/fork.c` | 3554 | kernel_clone/copy_process/dup_task_struct/copy_{fs,files,sighand,signal,mm,thread}/dup_mm/dup_mmap/mm_init/mm_release/alloc_pid 调用点/fork_init(pid/threads 上限)/pidfd |
| `fs/exec.c` | 2180 | do_execveat_common/alloc_bprm/bprm_mm_init/copy_strings/setup_arg_pages/bprm_execve/exec_binprm/search_binary_handler/begin_new_exec/setup_new_exec/exec_mmap/de_thread/unshare_sighand |
| `kernel/exit.c` | 1927 | do_exit/do_group_exit/exit_mm/exit_signals 调用点/exit_notify/forget_original_parent/find_new_reaper/reparent_leader/wait_task_zombie/release_task/__exit_signal/__unhash_process/do_wait/do_task_dead 调用点 |
| `fs/binfmt_elf.c` | — | load_elf_binary/elf_map/total_mapping_size/create_elf_tables/load_elf_interp |
| `fs/binfmt_script.c` | — | load_script（#! 解释器拼接） |
| `kernel/pid.c` | — | alloc_pid/free_pid/attach_pid（struct pid 的 idr 与哈希） |
| `kernel/sched/core.c` | — | sched_fork/__sched_fork/wake_up_new_task/do_task_dead/finish_task_switch 的 TASK_DEAD 分支 |
| `kernel/cred.c` | — | copy_creds/prepare_creds/commit_creds |
| `arch/x86/kernel/process*.c`、`arch/arm64/kernel/process.c` | — | copy_thread/start_thread/ret_from_fork（架构分岔） |
| `include/uapi/linux/sched.h` | — | CLONE_* 标志全表（含 64 位 clone3 专属标志） |

> 事实核对方式：本机 `/2T/kernel/linux-stable.git` 的 `v6.6` 标签；不确定的函数名/顺序/字段先 `git show v6.6:<path>` 再落笔。
