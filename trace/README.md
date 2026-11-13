# 内核观测（tracing）分册导读

本分册基于 **Linux 6.6** 源码，系统解剖内核观测子系统：从 printk 到 eBPF 的技术族谱、ftrace 框架的四大件（tracefs / tracer / 事件 / ring buffer）、静态插桩（tracepoint）与动态插桩（kprobe/uprobe）的完整实现、tracer 家族的每个成员，以及 perf 与 BPF 两大上层生态。所有结构体、宏展开、调用链均与 v6.6 源码逐行核对（kernel/trace/、include/linux/tracepoint.h、include/trace/trace_events.h、kernel/kprobes.c、arch/x86/kernel/kprobes/ 等）。

---

## 分册结构

| 章节 | 内容 | 关键源码 |
| :--- | :--- | :--- |
| [01-内核观测体系总览](docs/01-内核观测体系总览.md) | 八类观测技术定位与开销、ftrace 分层架构（trace_array/tracer/trace_event_call）、事件全链路、events/ 目录 | trace.c / trace.h / trace_events.c |
| [02-tracepoint 机制](docs/02-tracepoint机制.md) | TRACE_EVENT 七阶段展开、static_key/static_call/text_poke 联动、注册的 RCU+SRCU 协议、filter 谓词 VM、trigger、BPF 挂载 | tracepoint.h / tracepoint.c / trace_events.h / trace_events_filter.c |
| [03-ring buffer 设计](docs/03-ring-buffer设计.md) | per-CPU 四指针模型、无锁留位与嵌套保护、时间戳压缩协议、overwrite vs 消费者模式、读取协议、对比 eBPF ringbuf | ring_buffer.c |
| [04-kprobes 与 uprobes](docs/04-kprobes与uprobes.md) | int3 全路径与指令仿真、optprobe、聚合探针、kprobe_events 语法、kretprobe/rethook、kprobe-on-ftrace、uprobe XOL、开销对比 | kprobes.c / x86 kprobes 三件套 / trace_kprobe.c / trace_uprobe.c |
| [05-tracer 家族](docs/05-tracer家族.md) | function（dyn ftrace 补丁）、function_graph（返回栈）、irqsoff/preemptoff、wakeup 三兄弟、hwlat、osnoise/timerlat、stack tracer、延迟定位实战 | trace_functions*.c / fgraph.c / trace_irqsoff.c / trace_sched_wakeup.c / trace_hwlat.c / trace_osnoise.c / trace_stack.c |
| [06-perf 与 BPF 观测](docs/06-perf与BPF观测.md) | perf_event/pmu 内幕、PEBS、采样管线、tracepoint 并入 perf、BPF 三挂载、map 生态、bpftrace 一行流、生产选型 | events/core.c / perf.h / bpf_trace.c |
| [07-procfs 观测索引](docs/07-procfs观测索引.md) | /proc 文件→源全表（proc_create 逐一对源）、per-PID 族、★/proc/sched_debug 移除勘误（5.15 起）、与 sysctl/debugfs 的边界 | fs/proc/* + 各子系统注册点 |
| [08-sysctl 调参索引](docs/08-sysctl调参索引.md) | /proc/sys → 注册表全表（kernel.* 12 项+vm.* 7 项逐一对源）、register_sysctl 去中心化机制、★sched_base_slice_ns/migration_cost_ns 幻影勘误 | 各子系统 *_sysctls 表 |
| [09-debugfs 全景索引](docs/09-debugfs全景索引.md) | /sys/kernel/debug → 创建点全表（sched/zswap/shrinker/extfrag/bdi/wakeup_sources/kvm/clk 逐一对源）、★tracing 实为 tracefs 独立文件系统、workqueue 无 debugfs 勘误、四文件系统象限 | 各子系统 debugfs_create_* |
| [10-eBPF 验证器入门](docs/10-eBPF验证器入门.md) | bpf_check 两遍验证（DAG 检查+全路径下降）、branches 状态树回溯协议、is_state_visited 剪枝、复杂度三层上限（1M 步/8192 跳转/64 状态）、循环真实政策（loop_ok && bpf_capable 放行有界循环）、★全部报错消息逐字对源、PTR_MAYBE_NULL 类型标志位（5.17 重构，v5.16/17 双标签实证）、程序长度特权分级（syscall.c:2586） | verifier.c / bpf_verifier.h / include/linux/bpf.h |
| [11-tnum 三值数](docs/11-tnum三值数.md) | 数学底座：三态位知识代数（value/mask 不变式）、tnum_add 的进位携带 chi、tnum_range 过逼近诚实注释（含幻影 3 示例）、AND/OR/XOR 三公式精度阶梯、★tnum_mul 注释引 arXiv 2105.05398、var_off→smin/smax 的符号位技巧派生、★DIV/MOD 落 default 分支 mark_reg_unknown（一次除法清零全部位知识） | tnum.h / kernel/bpf/tnum.c / verifier.c |
| [12-profile 与 latencytop](docs/12-profile与latencytop.md) | 两件活化石：profile 的四模式与 cmdline/双哈希表翻转防 timer 中断活锁（2004 摊销计数原作）/proc/profile 格式与 readprofile（util-linux）；latencytop 的"内核替你睡"设计注释（含格式图）/回溯为 key 的定容账本/inter>5000μs 自愿睡过滤/★三套账目一个入口（与 schedstat/delayacct 的三向纠缠表）/force_schedstat_enabled 三调用方闭环 | profile.c / latencytop.c / sched/stats.c |
| [13-内核代码生成惯用法](docs/13-内核代码生成惯用法.md) | 横切地图：一份清单多种产物的六形态——极简式（kernel_read_file_id 一宏三表）/隔空共定义（LSM_HOOK 251 条三家消费）/多重包含本家（TRACE_EVENT 七阶段 + TRACE_HEADER_MULTI_READ）/BPF 双例（uapi MAPPER 的中间层重排·btf.c 三重包含教科书）/BTF_ID 远亲/对照组（syscall 表为何用脚本）+ 签名法找新成员 | kernel_read_file.h / lsm_hook_defs.h / define_trace.h / bpf.h / bpf_types.h |
| [14-BPF trampoline](docs/14-BPFtrampoline.md) | 函数边界插桩：按目标现场生成机器码的三层模型（trampoline 账本/tramp_image 的 percpu_ref 生命周期与 ip_after_call 探测点/38 链×~50B 预算）、x86 生成器流程、BPF_TRAMP_F_* 七标志组装语义、★BPF LSM 闭环（lsm_hook_defs 的 noinline nop 锚点——逻辑位置与物理插桩分离） | trampoline.c / bpf.h / bpf_jit_comp.c / hooks.c / bpf_lsm.c |
| [15-perf 上下文切换](docs/15-perf上下文切换.md) | 扇出最重一支：sched_out/in 的三件事、ctx 账本（pinned/flexible 两组分野）、★克隆快速路径的上下文整体交换（零 PMU 编程）、sched_in 的四级优先级协议（"cpu pinned, task pinned, cpu flexible, task flexible" 注释原话）、perf_sched_cb_usages 的 PMU 侧门、无事件≈1-2ns 成本画像 | kernel/events/core.c / include/linux/perf_event.h |
| [16-bpf_struct_ops](docs/16-bpf_struct_ops.md) | 反向用法：BPF 程序成为内核 ops 的函数指针——BTF 驱动注册（types.h 的 X-macro 清单）、struct_ops map 的双视图（uvalue=kprog_id/kvalue=trampoline 地址）、★update 全流程（逐成员 prepare_trampoline + orig_call=NULL + set_memory_rox W^X）、tcp_congestion_ops 唯一实战端、BPF_TRAMP_F_RET_FENTRY_RET 独占用户 | bpf_struct_ops.c / bpf.h / bpf_tcp_ca.c |
| [17-ftrace direct 共享地基](docs/17-ftrace_direct共享地基.md) | 桥接章：DIRECT 通道（register_ftrace_direct/call_direct_funcs 占位/direct_functions 哈希）与 livepatch 的 IPMODIFY（klp_ftrace_handler 的 func_stack+transition）的★协调协议（prepare_direct_functions_for_ipmodify 的通知-协商-重试三步 + bpf_tramp_ftrace_ops_func 的 -EAGAIN 信号与锁序）；DIRECT vs IPMODIFY 本质区别对照表（零间接 vs IP 改写） | ftrace.c / ftrace.h / livepatch/patch.c / bpf/trampoline.c |
| [18-ftrace 汇编层 DIRECT 分派](docs/18-ftrace汇编层DIRECT分派.md) | 汇编收官：ftrace_caller/regs_caller 的栈布局与 ftrace_call 标签 text_poke/★create_trampoline 模板拷贝法（"think twice" 注释）/★DIRECT 的 orig_ax 征用（零 text_poke 函数替换）/RSB 重平衡（Spectre-RSB 防护）/三级调用路径成本对比 | ftrace_64.S / arch/x86/kernel/ftrace.c / asm/ftrace.h |
| [19-text_poke_bp 家族](docs/19-text_poke_bp家族.md) | 安全原语：三步打补丁协议（INT3→尾部→首字节 + 每步 sync_core）、poke_int3_handler 的仿真执行、★完全避免 stop_machine（"We completely avoid stop_machine() here" 注释原话）、批量模式 text_poke_finish、全树消费方清点（ftrace/optprobe/static_key/static_call/livepatch/BPF trampoline 共享一个原语） | alternative.c :2155-2330 |
| [20-static_call](docs/20-static_call.md) | 运行时改目标的直接调用：双层机制（trampoline 模式一处补丁 vs inline 模式 objtool 扫 .static_call_sites 逐 site 补丁）、key/site/tramp 三角、五种指令变换（CALL/NOP/JMP/RET/JCC）、★xor5rax 技巧（5 字节清寄存器替代 call+ret）、tracepoint 的 __DO_TRACE_CALL 消费端 | static_call.h / arch/x86/kernel/static_call.c |
| [21-sync_core](docs/21-sync_core.md) | 流水线清空原语：三种实现取舍（SERIALIZE/IRET-to-self "works on every CPU, at any CPL...but it's a bit slow...unmasks NMIs"/CPUID "nasty...exits to a hypervisor" 注释原话）、IRET-to-self 的手工栈帧构造、★PTI 联动（CR3 切换本身序列化——sync_core_before_usermode 变空）、在 text_poke_bp 三步协议中的 on_each_cpu 全核 IPI 角色、消费者清单（text_poke/membarrier/stop_machine/jump_label） | asm/sync_core.h 111 行 |

## 学习路线

```
第 1 章（地图：谁在什么开销下解决什么问题）
   │
   ├─► 第 2 章（静态插桩：事件的"产生端"）──┐
   │                                        ├──► 第 4 章（动态插桩：补齐静态点覆盖不到的）
   ├─► 第 3 章（buffer：事件的"落地端"）────┘
   │
第 5 章（tracer 家族：框架之上的开箱即用诊断工具）
   │        └─ 依赖 2/3 章的 probe + buffer 原语
   ▼
第 6 章（perf/BPF：把观测从"记录"推进到"聚合/剖析"）
          └─ 复用 2 章的挂载点、3 章的 buffer 思想
```

* **先读 1/2/3 打地基**：无论用哪个工具，事件都是 tracepoint（或其变体）产生、ring buffer 落地；
* **4 章独立可读**：动态插桩自成体系，但引用 2 章的"合成事件"概念；
* **5 章是工具手册型**：每个 tracer 可独立查阅，实战一节建议动手；
* **6 章面向生产**：选型表与开销预算是本分册的落点。

## 三条主线贯穿全分册

1. **零开销关闭**：static_key（第 2 章）让未启用的 tracepoint 与 function tracer 成本趋近于零——这是整个观测生态能默认常驻内核的前提；
2. **per-CPU + 读时归并**：ring buffer（第 3 章）用"写侧无锁、读侧排序"换热路径安全，同一思想出现在 BPF ringbuf 与 perf mmap ring；
3. **一份埋点多种消费者**：tracepoint 的 funcs 数组（第 2 章）同时服务 ftrace 事件、perf、BPF（第 6 章）；kprobe 的聚合探针（第 4 章）同理——插桩与消费永远解耦。

## 与其他分册的联系

* **scheduler 分册**：第 5 章 wakeup tracer 与 wakeup_rt/dl 依赖调度器的 prio/rt_task/dl_task 语义；sched:* tracepoint 是调度分册各章的观测出口；
* **interrupt 分册**：irqsoff/osnoise 挂钩的 `irq:irq_handler_entry/exit` tracepoint 正是中断分册的 flow handler 路径埋点；
* **locking 分册**：lockdep 是观测族谱里的"检测器"分支（本分册只做定位与开销量级对比，机制细节见 locking 分册）；
* **syscalls/networking 分册**：第 10 章 verifier 的类型系统与 PTR_MAYBE_NULL 判空协议，是 seccomp cBPF 校验（syscalls/07）与 XDP 数据面（networking/07）背后共同的"证明安全"思想——eBPF 侧的完整机制在本章。

## 建议的动手环境

```bash
# 内核配置（观测全家桶）
CONFIG_FTRACE=y  CONFIG_FUNCTION_TRACER=y  CONFIG_FUNCTION_GRAPH_TRACER=y
CONFIG_IRQSOFF_TRACER=y  CONFIG_PREEMPT_TRACER=y  CONFIG_SCHED_TRACER=y
CONFIG_HWLAT_TRACER=y  CONFIG_OSNOISE_TRACER=y  CONFIG_TIMERLAT_TRACER=y
CONFIG_KPROBES=y  CONFIG_KPROBES_ON_FTRACE=y  CONFIG_OPTPROBES=y  CONFIG_UPROBES=y
CONFIG_HIST_TRIGGERS=y  CONFIG_BPF_SYSCALL=y  CONFIG_DEBUG_INFO_BTF=y
# 挂载
mount -t tracefs nodev /sys/kernel/tracing
```

配套工具链：`trace-cmd`（录制/解析 trace_pipe_raw）、`perf-tools`（Brendan Gregg 的 perf 前端脚本）、`bpftrace`（一行流）、`rtla`（6.6 内核源码树自带 tools/tracing/rtla）。

## 本目录 .rst 镜像导读

本目录镜像了内核 `Documentation/trace/` 的官方文档（.rst，内核作者视角的说明书），与本分册的 docs/ 源码解剖互补：

| 镜像文件 | 主题 | 配套章节 |
| :--- | :--- | :--- |
| ftrace.rst | ftrace 总入口：tracefs 文件、tracer 用法、instances | 01 / 05 |
| ftrace-design.rst | dyn ftrace 的设计与历史（mcount/fentry 补丁） | 05 章 function 节 |
| ftrace-uses.rst | ftrace 实战（延迟排查、过滤技巧） | 05 章实战 |
| tracepoints.rst / tracepoint-analysis.rst | tracepoint 原理与分析案例 | 02 |
| events.rst | events/ 目录、filter、trigger 语义 | 01 / 02 |
| kprobes.rst / kprobetrace.rst | kprobe 机制与 kprobe_events 语法 | 04 |
| uprobetrace.rst | uprobe_events 语法 | 04 |
| fprobe.rst / fprobetrace.rst | fprobe（ftrace 之上的轻量 probe） | 04 延伸 |
| ring-buffer-design.rst | ring buffer 设计文档 | 03 |
| histogram.rst / histogram-design.rst | hist trigger 语法与实现 | 02 trigger 节 |
| hwlat_detector.rst | 硬件延迟检测 | 05 hwlat 节 |
| osnoise-tracer.rst / timerlat-tracer.rst | 噪声分解 / 定时器延迟 tracer | 05 |
| mmiotrace.rst | MMIO 访问跟踪 | 外延 |
| boottime-trace.rst | 启动期 trace 配置（initcall 调优） | 外延 |
| user_events.rst | 用户态进程自注册事件 | 外延 |
| stm.rst / sys-t.rst | System Trace Module 通路 | 硬件外延 |
| intel_th.rst | Intel TH 硬件 trace | 硬件外延 |
| coresight/ | ARM CoreSight 硬件 trace | 硬件外延 |
| rv/ | Runtime Verification（运行时验证框架） | 形式化外延 |
| postprocess/ | trace 后处理脚本（含 function-graph-fold.vim） | 05 实战 |

> 阅读建议：rst 是"怎么用"，本分册是"为什么这样实现"。先用 rst 上手，遇到"为什么"回到分册对应章节找调用链与结构体。

## 章节速查（常见问题 → 落点）

| 我想知道/排查…… | 去哪 |
| :--- | :--- |
| 开某个观测工具要付多少性能税 | 01 章族谱表 / 06 章预算节 |
| events/ 目录每个文件是干嘛的 | 01 章第四节 |
| 事件时间戳跨 CPU 乱序 | 01 章 trace_clock / 03 章时间戳协议 |
| filter/trigger/hist 的实现原理 | 02 章第六、七节 |
| 事件丢了（dropped/overrun）为什么 | 03 章两种模式 / stats 解读 |
| kprobe 探不到某函数 | 04 章黑名单一节 |
| kprobe 慢、想上 optprobe | 04 章第五节 + `/sys/kernel/debug/kprobes/list` 状态 |
| 函数耗时、调用树 | 05 章 function_graph |
| 偶发高延迟的归因 | 05 章实战三步法 + rtla |
| CPU 在干什么（profiling） | 06 章 perf 采样管线 |
| 生产常驻的低开销指标 | 06 章 BPF 聚合与选型表 |
| eBPF 程序被 verifier 拒了怎么读日志 | 10 章第五节（真实报错消息对照） |
| eBPF 程序最多多少条指令/多少个 map/能不能写循环 | 10 章第四节（上限表）+ 第七节（循环政策） |
| verifier 日志里 var_off=(0x0; 0xffff...) 怎么读 | 11 章第七节（tnum_strn 格式与知识演化录像） |
| 为什么一次 DIV 之后 verifier 变"笨"了 | 11 章第五节分派表（default 分支 mark_reg_unknown） |

## 全局约定

* 未加说明的代码引用均指 **Linux 6.6**（对照仓库：linux-stable.git v6.6 标签）；
* `tracefs` 挂载点统一写 `/sys/kernel/tracing`（老内核的 debugfs 路径 `/sys/kernel/debug/tracing` 等价）；
* 开销量级均为数量级估算，实测方法见 04/06 章的思考题。
