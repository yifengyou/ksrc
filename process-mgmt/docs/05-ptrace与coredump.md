# ptrace 与 coredump（Linux 6.6）

> 阅读目标：掌握 ptrace 的两种附加模式与三种 stop 语义（signal-delivery-stop / group-stop / PTRACE_EVENT-stop）；还原 GDB 断点与单步的完整工作循环；吃透 `do_coredump` 的安全模型（dumpable 三态、SUID 的 root 落盘路径）、core_pattern 的文件与管道两种归宿、ELF core 的 note 结构；理解 core 与 kdump 的分工。

---

## 一、ptrace 模型总览

ptrace 是用户态调试的地基：**tracee 把自己的关键事件"上交"给 tracer 审阅，tracer 读写其内存与寄存器**。成为 tracee 有两条路：

```
① PTRACE_TRACEME（子进程自愿）：继承关系 = 真实父子。典型：shell 里 gdb 直接 run 程序
② PTRACE_ATTACH / PTRACE_SEIZE（tracer 主动）：附加任意后代进程（同 uid；yama ptrace_scope 收紧）
   SEIZE（3.4+）= "新行为"模式：附加不停顿（用 PTRACE_INTERRUPT 主动停）、支持 PTRACE_EVENT_STOP
```

状态挂在 `task->ptrace`（`include/linux/ptrace.h`）：

```c
#define PT_PTRACED	0x00000001
#define PT_SEIZED	0x00010000	/* SEIZE used, enable new behavior */
#define PT_TRACE_VFORK		PT_EVENT_FLAG(PTRACE_EVENT_VFORK)
#define PT_TRACE_CLONE		PT_EVENT_FLAG(PTRACE_EVENT_CLONE)
#define PT_TRACE_EXEC		PT_EVENT_FLAG(PTRACE_EVENT_EXEC)
#define PT_TRACE_EXIT		PT_EVENT_FLAG(PTRACE_EVENT_EXIT)   /* exit 前最后一瞥 */
#define PT_TRACE_SECCOMP	PT_EVENT_FLAG(PTRACE_EVENT_SECCOMP)
/* 事件标志由 PTRACE_SETOPTIONS 的 PTRACE_O_* 逐项点亮 */
```

> 📌 ptrace 关系的特殊性（第 3 章"两阶段退出"的另一半）：tracee 的 `->parent` 指向 tracer，`->real_parent` 才是血缘父亲——收尸、通知、`/proc` 展示处处体现双父并存。`PTRACE_O_EXITKILL` 是 tracer 死亡时的"陪葬保险"（给每个 tracee 发 SIGKILL）。

---

## 二、核心机制：三种 stop

ptrace 的一切交互都发生在 tracee 的**停止状态**里。三种 stop 语义各异，调试器框架（gdb/lldb）的全部复杂性都在区分它们：

### 2.1 signal-delivery-stop：信号先给 tracer 审

递送链（第 4 章 §递送链）在 `get_signal()` 的早期有个岔口（kernel/signal.c，已核对 6.6 调用点）：

```c
	/* get_signal() 主循环内，取到待递送信号 signr 之后： */
		if (unlikely(current->ptrace & PT_PTRACED)) {
			signr = ptrace_signal(signr, &ksig->info, type);
			/* ptrace_signal() 内核：
			   current->jobctl |= JOBCTL_STOP_DEQUEUED;   // 先记账"这个信号出队了"
			   signr = ptrace_stop(signr, CLD_TRAPPED, 0, info);
			   // —— 睡去等 tracer。tracer 可：
			   //    PTRACE_CONT + 信号  → 递送指定信号（含 0=取消）
			   //    PTRACE_SETSIGINFO   → 篡改 siginfo
			   //    PTRACE_GETSIGINFO   → 查看原始 siginfo
			   醒来后 signr == 0 → 信号被取消，get_signal 继续取下一个
			   signr != info->si_signo → 按篡改后的 si_signo/SI_USER 重建 */
		}
```

**这是断点命令 `continue` 时 SIGTRAP 的必经之路**：int3 异常（第 8 节）最终以 SIGTRAP 走到这里，gdb 收到 waitpid 的 `WIFSTOPPED`，查询/修改/放行。

### 2.2 group-stop：SIGSTOP 一族的组语义

