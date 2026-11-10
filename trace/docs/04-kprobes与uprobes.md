# kprobes 与 uprobes（Linux 6.6）

> 阅读目标：吃透动态插桩的双子星——kprobe 从 `register_kprobe()` 到 int3 陷阱处理的 x86 全路径（含单步副本、指令仿真、booster）；optprobe 用 5 字节相对 jmp 干掉 trap 的优化条件；kretprobe 的返回地址替换与 rethook 实例池；ftrace 集成（kprobe_events）的完整语法与 fetch 参数系统；uprobe 的用户态断点 + XOL 单步页；最后用开销对比表收束"静态 vs 动态"的选型逻辑。

---

## 一、kprobe 基础：三件套与 struct kprobe

```c
// include/linux/kprobes.h (v6.6)
typedef int (*kprobe_pre_handler_t) (struct kprobe *, struct pt_regs *);
typedef void (*kprobe_post_handler_t) (struct kprobe *, struct pt_regs *,
				       unsigned long flags);

struct kprobe {
	struct hlist_node hlist;          // 挂入 kprobe_table（按地址哈希）
	struct list_head list;            // 同地址多 probe 时的聚合链
	unsigned long nmissed;            // 被禁用期间/重入时错过计数
	kprobe_opcode_t *addr;            // 探针地址（插入 int3 的位置）
	const char *symbol_name;          // 符号名（注册时解析）
	unsigned int offset;              // 符号内偏移
	kprobe_pre_handler_t pre_handler; // ① 执行原指令前（可改 regs 改变流）
	kprobe_post_handler_t post_handler; // ② 单步完成后
	kprobe_opcode_t opcode;           // 被替换的第一字节（备份）
	struct arch_specific_insn ainsn;  // 架构相关：单步副本 insn、emulate_op 等
	u32 flags;                        // GONE/DISABLED/OPTIMIZED/FTRACE/...
};
```

* **pre_handler 返回值**：返回非 0 表示"我改了 regs->ip"——跳过单步与 post，直接按新 IP 继续（livepatch/ftrace 的 function override 用这招）；
* **fault_handler**：6.6 里已经没有独立的 `fault_handler` 字段了——探针副本执行中的缺页统一走 `kprobe_fault_handler`（x86_kprobes_core.c:1013），检查 `kprobe_running()` 且 fault 地址在 insn slot 内则跳过通知上游，避免把 probe 自身的缺页误报为 oops。

注册入口的骨架（kernel/kprobes.c:1613）：

```c
// kernel/kprobes.c (v6.6, 节选)
int register_kprobe(struct kprobe *p)
{
	addr = _kprobe_addr(p->addr, p->symbol_name, p->offset, &on_func_entry);
	p->addr = addr;                                  // 符号+偏移 → 绝对地址
	ret = check_kprobe_address_safe(p, &probed_mod); // ★安全检查（见 §4）
	mutex_lock(&kprobe_mutex);
	if (old_p) {                                     // 同地址已有探针
		ret = register_aggr_kprobe(old_p, p);    // → 聚合探针（§6）
		goto out;
	}
	cpus_read_lock();
	mutex_lock(&text_mutex);
	ret = prepare_kprobe(p);                         // arch_prepare_kprobe（副本）
	mutex_unlock(&text_mutex);
	cpus_read_unlock();
	hlist_add_head_rcu(&p->hlist,                    // 挂入哈希表（RCU）
			&kprobe_table[hash_ptr(p->addr, KPROBE_HASH_BITS)]);
	if (!kprobes_all_disarmed && !kprobe_disabled(p))
		arm_kprobe(p);                           // ★写 int3（text_poke）
	try_to_optimize_kprobe(p);                       // 尝试 optprobe
	mutex_unlock(&kprobe_mutex);
	...
}
```

---

## 二、x86 实现全路径

### 2.1 指令副本与 int3 插入

`arch_prepare_kprobe`（x86_kprobes_core.c:741）做三件事：

```c
int arch_prepare_kprobe(struct kprobe *p)
{
	if (alternatives_text_reserved(p->addr, p->addr))   // alternatives/paravirt 打补丁区
		return -EINVAL;
	if (!can_probe((unsigned long)p->addr))             // 见下
		return -EILSEQ;
	p->ainsn.insn = get_insn_slot();                    // ①insn slot 页上取槽位
	ret = arch_copy_kprobe(p);                          // ②复制并改写指令
	...
}
```

`can_probe` 的否决条件（arch 侧）：地址在指令边界之外（`is_probed_address_safe`——通过搜索前一条指令解码验证，x86 变长指令必须确认 offset 落在真实指令起点）、异常表条目（`search_exception_tables`——该指令可能被 fixup 跳转目标引用，复制执行会破坏约定）。

