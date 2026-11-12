# 任务延迟统计 delayacct（Linux 6.6）

> 阅读目标：吃透 delayacct 的**七族延迟记账**（blkio/swapin/freepages/thrashing/compact/wpcopy/irq）——从 `struct task_delay_info` 的 start/delay/count 三元组与共享收账器 `delayacct_end` 的协议，到 blkio 族"睡眠起表、唤醒收账"的调度器闭环（`__schedule` ↔ ttwu 双收账点）；理解导出通道 taskstats netlink（genl 族、per-CPU 监听者、退出广播）；★并做一次**幻影接口勘误**：`/proc/<pid>/delay` 在主线**从未存在过**（v2.6.18 引入 delayacct 至今唯一出口都是 taskstats）。全部对照 v6.6（kernel/delayacct.c、include/linux/delayacct.h、include/uapi/linux/taskstats.h、kernel/taskstats.c、kernel/sched/core.c、mm/）。前置：[14-调度器统计纵深 schedstat](14-调度器统计纵深schedstat.md)（姊妹账目）。

---

## 一、先勘误：/proc/<pid>/delay 是一个"从未存在"的接口

本仓库第 12 处幻影类勘误（家族：`/proc/sched_debug` 5.15 移除、`io.latency.percentile` 非独立文件、两个 sched sysctl 幻影）——但本次更彻底：**双标签 + git 历史搜索实证，`/proc/<pid>/delay` 从未进入主线**：

```
v2.6.18（delayacct 引入内核）~ v6.6 全谱系检查：
  fs/proc/base.c 中 delay 相关仅一处——
  task->sched_info.run_delay（那是 /proc/<pid>/schedstat 的三元组）
  git log -S 'proc_delay' -- fs/proc/ 历史零命中
官方文档（Documentation/accounting/delay-accounting.rst）：
  "Delay accounting uses the taskstats interface"——通篇只有 netlink 出口
```

第三方资料流传的 `/proc/<pid>/delay` 大概率源自 delayacct 早期 RFC 的 proc 接口提案（未入主线）与 `/proc/<pid>/schedstat` 的混淆。**正确出口唯一：taskstats netlink**（第七节）。

## 二、七族计数与记账协议（include/linux/delayacct.h:13-58）

```c
struct task_delay_info {          /* 挂 task_struct->delays，按需分配 */
	raw_spinlock_t lock;          /* 全族共用一把锁（头注释：单锁保原子性，
	                                  竞争激烈再考虑拆 XXX_lock） */
	/* 每族三件套：XXX_start（打点）/XXX_delay（累计 ns）/XXX_count */
	u64 blkio_start, blkio_delay;   u32 blkio_count;   /* 同步块 IO 等待 */
	u64 swapin_start, swapin_delay; u32 swapin_count;  /* 换入等待 */
	u64 freepages_start, freepages_delay; u32 freepages_count; /* 直接回收 */
	u64 thrashing_start, thrashing_delay; u32 thrashing_count; /* 抖动页 */
	u64 compact_start, compact_delay; u32 compact_count;       /* 内存规整 */
	u64 wpcopy_start, wpcopy_delay; u32 wpcopy_count;          /* 写保护拷贝 */
	u64 irq_delay;                  u32 irq_count;     /* IRQ/SOFTIRQ——
	                                   ★无 start：delta 由调用方供给 */
};
```

**共享收账器**（kernel/delayacct.c:99-109）——七族中六族的统一终点：

```c
static void delayacct_end(raw_spinlock_t *lock, u64 *start,
			  u64 *total, u32 *count)
{
	s64 ns = local_clock() - *start;
	if (ns > 0) {                    /* ★负 delta（时钟回绕）直接丢弃 */
		raw_spin_lock_irqsave(lock, flags);
		*total += ns;
		(*count)++;
		raw_spin_unlock_irqrestore(lock, flags);
	}
}
```

协议不对称性：**start 打点无锁**（写者只有 `current` 自己，单写者无竞争），**end 累加持锁**（读者可能是收账者本人之外的 waker——见第四节）。头注释的设计模板（:16-24 原话）：*"For each stat XXX, add following... Atomicity of updates to XXX_delay, XXX_count protected by single lock above"*。