`do_signal_stop` 处理 SIGSTOP/SIGTSTP 的组停止（第 4 章 job control）。tracee 处于 SEIZE 模式时 tracer 会额外收到 `PTRACE_EVENT_STOP` 通知；**group-stop 里 tracer 的 `PTRACE_CONT` 不会真正恢复运行**（组停止语义优先）——这是老 ATTACH 模式语义混乱的重灾区，SEIZE 用 `PTRACE_LISTEN` 显式表达"我知道它停了，等组事件"。

### 2.3 PTRACE_EVENT-stop：生命周期断点

`PTRACE_SETOPTIONS` 点亮的 `PTRACE_O_TRACE{EXEC,FORK,VFORK,CLONE,EXIT,SECCOMP}` 在对应事件点同步停住 tracee（`__ptrace_event` → `ptrace_notify` 家族，`waitpid` 的 status 高字节给出 `PTRACE_EVENT_*` 码）。第 2 章的 exec（`begin_new_exec` 内）、第 3 章的 exit（`do_exit` 的 `PTRACE_EVENT_EXIT`）各有一个挂钩点——交叉回看即知"断点前 tracee 已走到多深"。

---

## 三、GDB 工作循环还原

把机制串成调试器的日常（x86_64）：

```
设断点：
  ptrace(PTRACE_PEEKTEXT, addr)             读原指令字节（保存）
  ptrace(PTRACE_POKEDATA, addr, 0xCC|...)   写 int3（单字节！）

运行：PTRACE_CONT → tracee 跑到 int3 → #BP 异常（interrupt 分册的 trap 路径）
      → force_sig_fault(SIGTRAP, TRAP_BRKPT, addr) → 第4章递送链
      → get_signal → ptrace_signal → signal-delivery-stop → waitpid 醒来

恢复：读 RIP（多算了 1，回拨）→ 写回原指令 → 单步执行一条（PTRACE_SINGLESTEP
      置 EFLAGS.TF，下一条指令后 #DB trap 又一次 SIGTRAP/TRAP_TRACE）
      → 把 0xCC 写回去 → PTRACE_CONT
单步/下一行/条件断点 = 这三原语的循环组合；软件观察点（watchpoint）用
      PTRACE_POKEUSER 改调试寄存器 DR0-3+DR7（硬件观察点，4 个上限）
syscall 追踪：PTRACE_SYSCALL → syscall-enter-stop / syscall-exit-stop
      （entry 的寄存器在 kernel_pt_regs、exit 的返回值——strace 全部输出源于此）
```

**ptrace vs kprobe**（联系 interrupt 分册）：同为 int3 断点，ptrace 服务**用户态调试协议**（信号语义、per-process、waitpid 通知），kprobe 服务**内核观测**（pre/post handler、全局、无进程语义）。前者不能下在内核地址，后者不能下在用户地址。

---

## 四、ptrace_request：请求分派

`kernel/ptrace.c` 的 `ptrace_request` 处理通用请求，`arch_ptrace` 接住架构相关（x86 的 `PTRACE_GETREGS/SETREGS/PEEKUSER/POKEUSER`、i386 兼容层）。常用族谱：

| 族 | 请求 | 说明 |
| :--- | :--- | :--- |
| 数据 | PEEKTEXT/PEEKDATA/POKEDATA | 按字读写（gdb 的内存查看/补丁） |
| 寄存器 | GETREGS/SETREGS/GETREGSET/SETREGSET | REGSET 泛型（含 XSAVE 扩展态） |
| 信号 | GETSIGINFO/SETSIGINFO | 只对 signal-delivery-stop 有效 |
| 流程 | CONT/SYSCALL/SINGLESTEP/STEP | 各自的 stop-then-resume 语义 |
| 事件 | SETOPTIONS/GETSIGINFO | 事件开关与 payload |
| 附加 | ATTACH/SEIZE/DETACH/INTERRUPT | 关系管理 |

PREEMPT_RT 注意：ptrace 的 stop/wait 语义在 RT 下不变，但 tracer 的响应延迟受调度策略影响——实时 tracee + 普通 tracer 是经典卡死组合。

---

## 五、coredump：进程的临终遗像

### 5.1 触发与准入

第 4 章的处理矩阵里，默认动作属于"core"族（SIGSEGV/SIGBUS/SIGABRT/SIGILL/SIGFPE/SIGQUIT…）的致命信号走 `do_coredump`（前提：`RLIMIT_CORE` 允许、进程可 dump）。可 dump 性三态（`include/linux/sched/coredump.h`）：

