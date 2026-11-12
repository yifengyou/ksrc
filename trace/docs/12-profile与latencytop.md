# profile 与 latencytop：两件"活化石"观测器（Linux 6.6）

> 阅读目标：认识调度器旁系的两件遗留观测基础设施——**profile**（kernel/profile.c：PC 采样的直映射计数缓冲 + 为解决 timer 中断活锁而生的**双哈希表翻转**设计，2004 年 Nadia Yvette Chambers 的代表作）与 **latencytop**（kernel/latencytop.c：Arjan van de Ven 2008——"内核替你睡"的延迟归因，以**字符串化回溯**为 key 的定容累积账本）。理解二者与 schedstat/delayacct 的三向纠缠（`force_schedstat_enabled` 的两个调用方、SLEEP_PROFILING 与 `account_scheduler_latency` 在 `__update_stats_enqueue_sleeper` 里的并肩），以及 perf/BPF 时代它们仍被保留的理由。全部对照 v6.6（kernel/profile.c、kernel/latencytop.c、kernel/sched/stats.c、kernel/sched/core.c、fs/proc/base.c）。前置：[14 章 schedstat](../../scheduler/docs/14-调度器统计纵深schedstat.md)（姊妹篇——三套账目的另外两套）。

---

## 一、定位：perf 之前的两个答案

```
问题 A："CPU 时间花在内核哪段代码上？" → profile（PC 采样）
问题 B："应用感知的延迟，内核替它睡在哪？" → latencytop（睡眠原因归因）
两件都诞生于 perf/BPF 之前（2004/2008），如今功能已被
  perf record -g / BPF stacktrace 覆盖——但仍在树中：
  无依赖、极简、开机即可用（嵌入式与 kdump 时代工具链的底线）
```

## 二、profile：PC 采样的直映射计数（kernel/profile.c，501 行）

### 2.1 四种模式与 cmdline（profile.h:12-15 + profile.c:58-101）

```c
#define CPU_PROFILING    1    /* profile=2,4,8...（shift 值）*/
#define SCHED_PROFILING  2    /* profile=schedule */
#define SLEEP_PROFILING  3    /* profile=sleep（需 SCHEDSTATS，否则警告
                                 "kernel sleep profiling requires
                                  CONFIG_SCHEDSTATS"）*/
#define KVM_PROFILING    4    /* profile=kvm */
```

`profile_setup`（:57）：命名模式后可跟 `,shift` 调精度；数字模式打点 "kernel profiling enabled (shift: %u)"。`profile_init`（:100）的**只测文本段**注释（:106 原话）："only text is profiled"——`prof_len = (_etext - _stext) >> prof_shift`。

### 2.2 采样入口：tick 的三个落点

```
kernel/time/tick-common.c:101 / tick-legacy.c:36 / tick-sched.c:255
  → profile_tick(CPU_PROFILING)（profile.c:333）
      if (!user_mode(regs) &&
          cpumask_test_cpu(smp_processor_id(), prof_cpu_mask))
          profile_hit(type, (void *)profile_pc(regs));
★内核态 PC 才记；prof_cpu_mask 经 /proc/irq/prof_cpu_mask（:387，
  0600）限定 CPU 集
```

### 2.3 核心设计：双哈希表翻转防活锁（:141-160 注释块，2004 原作）

问题：timer 中断里对**全局共享的 prof_buffer** 做 atomic_add——缓存行弹跳会把 timer 中断自己拖成活锁。解法（注释原文逐句拆解）：

```
每 CPU 一对开放寻址哈希表（struct profile_hit {u32 pc, hits}，
  一页大小 NR_PROFILE_HIT 项）：
  ① 中断侧：pending 命中先入当前哈希表（per-CPU 无争用）；
     溢出时才对 prof_buffer 做 atomic_add 批量清账
     （"Amortized hit count accounting"——注释原话："this amortizes
       a number of atomic profile buffer increments likely to be far
       larger than the number of entries in the hashtable"）
  ② 读侧：read_profile（:397）先 profile_flip_buffers()——IPI 所有
     CPU 翻转 active/inactive 两表（cpu_profile_flip），再冲刷旧表
  ③ 第二张表的存在理由（注释原话）："The sole use of having a second
     hashtable is for avoiding cacheline contention that would
     otherwise happen during flushes...and so resurrect the interrupt
     livelock issue"——冲刷期间中断侧继续写另一张表
```