## 三、开关矩阵：static key + int 双轨（delayacct.c:21-85）

```
CONFIG_TASK_DELAY_ACCT（编译期）
   │
   ├─ cmdline "delayacct"          __setup（:37）
   ├─ sysctl kernel.task_delayacct （:47-82：写需 CAP_SYS_ADMIN，
   │                               SYSCTL_ZERO/ONE 边界，late_initcall
   │                               register_sysctl_init("kernel")）
   └─ delayacct_init(:39-44)：KMEM_CACHE(task_delay_info,
                      SLAB_PANIC|SLAB_ACCOUNT) + init_task 初始化
set_delayacct(:21-30) 同时翻两轨：
   static_branch_enable(&delayacct_key)   ← 六族钩子的热路径门
   delayacct_on = 1                       ← 任务创建路径的分配门
```

内联门面（delayacct.h:107-112 等十二个 inline）：先 `static_branch_unlikely(&delayacct_key)` 再 `current->delays` 判空——**先编译期常量、再 static key、再指针判空**三层递进。`delayacct_tsk_init`（:89-95）第一行 `tsk->delays = NULL`（注释原话 *"reinitialize in case parent's non-null pointer was dup'ed"*——fork 复制父结构后必须先清指针防共享）。

## 四、blkio 族闭环：调度器里唯一的跨上下文记账

```
io_schedule() → io_schedule_prepare(core.c:8990-8995)
                 current->in_iowait = 1
   │
   ▼
__schedule(core.c:6650-6653)：prev 睡去且 in_iowait
   atomic_inc(&rq->nr_iowait);
   delayacct_blkio_start();          ← 打点（无锁，current）
   │
   │  ……磁盘 IO 完成，任意 CPU 上的中断/其他进程发起唤醒……
   ▼
try_to_wake_up 的两个收账点：
   ① ttwu_do_activate(:3782-3785)：if (p->in_iowait) {
        delayacct_blkio_end(p);          ← 对 p 收账！
        atomic_dec(&task_rq(p)->nr_iowait); }
   ② 迁移分支(:4331-4336)：select_task_rq 换 cpu 时同样收账
```

★为什么 end 不用 `current`——delayacct.c:117-121 注释原话：

```
/* We cannot rely on the `current` macro, as we haven't yet
 * switched back to the process being woken. */
void __delayacct_blkio_end(struct task_struct *p)
```

**起表者与收账者是两个不同上下文**：start 在任务自己入睡的 `__schedule` 里，end 在唤醒者（可能是中断处理后的 waker）为 `p` 收账——这正是 `delayacct_end` 持锁的原因。`in_iowait` 是贯穿两个事件点的任务标记（io_schedule_prepare 置、唤醒路径查并随 ttwu_do_wakeup 清）。

## 五、mm 五族与 irq 族的钩子点（全部核对 v6.6）

| 族 | 钩子 | 语境 |
| :--- | :--- | :--- |
| swapin | mm/page_io.c:514 | `swap_readpage` 换入等待 |
| thrashing | mm/filemap.c:1235/:1388、mm/page_io.c:511 | 页缓存等待（`in_thrashing` 任务标记防嵌套重入——delayacct.c:220-233：外层 `*in_thrashing` 保存后内层直接跳过） |
| freepages | mm/vmscan.c:6831 | 直接回收节流（`throttle_direct_reclaim`） |
| compact | mm/page_alloc.c:3380 | 分配慢路径的同步规整 |
| wpcopy | mm/memory.c:3065、mm/hugetlb.c:5600 | **写时拷贝（COW）**：`wp_page_copy` 与 hugetlb 对应路径 |
| irq | kernel/sched/core.c:726 | `update_rq_clock_task` 内 `delayacct_irq(rq->curr, irq_delta)`——复用 `CONFIG_IRQ_TIME_ACCOUNTING` 已算好的 `irq_delta`（与 `psi_account_irqtime` 并肩同点），★无 start 打点、delta 直供 |

## 六、导出结构 taskstats（include/uapi/linux/taskstats.h，VERSION 14）

