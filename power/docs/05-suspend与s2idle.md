# suspend 与 s2idle 全流程（Linux 6.6）

> 阅读目标：吃透系统睡眠的三级等级（freeze/mem/disk）与 `enter_state→suspend_devices_and_enter→suspend_enter` 的逐段时序；深挖 s2idle 的循环-唤醒协议与 platform ops 双套接口；掌握 wakeup source 的记账与 `pm_wakeup_pending` 中止点、hibernate（S4）镜像机制、以及 pm_trace/pm_test 排障工具箱。

---

## 一、三个等级与入口

```
/sys/power/state:  freeze | mem | disk
                    │       │      └─ S4 hibernate（内存镜像入 swap 后断电）
                    │       └─ S3 suspend-to-RAM（真实断电，SDRAM 自刷新）
                    └─ s2idle / suspend-to-idle（软件冻结 + 深空闲，电不断）

/sys/power/mem_sleep:       s2idle|deep|shallow —— "mem" 实际映射哪个等级的选择器
                            （x86 上 s2idle 深受厂商固件质量影响，常见做法是
                              echo deep > /sys/power/mem_sleep 强制 S3）
```

用户态入口链（第 1 章已画主干）：`echo mem > /sys/power/state` → `pm_suspend(state)`（kernel/power/main.c）→ `enter_state(state)`（kernel/power/suspend.c）。

---

## 二、全流程时序（已逐段核对 v6.6 源码）

```
enter_state(state)
 ├─ sync_on_suspend? （6.6: sync_filesystems，可由 sysctl 关闭）
 ├─ suspend_prepare()
 │    ├─ pm_prepare_console()
 │    ├─ pm_notifier_call_chain(PM_SUSPEND_PREPARE)   /* 驱动/子系统的准备通知 */
 │    ├─ suspend_freeze_processes()                    /* 冻结任务（含可冻结 kthread）*/
 │    └─ pm_suspend_console()
 │
 ├─ suspend_devices_and_enter(state)
 │    ├─ platform_suspend_begin(state)                 /* platform_suspend_ops->begin */
 │    ├─ suspend_console()                             /* printk 转入 async 模式 */
 │    ├─ dpm_suspend_start(PMSG_SUSPEND)
 │    │    ├─ dpm_prepare()    （PREPARE 族回调，反序）
 │    │    └─ dpm_suspend()    （MAIN 族回调，反序）★每个设备间检查
 │    │                            pm_wakeup_pending() —— 唤醒竞态即中止回滚
 │    │
 │    └─ suspend_enter(state, &wakeup)
 │         ├─ platform_suspend_prepare()
 │         ├─ dpm_suspend_late(PMSG_SUSPEND)           （LATE 族，反序）
 │         ├─ platform_suspend_prepare_late()
 │         ├─ dpm_suspend_noirq(PMSG_SUSPEND)          （NOIRQ 族，反序）
 │         ├─ platform_suspend_prepare_noirq()
 │         ├─ [suspend_test(TEST_PLATFORM)]
 │         │
 │         ├─ if (state == PM_SUSPEND_TO_IDLE)
 │         │      └─ s2idle_loop()                     ★冻结态主循环（第三节）
 │         │
 │         ├─ else （S3 真断电路径）
 │         │      ├─ pm_sleep_disable_secondary_cpus() （次级 CPU 下线）
 │         │      ├─ [suspend_test(TEST_CPUS)]
 │         │      ├─ arch_suspend_disable_irqs()
 │         │      ├─ syscore_suspend()                 /* 时钟源/tick/其他 syscore 关停 */
 │         │      ├─ trace_suspend_resume? + check_wakeup_irqs
 │         │      ├─ platform_suspend_ops->enter(state) ★★真正下电：
 │         │      │      x86: acpi_enter_sleep_state → PM1a_CNT.SLP_TYP|SLP_EN 写入
 │         │      │           → CPU 停钟断电，世界暂停
 │         │      └─（断电点。唤醒后固件 reset vector → 实模式 →
 │         │           waking vector → restore_processor_state → 严格逆序恢复）
 │         │
 │         └─（s2idle 则从 loop 返回后走逆序：noirq resume → early → MAIN →
 │             console → tasks thaw → PM_POST_SUSPEND）
```

