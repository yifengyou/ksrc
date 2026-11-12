# BPF trampoline：在函数边界运行 eBPF（Linux 6.6）

> 阅读目标：吃透 eBPF "attach 到任意内核函数"的底层机制——trampoline 是**按目标函数现场生成的机器码**（不是解释执行）：`struct bpf_trampoline` 的链接模型（fentry/fexit/fmod_ret 三类槽位与 38 链接上限）、`BPF_TRAMP_F_*` 七标志的组装语义、x86 镜像生成器 `arch_prepare_bpf_trampoline` 的布局（保存参数/调 orig/回收返回值）、`bpf_tramp_image` 的生命周期（**percpu_ref**——与 [driver-model/07](../../driver-model/docs/07-资源生命周期三模式.md) 的两阶段模式闭环），以及 BPF LSM 怎么用 [13 章](13-内核代码生成惯用法.md) 的 `lsm_hook_defs.h` X-macro 第三消费端造出 251 个挂载锚点。全部对照 v6.6（kernel/bpf/trampoline.c、include/linux/bpf.h、arch/x86/net/bpf_jit_comp.c、security/bpf/hooks.c、kernel/bpf/bpf_lsm.c）。前置：[10-eBPF 验证器入门](10-eBPF验证器入门.md)｜[13-内核代码生成惯用法](13-内核代码生成惯用法.md)。

---

## 一、问题：fentry/fexit 的"探针代码在哪儿跑"

```
BPF_TRACE_FENTRY/FEXIT 程序要"在函数 X 的边界运行"：
  不能改 X 的源码；ftrace 的 ops 回调只是间接调用（有分发开销）；
  kprobe 是断点陷阱（更慢）
答案：为每个挂载目标生成一段定制机器码——trampoline：
  ① 保存被钩函数的参数到 BPF 可见的栈帧
  ② 依序调用挂着的 BPF 程序（已 JIT 的原生代码）
  ③ （fexit）回收原函数返回值喂给 BPF
  ④ 恢复寄存器、按语义跳回（或跳过原函数）
把这段代码以 ftrace direct（FTRACE_OPS_FL_DIRECT，[trace/05 §2.2]）
  的方式直接打在目标函数入口——调用路径上零间接层
```

## 二、结构：三层模型（include/linux/bpf.h）

```c
enum bpf_tramp_prog_type {          /* :1097-1103 —— 槽位分类 */
	BPF_TRAMP_FENTRY,              /* 前置：看参数 */
	BPF_TRAMP_FEXIT,               /* 后置：看返回值 */
	BPF_TRAMP_MODIFY_RETURN,       /* 可改返回值/短路 */
	BPF_TRAMP_MAX,
	BPF_TRAMP_REPLACE,             /* "more than MAX"：整体替换型 */
};

struct bpf_trampoline {             /* :1117 —— 挂载点的账本（哈希查）*/
	struct hlist_node hlist;
	struct ftrace_ops *fops;        /* ftrace direct 通道 */
	struct mutex mutex;
	u64 key;                        /* 挂载点身份（BTF id 等）*/
	struct { struct btf_func_model model; bool ftrace_managed; ... };
	struct bpf_prog *extension_prog;
	struct hlist_head progs_hlist[BPF_TRAMP_MAX];   /* 每槽位一组程序 */
	int progs_cnt[BPF_TRAMP_MAX];
	struct bpf_tramp_image *cur_image;
	...
};

/* :1036-1042 —— 链接数上限与体积预算 */
enum {
#if defined(__s390x__)
	BPF_MAX_TRAMP_LINKS = 27,
#else
	BPF_MAX_TRAMP_LINKS = 38,
#endif
};
/* :1032-1035 注释原话："Each call __bpf_prog_enter + call bpf_func +
   call __bpf_prog_exit is ~50 bytes on x86." —— 38×50≈1900 字节，
   超出即拒绝新挂载（trampoline 镜像体积的硬边界）*/
```

运行时的程序包装（`__bpf_prog_enter`/`__bpf_prog_exit`）负责迁移禁用、run_ctx 栈、计时——BPF 程序本体只是夹在中间的 `call bpf_func`。

## 三、x86 镜像生成器（arch/x86/net/bpf_jit_comp.c:2353）