`delayacct_add_tsk`（delayacct.c:129-201）把账本装进 netlink 报文结构：

```c
struct taskstats {                  /* 逐字段注释已核对 */
	__u16 version;                   /* = TASKSTATS_VERSION 14 */
	__u32 ac_exitcode; __u8 ac_flag, ac_nice;  /* 基础记账族 */
	/* ---- Delay accounting fields（仅 delayacct 开启才有值）---- */
	__u64 cpu_count, cpu_delay_total;
	  /* 头注释原话："count, delay_total NOT updated atomically"
	     ← 来源是 sched_info（CONFIG_SCHED_INFO 账本，无锁快照） */
	__u64 blkio_count, blkio_delay_total;
	  /* "does not account for delays in I/O submission"——只记完成等待 */
	__u64 swapin_count, swapin_delay_total;
	  /* "Delay waiting for page fault I/O (swap in only)" */
	__u64 cpu_run_real_total;
	  /* "value will adjust for cpu time stolen from the kernel in
	     involuntary waits due to virtualization"——KVM steal 修正版 */
	__u64 cpu_run_virtual_total;    /* 无 steal 修正版 */
	/* ---- Basic Accounting ---- */  ac_comm/ac_sched/ac_utime/ac_stime/
	/* nvcsw/nivcsw（自愿/非自愿切换）… */
	__u64 freepages_count/delay_total;        thrashing… compact…（v11 起）
	wpcopy… irq…                               /* 渐进追加至 v14 */
};
```

两个协议细节：① **溢出契约**（delayacct.c:167 注释原话）：*"zero XXX_total, non-zero XXX_count implies XXX stat overflowed"*——每次累加都做 `tmp < old ? 0 : tmp` 的回绕归零；② **sched_info 快照**（:146-149 注释原话）：*"No locking available for sched_info (and too expensive to add one) Mitigate by taking snapshot of values"*——cpu 族的三个源值（pcount/run_delay/sum_exec_runtime）先快照再加。

★**数据融合点**：`cpu_count += tsk->sched_info.pcount; cpu_delay_total += tsk->sched_info.run_delay`——taskstats 的 cpu 族就是 schedstat 三元组的 netlink 化身（[14 章](14-调度器统计纵深schedstat.md)的 SCHED_INFO 账本在此汇流）。

## 七、netlink 管道：genl 族与 per-CPU 监听者（kernel/taskstats.c）

```
generic netlink family（TASKSTATS_GENL_NAME，__ro_after_init :685）
  ├─ TASKSTATS_CMD_GET   → taskstats_user_cmd（:558，doit 挂接 :671）
  │    属性：PID / TGID（NLA_U32）——拉取式查询
  │    属性：REGISTER_CPUMASK / DEREGISTER_CPUMASK（NLA_STRING）
  │          ——★推式注册：监听指定 CPU 上发生的任务退出
  └─ CGROUPSTATS_CMD_GET → cgroupstats_user_cmd（:407）
       （cgroup 的 sleeping/oncpu 族计数，另一套账）

拉取：fill_stats_for_pid（:201，单线程）/ fill_stats_for_tgid（:213-268 持 sighand 锁遍历全部线程：
     每线程 delayacct_add_tsk + ac_etime/utime/stime + nvcsw/nivcsw 累计）
推送：任务退出 → fill_tgid_exit(:276-291) 把该线程账并入
     tsk->signal->stats（线程组聚合）→ taskstats_exit 广播给
     注册过本 CPU cpumask 的监听者
```

**官方工具**：`tools/accounting/getdelays.c`（既是示例程序也是现成工具——`-p PID`/`-t TGID` 拉取、`-m cpumask` 退出监听、`-d` 直接开关 delayacct）；`procacct.c` 同目录。

## 八、两套任务延迟账对照（与 schedstat 姊妹篇）