`arch_copy_kprobe`（line 708）的产物是把**原指令复制到 slot 页**并在其后追加一条 int3：

```
原函数（.text，只读）              insn slot 页（kprobe_insn_page，可执行）
┌───────────────┐                ┌───────────────────────────┐
│ push %rbp     │ ← addr         │ push %rbp   ← ainsn.insn  │ 复制的原指令
│ [int3]        │ ← 插入断点      │ int3        ← 单步陷阱    │ 副本后的断点
│ mov %rsp,%rbp │                │ (booster 场景改为:         │
└───────────────┘                │  jmp 回 addr+len)          │
                                 └───────────────────────────┘
```

RIP 相对寻址（`mov 0x10(%rip), %rax`）在复制时被 `__copy_instruction` 重写偏移（rel32 修正为新位置指向原目标）。`prepare_emulation`（line 560 起）进一步识别**改 IP 的指令**，登记 `p->ainsn.emulate_op`：

* `0xe8 call rel32` / `0xe9 jmp rel32` —— 相对调用/跳转，仿真为"直接改 regs->ip"；
* `0x0f 8x jcc rel32` —— 条件跳转按 EFLAGS 求值；
* `0xe0..0xe3 loop/jcxz`、`0xff /2 /4`（间接 call/jmp）—— 寄存器读出目标；
* far call/jmp、VM 指令（`0x0f 01 c0`）—— 直接 `-EOPNOTSUPP` 拒绝插桩。

`arch_arm_kprobe`（line 767）真正写断点：

```c
void arch_arm_kprobe(struct kprobe *p)
{
	u8 int3 = INT3_INSN_OPCODE;
	text_poke(p->addr, &int3, 1);      // 写入 0xCC（int3）
	text_poke_sync();                  // 同步所有 CPU 的 icache/流水线
	perf_event_text_poke(p->addr, &p->opcode, 1, &int3, 1);  // 通知 PMU 采样器
}
```

### 2.2 int3 陷阱处理：kprobe_int3_handler

```
执行流到达 p->addr → CPU 走 IDT[3]（中断门，关中断）→ do_int3 → kprobe_int3_handler
                                                        (x86_kprobes_core.c:958)
┌────────────────────────────────────────────────────────────────┐
│ regs->ip -= 1（指向 int3），addr = p->addr                       │
│ p = get_kprobe(addr)   ← kprobe_table 哈希查找（RCU/preempt off）│
│                                                                │
│ 分支 A: p 存在且无正在执行的 probe                               │
│   set_current_kprobe(p)（per-CPU current_kprobe = p）            │
│   status = KPROBE_HIT_ACTIVE                                   │
│   pre_handler(p, regs)：                                        │
│     返回 0 → setup_singlestep(p, regs, kcb, 0)  ← 继续单步       │
│     返回非0 → reset_current_kprobe()（探针改了 ip，直接放行）      │
│                                                                │
│ 分支 B: p 存在但 kprobe_running()（重入！）                       │
│   → reenter_kprobe()（§3）                                      │
│                                                                │
│ 分支 C: p 不存在但正在单步（ip 落在 p->ainsn.insn 页内）           │
│   → 这是"副本后的第二条 int3" → resume_singlestep：               │
│      regs->flags |= saved_flags（恢复 IF）                       │
│      regs->ip += (orig_ip - copy_ip) - 1  ← 换算回原函数下一条    │
│      kprobe_post_process → post_handler + 状态复位               │
└────────────────────────────────────────────────────────────────┘
```

`setup_singlestep`（line 843）的次序藏着两个关键点：

```c
static void setup_singlestep(struct kprobe *p, struct pt_regs *regs,
			     struct kprobe_ctlblk *kcb, int reenter)
{
	if (setup_detour_execution(p, regs, reenter))   // fprobe/direct 特例
		return;
#if !defined(CONFIG_PREEMPTION)
	if (p->ainsn.boostable) {
		/* Booster：副本后已是 jmp 回原代码，无需第二次 int3 —— 
		   直接 regs->ip = ainsn.insn，一条路跑到黑 */
		regs->ip = (unsigned long)p->ainsn.insn;
		return;
	}
#endif
	if (reenter) { save_previous_kprobe(kcb); ... KPROBE_REENTER; }
	else kcb->kprobe_status = KPROBE_HIT_SS;

	if (p->ainsn.emulate_op) {          // 改 IP 指令：纯仿真，不单步
		p->ainsn.emulate_op(p, regs);
		kprobe_post_process(p, regs, kcb);
		return;
	}
	regs->flags &= ~X86_EFLAGS_IF;      // 单步期间关中断
	regs->ip = (unsigned long)p->ainsn.insn;   // 跳去副本执行
}
```