### 2.4 /proc/profile（proc_create :485，S_IWUSR|S_IRUGO，subsys_initcall :500）

```
读格式：前 4 字节 = sample_step（1<<prof_shift），随后 prof_len 个
  atomic_t 计数（read_profile :397-422 逐段）
写语义（write_profile :436；:431 注释原话 "Writing to /proc/profile
  resets the counters"）：SMP 下写 4 字节 int 还可设采样倍频
  （setup_profiling_timer——多数架构返回 -EINVAL）
解析工具：util-linux 的 readprofile（内核树外！本仓 tools/ 无此工具）
```

## 三、latencytop："内核替你睡"的归因账本（kernel/latencytop.c，300 行）

### 3.1 设计文档（:5-39 头注释，含格式图）

关键句（:8-11 原话）：*"The latency that is tracked is **not** the 'traditional' interrupt latency (which is primarily caused by something else consuming CPU), but instead, it is the latency an application encounters **because the kernel sleeps on its behalf** for various reasons."*

```
两级账：系统级 latency_record[MAXLR=128] + 每任务 tsk->latency_record
key = 字符串化回溯（"A latency cause is identified by a stringified
     backtrace at the point that the scheduler gets invoked"）
值 = {count, time 累计, max 峰值}（定容——满了不再记新因，
     "until the buffer is flushed by writing to the /proc file;
      the userspace tool does this on a regular basis"）
```

### 3.2 采集路径（与 delayacct/schedstat 的三向纠缠）

```
__update_stats_enqueue_sleeper（kernel/sched/stats.c——14 章 §四）：
  sleep 分支 :68   account_scheduler_latency(p, delta >> 10, 1)  ← inter=1
  block 分支 :105  account_scheduler_latency(p, delta >> 10, 0)  ← inter=0
  同函数内 :101    profile_hits(SLEEP_PROFILING, get_wchan(p),
                              delta >> 20)  ← ms 精度的"睡在哪个 wchan"
__schedule 尾部（core.c:5959）：
  profile_hit(SCHED_PROFILING, __builtin_return_address(0))
                                ← 记的是 schedule() 的调用者地址！

★delayacct（15 章）喂 sleep/block 的 delta；schedstat 的状态机
  是唯一采集点；本二者的差异仅在"delta 拿去做什么"——三套账本
  共用一个入口的完美案例
```

`account_scheduler_latency`（latencytop.h:37-41）：`if (unlikely(latencytop_enabled))` 的单点门（对照 delayacct 的 static key 双轨——这里是简单全局 int）。

### 3.3 过滤与记录（__account_scheduler_latency :177）

```
/* 注释原话（:183）："Long interruptible waits are generally
   user requested..." */
if (inter && usecs > 5000)  return;    ← 可中断睡眠 >5ms 视为自愿（poll/select）
if (usecs <= 0)             return;    ← "Zero-time sleeps are non-interesting"

stack_trace_save_tsk(tsk, lat.backtrace, LT_BACKTRACEDEPTH=12, 0);
account_global_scheduler_latency(tsk, &lat);     ← 系统级（MAXLR 128）
逐项比对任务自己的记录：backtrace 全同 → count++/time+=/max 更新；
  新因 → 追加（任务侧定容 LT_SAVECOUNT=32）
```

### 3.4 接口与开关

```
/proc/latency_stats（0644，device_initcall）：
  "Latency Top version : v0.1" + "%i %lu %lu %ps..." 行
  （count/累计μs/峰值μs/回溯——头注释 :29-38 有逐列图解）
  写 = clear_global_latency_tracing（刷新语义）
/proc/<pid>/latency（fs/proc/base.c:3301/:3650，S_IRUGO）：
  同格式，读单任务账（lstats_show_proc :492）
sysctl kernel/latencytop（:68-88）：开启时同步
  force_schedstat_enabled()——★三个"强制开 schedstat"的调用方之二
  （另一个是 profile=sleep 的 profile_setup :67；第三个是
   sysctl sched_schedstats 本尊）
```