四个工程要点：

1. **中止点遍布全程**：`pm_wakeup_pending()`（唤醒事件在睡眠过程中抵达）在每个设备段间被检查，命中即整链回滚——这就是"刚点睡眠就来了个网卡包导致睡眠失败"的机制。
2. **回调族与阶段**严格对应第 1 章矩阵：`dpm_suspend_*` 三连（MAIN/LATE/NOIRQ）与 resume 三连严格逆序。
3. **syscore_suspend 是最后的知识分子**：clocksource、tick broadcast、sysdev 在 IRQ 已关的世界里按显式注册顺序静默（`syscore_ops` 注册表）——睡眠期间唯一还"活着"的只有唤醒源硬件。
4. **S3 与 s2idle 分叉**在 `suspend_enter` 内部：s2idle 不关次级 CPU/IRQ（各 CPU 进 cpuidle 深状态，第 3 章 `cpuidle_enter_s2idle`），S3 才走断电不归路。

---

## 三、s2idle_loop：冻结态的心跳（已核对源码）

```c
static void s2idle_loop(void)               /* kernel/power/suspend.c */
{
	for (;;) {
		if (s2idle_ops && s2idle_ops->wake) {
			if (s2idle_ops->wake())   /* 平台（ACPI）判定这是"真唤醒"？ */
				break;
		} else if (pm_wakeup_pending())
			break;                    /* 唤醒事件记账非零 → 退出睡眠 */

		if (s2idle_ops && s2idle_ops->check)
			s2idle_ops->check();      /* 假唤醒筛查（如触摸板误触）*/

		s2idle_enter();               /* 各 CPU 进最深 s2idle 态：
		                                cpuidle 的 enter_s2idle 路径（第3章），
		                                直到一个中断把 CPU 打醒 */
	}
}
```

循环的必要性：s2idle 期间任意设备中断都可能"惊醒"CPU，但**多数是假唤醒**（事件不需要用户可见响应）。loop 用平台 `check`（ACPI 侧 LPS0 深度模式事件判别）过滤后决定是否真正结束整个睡眠会话——否则继续睡。

**ACPI s2idle 实现**（`platform_s2idle_ops` 由 acpi 注册，8 个回调已核对：begin/prepare/prepare_late/check/wake/restore_early/restore/end）：
- `prepare_late`：进 **LPS0 深度模式**（`_DSM` 函数 5/6：固件托管设备电源如 NVMe、USB off 模式），保存 NVS
- `check`：轮询 ACPI 事件，识别 `ACPI_LPS0_SCREEN`/`ACPI_LPS0_EXIT` 等事件类
- GPE（通用事件）与 SCI 中断是唤醒主干；`acpi_os_execute_deferred` 处理苏醒后的 AML 执行

> s2idle 的历史使命：绕开质量参差的 S3 固件路径（笔记本厂商普遍 S3 支持劣化），用纯软件冻结换取可调试性。代价是"睡眠电流"依赖 LPS0/设备驱动配合——睡不下去的设备等于白冻。

---

## 四、wakeup source：唤醒的记账与协议

```
struct wakeup_source {                  /* drivers/base/power/wakeup.c */
	const char	*name;
	unsigned int	event_count;     /* 唤醒事件总数 */
	unsigned int	wakeup_count;    /* 真正触发睡眠中止的次数 */
	unsigned int	active_count;    /* activate↔deactivate 次数 */
	unsigned long	eventip ...      /* 已核对源码字段族 */
	ktime_t		prevent_suspend_time;  /* 活跃时长累计——"谁不让睡"的铁证 */
	...
};

关键路径：
设备驱动调用 pm_wakeup_event(dev, msec) / pm_wakeup(dev)
  → wakeup source activate（event_count++，prevent_suspend 计时开始）
  → ws 记入全局 wakeup_count
pm_wakeup_pending()                       /* suspend 全程的哨兵查询 */
  → 汇总"有活跃 wakeup source 或 wakeup_count 未消费"
```

**用户态原子协议（wakeup_count）**：systemd 的睡法——

```
1. read /sys/power/wakeup_count   → 得到当前计数 N
2. 判断"睡前世界安静"（自己的一轮检查）
3. write N 回去                    → 若期间又来了唤醒事件（计数已变），写失败
4. 写成功 → 立即 write mem 到 /sys/power/state
```