> 💡 **为什么单步副本而不是把原指令换回去单步**：SMP 上其他 CPU 仍在执行该函数，换回原字节会让他们"漏"过探针；而且"换回→单步→再写 int3"窗口里再进 int3 会递归。**复制到 slot 页执行**是 kprobe 的根本安全设计，int3-after-copy 让"单步完成"也复用同一个陷阱通道（比 TF 陷阱快，且不用处理 TF 的遗留语义）。

### 2.3 一个探针命中的完整时间线（开销解剖）

```
ns 级示意（现代 x86_64，数值为量级参考）:
 0    执行到 addr 的 int3 → #DB/#BP 异常压栈（~100-300ns：异常开销+IDT 查找）
 +    do_int3 → kprobe_int3_handler（哈希查找、status 机器）
 +    pre_handler（trace_kprobe: process_fetch_insn 取参数+写 ring buffer ~100ns）
 +    setup_singlestep → 跳副本执行原指令（几 ns~几十 ns）
 +    副本后的 int3 → 第二次异常！（又一次 ~100-300ns）
 +    resume_singlestep 换算 ip 返回原函数
 ────────────────────────────────────────────────
 合计 ~0.5-1.5 μs；booster 省掉第二次异常；optprobe 连第一次都省掉
```

`can_boost`（line 140）的位表（`twobyte_is_boostable`）决定哪些指令的副本可以直接以 `jmp` 收尾（booster）：跳转/调用类不行（emulate 路径），因为它们的"下一条指令"不是顺序的。

---

## 三、重入保护：kprobe_ctlblk 与 kprobe_busy

per-CPU 的 `kprobe_ctlblk`（kcb）保存执行状态机：`kprobe_status`（HIT_ACTIVE→HIT_SS→HIT_SSDONE）、`prev_kprobe`（嵌套保存栈）、`kprobe_saved_flags`（单步期间的 IF）。`reenter_kprobe`（line 918）处理"探针 handler 里又命中探针"：

* 状态为 SSDONE/ACTIVE/SS：`nmissed++`，`setup_singlestep(reenter=1)`——**只单步不调 handler**（防递归雪崩）；
* 状态已是 REENTER（嵌套中又嵌套）：`BUG()`——理论上探针回调路径全在 `.kprobes.text`（NOKPROBE_SYMBOL 标记），再次进入说明违反约定。

**kprobe_busy**（kprobes.c:1237）是另一个更狠的保护：去注册路径要 `hlist_del_rcu` + `synchronize_rcu` 之外，还必须在释放前挡住"正在 int3 handler 里读该 probe"的 CPU：

```c
static struct kprobe kprobe_busy = { .addr = (void *)get_kprobe };
void kprobe_busy_begin(void)     // __unregister_kprobe_top 期间调用
{
	preempt_disable();
	__this_cpu_write(current_kprobe, &kprobe_busy);  // 假探针占位
	kcb->kprobe_status = KPROBE_HIT_ACTIVE;
}
```

`kprobe_running()` 非空时新命中的探针走 reenter 路径不执行 handler——批量 disarming 期间的安全毯。kretprobe 的 trampoline handler 同样借它防递归（kprobes.h:244 的 `kretprobe_trampoline_handler` 内联里 begin/end 包夹）。

---

## 四、黑名单：哪些地方不能插桩

`check_kprobe_address_safe`（kprobes.c:1559）一口气否决八类位置：

```c
if (!(core_kernel_text(p->addr) || is_module_text_address(p->addr)) ||
    in_gate_area_no_mm(p->addr) ||
    within_kprobe_blacklist(p->addr) ||           // ① 显式黑名单
    jump_label_text_reserved(p->addr, p->addr) || // ② static_key 补丁点
    static_call_text_reserved(p->addr, p->addr) ||// ③ static_call 补丁点
    find_bug(p->addr) ||                          // ④ BUG 表条目
    is_cfi_preamble_symbol(p->addr)) {            // ⑤ CFI 前导（clang）
	ret = -EINVAL;
```

①的实现是链接器段：`NOKPROBE_SYMBOL(func)` 把函数地址放进 `__kprobe_blacklist` 段（`__start/stop_kprobe_blacklist`）。**谁必须进黑名单**：
* **int3 处理路径本身**（`do_int3`、`kprobe_int3_handler`、`get_kprobe`...）——在断点处理器里再下断点是无限递归；
* **NMI 敏感代码**：`nmi_enter/nmi_exit`、`__switch_to`（kretprobe 专项黑名单 `kretprobe_blacklist[]`，x86 上 `__switch_to` 换任务不换内核栈，返回地址替换会错乱）；
* **早启动代码**：`parse_early_param` 前的代码——那时 int3 handler 尚未就绪（`early_idt_handler` 不认识 kprobe）；
* **entry 汇编**（`entry_SYSCALL_64` 等）：栈布局非标准，regs 假设不成立。

