# ftrace direct：livepatch 与 BPF trampoline 的共享地基（Linux 6.6）

> 阅读目标：吃透 ftrace 的 **direct call** 通道——绕过 ops 链的函数指针直接改写机制，以及 **IPMODIFY 协调协议**（livepatch 与 BPF trampoline 在同一目标点上共存的代价）：`FTRACE_OPS_FL_DIRECT` 位与 `register_ftrace_direct`（ftrace.c:5411）、`call_direct_funcs` 的占位回调、`direct_functions` 哈希、livepatch 的 `klp_ftrace_handler` 与 `FTRACE_OPS_FL_IPMODIFY`、以及 ★`prepare_direct_functions_for_ipmodify` 的**通知-协商-重试**三步协议（`bpf_tramp_ftrace_ops_func` 的 `-EAGAIN` 信号）。全部对照 v6.6（kernel/trace/ftrace.c 8276 行、include/linux/ftrace.h、kernel/livepatch/patch.c、kernel/bpf/trampoline.c）。前置：[05-tracer 家族](05-tracer家族.md)（ftrace ops 链）｜[14-BPF trampoline](14-BPFtrampoline.md)（DIRECT 挂载的消费者）。

---

## 一、问题：两个"改函数行为的人"不能互不知晓

```
livepatch：把目标函数入口改到补丁函数（FTRACE_OPS_FL_IPMODIFY）
BPF trampoline：在同一入口放生成的机器码（FTRACE_OPS_FL_DIRECT）

冲突：同一 ip 上 DIRECT 已占位时，IPMODIFY 新来者无法注入——
  ftrace_caller 的分发逻辑只认一个 direct_call 地址
解法：ftrace 层的"通知-协商-重试"三步协议（§四）
```

## 二、DIRECT 通道的注册（ftrace.c:5411-5465）

```c
int register_ftrace_direct(struct ftrace_ops *ops, unsigned long addr)
{
	...
	mutex_lock(&direct_mutex);
	/* 确认目标 ip 尚未被其他 DIRECT 占用 */
	for_each_hash_entry(filter_hash) {
		if (ftrace_find_rec_direct(entry->ip))
			goto out_unlock;       /* 已有 direct → -EBUSY */
	}
	/* 插入 direct_functions 哈希（ip → addr）*/
	for_each_hash_entry(filter_hash) {
		new = ftrace_add_rec_direct(entry->ip, addr, ...);
		entry->direct = addr;
	}

	ops->func = call_direct_funcs;    /* 占位回调——不实际调用 */
	ops->flags = MULTI_FLAGS;          /* DIRECT | SAVE_ARGS */
	ops->trampoline = FTRACE_REGS_ADDR; /* 走 regs 版 ftrace_caller */
	ops->direct_call = addr;            /* ★真正的目标地址 */
	err = register_ftrace_function_nolock(ops);
	...
}
```

★**设计要点**：DIRECT 的 ops 仍然挂入 ops 链（参与记录管理），但 `ops->func` 只是占位（`call_direct_funcs` 不做实际工作）。**真正的调用**发生在汇编层：`ftrace_caller` 检测到 DIRECT ops 命中时，**直接改写 `ftrace_call` 指令的目标为 `ops->direct_call`**——绕过整个 ops 遍历循环，实现**零间接调用**。

`direct_functions`（:2539）是独立的 `ftrace_hash`（ip → addr 映射），用于快速判断"某 ip 是否已被 DIRECT 占用"——这就是 IPMODIFY 冲突检测的基础。

## 三、livepatch 的消费端（kernel/livepatch/patch.c）