```
SUID_DUMP_DISABLE(0)  setuid/setgid 程序、changed cred——不 dump
SUID_DUMP_USER(1)     普通进程——按 core_pattern 走
SUID_DUMP_ROOT(2)     setuid 但 sysctl fs.suid_dumpable=2——只允许 root 收尸
```

### 5.2 do_coredump 主流程（已逐段核对 v6.6 源码）

```
do_coredump(siginfo)
 ├─ binfmt->core_dump 存在？dumpable 非 0？——否则直接 fail（退回普通死亡）
 ├─ prepare_creds：
 │    SUID_DUMP_ROOT 时 cred->fsuid = GLOBAL_ROOT_UID   ★SUID 的遗像只落
 │    need_suid_safe = true                              root 可读的位置
 ├─ coredump_wait(sig, &core_state)    ★并冻结全员：
 │    组内其他线程被带进 SIGNAL_GROUP_EXIT 的"等 core"栅栏（ZW 队列），
 │    保证转储期间地址空间冻结（mm 的最后引用在此交接——第3章 exit_mm 的
 │    coredump 分支）
 ├─ override_creds(cred)               以（可能 root 的）临时身份写文件
 ├─ ispipe = format_corename(&cn, &cprm, &argv, &argc)   解析 core_pattern
 ├─ if (ispipe) 管道路径：
 │    call_usermodehelper(argv) 起 /usr/lib/systemd/systemd-coredump 这类收集器
 │    （在 init namespace 的 root 上下文跑——容器里的 core 管道不会逃逸）
 │    转储写入管道，受 coredump_pipe_limit（默认 RLIM_INFINITY? 核对 sysctl）约束
 │    need_suid_safe 时强制 limit=1（管道模式拒收 SUID dump）
 └─ else 文件路径：
      按 pattern 打开（O_CREAT|O_EXCL? 6.6 语义：不覆盖已有 core）
      need_suid_safe → 仅当 pattern 绝对路径且在 root 目录才落盘
      RLIMIT_CORE 为上限写满即止
      binfmt->core_dump(&cprm) ★elf_core_dump 主战场（第六节）
```

### 5.3 core_pattern：格式符全表

`/proc/sys/kernel/core_pattern`（`format_corename` 逐字符解析，已按 6.6 源码核对全部 case）：

| 格式符 | 含义（6.6 真实语义） | 格式符 | 含义 |
| :--- | :--- | :--- | :--- |
| `%%` | 字面 % | `%p` | tgid（进程所在 ns 视角，`task_tgid_vnr`） |
| `%P` | tgid（初始 ns，`task_tgid_nr`） | `%i` / `%I` | 线程 id（所在 ns / 初始 ns） |
| `%e` | comm（进程名，特殊字符转义） | `%f` / `%E` | exe 文件名 / 完整路径（`/` 转义为 `!`） |
| `%s` | 触发信号号 | `%d` | dumpable 模式（0/1/2） |
| `%h` | 主机名 | `%t` | 时间戳（epoch 秒） |
| `%u` / `%g` | uid / gid | `%c` | RLIMIT_CORE 上限 |

> ⚠️ 6.6 **没有** `%C`（容器名）与 `%S`（信号名）格式符——网上部分资料混入了新内核或提案内容。`%p` 输出的是 **tgid** 而非线程 id（线程 id 用 `%i`）。首字符 `|` = 管道模式（`ispipe=1`），其余按空格切 argv。经典配置：`|/usr/lib/systemd/systemd-coredump %P %u %g %s %t %c %e`。

### 5.4 coredump_filter 与大小控制

`/proc/PID/coredump_filter`（位图选择哪些 vma 进转储：匿名/私有/共享/ELF 头/hugepage…）；`RLIMIT_CORE`（ulimit -c）；`fs.suid_dumpable` 全局三态。文件模式还有 `core_uses_pid`（名字无 %p 时追加 .pid）。

---

## 六、ELF core 的内部结构

`fs/binfmt_elf.c` 的 `elf_core_dump`：