⑥⑦之外的模块规则：`.init.text` 在 `MODULE_STATE_COMING` 之后不可探测（free 掉了）；probe 模块要对目标模块 `try_module_get` 防探测期间被卸载。

---

## 五、optprobe：用 jmp 消灭 trap

`echo 1 > /sys/kernel/debug/kprobes/optimize`（默认开）后，`try_to_optimize_kprobe` 把普通探针升级为 `struct optimized_kprobe`（kprobes.h:357）：

```c
struct optimized_kprobe {
	struct kprobe kp;
	struct list_head list;          // 优化队列
	struct arch_optimized_insn optinsn;  // slot、size、rel32
};
```

**x86 窗口条件**（x86_kprobes_opt.c，v6.6 逐条核对）：

1. **函数入口**（`can_optimize` 隐含——`copy_optimized_instructions` 从函数符号扫到函数尾；非入口 probe 因"可能有人跳进中间"被拒）；
2. **rel32 范围**：`rel = slot - addr + JMP32_INSN_SIZE; |rel| > 0x7fffffff → -ERANGE`（2GB，vmalloc 的 slot 页与内核 text 的距离）；
3. **无跳转进入目标窗口**：解码整个函数，`insn_jump_into_range(&insn, paddr + INT3_INSN_SIZE, DISP32_SIZE)`——任何直接跳转若以 int3 后 4 字节（将被 jmp 覆盖）为目标则拒绝；间接跳转在非 retpoline/非 IBT 内核下也拒绝（jump table 无法静态分析）；
4. **异常表**：函数含 exception entry（fixup 可能跳进来）→ 拒绝；
5. **前 5 字节可重定位**：被覆盖的指令复制进 out-of-line 副本时 RIP 相对量可修正（`copy_optimized_instructions`）。

**out-of-line 副本的布局**（`arch_prepare_optimized_kprobe`，line 391）：

```
kernel .text                         optinsn slot 页（模板 + 副本）
┌────────────────┐                  ┌─────────────────────────────────┐
│ func:          │                  │ optprobe_template_entry:        │
│  push %rbp     │ ← addr           │   [保存 pt_regs 所需的寄存器/栈帧]│
│  [jmp rel32 ───┼─────────────────►│   mov $op, %rdi  ← TMPL_MOVE_IDX │
│  mov ...]      │ 5字节覆盖         │   call optimized_callback        │
│                │                  │   [恢复寄存器]                   │
└────────────────┘                  │   push %rbp ... ← 被覆盖的指令副本│
                                    │   jmp addr+5 ──────────────────►│ 回原函数继续
                                    └─────────────────────────────────┘
```

`optimized_callback`（opt.c 前部）在关中断上下文里构造伪 pt_regs（用模板保存的寄存器），调 `opt_pre_handler`（kprobes.c:422——聚合链上每个子 probe 的 pre_handler），然后从副本继续执行。**全程零异常**——这是 optprobe 比 int3 快 5~10 倍的本质。

打补丁的顺序（`arch_optimize_kprobes`，line 473 注释）：
1. 先以**普通 int3 探针**注册并运行；
2. `synchronize_rcu_tasks()`——等所有可能正停留在 int3 后 4 字节上的执行流离开（RCU tasks 的"每任务都在可睡眠点"语义保证）；
3. `text_poke_bp`（int3 逐字节法）把 int3 覆盖为 `jmp rel32`——旧执行流要么还没到（将 jmp），要么已在 int3 处理里（走旧 probe，两者等价）。

去优化反向：jmp → int3 → 等宽限期 → int3 → 原字节。

---

## 六、聚合探针与 trace_kprobe（ftrace 集成）

### 6.1 aggr probe：同地址多消费者

第二个 probe 注册到同一地址时，`register_aggr_kprobe` 创建/复用聚合体（kprobes.c:1274 `init_aggr_kprobe`）：

```c
static void init_aggr_kprobe(struct kprobe *ap, struct kprobe *p)
{
	copy_kprobe(p, ap);                 // 复制 insn slot 引用
	ap->addr = p->addr;
	ap->pre_handler = aggr_pre_handler; // ★迭代器：调链上所有子 probe
	if (p->post_handler && !kprobe_gone(p))
		ap->post_handler = aggr_post_handler;
	INIT_LIST_HEAD(&ap->list);
	list_add_rcu(&p->list, &ap->list);  // 子 probe 挂链
	hlist_replace_rcu(&p->hlist, &ap->hlist);  // 哈希表换成聚合体
}
static int aggr_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	list_for_each_entry_rcu(kp, &p->list, list) {
		if (kp->pre_handler && likely(!kprobe_disabled(kp))) {
			set_kprobe_instance(kp);
			if (kp->pre_handler(kp, regs))
				return 1;
		}
		reset_kprobe_instance();
	}
	return 0;
}
```