```
arch_prepare_bpf_trampoline(im, image, image_end, m, flags, tlinks, orig_call)
  /* m = btf_func_model：目标函数的原型（参数个数/类型/位置）——
     BTF 驱动代码生成（[13 章] "BTF 族"的运行期消费）*/
  ① 参数布局：逐参判定（结构体参数 BTF_FMODEL_STRUCT_ARG 走栈）
     nr_regs > MAX_BPF_FUNC_ARGS → 拒绝
  ② 栈帧规划：为 orig_call / fentry 程序的返回值预留槽（save_ret）
  ③ BPF_TRAMP_F_IP_ARG：额外把指令地址作为伪参数传给 BPF 程序
  ④ 按槽位发射指令流：
     prologue（保存寄存器）
     → fentry 链逐个（enter/bpf_func/exit 三连，~50B each）
     → CALL_ORIG：call orig_call（fexit/fmod_ret 需要真返回值；
        fentry-only 则跳过直接 RETORE_REGS 返回）
     → fmod_ret 链（可用返回值决策）
     → epilogue + 按标志恢复/跳帧（SKIP_FRAME：ret 而非 jmp 回父帧）
  ⑤ 记录 im->ip_after_call / im->ip_epilogue —— ★给并发更新用的
     "旧代码仍在执行"探测点（见 §四）
```

每次挂/卸程序都**重新生成整段镜像**，再原子换入口指针（`bpf_trampoline_update`，trampoline.c:33）——旧镜像延迟退役。

## 四、bpf_tramp_image：镜像的生命周期（bpf.h:1105-1116）

```c
struct bpf_tramp_image {
	void *image;                 /* 生成的机器码本体 */
	struct bpf_ksym ksym;        /* ★kallsyms 可见（/proc/kallsyms 里
	                                bpf_trampoline_XXXX 符号的来源）*/
	struct percpu_ref pcref;     /* ★正在执行此镜像的 CPU 计数——
	                                [driver-model/07] percpu_ref 的
	                                kill 切原子两阶段在此闭环 */
	void *ip_after_call;         /* 更新探测点（见 §三⑤）*/
	void *ip_epilogue;
	union { struct rcu_head rcu; struct work_struct work; };  /* 延迟释放 */
};
```

**更新协议**：换新镜像后旧镜像不能立刻释放——某 CPU 可能正执行在旧代码中间。方案：旧镜像 `percpu_ref_kill`（进入原子计数）→ 新入口生效 → 等执行流离开（text_poke 的 sync + pcref 归零）→ 经 workqueue 释放。**探测点 `ip_after_call`** 用于"已过 orig_call 调用则无妨快速换"的细分判断。

## 五、BPF_TRAMP_F_* 七标志（bpf.h:1006-1030）

| 标志 | 语义（头注释节选） |
| :--- | :--- |
| `RESTORE_REGS` BIT(0) | fentry-only：恢复寄存器返回原函数继续 |
| `CALL_ORIG` BIT(1) | 调用原函数（fexit/fmod_ret 需要） |
| `SKIP_FRAME` BIT(2) | 跳过当前帧直接 ret 到父帧 |
| `IP_ARG` BIT(3) | 指令地址作为伪参数（fentry/fexit 可用 `bpf_get_func_ip`） |
| `RET_FENTRY_RET` BIT(4) | 返回 fentry 程序的返回值（bpf_struct_ops 专用） |
| `ORIG_STACK` BIT(5) | 保留原栈（fexit/fmod_ret 读原函数栈上的大参数） |
| `SHARE_IPMODIFY` BIT(6) | 与 livepatch 共享目标点 |

组合即文档（头注释 :1050-1067 给了三种典型组装的 flags 算式）——trampoline 是**由标志参数化的微型代码合成器**。

## 六、BPF LSM：X-macro 第三消费端的完整闭环（security/bpf/hooks.c + kernel/bpf/bpf_lsm.c + include/linux/bpf_lsm.h）

[13 章](13-内核代码生成惯用法.md) 的 `lsm_hook_defs.h` 251 条清单在 BPF LSM 侧被消费**两次**：

