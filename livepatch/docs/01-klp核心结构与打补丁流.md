# livepatch 核心结构与打补丁流（Linux 6.6）

> 阅读目标：掌握 livepatch 的**三层描述结构**（klp_patch → klp_object → klp_func）、`klp_enable_patch` 的完整注册流（重定位/对象加载/ftrace 挂钩）、`klp_ftrace_handler` 的栈式派发与 `transition` 窗口语义。前置：[module 分册](../../module/README.md)（KO 加载与符号解析）、[trace 分册 ftrace](../../trace/docs/05-tracer家族与perf-BPF.md)。

---

## 一、三层描述结构（include/linux/livepatch.h，已核对 6.6）

```c
struct klp_func {
	const char	*old_name;      /* 被补丁函数名（kallsyms 查 old_func）*/
	void		*new_func;      /* 补丁体 */
	unsigned long	old_sympos;     /* 重名符号取第几个（1 起）*/
	void		*old_func;      /* 加载后由 kallsyms 解析填入 */
	struct list_head stack_node;   /* 挂 klp_ops->func_stack（栈式派发）*/
	unsigned long	old_size, new_size;  /* 6.6：老函数区段长度（栈检查用）*/
	bool nop, patched, transition; /* nop：占位"恢复原样"的哑补丁 */
};

struct klp_object {
	const char	*name;      /* NULL = vmlinux 本体；否则目标模块名 */
	struct klp_func	*funcs;
	struct klp_callbacks callbacks;  /* pre/post_patch/unpatch 四钩子 */
	struct module	*mod;       /* name 解析出的目标模块 */
	bool dynamic, patched;
	struct list_head func_list;      /* 二进制进 func_list，源码进 funcs */
};

struct klp_patch {
	struct module	*mod;        /* 补丁自身是 KO */
	struct klp_object *objs;
	struct klp_state *states;     /* 系统状态声明（防不兼容叠加）*/
	bool replace;                 /* ★原子替换：清掉之前所有补丁 */
	struct list_head list;        /* 挂全局 klp_patches 链 */
	bool enabled, forced;
	struct work_struct free_work; struct completion finish; /* 异步卸载 */
};
```

## 二、注册流：klp_enable_patch（kernel/livepatch/core.c，已核对）

```
KO 模块 init（KLP 宏生成）→ klp_enable_patch(patch):
  ① 挂 klp_patches 链、isolate 补丁模块（禁手动 rmmod）
  ② klp_init_*：逐对象/函数登记；klp_check_stack 对称性、
     klp_verify_patch_complete 前置
  ③ 若目标模块未加载（obj->name 非空但 mod==NULL）→ 挂 module
     coming/going 通知链，等它上线再补（klp_module_coming）
  ④ __klp_enable_patch:
     klp_transition_patch 占坑（并发 enable 直接 -EBUSY）
     → klp_init_transition(patch, KLP_PATCHED)（下一章）
     → smp_wmb → 对每个已加载对象 klp_patch_object：
        klp_patch_func（逐函数挂 ftrace，见下）
     → klp_try_complete_transition（等各任务过过渡期）
```

**重定位**（`klp_init_object_loaded` / `klp_write_relocation`）：补丁体引用老内核符号时，按 `.klp.relocs`（特殊节）把 kallsyms 解析出的地址写进补丁模块的重定位槽——livepatch 依赖 module 分册的 kallsyms 作为"老世界地图"。

## 三、ftrace 集成：klp_ftrace_handler（kernel/livepatch/patch.c，已核对）

```
挂点：register_ftrace_function 挂 old_func 入口，ops 带
      FTRACE_OPS_FL_SAVE_REGS | FTRACE_OPS_FL_IPMODIFY
      （可改 regs->ip 的 ftrace 特权——整条路上仅 livepatch 用）
派发：notrace klp_ftrace_handler(ip, parent_ip, fops, fregs):
  ops = container_of(fops, klp_ops)          ← 同一 old_addr 可叠补丁
  func = list_first_or_null_rcu(&ops->func_stack) ← 栈顶=最新补丁
  ftrace_test_recursion_trylock 防递归
  ★ if (func->transition)                     ← 过渡窗口（下一章）：
      读 task->patch_state：目标态=PATCHED 走 new_func，
      否则走 old_func —— "一半任务已切换"的瞬时一致性靠这里
  else（transition 结束）：无条件走 new_func
  改写：klp_arch_set_pc(fregs, 新地址)（x86: 修 regs->ip 直接跳补丁体）
```

`klp_ops` 以 `old_addr` 为键全局缓存（`klp_find_ops`）：多补丁叠同一函数共用一个 ftrace ops，`func_stack` 维持补丁栈序——**叠加补丁与回滚的底座**。

## 四、观测与实战

```
/sys/kernel/livepatch/<补丁名>/
  enabled(RW)  transition(RO)  force(WO)     ← 属性已核对（__ATTR 族）
cat /sys/kernel/livepatch/*/transition       # 1=还在过渡（有任务睡在旧代码上）
echo 1 > force                                # 卡死兜底：强制收尾（丢一致性）
dmesg | grep livepatch:                       # "enabling patch ..." 全程
KLP 版权威文档：仓库内 livepatch/*.rst 镜像
  （callback/cumulative/system-state/shadow-vars 各有专文）
```

## 五、思考题

1. `old_sympos` 存在的原因？（对照 module 分册 kallsyms：同名静态符号可多个，地址序未必稳定。）
2. `klp_ops` 与 `func_stack` 为什么全局缓存而不是每补丁一份 ftrace ops？（提示：ftrace 对同一 ip 的 ops 数量与 IPMODIFY 冲突检查。）
3. 补丁模块被"隔离"（禁止手动 rmmod）的机制动机？（对照 transition 未完成时 free_work 的引用关系。）
4. `klp_ftrace_handler` 为什么必须 notrace？（对照 [trace 分册 ftrace](../../trace/docs/05-tracer家族与perf-BPF.md) 的递归路径。）
5. 对未加载的目标模块（obj->name 有值、mod 为空）补丁如何迟到生效？（对照 module 通知链 KLP 的 coming 分支。）

---

> 下一章：[一致性模型与 transition](02-一致性模型与transition.md) —— "栈上还有旧代码"怎么办。
