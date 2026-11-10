# seccomp 过滤器与 BPF 验证（Linux 6.6）

> 阅读目标：补齐 [02-seccomp](02-seccomp.md) 的**加载与验证侧**——`seccomp_check_filter` 的 cBPF 白名单与指令改写、通用验证器 `bpf_check_classic` 与 cBPF→eBPF 迁移、filter 的 `prev` 链与 **action cache 位图快路**（5.18+，6.6 现役）、TSYNC 对 `no_new_privs` 的传播、USER_NOTIF 生命周期。前置：[02-seccomp](02-seccomp.md)、[04-LSM 与安全钩子顺序](04-LSM与安全钩子顺序.md)。

---

## 一、attach 全链（kernel/seccomp.c，已核对 6.6）

```
seccomp(SECCOMP_SET_MODE_FILTER, flags, prog):
  前置：CAP_SYS_ADMIN 或 task_no_new_privs(caller)（否则 -EACCES）
  flags（5 个，值已核对）：
    TSYNC(1<<0)  LOG(1<<1)  SPEC_ALLOW(1<<2)  NEW_LISTENER(1<<3)
    TSYNC_ESRCH —— 第六个 WAIT_KILLABLE_RECV 同样在 6.6（wait_killable_recv 字段）
  → seccomp_prepare_filter(fprog)
  → bpf_prog_create_from_user(&sfilter->prog, fprog,
        seccomp_check_filter, save_orig)   ← 两层验证入口
  → seccomp_cache_prepare（位图快路预计算）
  → seccomp_attach_filter（挂 prev 链头；TSYNC 走 §五）
```

## 二、两层验证器：先 seccomp 白名单，再通用 cBPF 检查

```
第一层 seccomp_check_filter（seccomp.c:280，已核对逐 case）：
  BPF_LD|BPF_W|BPF_ABS   → 改写为 BPF_LDX|W|ABS（"从 seccomp_data 取字"
                            的语义占用 packet-load 形式）；k 必须
                            4 字节对齐且 < sizeof(struct seccomp_data)
  BPF_LD|BPF_W|BPF_LEN   → 改写为 BPF_LD|IMM，k=sizeof(seccomp_data)
  白名单 ALU：ADD/SUB/MUL/DIV/AND/OR（K/X 变体）
  白名单 JMP：JA、JEQ/JGE/JGT/JSET（K/X 变体）
  RET|K / RET|A
  其余（含 BPF_LD|BPF_IMM 载入任意立即数到 A？—— 不在白名单）→ -EINVAL
第二层 通用 cBPF 验证器 bpf_check_classic（kernel/bpf/core.c）：
  跳转目标界检查（不越界/不成环）、除零常量、BPF_MAXINSNS=4096 上限
  （include/uapi/linux/bpf_common.h）
第三步 迁移：bpf_prog_create_from_user → bpf_prepare_filter →
  bpf_migrate_filter —— cBPF 指令逐条翻译成 eBPF；§一 的 LDX 改写
  正是为迁移后的寻址模式做准备
```

**设计逻辑**：seccomp 只开放"读 seccomp_data → 算术比较 → 返回 verdict"这一种形状——没有内存访问、没有指针、验证器只需白名单+界检查（与 eBPF 验证器的图遍历相比，成本近零且完备性可证）。

## 三、filter 结构与 prev 链（已核对 6.6）

```c
struct seccomp_filter {
	refcount_t refs, users;
	bool log;  bool wait_killable_recv;
	struct action_cache cache;   /* 位图快路（下节）*/
	struct seccomp_filter *prev; /* ★链式叠加：新 filter 挂头，
	                                运行时从新到旧全跑，最严 verdict 生效 */
	struct bpf_prog *prog;
	struct notification *notif;  struct mutex notify_lock;
	wait_queue_head_t wqh;
};
```

`prev` 链是 TSYNC 的共享载体：同一线程组的过滤器链被多任务引用（`users` 计数）；"永不解除"语义由不可摘除保证（只能整体加新层）。