注意 `add_new_kprobe`（line 1258）：新子 probe 带 post_handler 时会 `unoptimize_kprobe(ap, true)`——**optprobe 模板不保存单步后的 post 调用上下文**，必须回落为 int3 探针。BPF 程序 + perf 同时挂一个 kprobe 就走这条路（聚合链两个消费者）。

### 6.2 kprobe_events：动态事件语法

`trace_kprobe`（kernel/trace/trace_kprobe.c）把 kprobe 包装成 `trace_event_call`（`TRACE_EVENT_FL_KPROBE`），经 `kprobe_events` 文件创建（v6.6 语法原文，line 743）：

```
p[:[GRP/]EVENT] [MOD:]KSYM[+OFFS]|KADDR [FETCHARGS]     # kprobe
r[MAXACTIVE][:[GRP/]EVENT] [MOD:]KSYM[+0] [FETCHARGS]   # kretprobe
p[:[GRP/]EVENT] [MOD:]KSYM[+0]%return [FETCHARGS]       # %return 新语法

FETCHARGS:
  $retval          返回值（kretprobe）
  $stack           栈地址
  $stackN          栈上第 N 个 long
  $comm            当前任务 comm
  @ADDR            内核地址处的内存
  @SYM[+|-offs]    数据符号 + 偏移
  %REG             寄存器（%ax、%di ...）
  +|-offs(ARG)     解引用：ARG 的值 ± 偏移处读内存
  NAME=FETCHARG    字段别名
  FETCHARG:TYPE    类型（u32/s64/string/...）
```

解析侧的健壮性细节（v6.6）：`number_of_same_symbols()` 检查重名符号（`kallsyms` 里同名函数必须用地址消歧，否则 `NON_UNIQ_SYMBOL` 错误）；`kprobe_on_func_entry()` 判定入口并置 `TPARG_FL_FENTRY`（ftrace 地址一致时优先走 kprobe-on-ftrace，见 §8）；BTF 参数支持（`MAX_BTF_ARGS_LEN`，`$arg1` 式取参依赖 BTF）。

**probe 的执行端**（`kprobe_dispatcher`，line 1665）→ `trace_probe_dispatch` → fetch 参数系统：

```
kprobe_dispatcher(kp, regs)
  └─ trace_probe_dispatch(tp, regs)
       ├─ process_fetch_insn(code, rec, dest, ...)     # FETCHARGS 编译成的
       │    每个 FETCHARG 是 struct fetch_insn 链表:   # 指令链
       │    [FETCH_OP_REG ax] → [FETCH_OP_DEREF +0x10] → [FETCH_OP_ST_OFFSET 8]
       └─ trace_event_buffer_reserve / commit           # 与 TRACE_EVENT 同一套
```

fetch 指令链在解析时编译好（`traceprobe_parse_probe_arg`），运行时 `process_fetch_insn`（line 1301）顺链求值——寄存器读、内核内存解引用（`copy_from_kernel_nofault`）、符号寻址都是指令类型。这样 kprobe 事件与静态 tracepoint 事件在 ring buffer 层完全同构：`format` 文件、filter、trigger、hist 全部直接可用。

---

## 七、kretprobe：返回地址替换

v6.6 默认 `CONFIG_KRETPROBE_ON_RETHOOK=y`——kretprobe 建立在通用 **rethook** 框架上（kernel/trace/rethook.c，本应给 fprobe 复用）：

```c
// include/linux/kprobes.h (v6.6)
struct kretprobe {
	struct kprobe kp;              // 借道 kprobe 打入口断点
	kretprobe_handler_t handler;         // 返回时回调
	kretprobe_handler_t entry_handler;   // 入口回调（可选，可取参数）
	int maxactive;                 // 并发实例上限
	int nmissed;                   // 池耗尽计数
	size_t data_size;              // 每实例私有数据
	struct rethook *rh;            // ★rethook 句柄
};
```

注册流程（kprobes.c:2193）：

```c
int register_kretprobe(struct kretprobe *rp)
{
	ret = kprobe_on_func_entry(...);        // ①只允许函数入口
	...
	if (rp->maxactive <= 0)
		rp->maxactive = max_t(unsigned int, 10, 2*num_possible_cpus());  // ②默认池深
	rp->rh = rethook_alloc((void *)rp, kretprobe_rethook_handler);
	for (i = 0; i < rp->maxactive; i++) {   // ③预分配全部实例
		inst = kzalloc(struct_size(inst, data, rp->data_size), GFP_KERNEL);
		rethook_add_node(rp->rh, &inst->node);
	}
	rp->kp.pre_handler = pre_handler_kretprobe;   // ④入口 handler
	ret = register_kprobe(&rp->kp);               // ⑤注册入口断点
	...
}
```

