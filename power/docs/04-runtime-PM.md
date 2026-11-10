# runtime PM 与设备电源管理（Linux 6.6）

> 阅读目标：掌握设备级运行时电源管理的三件套——引用计数（usage_count）、状态机（RPM 四态）、异步工作队列（pm_wq）；吃透 `rpm_suspend`/`rpm_resume` 的判定条件与 autosuspend 防抖模型；理解父子层级与 device link 的联动语义、irq_safe 的代价、以及驱动接入 runtime PM 的规范姿势。

---

## 一、模型：从"系统睡眠"到"毫秒级微睡眠"

系统睡眠（第 5 章）是全局事件：所有设备一起下电、走秒级流程。runtime PM 解决的是**单设备、亚秒粒度**的问题：

```
设备 A 空闲 200ms → 下电（runtime_suspend，功耗≈0）
下次访问 → 上电（runtime_resume，代价 = exit_latency）
收益 = 200ms × (运行功耗 − 挂起功耗) − 上电开销
```

显然这是**每设备独立经济账**：DDR、USB 控制器、GPU、PCIe 链路各自的阈值不同。核心数据全部挂在 `device->power`（`struct dev_pm_info`，已核对 6.6 源码）：

```c
struct dev_pm_info {                       /* include/linux/pm.h, 6.6 节选 */
	struct hrtimer		suspend_timer;  /* autosuspend 到期定时器 */
	u64			timer_expires;
	struct work_struct	work;            /* pm_wq 上的延迟工作 */
	wait_queue_head_t	wait_queue;       /* 等待状态迁移完成的队列 */
	atomic_t		usage_count;      /* ★引用计数：>0 必须 active */
	atomic_t		child_count;      /* "活跃"子设备数（父级判定用）*/
	unsigned int		disable_depth:3;  /* runtime PM 禁用深度（默认 1！）*/
	unsigned int		idle_notification:1;
	unsigned int		request_pending:1;  /* 请求已入队待执行 */
	unsigned int		deferred_resume:1;  /* suspend 过程中来了 resume：延迟 */
	unsigned int		needs_force_resume:1;
	unsigned int		runtime_auto:1;    /* autosuspend 使能 */
	bool			ignore_children:1;  /* 父不因子活跃而醒 */
	unsigned int		no_callbacks:1;    /* 无回调（只计状态）*/
	unsigned int		irq_safe:1;        /* 回调可在原子上下文跑 */
	unsigned int		use_autosuspend:1;
	unsigned int		timer_autosuspends:1;
	unsigned int		links_count;       /* 与之联动 supplier link 数 */
	enum rpm_request	request;         /* 队列里的请求类型 */
	enum rpm_status		runtime_status;  /* ★当前状态 */
	enum rpm_status		last_status;
	int			runtime_error;    /* 熔断标志：出错后停摆 */
	int			autosuspend_delay; /* ms */
	u64			last_busy;        /* 最后一次"忙"的时刻戳 */
	...
};
```

```
enum rpm_status { RPM_ACTIVE, RPM_RESUMING, RPM_SUSPENDED, RPM_SUSPENDING };
enum rpm_request { RPM_REQ_IDLE, RPM_REQ_SUSPEND, RPM_REQ_AUTOSUSPEND, RPM_REQ_RESUME };
```

> ⚠️ **`disable_depth` 默认为 1**：驱动 probe 时 runtime PM 处于禁用态，必须显式 `pm_runtime_enable()`（通常与 `pm_runtime_use_autosuspend()` + `pm_runtime_put_autosuspend()` 成组出现在 probe 尾部）——这是接入 runtime PM 的第一课，漏调则设备永远 ACTIVE。

---

## 二、API 家族：get/put 矩阵

所有 API 围绕"动计数"与"要不要顺便做状态迁移"两个轴展开：

| API | usage_count | 顺带动作 | 典型场景 |
| :--- | :--- | :--- | :--- |
| `pm_runtime_get_sync()` | +1 | 同步 resume（等回调完成） | 打开设备/开始 DMA |
| `pm_runtime_get_noresume()` | +1 | 只记账，不 resume（防竞态占位） | probe/移除路径 |
| `pm_runtime_resume_and_get()` | +1 | 同步 resume，**错误时回滚计数**（6.6 推荐版） | 替代 get_sync |
| `pm_runtime_put_sync()` | −1 | 同步 idle/suspend 判定 | 关闭设备 |
| `pm_runtime_put_noidle()` | −1 | 只记账（绝不 suspend） | 错误路径 |
| `pm_runtime_put_sync_autosuspend()` | −1 | 排程 autosuspend 定时器 | 正常收尾（推荐） |
| `pm_runtime_mark_last_busy()` | — | 刷新 `last_busy`（重置防抖窗） | 每次完成一笔工作 |
| `pm_runtime_set_active()/set_suspended()` | — | 强制改状态（初始化/失联恢复） | 探测到硬件状态时 |
| `pm_runtime_barrier()` | — | 排空 pending 请求 | 与系统睡眠交接 |