这个"读-改-写回"握手把"刚要睡就来了电话"的丢事件竞态关进了协议里。

**排障金手指**：`echo 1 > /sys/power/pm_wakeup_irq`（下次唤醒时打印唤醒 IRQ），配合 `/sys/kernel/debug/wakeup_sources` 的 `prevent_suspend_time` 排序——"谁半夜一直闹"一查便知。

---

## 五、hibernate（S4）概览

`kernel/power/hibernate.c` 与 suspend 共享设备冻结/任务冻结骨架，差异在"下电前把内存拍快照、上电后把快照贴回去"：

```
入眠：snapshot_alloc_page? → 遍历可写页面生成镜像（lzo/zstd 压缩）
      → 写入 swap 分区（签名头：镜像大小/校验）
      → 平台 poweroff（真实断电，比 S3 更彻底）
唤醒：冷启动 → bootloader 识别 swap 签名（resume= 参数）
      → restore_image：把镜像页按原 PFN 灌回内存
      → 跳回"冻结前"的内核执行流（设备此时全部未重新初始化！）
      → 逆序 resume 设备（与 suspend 对称）
```

三个坑位知识：镜像恢复后设备处于"断电前状态"但总线/控制器已冷启——依赖每个驱动的 `restore` 回调重新涂胶（`dev_pm_ops` 的 poweroff/restore 族，第 1 章矩阵）；`resume_offset`/`resume=` 内核参数指定 swap；**安全性**：快照含全部内存明文（swap 加密/加密 hibernate 的组合是另一个话题）。

---

## 六、排障工具箱（浓缩版）

```
/sys/power/pm_test        逐级演练：devices→platform→processors→core
                          （不真断电，走完全部回调——驱动 suspend 支持的验收工具）
/sys/power/pm_print_times 每设备回调耗时打印（CONFIG_PM_DEBUG）
/sys/power/pm_wakeup_irq  下次唤醒打印 IRQ 号
/sys/kernel/debug/wakeup_sources   prevent_suspend_time 排序找"睡不着元凶"
pm_trace（CONFIG_PM_TRACE）：把挂点 hash 进 RTC CMOS——睡死后开机 dmesg 里
                          "Hash matches drivers/base/power/main.c:xxx"
                          直接指认卡死的回调行（老而极好用）
no_console_suspend        睡眠期间保留 console（驱动调试，有死锁风险）
```

标准排查流：`pm_test` 逐档定位卡死阶段 → `pm_print_times` 找慢设备 → 唤醒失败用 `pm_wakeup_irq` + `wakeup_sources` → 死机用 `pm_trace`。

---

## 七、思考题

1. `dpm_suspend` 的设备间检查 `pm_wakeup_pending()` 若命中，回滚走的是哪条路径？为什么"设备 N 挂了一半"是安全的（对照第 1 章三阶段语义）？
2. s2idle_loop 为什么不能简单写成"睡到 pm_wakeup_pending() 为真"？平台 `check` 回调过滤掉的"假唤醒"如果不滤，用户会观察到什么？
3. `platform_suspend_ops->enter()` 返回后的代码在 S3 场景下**何时**真正执行？（提示：恢复的是新 boot 还是原 boot？谁跳回来的？）
4. hibernate 恢复镜像后跳回冻结前执行流——此时调度器/RCU/tick 看到的世界连续吗？`syscore_resume` 与设备 `restore` 的顺序为什么必须严格逆序？给一个"乱序即死"的具体例子。
5. 设计：某设备驱动在 s2idle 下必须保持供电（远程唤醒）但在 S3 必须断电。它的 `dev_pm_ops` 三个族（freeze/suspend/poweroff）应分别怎么写？`dpm_suspend` 的兼容判定（第 4 章第五节）会不会跳过它？

---

> 本分册 5 章至此完整。回看：[01 总体框架](01-电源管理总体框架.md)｜[02 cpufreq](02-cpufreq.md)｜[03 cpuidle](03-cpuidle.md)｜[04 runtime PM](04-runtime-PM.md)｜[05 suspend与s2idle](05-suspend与s2idle.md)