```
ELF header (ET_CORE) + ProgramHeaders（每 vma 一个 PT_LOAD）
Notes（Note 段，elf_align 级联）：
  NT_PRSTATUS   每线程一个：寄存器现场（pt_regs）、信号、栈信息
  NT_PRPSINFO   进程身份（comm/pid/ppid）
  NT_PRFPREGSET / NT_X86_XSTATE  浮点与向量扩展态（XSAVE 区）
  NT_SIGINFO    触发信号原信息      NT_AUXV    auxv（ld.so 视角）
  NT_FILE       文件映射清单（路径+offset）  NT_TASKSTRUCT 等
```

**为什么 note 里的寄存器是 ptrace 视角**：`elf_core_dump` 抓的是 `core_dump_params.cpu` 上 `dump_task_regs`（本质 GETREGSET 同源数据）——与 gdb attach 时读到的一致，所以 core 与 live 调试的帧解析代码完全复用。vma 的内容按 `dump_ok` 过滤（对齐 coredump_filter）后线性写文件；`vma_meta` 数组（6.6 的 coredump_params 字段）在 `coredump_wait` 阶段预抓 vma 元数据——**地址空间在冻结前被快照**，转储期间不再依赖 mm 遍历。

---

## 七、core vs kdump：两个"临终快照"的分工

| | coredump | kdump（联系 ras 分册） |
| :--- | :--- | :--- |
| 快照对象 | 用户进程的 mm | 全物理内存（含内核） |
| 触发 | 进程级致命信号 | panic/oops |
| 执行者 | 进程自己（垂死中） | 预留的 crashkernel 小系统 |
| 产物 | core 文件（ELF note） | vmcore（可 grep 内核数据结构） |
| 典型工具 | gdb | crash（内核视角的 gdb） |

进程崩在系统还健康时用 core；系统本身崩了才轮到 kdump。systemd-coredump 管道模式把两者黏合（core 归档 + coredumpctl 检索）。

---

## 八、观测与实战

```
# 找谁崩了、埋在哪：
coredumpctl list / coredumpctl info <pid>
# gdb 读 core（符号与 build-id 对齐是第一坑）：
gdb ./prog core.12345    → bt / info registers / x/gx addr
# 实时跟随转储：
strace -e ptrace 跟 gdb；perf trace -e signal:signal_deliver,signal:signal_generate
# 排障速查：
core 没生成？三查：ulimit -c / fs.suid_dumpable / core_pattern 写权限与磁盘
core 被截断？RLIMIT_CORE 与 pattern 磁盘配额；管道收集器的 journal 限额
容器里没 core？管道 helper 在 init ns 跑——检查宿主 systemd-coredump 是否忽略
沙箱进程（namespace）pid 语义：用 %P 全局 pid 或 %I 初始 ns pid 免歧义
```

---

## 九、思考题

1. `ptrace_signal` 在 `ptrace_stop` 返回 `signr==0` 时信号被"取消"。这个取消发生在信号已出队之后——与第 4 章 `collect_signal` 的出队顺序如何衔接？被取消的信号还能失而复得吗？
2. group-stop 中 tracer 的 `PTRACE_CONT` 为何无效？SEIZE 的 `PTRACE_LISTEN` 把决定权交还给谁？（对照 jobctl 的组协议。）
3. SUID_DUMP_ROOT 路径里 `cred->fsuid = GLOBAL_ROOT_UID` 而不是直接以 root 跑 helper。为什么 `need_suid_safe` 还额外要求"绝对路径"？防的具体攻击场景是什么？
4. 管道 core_pattern 在**容器内**崩溃、**宿主**收尸：转储数据的进程身份是谁？`%P` 与容器内 `%p` 的差异如何在 systemd-coredump 里消化？
5. `coredump_wait` 把同组线程挂在 SIGNAL_GROUP_EXIT 栅栏上。若其中某线程此刻正持有一把用户态锁（pthread_mutex），core 里的其他线程栈帧会呈现什么？这解释了 gdb 看多线程 core 时哪个经典假象？
6. 设计：给嵌入式设备（无 systemd、存储只读）设计崩溃遗像方案——core 走网络（管道 helper 发 UDP？mtd 分区？），列出至少三项安全过滤（RLIMIT/管道限额/敏感 vma 过滤）。

---

> 本分册至此 5 章完整：[01 fork](01-fork与clone路径全解.md)｜[02 execve](02-execve与mm替换.md)｜[03 exit](03-exit与僵尸孤儿.md)｜[04 信号](04-信号子系统.md)｜[05 ptrace与coredump](05-ptrace与coredump.md)
