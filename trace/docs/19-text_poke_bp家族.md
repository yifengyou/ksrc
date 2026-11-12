# text_poke_bp：修改运行代码的安全原语（Linux 6.6）

> 阅读目标：吃透 x86 上"安全修改正在执行的指令"的底层协议——`text_poke_bp_batch` 的**三步打补丁法**（写 INT3 → 写尾部字节 → 换首字节，每步间 sync cores）；`poke_int3_handler` 的**陷阱模拟**（INT3 断点中仿真旧指令执行，而非真跳转）；为什么**完全避免 stop_machine**（替代为 int3 + sync_core）；以及全树消费方清点（ftrace/optprobe/static_key/static_call/livepatch——它们共享这一个原语）。全部对照 v6.6（arch/x86/kernel/alternative.c 的 :2155-2330 区域）。前置：[18-ftrace 汇编层](18-ftrace汇编层DIRECT分派.md)（ftrace 对 text_poke_bp 的调用）。

---

## 一、问题：修改正在被执行的指令

```
多核系统上，CPU0 正在执行某函数的第一条指令时，CPU1 要把这条
指令替换成别的——直接 memcpy 会产生撕裂读（部分旧部分新的
指令字节）→ 未定义行为

传统解法：stop_machine() —— 全核停下来再改
  缺点：延迟毫秒级（全核同步 IPI + 调度），高频打补丁不可用

text_poke_bp 的解法：INT3 断点 + 三步替换
  延迟微秒级（on_each_cpu 的 sync_core 而非调度停摆）
```

头注释原话（alternative.c:2158-2160）：*"We completely avoid stop_machine() here, and achieve the synchronization using int3 breakpoint."*

## 二、三步协议（alternative.c:2155-2330）

```
text_poke_bp_batch(tp, nr_entries)
  │
  ├─ 第一步：写 INT3 到所有要改的位置（首字节替换为 0xCC）
  │    tp[i].old = *(u8 *)addr;            /* 记住旧首字节 */
  │    text_poke(addr, &int3, 1);          /* 写 INT3 */
  │    text_poke_sync();                   /* ★on_each_cpu(sync_core) */
  │
  ├─ 第二步：写尾部字节（跳过首字节）
  │    text_poke(addr + 1, new + 1, len - 1);
  │    text_poke_sync();
  │    /* 注释原话 :2275："According to Intel, this core syncing is
  │       very likely not necessary and we'd be safe even without it.
  │       But better safe than sorry (plus there's not only Intel)." */
  │
  └─ 第三步：替换首字节（INT3 → 新指令首字节）
       text_poke(addr, &new[0], 1);
       text_poke_sync();
```

★**为什么分三步**：x86 指令是变长的——如果一次写完 5 字节，另一核可能只看到前 2 字节就执行了（撕裂）。INT3 是**单字节**——写它总是原子的。写完 INT3 后，尾部的字节修改不可能被误执行（因为 CPU 碰到 INT3 会陷阱）——此时任何撕裂读到的"新尾部 + INT3"都会走陷阱而非执行错误指令。

## 三、INT3 陷阱的仿真执行（poke_int3_handler :2068-2125）

```c
noinstr int poke_int3_handler(struct pt_regs *regs)
{
	if (user_mode(regs))
		return 0;    /* 用户态 INT3 不归我们管 */

	/*
	 * 注释原话（:2078-2086）："Having observed our INT3 instruction,
	 * we now must observe bp_desc with non-zero refcount:
	 *   bp_desc.refs = 1    INT3
	 *   WMB              RMB
	 *   write INT3       if (bp_desc.refs != 0)"
	 */
	smp_rmb();
	desc = try_get_desc();
	if (!desc)
		return 0;

	ip = (void *)regs->ip - INT3_INSN_SIZE;  /* 回退 1 字节 */

	/* 二分查找确认这个 INT3 是我们打的 */
	/* 匹配则 emulate：仿真旧指令的执行效果，跳过补丁点 */
	...
}
```

★**仿真而非跳转**：陷阱 handler 里拿到 `emulate` 参数（旧指令的替代语义）——不真执行旧指令（可能已被部分覆盖），而是**让 pt_regs 直接反映执行结果**（如 NOP 就直接跳过；call 就 push 返回地址改 RIP）。这就是 `text_poke_bp(addr, opcode, len, emulate)` 第四个参数的用途。

## 四、批量模式：text_poke_finish（:2400-2425）

```
/* 批量路径（同一次 stop_machine 替代内完成大量补丁） */
text_poke_queue(addr, opcode, len, emulate)   /* 排队 */
text_poke_queue(addr, opcode, len, emulate)   /* 排队... */
text_poke_queue(addr, opcode, len, emulate)
text_poke_finish()                             /* 一次 bp_batch 全打 */

★ftrace 批量启用时数千个调用点——一次三步协议批量处理
（ftrace.c:504 的 ftrace_modify_all_code 路径）
```

