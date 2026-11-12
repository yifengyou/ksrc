# usermodehelper：内核运行用户态程序的基础设施（Linux 6.6）

> 阅读目标：吃透"内核态发起、用户态执行"的统一通道——`call_usermodehelper` 两段式 API（setup/exec）、三种执行形态（async/sync/work 的 SIGCHLD 舞步与 CLONE_PARENT 转投 kthreadd）、五等待标志的**所有权协议**（`xchg(&sub_info->complete, NULL)` 的 complete-or-free 二象性）、三态深度 `UMH_ENABLED/FREEZING/DISABLED` 与 initramfs/冻结的双端时序、kmod 的 modprobe 全链（50 并发闸与 5s 依赖环检测）。全部对照 v6.6（kernel/umh.c、include/linux/umh.h、kernel/module/kmod.c——★2023 年从老 kernel/kmod.c 拆出，Luis Chamberlain）。前置：[01-模块布局与加载器](01-模块布局与加载器.md)｜[05-kernel_read_file](05-kernel_read_file加载基础设施.md)。

---

## 一、问题：内核"跑一个用户态程序"的三难

```
难① rootfs 就绪性：helper 是磁盘上的二进制——initramfs 没解包完就是无米之炊
难② 凭证工程：发起者可能是任意内核上下文（中断里的 firmware 回退！），
    但 helper 必须以可控的 root 权限跑
难③ 冻结协议：suspend 时用户态全体冻结——helper 既是"用户态"又受内核委派，
    两头不靠，必须显式编排
```

三个消费者场景串起全章：`request_module`（kmod）、固件 sysfs 回退（[driver-model/04](../../driver-model/docs/04-firmware加载与request_firmware生命周期.md) 的 uevent/umh 锁路径）、coredump 管道处理器（fs/coredump.c:635，`core_pattern` 的 `|program`）。

## 二、两段式 API 与 subprocess_info（umh.h:17-35）

```c
#define UMH_NO_WAIT   0x00   /* 完全不等（可从中断上下文调用）*/
#define UMH_WAIT_EXEC 0x01   /* 等 exec 成功，不等进程退出 */
#define UMH_WAIT_PROC 0x02   /* 等进程退出 */
#define UMH_KILLABLE  0x04   /* 等待可被 SIGKILL 打断 */
#define UMH_FREEZABLE 0x08   /* 等待可被冻结 */

struct subprocess_info {          /* __randomize_layout（randstruct）*/
	struct work_struct work;      /* 挂 system_unbound_wq */
	struct completion *complete;  /* ★所有权协议的关节（见 §四）*/
	const char *path; char **argv, **envp;
	int wait, retval;
	int  (*init)(struct subprocess_info *, struct cred *);  /* exec 前定制 */
	void (*cleanup)(struct subprocess_info *);              /* 释放前回调 */
	void *data;
} ;
```

两段式：`call_usermodehelper_setup`（umh.c:356，分配 + 初始化 + **init/cleanup 回调槽位**）→ `call_usermodehelper_exec`（:402）。便捷封装 `call_usermodehelper`（:483）＝setup+exec；UMH_NO_WAIT 变体用 GFP_ATOMIC（"safe to call from interrupt context"，:478 注释）。

## 三、执行链三形态

### 3.1 exec_async：凭证工程（umh.c:64-124）

```c
current->fs->umask = 0022;                 /* 新 fs_struct，重置掩码 */
set_user_nice(current, 0);                 /* :86 注释原话："Our parent
                                             (unbound workqueue) runs with
                                             elevated scheduling priority.
                                             Avoid propagating that into
                                             the userspace child." */
new = prepare_kernel_cred(current);        /* :89 全新 kernel creds */
new->cap_bset = cap_intersect(usermodehelper_bset, new->cap_bset);
new->cap_inheritable = cap_intersect(...); /* 与 sysctl 可调的 bset 求交 */
if (sub_info->init) sub_info->init(sub_info, new);  /* 定制钩子（如改 uid）*/
commit_creds(new);
wait_for_initramfs();                      /* :109 ★难①的执行端 */
retval = kernel_execve(sub_info->path, argv, envp);
```

### 3.2 exec_sync 与 exec_work：父子关系的两种处理

```c
/* exec_sync（:127-141）——UMH_WAIT_PROC 专用 */
kernel_sigaction(SIGCHLD, SIG_DFL);        /* 注释原话："If SIGCLD is
                                             ignored do_wait won't
                                             populate the status." */
pid = user_mode_thread(exec_async, sub_info, SIGCHLD);
kernel_wait(pid, &sub_info->retval);       /* 阻塞的是 wq worker */
kernel_sigaction(SIGCHLD, SIG_IGN);

/* exec_work（:158-177）——分派器 */
if (wait & UMH_WAIT_PROC) → exec_sync（worker 就地等）
else → user_mode_thread(..., CLONE_PARENT | SIGCHLD)
        /* :168-169 注释原话："Use CLONE_PARENT to reparent it to kthreadd;
           we do not want to pollute current->children, and we need a parent
           that always ignores SIGCHLD to ensure auto-reaping." */
```