同步/异步轴：`*_sync` 等待状态机走完；`pm_request_*`/`pm_runtime_*`（不带 sync）只投递 work。**irq_safe=0 的设备回调在进程上下文（pm_wq）执行**——回调里可以睡眠；`irq_safe=1` 则在调用方上下文直接执行（原子）。

---

## 三、状态机核心：rpm_suspend / rpm_resume

### 3.1 队列化入口：pm_runtime_work（已核对源码）

```
pm_request_suspend(dev) 等异步 API
  └─ dev->power.request = RPM_REQ_*; request_pending = 1
  └─ queue_work(pm_wq, &dev->power.work)

pm_runtime_work(work)                    /* drivers/base/power/runtime.c */
  └─ switch (dev->power.request) {
       RPM_REQ_IDLE        → rpm_idle(dev, RPM_NOWAIT)
       RPM_REQ_SUSPEND     → rpm_suspend(dev, RPM_NOWAIT)
       RPM_REQ_AUTOSUSPEND → rpm_suspend(dev, RPM_NOWAIT | RPM_AUTO)
       RPM_REQ_RESUME      → rpm_resume(dev, RPM_NOWAIT)
     }
```

### 3.2 rpm_suspend 的六道关卡（已核对 6.6 源码）

```c
static int rpm_suspend(struct device *dev, int rpmflags)
{
 repeat:
	/* ① 条件检查：usage_count>0？disable_depth>0？runtime_error？
	   子设备仍有活跃（child_count 非 0 且 !ignore_children）？
	   → 全部拒绝 */
	if (rpm_check_suspend_allowed(dev) < 0) goto out;

	/* ② 正在 RESUMING 且非异步 → -EAGAIN（sync 版等待或重试）*/
	if (runtime_status == RPM_RESUMING && !(rpmflags & RPM_ASYNC))
		retval = -EAGAIN;

	/* ③ autosuspend 防抖：RPM_AUTO 时若 last_busy+delay 未到期，
	      编程 hrtimer（带 25% slack —— 源码注释：聚合唤醒、省粒度）后离开 */
	if ((rpmflags & RPM_AUTO) && runtime_status != RPM_SUSPENDING) {
		expires = pm_runtime_autosuspend_expiration(dev);
		if (expires) {
			hrtimer_start_range_ns(&dev->power.suspend_timer,
					       ns_to_ktime(expires),
					       slack, HRTIMER_MODE_ABS);
			dev->power.timer_autosuspends = 1;
			goto out;                 /* 到期由 pm_suspend_timer_fn
		                                       重新投递 RPM_REQ_AUTOSUSPEND */
		}
	}

	/* ④ 清掉更早的 pending 请求（新决定覆盖旧决定）*/
	pm_runtime_cancel_pending(dev);

	/* ⑤ 已有并发 suspend 在跑：异步返回 -EINPROGRESS；
	      irq_safe 设备自旋重试（不能睡眠！）；普通设备在 wait_queue 上等 */
	if (runtime_status == RPM_SUSPENDING) { ... prepare_to_wait ... }

	/* ⑥ 正式迁移：RPM_SUSPENDING → 调用回调（子系统/runtime_suspend）
	      → 成功：RPM_SUSPENDED（记账 suspended_time）；
	      失败：runtime_error 或回 ACTIVE；父级计数联动（child_count--）*/
	...
}
```

`rpm_resume` 镜像对称：条件检查（disable_depth/runtime_error）→ RESUMING 中则等待 → 回调 `runtime_resume` → ACTIVE + `last_busy` 刷新 + 父级 `child_count++`。**deferred_resume** 位处理经典竞态：suspend 回调执行中途来了 get 请求——先把 suspend 做完，随后立即补一次 resume（而不是留下"ACTIVE 状态但设备实际挂起"的谎言状态）。

### 3.3 autosuspend：为什么必须有防抖

```c
/* 推荐的驱动模式（probe 尾部）*/
pm_runtime_use_autosuspend(dev);              /* 开 autosuspend */
pm_runtime_set_autosuspend_delay(dev, 100);   /* 空闲 100ms 才挂起 */
pm_runtime_mark_last_busy(dev);
pm_runtime_put_autosuspend(dev);              /* 计数归 0，启动定时器 */

/* 每次工作完成 */
pm_runtime_mark_last_busy(dev);               /* 重置 100ms 窗口 */
pm_runtime_put_autosuspend(dev);
```

