# umh 消费者全景：谁在用 call_usermodehelper（Linux 6.6）

> 阅读目标：把 [06 章](06-usermodehelper机制.md) 的机制放到真实使用现场——全树普查 **19 个文件、六类消费者**：三大家族深挖（coredump 管道的 **stdin 注入与 RLIMIT_CORE={1,1} 魔数**、request_key 的 **凭证注入 init 回调**、UMD 的 **tmpfs 私有挂载 blob**——bpfilter 的通道）、cgroup v1 release_agent（安全敏感史）、orderly_poweroff、NFS/bridge upcall 族与 legacy 驱动族；归纳 **UMH_WAIT_EXEC vs UMH_WAIT_PROC 的选择规律**。全部对照 v6.6 逐一定位。前置：[06-usermodehelper 机制](06-usermodehelper机制.md)。

---

## 一、普查总表（git grep `call_usermodehelper*`，v6.6 实测 19 文件）

| 类 | 消费者 | 程序 | 等待 | 出处 |
| :--- | :--- | :--- | :--- | :--- |
| 按需加载 | kmod/modprobe | `modprobe_path`（sysctl） | WAIT_PROC\|KILLABLE | [06 章](06-usermodehelper机制.md) |
| **管道处理** | coredump | `core_pattern` 的 `\|prog` | WAIT_EXEC | fs/coredump.c:639 |
| **凭证注入** | request_key | `/sbin/request-key` | 调用者定 | security/keys/request_key.c:104 |
| **驻留驱动** | UMD（bpfilter） | 内嵌 blob | WAIT_EXEC | kernel/usermode_driver.c:179 |
| 事件回调 | cgroup v1 release_agent | `release_agent_path` | WAIT_EXEC | kernel/cgroup/cgroup-v1.c:817 |
| 系统操作 | orderly_poweroff / ctrl-alt-del | `/sbin/poweroff`/reboot_cmd | WAIT_EXEC | kernel/reboot.c:823 |
| upcall | nfsdcltrack | `/sbin/nfsdcltrack`（模块参数） | WAIT_PROC | fs/nfsd/nfs4recover.c:1821 |
| upcall | NFS cache_lib | （nfs cache helper） | WAIT_EXEC | fs/nfs/cache_lib.c:52 |
| upcall | ocfs2 用户栈 | `user_stack_glue` | WAIT_PROC | fs/ocfs2/stackglue.c |
| upcall | bridge 用户态 STP | `/sbin/bridge-stp` | WAIT_PROC | net/bridge/br_stp_if.c:135（BR_STP_PROG 于 br_private.h:54） |
| 策略 | TOMOYO | loader | WAIT_PROC | security/tomoyo/load_policy.c |
| legacy 驱动 | drbd（外部 helper）/uvesafb（v86d）/pnpbios/windfarm/baycom/greybus watchdog | 各自 | 混合 | drivers/… |
| （遗迹） | wireless regdb | — | — | net/wireless/reg.c:4300 仅注释提及（CRDA 时代 uevent 路径，6.6 已改固件直载 regulatory.db） |

## 二、三大家族深挖

### 2.1 coredump：管道协议与两个魔数（fs/coredump.c）

```
core_pattern 首字符 '|' → ispipe（format_corename :206）
  %p %s 等展开 + 空格切分 argv（"%e and %E don't get split if they
  have spaces in them" 注释 :233 附近）

do_coredump（:519）：
  dump_count 检查：core_pipe_limit 并发闸（:617 "Pid %d(%s) over
    core_pipe_limit"——管道型并发 dump 有限额，防 fork 炸弹）
  call_usermodehelper_setup(helper_argv[0], helper_argv, NULL,
                            GFP_KERNEL, umh_pipe_setup, NULL, &cprm)
                            /* :635 ★init 回调接管一切布线 */
  call_usermodehelper_exec(sub_info, UMH_WAIT_EXEC)   /* :639-641 */

umh_pipe_setup（:501-517）——init 回调的教科书：
  create_pipe_files(files, 0);
  cp->file = files[1];                    /* 内核保留写端 */
  replace_fd(0, files[0], 0);             /* ★helper 的 stdin 即
                                            coredump 管道读端 */
  current->signal->rlim[RLIMIT_CORE] = (struct rlimit){1, 1};
                                          /* :514 魔数：RLIMIT_CORE=1
                                             意为"只许管道、禁止文件"——
                                             helper 自己再 core 时不会
                                             递归写文件 */
失败可见："Core dump to |%s pipe failed"（:644）与
          "Process %d(%s) has RLIMIT_CORE set to 1"（:608——调用者
          设 1 的用户会在 dmesg 看到这条解释）
```