为什么走 **system_unbound_wq**：:145-149 注释原话——*"We need to create the usermodehelper kernel thread from a task that is affine to an optimized set of CPUs (or nohz housekeeping ones)... Unbound workqueues provide such wide affinity"*，且 workqueue 提供调用者可能不具备的**特权等级**。

## 四、五等待标志与所有权协议（exec :402-470 的 xchg 舞步）

```
helper_lock()（running_helpers++）
if (usermodehelper_disabled) → -EBUSY       ← ★固件 nowait 路径的 -EBUSY 源头
if (strlen(path) == 0) → 静默成功           ← STATIC_USERMODEHELPER_PATH=""
sub_info->complete = (wait == UMH_NO_WAIT) ? NULL : &done;   ← 所有权分配：
                                                              ★NULL=完成权归
                                                              umh_complete（它
                                                              负责释放 sub_info）
queue_work(system_unbound_wq, &work);
UMH_NO_WAIT → 立刻返回（sub_info 的生死此后与调用者无关）
UMH_KILLABLE → wait_for_completion_state(&done, TASK_KILLABLE)
   被打断时：xchg(&sub_info->complete, NULL)
     非空 → 拿回所有权，自己释放（unlock 分支）
     空   → 对方已在 complete 的路上 → 落穿为不可打断等待
            （:450-457 注释："umh_complete() shall call complete() in a
             moment...will not block SIGKILL'ed processes for long"）
```

`umh_complete`（:47-57）的**二象性**：completion 指针非空就 `complete(comp)`（唤醒等待者），为空就 `call_usermodehelper_freeinfo`（调用 cleanup 并 kfree）——一个函数、两种死法，xchg 是唯一裁判。

## 五、三态深度与两端的时序编排

```c
enum umh_disable_depth {        /* umh.h:45-49——★只有三态 */
	UMH_ENABLED = 0,
	UMH_FREEZING,
	UMH_DISABLED,
};
static enum umh_disable_depth usermodehelper_disabled = UMH_DISABLED;  /* :187 初始关！*/
```

```
启动端（难①的编排端）：
  boot → ... → initramfs 解包 → init/initramfs.c:750 usermodehelper_enable()
  执行端每跑一个 helper：wait_for_initramfs()（:109）双保险
  （no initramfs 场景：init/noinitramfs.c:22 同样 enable）

冻结端（难③）：
  freeze_processes（kernel/power/process.c:125）
    → __usermodehelper_disable(UMH_FREEZING)
       ：设深度 + wait_event_timeout(running_helpers==0,
         RUNNING_HELPERS_TIMEOUT=5*HZ（:208）) 超时 → -EAGAIN 并回滚
  thaw_processes → usermodehelper_enable()（process.c:210）

读侧协议（"我要用 helper，但可让步"）：
  usermodehelper_read_trylock（:210）  可失败（UMH_DISABLED → -EAGAIN）
  usermodehelper_read_lock_wait（:240） 带超时等待解冻
  usermodehelper_read_unlock（:267）
  ★固件回退双用点：fallback.c:136（nowait 的超时版）/:143（trylock 版）
   ——任务 58 的 "firmware: %s loading timed out" 报错正是 umh 冻结期的写照
```

## 六、kmod：modprobe 全链（kernel/module/kmod.c，180 行）

```c
#define MAX_KMOD_CONCURRENT 50        /* :46——定档注释（:30-45）原话：
    "threads = div64_u64(totalram_pages * PAGE_SIZE, THREAD_SIZE * 8)"
    少于 50 意味着 <3200 页（约 13M 内存）的系统——几乎不存在 */
#define MAX_KMOD_ALL_BUSY_TIMEOUT 5   /* :59——全忙 5 秒判定依赖环：
    "recursive module dependency creating a loop. We have no option
     but to fail." */
char modprobe_path[KMOD_PATH_LEN] = CONFIG_MODPROBE_PATH;  /* :64 sysctl 可调 */
```

```
__request_module（:132）：
  WARN_ON_ONCE(wait && current_is_async())    /* :144——init/05 章的死锁
                                                 警戒线（modprobe→init→
                                                 async_synchronize_full→
                                                 等自己）*/
  !modprobe_path[0] → -ENOENT                 /* sysctl 置空=关自动加载 */
  security_kernel_module_request(module_name) /* LSM 门 */
  down_timeout(&kmod_concurrent_max, 5*HZ)    /* :159 并发闸；超时告警
    原文（:161）："request_module: modprobe %s cannot be processed,
     kmod busy with %d threads for more than %d seconds now" */
  kmod_dup_request_exists_wait(...)           /* :168 ★去重层——重复
    request_module 并发等待共享结果（与任务 63 的 finit_module
    idempotent 层呼应：一个防并发 kmod 请求、一个防并发 insmod）*/
  call_modprobe(name, wait ? UMH_WAIT_PROC : UMH_WAIT_EXEC)  /* :173 */

call_modprobe（:72-111）：
  argv = { modprobe_path, "-q", "--", module_name, NULL }
  envp = { "HOME=/", "TERM=linux",
           "PATH=/sbin:/usr/sbin:/bin:/usr/bin", NULL }
  setup(..., cleanup=free_modprobe_argv) + exec(wait | UMH_KILLABLE)
  + kmod_dup_request_announce(...)           /* 向去重层广播结果 */
```