## 四、运行路径：位图快路 + 禁迁移执行（已核对）

```
seccomp_run_filters(sd):
  ① seccomp_cache_check_allow(f, sd) —— action_cache 的
     allow_native 位图（compat 系统调用表独立一张 allow_compat）：
     命中=该 syscall 在此 filter 下恒 ALLOW → 直接放行，
     不进解释器（白名单场景的 0 开销快路）
  ② 未命中 → bpf_prog_run_pin_on_cpu(f->prog, sd)
     ——当前 CPU 钉住执行（防迁移引发的 per-CPU 竞态）
  ③ verdict 按 SECCOMP_RET_ 分派（uapi 已核对全表）：
     KILL_PROCESS=0x80000000 / KILL_THREAD=0x00000000 /
     TRAP=0x00030000 / ERRNO=0x00050000 / USER_NOTIF=0x7fc00000 /
     TRACE=0x7ff00000 / LOG=0x7ffc0000 / ALLOW=0x7fff0000
```

## 五、TSYNC 与 no_new_privs 的传播（已核对）

```
TSYNC 遍历线程组逐线程 attach；关键源码行为（627-628 行）：
  if (task_no_new_privs(caller))
      task_set_no_new_privs(thread);
——把调用者的 NNP 传播给全组：为什么必须？没有 NNP 的线程可
  exec 一个 setuid 二进制拿特权——seccomp 拦不住 execve 之后的
  世界；NNP 关死这条路，filter 才真正"终身有效"
失败语义：TSYNC 默认 -1 报告失败数；TSYNC_ESRCH 改报 -ESRCH
```

## 六、USER_NOTIF 生命周期（已核对）

```
verdict=USER_NOTIF → notification 对象（notif）状态机：
  SECCOMP_NOTIFY_SENT（已发 fd，等回复）→ REAP / 再 SENT；
  wait_killable_recv=1 时等待任务睡 KILLABLE（被 kill 可中断——
  防监控进程死了把被监管进程永久挂起）
ioctl 族（6.6 已核对）：
  NOTIF_ID_VALID —— 另有 WRONG_DIR 历史形式（早期 ioctl 方向位
  定义反了，保留兼容——seccomp.c:50 注释原样）
  NOTIF_ADDFD —— 监控方注入 fd（EA_IOCTL 形式：可携带替换语义）
```

## 七、观测与实战

```
grep Seccomp /proc/<pid>/status          # 0/1/2 + Seccomp_filters 层数
auditd：SECCOMP 事件（A_SECCOMP）带 filter 层号与 verdict
libseccomp：SCMP_ACT_* 与 RET_ 宏一一对应（scmp_sys_resolver 生成器）
性能：白名单大规则集下 cache 快路消掉 99% 解释执行
  （对比：cache 加入前后的 strace -c 系统调用耗时差异）
```

## 八、思考题

1. seccomp 的 cBPF 白名单为什么禁掉 `BPF_LD|BPF_IMM`（载入任意立即数）却不禁止比较指令？（提示：立即数必须来自 seccomp_data 的合法字段偏移——白名单的目的是"形状"而非"数值"。）
2. `prev` 链"只增不删"对容器逃逸防护意味着什么？（推演：子进程继承父 filter，链更长——如何利用这一点做不可绕过的监控。）
3. cache 位图为什么 native/compat 分表？（对照 [01-入口分发](01-系统调用入口与分发.md) 的 x32 ABI。）
4. `bpf_prog_run_pin_on_cpu` 的"钉住"防的是什么竞态？（对照 [02-seccomp](02-seccomp.md) 的 filter 复用语义。）
5. ADDFD 的"注入 fd"能力如何避免被监控进程用 `dup` 逃逸？（提示：注入是一次性动作，返回的 fd 在被监控进程的表中占位。）

---

> 回看：[02-seccomp](02-seccomp.md)｜[04-LSM 与安全钩子顺序](04-LSM与安全钩子顺序.md)｜[06-vDSO](06-vDSO与clock_gettime.md)