## 四、三套账目一个入口（回扣 14/15 章）

| | schedstat（14 章） | delayacct（15 章） | latencytop + profile |
| :--- | :--- | :--- | :--- |
| 入口 | `__update_stats_*` 状态机 | **同一个状态机**（同函数） | **同一个状态机** + `__schedule` 尾 |
| 记什么 | 等待/睡眠分桶计数 | 七族延迟（ns 累计） | 回溯为 key 的归因 + PC/wchan 采样 |
| 开关 | static key `sched_schedstats` | static key `delayacct_key` | int `latencytop_enabled` / `prof_on` |
| 强开链 | 本尊 | —（无人强开 delayacct） | 两者都会 `force_schedstat_enabled`（★delayacct 反而不需要——它的账不依赖 schedstat 的计数器） |
| 导出 | /proc/schedstat 等 | taskstats netlink | /proc/latency_stats、/proc/profile |

## 五、观测实战

```bash
# profile（需 cmdline 预开：profile=2 起的 2^n 为桶宽字节）
#   重启后：
readprofile -m /boot/System.map -n 20       # util-linux 工具，Top 20 热点
readprofile -r                               # 顺带清零（写 /proc/profile）
cat /proc/irq/prof_cpu_mask                  # 采样 CPU 限定

# latencytop（运行时可开）
echo 1 > /proc/sys/kernel/latencytop         # 同步强开 schedstat
latencytop                                   # 用户态 TUI（发行版包）
cat /proc/latency_stats | head -6            # 无 TUI 时直接读
echo 5 > /proc/123/latency                   # ★错误示范：该文件只读
cat /proc/123/latency                        # 单任务账（S_IRUGO）

# 与现代工具的对拍
perf record -g -e sched:sched_stat_sleep -a sleep 5   # 同一入口的 tracepoint 侧
```

## 六、思考题

1. 双哈希表翻转用 `cpu_profile_flip`（int 取反）而非 per-CPU 锁选择 active 表——为什么读侧冲刷**必须** IPI（on_each_cpu）而不能自读翻转位？（提示：冲刷与中断写入的原子交接窗口。）
2. `SCHED_PROFILING` 记 `__builtin_return_address(0)`、`SLEEP_PROFILING` 记 `get_wchan(p)`——两个"在哪"分别回答什么问题？（提示：谁在调 schedule vs 任务挂在哪个等待点。）
3. latencytop 的 `inter && usecs > 5000` 过滤把"用户自愿的长睡"排除——举一个会被误伤的真实场景（poll 超时之外的）。（提示：NFS hard mount 的 interruptible 长睡。）
4. 定容账本（128 系统级/32 任务级）满了就丢新因——为什么不做哈希扩容？（对照 2008 年的设计语境：内存、栈回溯成本、以及"热点因必然早出现"的假设。）
5. `sysctl kernel/latencytop=1` 会强开 schedstat，但 `kernel.task_delayacct=1` 不强开任何东西——从"谁依赖谁的计数器"解释这条不对称。（对照 15 章：delayacct 的 delta 来自自己的打点，latencytop 复用 schedstat 的 sleep/block 分桶。）
6. 若要在 6.6 上重写 readprofile：/proc/profile 头 4 字节的 `sample_step` 为什么必须随缓冲一起导出？（提示：地址→桶号的反解公式 bucket = (pc - _stext) >> prof_shift。）

---

> 回看：[01-内核观测体系总览](01-内核观测体系总览.md)（八类观测技术中的"采样器"支系）｜[scheduler/14-schedstat](../../scheduler/docs/14-调度器统计纵深schedstat.md)（三套账目的中枢）｜[scheduler/15-delayacct](../../scheduler/docs/15-任务延迟统计delayacct.md)（delta 的上游）｜[05-tracer 家族](05-tracer家族.md)（irqsoff/preemptoff 记的是"另一类延迟"——CPU 被占）｜[power 分册](../../power/README.md)（tick 三落点的 NOHZ 语境）