## 七、安全面：三个旋钮

1. **`CONFIG_STATIC_USERMODEHELPER`**（setup :369-371）：编译期把所有 helper 的 path **覆盖**为 `CONFIG_STATIC_USERMODEHELPER_PATH`——容器/锁定系统的"唯一合法通道"（典型：一个能按白名单转发的迷你 init）。该值置 **""** 时 exec 直接静默成功（:419-423）——头注释（:397-400）的警告原话：*"successful return value does not guarantee the helper was called at all. You can't rely on sub_info->{init,cleanup} being called even for UMH_WAIT_* wait modes as STATIC_USERMODEHELPER_PATH="" turns all helpers into a successful no-op."*
2. **能力收缩**：sysctl `kernel/usermodehelper_bset` / `usermodehelper_inheritable`（proc_cap_handler :498——写需 **CAP_SETPCAP + CAP_SYS_MODULE 双许可** :506-508），与 helper 新 creds 求交（§3.1）
3. **LSM**：`security_kernel_module_request`（kmod.c:155）按模块名审查。

## 八、观测实战

```bash
# 1) modprobe 通道体检
cat /proc/sys/kernel/modprobe                  # 当前 modprobe_path
cat /proc/sys/kernel/kmod_concurrent_max 2>/dev/null || true
#    ★注：并发上限是编译期常量（MAX_KMOD_CONCURRENT=50），无 sysctl

# 2) 冻结期排障（suspend 失败 + -EBUSY 毛刺）
dmesg | grep -iE 'usermodehelper|request_module.*busy'
journalctl -b -u systemd | grep -i 'thaw\|freeze'   # 用户态侧对账

# 3) coredump 管道处理器（另一主顾）
cat /proc/sys/kernel/core_pattern              # 首字符 | → 走 coredump.c:635

# 4) 去重层观测（6.6 CONFIG_MODULE_DEBUG_AUTOLOAD_DUPS）
cat /sys/module/module/parameters/*dups* 2>/dev/null

# 5) 冻结窗口内谁在等 umh（锁持有者）
cat /proc/lock_stats | grep -A2 umhelper       # CONFIG_LOCK_STAT=y 时
```

## 九、思考题

1. `exec_work` 的两分支一个用 `user_mode_thread(..., SIGCHLD)`、一个用 `CLONE_PARENT|SIGCHLD`——SIGCHLD 两个语义位各为什么必要？（提示：sync 分支谁在 wait？async 分支谁在收尸？）
2. UMH_NO_WAIT 下 `sub_info->complete = NULL` 的所有权反转：如果调用者在 NO_WAIT 后又碰了 sub_info 会怎样？为什么 GFP_ATOMIC 只对 NO_WAIT 合法？（提示：中断上下文 + 生命周期。）
3. 初始 `UMH_DISABLED` 与 `wait_for_initramfs()` 是同一道闸的**两端**——为什么有了 state 还要每 helper 运行时再等？（提示：enable 的调用点在 initramfs.c:750，竞态窗口在哪？）
4. `MAX_KMOD_ALL_BUSY_TIMEOUT=5` 判定"依赖环"的依据为什么是**全忙**而非"个别慢"？（对照定档注释的内存公式：50 并发对多小的系统才可能全忙？）
5. STATIC_USERMODEHELPER_PATH="" 使所有 helper 变静默成功——这为什么是特性而不是漏洞？（提示：比 -ENOENT 失败更安全的三种场景：闭源固件加载器、容器镜像、启动早期。）
6. 固件 nowait 路径（fallback.c:136 的 `usermodehelper_read_lock_wait`）在 suspend 中的行为：返回 0 / 超时 / -EINVAL 三种结局分别对应冻结协议的哪个阶段？（提示：read_lock_wait 的 timeout 参数与 RUNNING_HELPERS_TIMEOUT 的关系。）

---

> 回看：[01-模块布局与加载器](01-模块布局与加载器.md)（request_module 的下游）｜[05-kernel_read_file](05-kernel_read_file加载基础设施.md)（finit_module 与 request_module 的两条入口）｜[driver-model/04-firmware 加载](../../driver-model/docs/04-firmware加载与request_firmware生命周期.md)（umh 读锁的主顾）｜[init/05-async 异步基础设施](../../init/docs/05-async异步基础设施.md)（current_is_async 死锁警戒线的另一半）｜[power 分册](../../power/README.md)（freeze/thaw 编排的顶层）｜[process-mgmt 分册](../../process-mgmt/README.md)（coredump 管道的进程侧）