命中序列：

```
函数入口 int3/优化跳转命中:
  pre_handler_kretprobe (kprobes.c:2118)
    rhn = rethook_try_get(rp->rh)       // 从池取实例（free 链表，无锁 pop）
      └─ 取不到 → rp->nmissed++，放弃这次返回探针（★maxactive 太小的后果）
    entry_handler(ri, regs)             // 用户入口回调（可存参数进 ri->data）
    rethook_hook(rhn, regs, kprobe_ftrace(p))
      ├─ 把栈上的返回地址替换为 arch_rethook_trampoline      ← x86:
      │    *(unsigned long *)regs->sp = trampoline 地址        kprobes_trampoline.S
      └─ rhn->ret_addr = 原返回地址; rhn->frame_pointer = rbp
          （rethook_node 挂进 current->rethooks 链，按栈帧排序）

函数 return:
  执行流跳到 __kretprobe_trampoline（汇编，不用 C 栈帧）
    → kretprobe_trampoline_handler(regs, frame_pointer)
        遍历 current->rethooks 中 fp 匹配的节点：
          handler(ri, regs)（rp->handler，regs 里已是返回时刻的寄存器）
          恢复真正的返回地址 → ret
    → rethook_recycle(rhn)             // 实例还池
```

> 💡 **frame_pointer 的用途**：递归调用同一个被探函数时，栈上有 N 个被替换的返回地址；返回时刻必须知道"我是哪一层"。x86 用 rbp 做匹配（`HAVE_FUNCTION_GRAPH_FP_TEST` 同款思路）。**不用 fp 的架构**（ORC unwinder）靠 `arch_rethook_find_prev_return` 沿栈扫——这也是 kretprobe 在深度递归下开销敏感的原因。`kretprobe_blacklist`（x86 上仅 `__switch_to`）挡住换栈函数。

---

## 八、kprobe on ftrace：第三种执行模式

函数入口的 kprobe 若恰是 ftrace 记录点（`ftrace_location(addr)` 命中，`check_ftrace_location` 自动判定），`CONFIG_KPROBES_ON_FTRACE=y` 时不插 int3——**借 ftrace 的 5 字节调用点**：

```c
// arch/x86/kernel/kprobes/ftrace.c (v6.6, 节选)
void kprobe_ftrace_handler(unsigned long ip, unsigned long parent_ip,
			   struct ftrace_ops *ops, struct ftrace_regs *fregs)
{
	struct pt_regs *regs = ftrace_get_regs(fregs);
	bit = ftrace_test_recursion_trylock(ip, parent_ip);
	p = get_kprobe((kprobe_opcode_t *)ip);
	...
		regs->ip = ip + sizeof(kprobe_opcode_t);   // 模拟"int3 后"语义
		__this_cpu_write(current_kprobe, p);
		if (!p->pre_handler || !p->pre_handler(p, regs)) {
			/* 假装单步完成：跳过 5 字节 nop 回到函数体 */
			regs->ip = (unsigned long)p->addr + MCOUNT_INSN_SIZE;
			if (p->post_handler) { ...; p->post_handler(p, regs, 0); }
			regs->ip = orig_ip;                   // 恢复真实执行点
		}
	...
}
```

优势：ftrace 调用点由编译器保证在入口、无指令解码、无第二次异常——**入口 kprobe 的最优形态**（比 optprobe 还便宜，因为连副本都不需要）。限制：只在函数入口（+offset 不行）、依赖 `CONFIG_FUNCTION_TRACER` 的记录点存在。

---

## 九、uprobe：用户态的动态探针

### 9.1 事件语法与注册

```
p[:[GRP/]EVENT] BIN:OFFSET [FETCHARGS]                # uprobe
r[:[GRP/]EVENT] BIN:OFFSET [FETCHARGS]                # uretprobe（v6.6 也支持 %return 后缀）
p[:[GRP/]EVENT] BIN:OFFSET(REF_CTR_OFFS) [FETCHARGS]  # 带 sdt 引用计数偏移

# 例：
echo 'p:libc/strlen /usr/lib/x86_64-linux-gnu/libc.so.6:0x18a0a0 \
      +0(%di):u8' > uprobe_events
```

解析（trace_uprobe.c:560 起）：`kern_path` 定位文件（必须是普通文件 `d_is_reg`）、`offset` 是**文件内偏移**（不是 vaddr）、`ref_ctr_offset` 可选——指向目标进程内存里的一个计数器（SDT/USDT 的 semaphore），`uprobe` 命中时 smp 库自动 `+1` 告知用户态"此探针活跃"（perf/bpf 的 USDT 支持用它）。

### 9.2 断点与 XOL：单步出局