### 2.2 request_key：凭证注入（security/keys/request_key.c）

```c
/* :80-84 ★init 回调改写 helper 的凭证（commit_creds 之前） */
static int umh_keys_init(struct subprocess_info *info, struct cred *cred)
{
	struct key *keyring = info->data;
	return install_session_keyring_to_cred(cred, keyring);
}

/* :99-112 */
info = call_usermodehelper_setup(path, argv, envp, GFP_KERNEL,
                                 umh_keys_init, umh_keys_cleanup,
                                 session_keyring);
key_get(session_keyring);
return call_usermodehelper_exec(info, wait);
```

★**语义**：request_key 的 upcall（`/sbin/request-key`，由 `request_key_auth` 的 keyctl 触发 :196）必须**以请求者的会话钥匙环身份**去解密/构造密钥——init 回调把 session keyring 装进 helper 的全新 creds（[06 章 §3.1](06-usermodehelper机制.md) 的 `prepare_kernel_cred + sub_info->init` 机制在此落到实处）。这是"helper 需要调用者上下文"一整类需求的范本。

### 2.3 UMD：驻留的"用户态驱动"（kernel/usermode_driver.c）

```c
/* fork_usermode_driver（:169-188）：setup + exec 的"驻留"用法 */
sub_info = call_usermodehelper_setup(info->driver_name, argv, NULL,
                 GFP_KERNEL, umd_setup, umd_cleanup, info);
call_usermodehelper_exec(sub_info, UMH_WAIT_EXEC);
/* 只等 exec——此后 helper 作为常驻进程活着，与内核用管道对话 */
struct umd_info { ... /* 两根管道 + tgid */ };
umd_info->tgid = get_pid(task_tgid(current));   /* :133 管道布线回调里记 pid */

/* umd_load_blob（:60-76）：★内嵌二进制的挂载术 */
struct vfsmount *blob_to_mnt(const void *data, size_t len, const char *name)
{
	type = get_fs_type("tmpfs");
	mnt = kern_mount(type);        /* ★内核私有 tmpfs 挂载——
	                                  用户态不可见 */
	/* blob 写入挂载点内文件，driver_name 指向它 */
}
```

**用途**：bpfilter——把 `bpfilter_umh` blob（编译进内核的精简用户态程序）经 tmpfs 私有挂载变成可 exec 的文件，fork 成常驻进程处理 eBPF 网络规则。**UMD 是 umh 从"一次性 helper"到"常驻服务"的形态跃迁**：`umd_unload_blob`（:82，`kern_unmount` 收尾）与 `umd_cleanup_helper`（管道关闭）配套。

## 三、cgroup v1 release_agent：work 队列里的告别回调

```
notify_on_release 置位的 cgroup 变空（最后一个任务离开）
  → schedule release_agent_work（cgroup-v1.c:780 cgroup1_release_agent）
       argv = { release_agent_path, <cgroup 路径>, NULL }   /* :808-810 */
       envp = { "HOME=/", "PATH=/sbin:/bin:/usr/sbin:/usr/bin" }  /* 最小环境 */
       call_usermodehelper(..., UMH_WAIT_EXEC)               /* :817 */
```

★**安全史**：release_agent 是容器逃逸的经典素材（写宿主 cgroupfs 的 release_agent + notify_on_release 即可让宿主以 root 跑任意程序）——cgroup v2 **移除了该机制**（根因之一）；注释 :772-774 解释了为何 UMH_WAIT_EXEC："a separate release agent task is forked"，内核不等它跑完。6.6 中 v1 该机制仍在（legacy 挂载）。

## 四、reboot：有序关机与 ctrl-alt-del

```c
/* kernel/reboot.c */
static char poweroff_cmd[POWEROFF_CMD_PATH_LEN] = "/sbin/poweroff";  /* :809 */
static int run_cmd(const char *cmd)             /* :812 */
{
	argv = argv_split(GFP_KERNEL, cmd, NULL);   /* 支持 cmdline 带参数 */
	call_usermodehelper(argv[0], argv, envp, UMH_WAIT_EXEC);   /* :823 */
}
/* 用户：orderly_poweroff（:851，走 poweroff_cmd）/ ctrl-alt-del（:836，
  走 reboot_cmd——cad= 开机参数）——都从 work 上下文调用 */
```

