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
* **locking 分册**：lockdep 是观测族谱里的"检测器"分支（本分册只做定位与开销量级对比，机制细节见 locking 分册）。

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

## 全局约定

* 未加说明的代码引用均指 **Linux 6.6**（对照仓库：linux-stable.git v6.6 标签）；
* `tracefs` 挂载点统一写 `/sys/kernel/tracing`（老内核的 debugfs 路径 `/sys/kernel/debug/tracing` 等价）；
* 开销量级均为数量级估算，实测方法见 04/06 章的思考题。