uprobe 的内核实现（kernel/events/uprobes.c）与 kprobe 同构但跨了特权级：

```
目标进程用户态执行到 OFFSET 处:
  ①内核早已把该文件页映射里的首字节替换为 0xCC（install_breakpoint，对每个
    映射了该 inode+offset 的 mm 生效——所有进程共享同一 uprobe）
  ②#BP 异常（ CPL3 → IDT[3]）→ uprobe_breakpoint_handler
  ③arch_uprobe 抽象层分析过该指令（x86: arch_uprobe_analyze_insn）：
     - 不支持（变长前缀怪癖、rip 相对且跨界）→ 注册时就拒绝
     - 可仿真（push/call 等简单指令）→ arch_uprobe_emulate_step 直接改 regs
     - 需执行 → XOL（eXecute Out-of-Line）
  ④XOL: get_xol_area() —— 该进程 mm 里预留的一页
        （__create_xol_area：mmap 在 vdso 附近的特殊地址，页内 slot 位图分配）
        xol_get_insn_slot() 把指令副本写进 slot（RIP 相对已修正）
        regs->ip = slot；同时设 single-step（x86 用 TF 标志 + our own debug trap）
  ⑤单步完成 #DB → uprobe_single_step_handler → arch_uprobe_post_xol
        修正 rip（+ 相对跳转目标）、恢复 TF、放回原执行点+1 字节
  ⑥uprobe_dispatcher（trace_uprobe.c:1473）→ 与 kprobe 同一套 fetch/事件写入
```

xol_area 是**每 mm 一份**（fork 继承，子进程复制父的 slot 位图并在首次命中重建）；slot 用位图 + `wait_relock` 重试分配，命中即占用直到进程退出（`uprobe_free_utask`）。

**uretprobe** 与 kretprobe 思路一致：入口 handler 把栈上返回地址替换为 `arch_uretprobe_trampoline`，`return_instance` 链按 rbp 排序，返回时刻先调 ri 的 handler 再跳回真实地址。唯一大差异：**必须用 frame pointer**（用户态栈布局由编译器决定，内核无法用 ORC）——`-fomit-frame-pointer` 的二进制上 uretprobe 不可靠。

### 9.3 consumer 模型

```c
// include/linux/uprobes.h (要点)
struct uprobe_consumer {
	uprobe_handler_t handler;          // 命中回调（返回 UPROBE_HANDLER_* 决定链路）
	uprobe_handler_t ret_handler;      // uretprobe 回调
	bool (*filter)(struct uprobe_consumer *uc, enum uprobe_filter_ctx,
		       struct mm_struct *mm);  // ★按进程过滤（PID/comm 任意逻辑）
};
```

一个 inode+offset 的 uprobe 可挂多个 consumer（trace_uprobe 事件、perf、BPF 各一），`filter_chain()` 逐一询问——**过滤后才对 mm 安装断点**，非目标进程零开销。断点安装是延迟的：mmap 命中 `UPROBE_FILTER_MMAP` 或 fork/exec 时逐 vma 补装（`vma_has_uprobes`/`install_breakpoint`）。

### 9.4 JIT 与内联的挑战

* **JIT 代码**：uprobe 定位在**文件 inode+offset**，JIT 产物（V8/LuaJVM）没有文件映射——无法用 uprobe；替代方案：perf 的软件断点（`perf probe -x` 也不行）、BPF trampoline、或让运行时自建 USDT。
* **内联**：内联函数没有独立符号/入口，符号表 `gdb` 都看不见；`perf probe -x` 只能落在实际存在的指令偏移上。
* **静态链接 & 符号剥离**：offset 依赖 ELF 有符号表（或 DWARF）；strip 过的二进制只能手算 offset（`readelf -l` 换算 vaddr→file offset）。
* **地址随机化与版本漂移**：库升级后 offset 变化——USDT（`.note.stapsdt` 段里的 provider:probe+offset+semaphore 三元组）是唯一稳定契约，`readelf -n` 可列出，bpftrace `usdt:./bin:provider:probe` 直接消费。

---

## 十、开销对比总表与选型

同一观测点（以函数入口、记录一个事件到 ring buffer 计）：

| 方式 | 执行路径 | 每命中开销（量级） | 可取参数 | 部署 |
| :--- | :--- | :--- | :--- | :--- |
| 静态 tracepoint（启用） | static_key→static_call→probe | ~100-200ns | 编译期定死 | 内核已内置 |
| function tracer | ftrace call → ops → buffer | ~100-300ns（每函数） | ip/parent_ip | 已内置 |
| kprobe @ int3 | int3 异常×2 + 副本单步 | ~0.5-1.5μs | 任意 regs/内存 | kprobe_events/BPF |
| kprobe @ optprobe | jmp 副本 → callback → jmp 回 | ~100-250ns | 同上（入口） | 同上 |
| kprobe on ftrace | ftrace call → handler（模拟单步） | ~100-200ns | 同上（入口） | 同上 |
| uprobe | 用户 int3 异常 + XOL + 用户态 #DB | ~1-2μs | 用户 regs/内存 | uprobe_events/BPF |
| BPF kprobe 程序 | 宿主 kprobe + 验证过的程序 | 宿主开销 + ~50-500ns | 受验证器限制 | bpftrace/C |