## 五、upcall 族与 legacy 族：一张选型谱

```
upcall（内核有事件 → 用户态专家应答）：
  nfsdcltrack：NFSv4 clientid 持久化（模块参数 cltrack_prog :1667
    可换路径；WAIT_PROC——必须拿到结果才继续）
  ocfs2 用户栈：cluster 栈的 fsck/控制操作
  bridge-stp：bridge 把 STP 决策交给用户态实现（BR_KERNEL_STP 内核态
    之外的选项）
legacy 驱动（"内核缺能力，用户态补"的年代产物）：
  uvesafb：x86 实模式 BIOS 调用靠 v86d 代跑（WAIT_PROC）
  pnpbios/windfarm/baycom：各自固件/风控/ham 调制的用户态搭档
  drbd：外部 split-brain 处理 helper（WAIT_PROC）
  greybus watchdog：svc 超时的救援动作（WAIT_EXEC）
```

**WAIT_EXEC vs WAIT_PROC 的规律**（全表归纳）：

| 等待 | 语义 | 选它的场景 |
| :--- | :--- | :--- |
| WAIT_EXEC | "程序起来了就行" | fire-and-forget：release_agent/coredump（管道已布好，写入方是内核）/poweroff/UMD（后续靠管道对话） |
| WAIT_PROC | "要拿返回值" | 协议型 upcall：modprobe（成败决定 request_module 返回）/nfsdcltrack/bridge-stp/tomoyo |
| KILLABLE 修饰 | 等待可被打断 | 调用者可能被杀的长等待（modprobe） |

## 六、观测实战

```bash
# 1) 谁配了什么 helper
cat /proc/sys/kernel/modprobe                 # modprobe 路径
cat /proc/sys/kernel/core_pattern             # | 开头即管道消费者
cat /sys/fs/cgroup/release_agent 2>/dev/null  # v1 挂载才可见
cat /sys/module/nfsd/parameters/cltrack_prog  2>/dev/null  # nfsdcltrack

# 2) 一次 coredump 管道调用的完整链路
dmesg | grep -iE 'core dump|RLIMIT_CORE'      # 失败原因直接可见
cat /proc/sys/kernel/core_pipe_limit          # 并发闸（0=不限）

# 3) helper 进程的现场识别（ps 里它们的父是谁？）
ps -eo pid,ppid,cmd | grep -E 'request-key|bridge-stp|v86d'
#  UMH_WAIT_EXEC 的 async 形态父进程是 kthreadd（CLONE_PARENT 转投，
#  见 06 章 §3.2）；WAIT_PROC 是短暂子进程
```

## 七、思考题

1. coredump 选 WAIT_EXEC 而非 WAIT_PROC——若换成 WAIT_PROC，一个挂死的 systemd-coredump 会怎样？（提示：崩溃任务在 `do_coredump` 里等多久？dump_count 闸门与并发崩溃风暴。）
2. `RLIMIT_CORE={1,1}` 的魔数为什么恰好是 1 而不是 0？（提示：0 的传统语义是什么？管道模式与文件模式的判定次序。）
3. request_key 的凭证注入发生在 `commit_creds` **之前**（init 回调）——为什么不能让 helper 起来后自己 `keyctl join`？（提示：helper 是全新 creds 的 root；会话钥匙环是 per-thread-group 的。）
4. UMD 用 `kern_mount` 的私有 tmpfs 而不是 memfd+`/proc/self/fd`——后者哪里不满足？（提示：execve 对 /proc/self/fd 引用的语义；命名稳定性与多实例。）
5. release_agent 攻击链的前提是"容器内可写宿主 cgroupfs"——cgroup v2 去掉该机制的完整理由还有哪些？（对照 notify_on_release 的轮询成本与 work 队列的反压。）
6. bridge-stp 的 WAIT_PROC 意味着内核会为一次 STP 决策**阻塞**——这发生在什么上下文？危险吗？（提示：br_stp_call_user 的调用点在哪个锁内/软中断外？）

---

> 回看：[06-usermodehelper 机制](06-usermodehelper机制.md)（三执行形态与所有权协议）｜[07-idempotent](07-idempotent模块加载.md)（modprobe 消费端的去重）｜[process-mgmt 分册](../../process-mgmt/README.md)（coredump 的进程侧全链）｜[cgroup 分册](../../cgroup/README.md)（v1 release_agent 与 v2 的移除）｜[networking 分册](../../networking/README.md)（bpfilter/UMD 的网络面）