## 五、全树消费方清点

| 消费方 | 调用点 | 用途 |
| :--- | :--- | :--- |
| **ftrace** | arch/x86/kernel/ftrace.c:188/:192/:504 | NOP↔call ftrace_caller 的切换 |
| **optprobe** | arch/x86/kernel/kprobes/opt.c | 5 字节 jmp 到优化探针 |
| **static_key** | arch/x86/kernel/jump_label.c | 5 字节 NOP↔jmp 的翻转 |
| **static_call** | arch/x86/kernel/static_call.c | call 目标的运行时改写 |
| **livepatch** | 间接经 ftrace 的 IPMODIFY 路径 | 函数入口的重定向 |
| **alternatives** | arch/x86/kernel/alternative.c 自身 | boot 期的 SMP lock 替换等 |
| **BPF trampoline** | 经 ftrace 的 register_ftrace_direct | trampoline 入口的安装 |

★所有这些子系统**不各自实现文本补丁**——共享一个 `text_poke_bp` 原语 + `text_mutex` 全局锁 + `bp_desc` 单例（描述当前正在打的所有补丁的 INT3 handler 状态）。

## 六、与 stop_machine 的对比

| 维度 | stop_machine | text_poke_bp |
| :--- | :--- | :--- |
| 停顿 | 全核调度停止（ms 级） | 零调度停摆（INT3 陷阱 + sync_core） |
| 延迟 | IPI 广播 + 等待最慢 CPU | on_each_cpu(sync_core)——每个 CPU 一次自 IPI |
| 安全性 | 绝对安全（无执行） | INT3 保证：任何撕裂读都走陷阱 |
| 批量 | 一次也行但贵 | 天然批量（bp_batch 的 nr_entries） |
| 使用场景 | 大块改写/数据结构修改 | **指令级**补丁（1~15 字节） |

## 七、观测实战

```bash
# 1) text_poke 的事件（perf 可观测）
perf record -e text_poke -a sleep 5
#   Intel PT 的解码依赖此事件同步代码流

# 2) bp_desc 的并发（实验环境）
bpftrace -e 'kprobe:poke_int3_handler { @int3++; }'

# 3) 批量大小（ftrace 全开时的效率验证）
cd /sys/kernel/debug/tracing
echo function > current_tracer
#   → ftrace_modify_all_code 的批量 text_poke

# 4) 代码段权限（W^X 的 runtime 例外）
cat /proc/iomem | grep -i "kernel text"
#   text_poke 临时映射一个可写别名页
```

## 八、思考题

1. INT3 写入是原子的（单字节）——但第二步写"尾部字节"不是原子的，为什么不危险？（提示：此时首字节已变为 INT3——任何 CPU 尝试执行这条指令都走陷阱，不会执行"旧首字节+新尾部"的撕裂组合。）
2. `poke_int3_handler` 在 INT3 陷阱里做仿真——仿真的正确性要求是什么？（提示：emulate 回调必须**语义等价**于旧指令——副作用/标志位/RIP 变化都要一致。）
3. `bp_desc.refs` 的 RMB/WMB 协议——为什么不用 atomic_inc？（提示：handler 运行在 INT3 陷阱——不能睡眠不能持锁；try_get_desc 的 atomic_read 非零判断是唯一的进入条件。）
4. `text_poke_sync()` 用 `on_each_cpu(do_sync_core, NULL, 1)`——`do_sync_core` 做什么？（提示：sync_core——让 CPU 清空指令预取队列/流水线中可能缓存的旧指令字节。）
5. 第三步"首字节替换"后如果 CPU 还在执行 INT3 handler——它会看到什么？（提示：INT3 handler 已经在仿真模式——退出后 RIP 已被改写为跳过补丁点——不再读那个位置。）
6. 为什么不能把三步合成两步（先 INT3 再一次性写全）？（提示：写全包括首字节——写首字节本身可能撕裂（跨 cacheline 边界），而 INT3 恰恰是为了保证首字节写入的原子性。）

---

> 回看：[18-ftrace 汇编层](18-ftrace汇编层DIRECT分派.md)（ftrace 对 text_poke_bp 的调用）｜[05-tracer 家族](05-tracer家族.md)（ftrace_make_call/nop 的使用者）｜[02-tracepoint 机制](02-tracepoint机制.md)（static_key 的翻转即 text_poke_bp 的另一消费者）｜[locking/debug-adversarial](../../locking/debug-adversarial.md)（上刑家族——text_poke_bp 是它们的共享地基）