选型逻辑：
1. **有的选就选 tracepoint**（开销低一个量级、参数语义稳定、BPF 直接挂）；
2. **没埋点的内核函数**：入口优先（ftrace/optimized 自动生效），中段必须 int3；
3. **需要返回值/耗时**：kretprobe/uretprobe 或 BPF 的 kretprobe 挂载；
4. **用户态库**：uprobe/USDT；
5. **要聚合不要全量**：上 BPF（第 6 章）。

---

## 十一、实战观测

```bash
# 1. 动态事件全流程
cd /sys/kernel/tracing
echo 'p:test/vfs_read __do_sys_read bytes=$count' > kprobe_events
echo 'r:test/vfs_read_ret ret=$retval' >> kprobe_events
echo 1 > events/test/vfs_read/enable
echo 1 > events/test/vfs_read_ret/enable
cat trace | tail -5

# 2. 观察 probe 状态与优化
cat /sys/kernel/debug/kprobes/list
#   输出: func+0x0 [OPTIMIZED] / [FTRACE] / [DISABLED] 前缀标注执行模式
cat /sys/kernel/debug/kprobes/enabled      # 全局 arm 开关

# 3. maxactive 实验：递归函数上池耗尽
echo 'r10:test/fib_ret fib' > kprobe_events     # 只给 10 个实例
# 跑 fib(20) 递归后，看命中/错过统计（trace_kprobe.c:1253 的 profile 接口）：
cat /sys/kernel/tracing/kprobe_profile
#   输出三列：事件名 / 命中次数(nhit) / nmissed（kprobe 重入 + 池耗尽）

# 4. uprobe 实测
readelf -sW /usr/lib/x86_64-linux-gnu/libc.so.6 | grep -w memcpy  # 拿地址
echo 'p:libc/mcpy /usr/lib/x86_64-linux-gnu/libc.so.6:0x9a1d0 size=$dx' \
     > uprobe_events
echo 1 > events/libc/mcpy/enable
cat trace | head

# 5. BPF 视角（第 6 章预告）
bpftrace -e 'kprobe:vfs_read { @[comm] = count(); }'

# 6. 黑名单验证
echo 'p kprobe_int3_handler' > kprobe_events   # → 报错：address not allowed
cat available_filter_functions | head          # ftrace 侧也排除（与黑名单同源）
```

---

## 十二、思考题

1. kprobe 单步用"副本后追加 int3"而不是 x86 的 TF 单步标志。TF 方案在 SMP 与 IF 标志交互上有哪些坑？（提示：TF 陷阱的优先级、`pushf` 类指令会把 TF 带进用户态。）
2. `optprobe` 打 jmp 前必须先以 int3 探针运行并 `synchronize_rcu_tasks()`。为什么这里的宽限期必须是 **RCU tasks**（等每个任务到达可睡眠点）而不是普通 `synchronize_rcu`？（提示：可能正"睡"在 int3 后 4 字节上的执行流是什么状态？自旋压缩循环里的内核线程对普通 RCU 而言算不算静默？）
3. kretprobe 默认 `maxactive = max(10, 2*num_possible_cpus())`。什么样的工作负载会把 `nmissed` 打成正数？观察 `nmissed` 之后应如何调整？
4. `kprobe_ftrace_handler` 里为什么要把 `regs->ip` 先改成 `ip + 1` 再改回 `addr + MCOUNT_INSN_SIZE`？（提示：pre_handler 代码里普遍假设"regs->ip 指向断点后一条"，ftrace 路径是模拟出来的语义。）
5. uprobe 的 xol_area 按 mm 分配而非全局池，代价是每进程一页内存。为什么不能像 kprobe 的 insn slot 那样全局共享？（提示：XOL 页必须在用户地址空间、且 RIP 相对指令的修正值依赖 slot 位置。）
6. 同一个内核函数分别用 tracepoint（若存在）、kprobe-on-ftrace、int3 kprobe 三种方式各采 100 万次命中（用 `perf stat -e cycles` 圈定区间），设计实验隔离出"异常开销"与"事件写入开销"两个分量。

---

> 下一章：[tracer 家族](05-tracer家族.md) —— 从 `struct tracer` 插件框架出发：function/function_graph/irqsoff/wakeup/hwlat/osnoise/timerlat/stack 逐个拆解。
