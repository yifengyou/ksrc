# seccomp：系统调用的防火墙（Linux 6.6）

> 阅读目标：掌握 seccomp 的两种模式与七种处置动作；吃透过滤器的 cBPF 字节码模型（SECCOMP_DATA、掩码、返回值协议）；理解 TSYNC 的单调性承诺、USER_NOTIF 的进程外仲裁与 TOCTOU 防护、no_new_privs 的必要性；与 ptrace/LSM/容器的协作分工。

---

## 一、模型与模式

seccomp（secure computing mode，2005）挂在上一章入口层的第一道钩子（`syscall_enter_from_user_mode → __secure_computing`，kernel/entry/common.c:72 已核对）——**在 audit、ptrace 之前**先审每个系统调用号。

```
SECCOMP_MODE_STRICT  最古老：只放行 read/write/_exit/sigreturn 四个调用，
                      其余直接 SIGKILL（"计算保险箱"语义）
SECCOMP_MODE_FILTER  主流：挂任意数量的 BPF 过滤器（按安装顺序叠加）
内核内部另有 SECCOMP_MODE_DEAD（seccomp.c:33，已核对）：进程已注定被
                      杀（信号已投递尚未送达的窗口态）
```

安装：`prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, prog)` 或 `seccomp(2)` 系统调用（带 flags：TSYNC/LOG/NEW_LISTENER 等）。

---

## 二、过滤器：cBPF 的方言

过滤器是**经典 BPF**（sock_filter 指令集，非 eBPF）程序，数据视角：

```
struct seccomp_data {
	int nr;                    /* 系统调用号（按架构）*/
	__u32 arch;                /* AUDIT_ARCH_*（防跨架构调用号混淆）*/
	__u64 instruction_pointer; /* 发起调用的用户态 RIP */
	__u64 args[6];             /* 参数原样（不解引用！纯数值比较）*/
};
```

指令集只能做：装载数据字段、立即数比较、跳转、返回——**无内存访问、无循环**（验证器保证），执行时间恒定。返回值低 16 位是动作、高 16 位携带附加数据（ERRNO 值/USER_NOTIF 的 id 等）。

掩码比较的标准骨架（匹配调用号需先 `AND` 掩码再 `==`，因为 BPF 比较是 32 位宽而调用号在 64 位寄存器）——libseccomp 替用户生成这些样板。

---

## 三、处置动作（返回值协议）

多个过滤器叠放时取**最严**结果（seccomp.c 的 `__seccomp_filter` 分派，各 case 已核对）：

| 返回值 | 行为 | 典型用例 |
| :--- | :--- | :--- |
| `KILL_PROCESS` | SIGKILL 整进程（SIGSYS 记账） | 违规即死（默认最严档） |
| `KILL_THREAD` | 只杀当前线程（老 `SECCOMP_RET_KILL`） | 调试期 |
| `TRAP` | 发 SIGSYS（siginfo 带 syscall nr） | 程序自处理违规 |
| `ERRNO` | 以指定 errno 返回，**不执行** | "假装不支持" |
| `USER_NOTIF` | 转给监听进程仲裁（第四节） | 进程外策略（容器运行时） |
| `TRACE` | 转 ptrace tracer 决定 | 调试器（process-mgmt 分册第 5 章 PTRACE_EVENT_SECCOMP） |
| `LOG` | 审计后**放行** | 观察模式 |
| `ALLOW` | 放行 | 白名单匹配 |

**速度语义**：过滤器链每次系统调用入口线性执行——白名单越短越快；全部为纯寄存器比较，耗时上界恒定（百 ns 级，容器热路径的可接受税）。

---

## 四、USER_NOTIF：进程外仲裁

`seccomp(2)` 加 `SECCOMP_FILTER_FLAG_NEW_LISTENER` 返回一个 fd——持有者（如容器运行时）在**进程外**决定被拦截的调用怎么办：