```c
static void notrace klp_ftrace_handler(unsigned long ip, unsigned long parent_ip,
					struct ftrace_ops *fops,
					struct ftrace_regs *fregs)
{
	struct klp_ops *ops = container_of(fops, struct klp_ops, fops);
	...
	bit = ftrace_test_recursion_trylock(ip, parent_ip);
	func = list_first_or_null_rcu(&ops->func_stack, struct klp_func, stack_node);
	/* func_stack：多个补丁版本的栈——最新在最前 */
	...
	if (unlikely(func->transition)) {
		smp_rmb();
		patch_state = current->patch_state;
		/* 过渡期：按任务的 patch_state 选择新旧函数 */
	}
	if (patch_state == KLP_PATCHED)
		new_func = func->new_func;   /* 补丁版 */
	else
		new_func = func->old_func;   /* 原版 */
	...
	ftrace_regs_set_instruction_pointer(fregs, new_func);
	/* ★改写返回地址——调用者回到"新函数" */
}

/* 注册侧（klp_patch_object）： */
ops->fops.func = klp_ftrace_handler;
ops->fops.flags = FTRACE_OPS_FL_DYNAMIC |
#ifndef CONFIG_HAVE_DYNAMIC_FTRACE_WITH_ARGS
		  FTRACE_OPS_FL_SAVE_REGS |
#endif
		  FTRACE_OPS_FL_IPMODIFY |      /* ★声明"我要改 IP" */
		  FTRACE_OPS_FL_PERMANENT;
ftrace_set_filter_ip(&ops->fops, ftrace_loc, 0, 0);
register_ftrace_function(&ops->fops);
```

★**livepatch 不是 DIRECT**——它走传统 ops 回调路径，但**改写返回 IP**（`ftrace_regs_set_instruction_pointer`）。`IPMODIFY` 标志的意义是**向 ftrace 层声明"我需要改 IP"**——ftrace 因此知道：如果有 DIRECT ops 占用了同一 ip，需要协调。

## 四、IPMODIFY 协调协议（ftrace.c:7916-7958 + bpf/trampoline.c:35-100）

当 IPMODIFY ops（livepatch）注册到一个已被 DIRECT ops（BPF trampoline）占用的 ip 时：

```
register_ftrace_function(ops)          ← livepatch 侧
  └─ prepare_direct_functions_for_ipmodify(ops)  /* ftrace.c:7916 */
       │  lockdep_assert_held_once(&direct_mutex)
       │  if (!(ops->flags & FTRACE_OPS_FL_IPMODIFY))  → 不协调
       │
       │  遍历 ops 的 filter_hash 的每个 ip：
       │    在 ftrace_ops_list 中查找 DIRECT ops 占用此 ip
       │    if (found_op) {
       │        ret = op->ops_func(op,
       │            FTRACE_OPS_CMD_ENABLE_SHARE_IPMODIFY_PEER);
       │            /* ★通知 DIRECT ops："有 IPMODIFY 想来，
       │               你要不要调整" */
       │    }
       │
       ▼ BPF trampoline 的响应（bpf_tramp_ftrace_ops_func）：
       case FTRACE_OPS_CMD_ENABLE_SHARE_IPMODIFY_SELF:
           /* register_ftrace_direct 内部触发 */
           if (tr->flags & BPF_TRAMP_F_CALL_ORIG &&
               !(tr->flags & BPF_TRAMP_F_ORIG_STACK)) {
               tr->flags |= BPF_TRAMP_F_SHARE_IPMODIFY;
               return -EAGAIN;    /* ★信号：trampoline 需要重建 */
           }
           return 0;

       case FTRACE_OPS_CMD_ENABLE_SHARE_IPMODIFY_PEER:
           /* IPMODIFY（livepatch）来敲门 */
           tr->flags |= BPF_TRAMP_F_SHARE_IPMODIFY;
           break;
```

★**-EAGAIN 信号的设计**：`register_ftrace_direct` 收到 `-EAGAIN` 后知道"trampoline 需要 rebuilt 以添加 ORIG_STACK 支持"（livepatch 的 IPMODIFY handler 会修改 IP，因此 BPF trampoline 需要在栈上保留原函数地址）。ftrace 调用者**释放锁、等待 trampoline 重建完成、然后重试**——避免了在持有 `direct_mutex` 时做耗时的 trampoline 重生成。