| 维度 | schedstat（14 章） | delayacct（本章） |
| :--- | :--- | :--- |
| 就绪等待 | `sched_info.run_delay`（CONFIG_SCHED_INFO） | `cpu_delay_total`（同源！融合进 taskstats） |
| 睡眠等待 | sleep/block（SCHEDSTATS 全量计数） | **blkio/swapin/freepages/thrashing/compact/wpcopy**（按"为什么睡"分类） |
| 中断侵扰 | 无 | irq 族（复用 IRQ_TIME_ACCOUNTING） |
| 开关 | static key `sched_schedstats` | static key `delayacct_key` + int 双轨 |
| 导出 | /proc/schedstat、/proc/<pid>/schedstat、tracepoint | **仅 taskstats netlink** |
| 粒度 | 任务 + rq + sched_domain | 任务 + 线程组（signal->stats 聚合） |
| 哲学 | 调度器自身行为 | **任务视角的等待归因**（IO？内存？CPU？） |

## 九、观测实战

```bash
# 0) 开关（默认关；开账后新建任务才分配 delays——
#    delayacct_tsk_init 在 fork 时机查 delayacct_on）
echo 1 > /proc/sys/kernel/task_delayacct    # 需 CAP_SYS_ADMIN
# 或内核命令行直接：delayacct

# 1) 官方工具拉取（源码树自带）
make -C tools/accounting getdelays
tools/accounting/getdelays/getdelays -p <pid>     # 单任务七族
tools/accounting/getdelays/getdelays -t <tgid>    # 线程组聚合
# 输出解读：blkio_count/blkio_delay_total（同步 IO 等待次数/累计 ns）、
#           freepages_*（直接回收惩罚）、thrashing_*（页抖动）、
#           compact_*、wpcopy_*、irq_*、cpu_*（与 schedstat 同源）

# 2) 全量退出监听（谁在启动期挨罚）
tools/accounting/getdelays/getdelays -m 0-$(nproc --ignore-1) -d

# 3) 结合 14 章：cpu 族 = /proc/<pid>/schedstat 三元组
#    blkio+swapin+freepages+thrashing+compact+wpcopy + irq ≈
#    "总睡眠时间"的归因分解（对照 /proc/<pid>/status 的 voluntary/involuntary）

# 4) 容器场景：CGROUPSTATS_CMD_GET（getdelays -C /sys/fs/cgroup/...）
```

## 十、思考题

1. `delayacct_end` 只在 `ns > 0` 时记账——什么场景会产生负 delta？（提示：local_clock 的单调性保证范围；跨 CPU 收账时 start 与 end 的时钟源。）
2. blkio 族为什么必须持锁收账而 start 不用？（提示：起表者与收账者的上下文关系；对照第四节"two contexts"论断。）
3. `delayacct_tsk_init` 先 `tsk->delays = NULL` 再条件分配——如果 fork 路径忘了清指针会怎样？（提示：父子共享同一 task_delay_info 的锁与计数。）
4. thrashing 族的 `in_thrashing` 嵌套保护为什么用"外层保存、内层跳过"而不是引用计数？（提示：bool 标记的成本 vs 计数的正确性边界——嵌套页等待的真实形态。）
5. taskstats 的 `cpu_delay_total` 与 `/proc/<pid>/schedstat` 第二列同源——为什么前者"NOT updated atomically"却仍可用？（提示：快照 + 回绕契约 + 统计学容错。）
6. 若把 delayacct 的导出做成 /proc/<pid>/delay（每任务一个 seq_file），对比 netlink 方案的性能差异在哪？（提示：proc 读取要 stop_world 吗？netlink 报文在谁的上下文里填充？per-CPU 监听者把"谁拉"变成了"推给谁"。）

---

> 关联：[14-调度器统计纵深 schedstat](14-调度器统计纵深schedstat.md)（姊妹账目与 cpu 族同源）｜[04-调度时机与上下文切换](04-调度时机与上下文切换.md)（io_schedule 与 in_iowait 的切换路径）｜[mm 分册](../../mm/README.md)（freepages/compact/wpcopy 的内存机制背景）｜[trace/07-procfs 观测索引](../../trace/docs/07-procfs观测索引.md)（/proc/<pid>/schedstat 表项）｜[cgroup 分册](../../cgroup/README.md)（CGROUPSTATS 的 cgroup 侧）