```
被拦截进程                        监听进程（运行时）
    │ ← io_uring/普通 read ◄─── seccomp_notif（pid/调用号/args/cookie）
    │                              │ 决策：
    │ ◄─ ioctl(SECCOMP_IOCTL_NOTIF_SEND) ─┤ ① ERRNO/ALLOW/…（写回处置）
    │   （挂起在目标调用上）                │ ② SECCOMP_IOCTL_NOTIF_ADDFD：
    │                                      │    替目标进程"注入 fd"
    │                                      │    （如把宿主侧打开的文件塞给它）
```

**TOCTOU 防护**（seccomp 的经典难题）：目标进程可在仲裁期间 `execve` 换身份——`NOTIF_ADDFD/SEND` 用 **notification id** 校验"这条通知还属于当初那个进程"（id 为 **per-filter 单调递增计数器**：`seccomp_next_notify_id()` 返回 `filter->notif->next_id++`——第 3 轮复查勘误：原文误称"由 pid 与时间戳派生"；knotif 里另存 `struct pid` 供身份比对，id 失配即拒绝 ioctl）。seccomp.c:78 注释块完整描述了 INIT→SENT→REPLIED 状态机（已核对）。

---

## 五、TSYNC 与 no_new_privs

* **线程同步（TSYNC）**：`seccomp(2)` 带 `FILTER_FLAG_TSYNC` 时把过滤器同步给整个线程组；承诺**只单调变严**——若某线程已有更严的过滤器（不同体系），同步失败放回不一致错误。多线程服务的容器化必须走 TSYNC（否则新线程继承的是安装时刻的链，旧线程不受新过滤器约束）。
* **no_new_privs（prctl PR_SET_NO_NEW_PRIVS）**：装过滤器**要求**先设它——看穿/绕过过滤器语义（过滤器限制的是自己，但 setuid 程序的提权行为不在威胁模型内）。systemd 的 `NoNewPrivileges=`、Chrome 的沙箱都把它作为第一步。

---

## 六、观测与实战

```
# 进程的 seccomp 状态
grep Seccomp /proc/PID/status          # 0/1/2 = disabled/strict/filter
grep NoNewPrivs /proc/PID/status

# 看谁被拦（audit）
ausearch -m SECCOMP -i                 # KILL/LOG 的审计流（type=1326）

# 最小过滤器骨架（生产请用 libseccomp——号值它按架构翻译）
struct sock_filter code[] = {
  BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, nr)),
  BPF_JUMP(BPF_JMP|BPF_JEQ, __NR_execve, 0, 1),   /* 是 execve？*/
  BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_KILL_PROCESS),
  BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),      /* 其余放行 */
};

# 容器侧
docker run --security-opt seccomp=profile.json    # Docker 默认白名单
systemd-run -p SystemCallFilter=@system-service   # 按调用组白名单
```

排障速查：容器内诡异 SIGSYS → 对照 profile 与 `/proc/PID/syscall`（被杀瞬间的调用号）→ 补白名单或改 ERRNO。**先查 `Seccomp:` 字段**再怀疑应用——`2` 状态下大量"随机崩溃"其实是过滤器在干活。

---

## 七、思考题

1. 过滤器为什么必须**不可卸载**？给出"允许卸载"后的一步攻击链（提示：装严过滤器 → 通过某调用降权 → 卸载 → 提权）。
2. TSYNC 放宽失败的根本原因是什么？（对照"线程已装更严过滤器"与承诺的单调性。）
3. `SECCOMP_RET_TRACE` 与 ptrace 的时序里，tracer 改写 syscall nr 的能力来自上一章的哪个机制？（对照入口层的 nr 重写窗口。）
4. USER_NOTIF 的 TOCTOU 防护为什么必须做在 addfd/SEND 两端？notification id 校验的"身份"具体由什么派生？
5. seccomp 与 LSM（admin-guide/LSM 分册）的分工线画在哪？各举一个对方管不了的场景。（提示：参数数值 vs 对象标签。）

---

> 下一章：[io_uring](03-io_uring.md) —— 异步 IO 的终极形态与它的安全风波。