**锁序**（bpf/trampoline.c 注释原话）：`tr->mutex → direct_mutex → ftrace_lock`。PEER 命令在 direct_mutex 已持有后到达——用 `mutex_trylock(&tr->mutex)` 防死锁。

## 五、两种"改行为"方式的本质区别

| 维度 | DIRECT（BPF trampoline） | IPMODIFY（livepatch） |
| :--- | :--- | :--- |
| ftrace 层 | 占位回调（`call_direct_funcs`） | 实回调（`klp_ftrace_handler`） |
| 实际调用 | ftrace_caller 汇编直接跳 `direct_call` | handler 改写 `fregs->ip` 后返回 |
| 间接层级 | ★零间接（一条 jmp/call） | 一次 handler 调用 + 一次间接跳 |
| 功能 | 只能"包裹"（fentry/fexit/fmod_ret） | 可以"替换"（改写 IP） |
| 声明 | `FTRACE_OPS_FL_DIRECT` | `FTRACE_OPS_FL_IPMODIFY` |
| 排他 | 同一 ip 只能一个 DIRECT | 可与 DIRECT 共存（SHARE_IPMODIFY） |

## 六、观测实战

```bash
# 1) direct 占用情况
cat /sys/kernel/debug/tracing/available_filter_functions | head
#   （direct 占用的 ip 仍在此列出——不影响 ftrace 可见性）

# 2) livepatch 的 IPMODIFY ops
cat /sys/kernel/debug/tracing/enabled_functions | head
#   输出形如：kernel_function (1) direct: tramp:0x... (1 op)
#   "direct:" 标记 = DIRECT 占用

# 3) 共存验证
#   加载一个 BPF fentry prog（占 DIRECT）→ 尝试 livepatch 同一函数
#   → dmesg 观察协商与 SHARE_IPMODIFY 标志
dmesg | grep -i "share_ipmodify\|ipmodify"

# 4) ftrace 的三种调用路径对比
perf stat -e cycles -a -- bpftrace -e 'kfunc:__x64_sys_write { @=count(); }'
#   fentry（DIRECT）：零间接
#   kprobe：int3 陷阱 + single-step
#   livepatch：handler + IP 改写
```

## 七、思考题

1. `call_direct_funcs` 不做实际工作——为什么仍要设为 `ops->func`？（提示：ops 链的遍历逻辑需要每个 ops 有 func——不为 NULL；真正调用在汇编层做。）
2. livepatch 用 IPMODIFY 而不用 DIRECT——为什么？（提示：DIRECT 是"包夹原函数"，livepatch 需要"完全替换"——两者的调用语义根本不同。）
3. `prepare_direct_functions_for_ipmodify` 在 `register_ftrace_function` 的哪一步调用——注册前还是注册后？为什么？（提示：协商必须在真正"接管 ip"之前完成，否则 BPF 程序可能已在跑。）
4. `-EAGAIN` 重试协议为什么不在 `direct_mutex` 内完成 trampoline 重建？（提示：trampoline 重建可能毫秒级；持锁过久阻塞其他 DIRECT 注册者。）
5. `FTRACE_OPS_FL_PERMANENT`（livepatch 设置）意味着什么？（提示：此 ops 一旦注册**不可卸载**——livepatch 的一致性模型要求补丁在位时不能移除。）
6. 同一函数同时有 kprobe + DIRECT + IPMODIFY 三方——调用路径是什么？（提示：kprobe 的 int3 独立于 ftrace；DIRECT 与 IPMODIFY 在 ftrace_caller 内协商——SHARE_IPMODIFY 下 BPF trampoline 添加 ORIG_STACK，livepatch 的 handler 在 BPF 程序之前或之后？）

---

> 回看：[05-tracer 家族](05-tracer家族.md)（ftrace ops 链与 function tracer 基础）｜[14-BPF trampoline](14-BPFtrampoline.md)（DIRECT 的消费者）｜[16-bpf_struct_ops](16-bpf_struct_ops.md)（orig_call=NULL 的对照——完全替代型）｜[livepatch 分册](../../ras/kdump/README.md)（klp 机制全貌）