没有防抖的世界：网络包每 50ms 到一个 → 设备 50ms 挂/50ms 醒循环——上下电开销可能**超过**省下的电。autosuspend 用"最后一个 busy 时刻 + delay"把抖动折叠掉；`pm_runtime_autosuspend_expiration` 计算 `last_busy + delay` 是否已过，未过则续表（带 25% slack 聚合 timer 编程本身的开销）。

---

## 四、层级与链接：谁的电源牵着谁

### 4.1 父子语义（device tree 拓扑）

默认规则：**子设备活跃（runtime ACTIVE）时父设备不许挂起**——`child_count` 维护"活跃子数"（resume 时 ++，suspend 时 --），`rpm_check_suspend_allowed` 检查它。反向：**父挂起时子必须已挂起**（检查子状态）。`ignore_children` 提供逃生门（如 PCIe root port 对其下功能：链路时钟门控由 ASPM 自理）。

### 4.2 device link：跨树依赖

`device_link_add(consumer, supplier, DL_FLAG_RPM_ACTIVE|...)`：consumer resume 时框架自动先 resume supplier（`links_count` 计数联动）。典型消费者：声卡 codec↔I2S 控制器、USB 设备↔hub——比手工在各驱动里互相 get/put 可靠得多（第 1 章设备序问题的 runtime 版解药）。

---

## 五、与系统睡眠的交接

`dpm_suspend()` 之前，PM core 调 `pm_runtime_suspend`（async 关闭、barrier 排空）把设备先挂到 runtime SUSPENDED。之后系统级回调的执行有三种形态（`dpm_prepare/suspend` 内部判定）：

1. 设备 runtime SUSPENDED 且**两套回调兼容**（`runtime_suspend` 与 `suspend` 语义等价，或驱动声明了 `PM_EVENT_...` 复用）→ 跳过系统回调，resume 后回 runtime ACTIVE
2. runtime SUSPENDED 但需要做**额外**动作（如总线上发 standby 命令）→ 先 runtime resume 再走系统 suspend（最保守、最慢）
3. ACTIVE → 直接系统回调

唤醒侧 `dpm_resume` 反向对称，`pm_runtime_reinit` 重建 runtime 状态。**驱动接入清单**：`runtime_suspend/resume` 必须与 `suspend/resume` 状态兼容，或用 `dev_pm_ops` 的 `.suspend` 明确写 `pm_runtime_suspend(dev)` 委托——否则睡眠后回来就是双重挂起/状态错乱。

---

## 六、观测与排障

```
/sys/devices/.../power/
    runtime_status      active/suspended/resuming/suspending
    runtime_usage       usage_count 当前值
    runtime_active_kids / runtime_enabled
    control             on（强制 active）/ auto（交给框架）——排障开关
    autosuspend_delay_ms
    runtime_suspended_time / runtime_active_time   累计驻留
tracepoint: power/rpm_suspend、rpm_resume（状态机全事件流；
    bpftrace 可统计每设备挂起次数与时长分布）
```

经典故障速查：

| 症状 | 原因 | 验证 |
| :--- | :--- | :--- |
| 设备永远 active | 漏 `put`（计数泄漏）；`disable_depth` 未清 | `runtime_usage` 恒 >0；`control=on` |
| 设备从不 suspend | 防抖窗内反复 mark_last_busy；父/子活跃 | trace rpm_suspend 频率 |
| 唤醒后设备死 | runtime 与系统回调不兼容的双重挂起 | pm_print_times + 对照第五节 |
| suspend 随机失败 | 回调里睡眠了 irq_safe 设备 / -EAGAIN 风暴 | runtime_error 位 + trace |

---

## 七、思考题

1. `pm_runtime_get_noresume()` 只加计数不 resume。它防住的具体竞态序列是什么？（提示：open 路径与并发 autosuspend timer 到期的交错。）为什么不直接用 `get_sync`？
2. 防抖 timer 用 `hrtimer_start_range_ns` 带 25% slack。对"每 50ms 一个包"的网络设备，这个 slack 会造成什么可观测现象？是 bug 还是特性？
3. `ignore_children` 的正当使用场景（PCIe ASPM）与误用场景（普通平台总线）各是什么？误用时最先观察到的故障是什么？
4. `deferred_resume` 为什么不直接中断 suspend 回调返回"失败"？回到 ACTIVE 后发起者的 get_sync 看到的返回值应该是什么语义？
5. 设计：为一个"每 100ms 突发 10 笔 I2C 事务"的传感器选择 autosuspend_delay 与挂起策略，并计算挂起收益公式中各项的取值。

---

> 下一章：[suspend 与 s2idle 全流程](05-suspend与s2idle.md) —— 系统级睡眠的完整时序、wakeup source 协议与 S3/S4 的平台路径。