```c
/* ① 声明锚点（include/linux/bpf_lsm.h:16-19）*/
#define LSM_HOOK(RET, DEFAULT, NAME, ...) \
	RET bpf_lsm_##NAME(__VA_ARGS__);
#include <linux/lsm_hook_defs.h>

/* ② 定义锚点（kernel/bpf/bpf_lsm.c:21-29）——注释原话（:21-23）："For every
   LSM hook that allows attachment of BPF programs, declare a nop
   function where a BPF program can be attached." */
#define LSM_HOOK(RET, DEFAULT, NAME, ...)	\
noinline RET bpf_lsm_##NAME(__VA_ARGS__)	\
{						\
	return DEFAULT;				\
}
#include <linux/lsm_hook_defs.h>
/* 同文件 :27-29 再消费一遍：BTF_ID(func, bpf_lsm_##NAME) 收进
   bpf_lsm_hooks 集合（[13 章] BTF_ID 族）——attach 时校验目标合法性 */

/* ③ 注册为 LSM（security/bpf/hooks.c:10-34）*/
static struct security_hook_list bpf_lsm_hooks[] __ro_after_init = {
	#define LSM_HOOK(RET, DEFAULT, NAME, ...) \
	LSM_HOOK_INIT(NAME, bpf_lsm_##NAME),
	#include <linux/lsm_hook_defs.h>
	#undef LSM_HOOK
	LSM_HOOK_INIT(inode_free_security, bpf_inode_storage_free),
	LSM_HOOK_INIT(task_free, bpf_task_storage_free),
};
DEFINE_LSM(bpf) = { .name = "bpf", .init = bpf_lsm_init, ... };
```

★机制闭环：`bpf_lsm_##NAME` 是**返回默认值的 noinline 空函数**——平时 LSM 链调到它等于直通；BPF_LSM 程序 attach 时，trampoline 在**这个 nop 函数的入口**生成镜像。因此 BPF 程序的 hook 点在 LSM 链的 bpf 位置（顺序由注册决定），而 trampoline 机器码插在 bpf_lsm_##NAME 处——**逻辑位置（LSM 链）与物理插桩（函数入口）分离**，互不干扰。

## 七、观测实战

```bash
# 1) trampoline 在 kallsyms 里的样子（ksym 字段的直接产物）
grep bpf_trampoline /proc/kallsyms | head

# 2) 挂载与链接（fentry/fexit/lsm 程序）
bpftool link show                    # trampoline link 与其 prog/target
bpftool prog show | grep -E 'tracing|lsm'
bpftool btf dump file /sys/kernel/btf/vmlinux | head -3   # func model 的源头

# 3) LSM 挂载实验（bpf_lsm 锚点可见性）
bpftool prog load lsm.o /sys/fs/bpf/lsm \
  && bpftool link attach prog /sys/fs/bpf/lsm lsm bpf_lsm/file_open

# 4) 性能：fentry 与 kprobe 的对比
perf stat -e cycles -a -- bpftrace -e 'kfunc:__x64_sys_write { @[comm]=count; }'
#   fentry 路径无 int3 陷阱、无 single-step；trampoline 直接调用
```

## 八、思考题

1. trampoline 为什么用 ftrace direct 通道而不是独立 int3（像 kprobe）？（提示：FTRACE_OPS_FL_DIRECT 的 nop 占位与 text_poke 基础设施复用——[trace/05 §2.2] 的 ops 链。）
2. `BPF_MAX_TRAMP_LINKS=38` 的上限由"~50 字节/链"推出——s390x 为何是 27？（提示：不同架构指令密度与 JIT 序列长度；预算的是镜像总体积。）
3. fexit 需要 `ORIG_STACK` 读大参数——fentry 为什么不需要？（提示：参数在 fentry 时刻还在寄存器/尚未被 callee 覆盖；返回后呢？）
4. 旧镜像的 `ip_after_call` 探测点解决什么竞态？（对照 `percpu_ref` 的 kill 切原子：探测点细分的是"执行到哪一段"。）
5. BPF LSM 的 nop 锚点为何必须 `noinline`？（提示：若被内联，挂载点消失了——锚点函数既是 LSM 链的成员又是 trampoline 的物理宿主。）
6. `BPF_TRAMP_MODIFY_RETURN` 能改返回值——它对"原函数已经执行完"这一事实意味着什么？（提示：orig 已跑，副作用已发生，只是返回值被换——与 fmod_ret 语义注释的 "prevent execution" 场景对照：短路发生在调用 orig 之前还是之后？）

---

> 回看：[10-eBPF 验证器入门](10-eBPF验证器入门.md)（挂载前的安全证明）｜[11-tnum 三值数](11-tnum三值数.md)（verifier 的数学）｜[13-内核代码生成惯用法](13-内核代码生成惯用法.md)（lsm_hook_defs 与 BTF_ID 族）｜[05-tracer 家族](05-tracer家族.md)（ftrace direct 的 ops 基础）｜[driver-model/07-资源生命周期三模式](../../driver-model/docs/07-资源生命周期三模式.md)（percpu_ref 的两阶段关闭在此闭环）
