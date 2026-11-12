# 上刑测试特性：故意制造麻烦的调试设施（Linux 6.6）

> 阅读目标：盘点内核里"给驱动/子系统作者上刑"的调试特性家族——它们**故意**引入延迟、随机化、失败或检查，把潜伏 bug 放大成可复现崩溃/WARN。五个代表：`CONFIG_DEBUG_KOBJECT_RELEASE`（随机 1~4s 延迟清理）、fault injection 框架（概率性失败注入）、`CONFIG_DEBUG_LIST`（链表完整性检查）、`CONFIG_DEBUG_ATOMIC_SLEEP`（原子上下文睡眠检测）、`CONFIG_PROVE_LOCKING`/lockdep（锁序证明）。理解这类特性的共同设计哲学——**用确定性放大替代概率性触发**——与它们各自"上刑"的具体姿势。全部对照 v6.6（lib/kobject.c:697-716、lib/fault-inject.c、lib/list_debug.c、include/linux/kernel.h:126-140、kernel/locking/lockdep.c）。前置：[driver-model/07 §5.1](../driver-model/docs/07-资源生命周期三模式.md)（kobject 延迟清理的首次亮相）。

---

## 一、问题：概率 bug 的不可复现地狱

```
"程序在我机器上不崩溃"：
  use-after-free / 锁序反转 / 原子睡眠 / 链表损坏——都是概率事件：
  时间窗极窄 / 竞态依赖精确时序 / 损坏在很久之后才爆

上刑特性的设计原则：
  ① 放大时间窗（延迟——让竞态窗口从纳秒变秒）
  ② 制造确定性触发（随机化——让"必然"变"偶尔"变"频繁"）
  ③ 主动检查不变式（在错误操作的当场而非后果爆出时报警）
  ④ 系统性注入失败（把"内存不足才走的路径"变成常走）
```

## 二、五件刑具

### 2.1 随机延迟：CONFIG_DEBUG_KOBJECT_RELEASE（lib/kobject.c:697-716）

```c
#ifdef CONFIG_DEBUG_KOBJECT_RELEASE
static void kobject_delayed_cleanup(struct work_struct *work)
{
	kobject_cleanup(container_of(to_delayed_work(work),
				     struct kobject, release));
}
#endif

static void kobject_release(struct kref *kref)
{
	struct kobject *kobj = container_of(kref, struct kobject, kref);
#ifdef CONFIG_DEBUG_KOBJECT_RELEASE
	unsigned long delay = HZ + HZ * get_random_u32_below(4);
	/* ★随机 1~4 秒——让 "put 后对象立即可用" 的假设暴露 */
	pr_info("'%s' (%p): %s, parent %p (delayed %ld)\n", ...);
	INIT_DELAYED_WORK(&kobj->release, kobject_delayed_cleanup);
	schedule_delayed_work(&kobj->release, delay);
#else
	kobject_cleanup(kobj);
#endif
}
```

**上刑姿势**：把 `kobject_put` 归零后的清理**随机推迟 1~4 秒**。那些假设"put 之后对象立即消失/轮到我"的代码，在测试机上会看到对象"幽灵般"多活了 1~4 秒——竞态窗口从纳秒放大到秒级，原本万分之一概率的 bug 变成几乎每次触发。

### 2.2 概率失败：fault injection 框架（lib/fault-inject.c，437 行）

```c
struct fault_attr {                   /* include/linux/fault-inject.h:17-31 */
	unsigned long probability;    /* 概率（1/interval）*/
	unsigned long interval;       /* 间隔 */
	atomic_t times;               /* 总次数上限 */
	atomic_t space;               /* 可用配额 */
	unsigned long verbose;
	bool task_filter;             /* 按进程过滤 */
	unsigned long stacktrace_depth;
	unsigned long require_start, require_end;  /* 代码地址范围 */
	unsigned long reject_start, reject_end;
	...
};

/* cmdline 语法（setup_fault_attr，fault-inject.c:14-22）：
   "<interval>,<probability>,<space>,<times>"  */

bool should_fail(struct fault_attr *attr, ssize_t size)
{
	/* 按 probability/interval 决定是否命中；times/space 限额；
	   require/reject 地址过滤；task_filter 按进程白名单 */
	if (in_task() && ... && !fail_task(attr, current))
		return false;
	if (atomic_read(&attr->times) == 0)
		return false;
	... /* 详细判定链 */
	if (attr->probability > get_random_u32_below(...) % ... )
		return false;
	...
}
```

**消费端**：
| 注入器 | 配置 | 注入点 |
| :--- | :--- | :--- |
| `should_failslab` | CONFIG_FAILSLAB | `kmalloc/slab_alloc` 路径 |
| `should_fail_alloc_page` | CONFIG_FAIL_PAGE_ALLOC | `__alloc_pages` |
| `should_fail_io` | CONFIG_FAIL_MAKE_REQUEST | 块层 make_request |
| `should_fail_futex` | CONFIG_FAIL_FUTEX | futex 路径 |
| debugfs 接口 | `/sys/kernel/debug/fail*/` | 属性实时可调 |

**上刑姿势**：把"内存恰好耗尽/IO 恰好失败"的**罕见路径**变成**可编程的常态**——错误处理代码（通常从未被测过）终于被系统性执行。

### 2.3 不变式检查：CONFIG_DEBUG_LIST（lib/list_debug.c）

```c
/* lib/list_debug.c —— "Check that the data structures for the list
   manipulations are reasonably valid. Failures here indicate memory
   corruption (and possibly an exploit attempt)." 头注释原话 */

__list_valid_slowpath
bool __list_add_valid_or_report(struct list_head *new,
				struct list_head *prev,
				struct list_head *next)
{
	if (CHECK_DATA_CORRUPTION(prev == NULL,
			"list_add corruption. prev is NULL.\n") ||
	    CHECK_DATA_CORRUPTION(next->prev != prev,
			"list_add corruption. next->prev should be prev (%px), but was %px. (next=%px).\n", ...) ||
	    CHECK_DATA_CORRUPTION(prev->next != next, ...) ||
	    CHECK_DATA_CORRUPTION(new == prev || new == next,
			"list_add double add: new=%px, prev=%px, next=%px.\n", ...))
		return false;
	return true;
}
```

**上刑姿势**：每次 `list_add/list_del` 都做**交叉一致性检查**（next->prev == prev && prev->next == next）——损坏在**发生的当下**被抓住（而非链表遍历时崩溃在别处）。`CHECK_DATA_CORRUPTION` 支持 panic（`panic_on_warn`）或 WARN 继续。

### 2.4 上下文违规：CONFIG_DEBUG_ATOMIC_SLEEP（include/linux/kernel.h:126-140）

```c
/* 头注释原话："might_sleep - annotation for functions that can sleep.
   This macro will print a stack trace if it is executed in an atomic
   context (spinlock, irq-handler, ...)". */

extern void __might_sleep(const char *file, int line);
extern void __cant_sleep(const char *file, int line, unsigned int preempt_offset);

#define might_sleep()                          \
	do {                                       \
		might_resched();                       \
		if (atomic_read(&current->flags) & PF_ATOMIC_TEST) \
			__might_sleep(__FILE__, __LINE__); \
	} while (0)
```

**上刑姿势**：`might_sleep()` 散布在所有可睡眠函数入口（kmalloc/mutex_lock/...）——在 spinlock/中断上下文调到它们时**立即**报错（而非真正睡了之后死锁/崩溃）。

### 2.5 全局不变式：CONFIG_PROVE_LOCKING / lockdep（kernel/locking/lockdep.c）

```
lockdep 做的事（本分册已有 mutex.md 详述，此处只列"上刑"视角）：
  ① 记录所有锁的获取顺序 → 构建全局依赖图
  ② 每次 acquire 时检查"添加这条边是否引入环"
  ③ 环 = 潜在死锁 → 当场 WARN（带完整锁栈）
  ④ 额外检查：irq-safe/unsafe 混用、递归、init 前使用
上刑姿势：把死锁从"恰好两个线程同时走对路径"的概率事件
  变成"图中有环"的确定性判定——哪怕实际永远不会同时走到
```

## 三、设计维度对照表

| 特性 | 刑具类型 | 热路径成本 | 生产可用 | 发现的 bug 类型 |
| :--- | :--- | :--- | :--- | :--- |
| DEBUG_KOBJECT_RELEASE | **延迟放大** | 无（release 路径） | ✗（用户体验差） | 生命周期竞态 |
| FAILSLAB 等 | **失败注入** | 一次 should_fail 调用 | ✓（probability=0 即关） | 错误处理覆盖 |
| DEBUG_LIST | **不变式检查** | 每次 list_add/del ~10ns | 半（可容忍） | 内存损坏 |
| DEBUG_ATOMIC_SLEEP | **上下文断言** | might_sleep 处一次 branch | ✓（几乎零成本） | 原子上下文睡眠 |
| PROVE_LOCKING | **全局图检查** | 每次 lock/unlock ~100ns | ✗（内存/性能重） | 死锁/锁序违规 |
| （对照）KASAN/KFENCE/KCSAN | **内存/Race 检测** | 各异 | 部分 | UAF/越界/竞态 |

## 四、组合拳：debug kernel 的配置矩阵

```bash
# 典型"上刑内核"配置（驱动开发 CI 用）
CONFIG_DEBUG_KERNEL=y
CONFIG_DEBUG_KOBJECT_RELEASE=y       # 生命周期
CONFIG_DEBUG_LIST=y                  # 链表
CONFIG_DEBUG_ATOMIC_SLEEP=y          # 上下文
CONFIG_PROVE_LOCKING=y               # 锁序
CONFIG_FAILSLAB=y                    # 失败注入
CONFIG_PANIC_ON_WARN=y               # 零容忍（CI 环境）
CONFIG_KASAN=y                       # 内存（如架构支持）
CONFIG_KCSAN=y                       # 数据竞态

# 运行时调节（debugfs）
echo 1 > /sys/kernel/debug/failslab/task-filter
echo 1 > /sys/kernel/debug/failslab/probability
#   → 精确控制"哪个进程的 kmalloc 失败"
```

## 五、观测实战

```bash
# 1) kobject 延迟清理的现场
dmesg | grep "delayed"
#   ".... (delayed 37)" → 37 jiffies 后才清理

# 2) 故障注入的命中率
cat /sys/kernel/debug/failslab/times     # 剩余注入次数
cat /sys/kernel/debug/failslab/probability

# 3) list corruption 的场景
dmesg | grep "list_add corruption\|list_del corruption"

# 4) might_sleep 触发
dmesg | grep "BUG: sleeping function called from invalid context"

# 5) lockdep 图规模
cat /proc/lockdep_stats | head -5
```

## 六、思考题

1. `CONFIG_DEBUG_KOBJECT_RELEASE` 的 `get_random_u32_below(4)` 为什么随机而不是固定延迟？（提示：固定延迟可以被驱动"适应"——它只需在 delay 后检查一次；随机化保证任何时序假设都不安全。）
2. `should_fail` 的 `task_filter` 为什么存在？（提示：全局注入会破坏内核自身的工作队列/slab 管理——只让目标任务吃失败才能维持系统运行。）
3. `CONFIG_DEBUG_LIST` 的检查在**操作时刻**而非**使用时刻**——为什么这比遍历时发现更好？（提示：损坏源头的栈还在——遍历时栈已远离犯罪现场。）
4. `might_sleep` 的成本是"每次函数入口一个分支"——为什么不能在生产内核默认开？（提示：kmalloc 路径上的额外分支在高频微分配下的积累成本。）
5. lockdep "图有环就报" 会产生假阳性（实际时序不可能同时走到）——这是 bug 还是特性？（提示：宁可误报不漏报——"可能死锁"在并发正确性中等价于"会死锁"。）
6. 这些刑具的共同前提是什么？（提示：**不变式的明确声明**——链表的一致性、原子的不可睡、锁序的偏序关系……没有清晰的不变式就没有"违反"可言。）

---

> 回看：[mutex](mutex.md)（lockdep 的宿主之一）｜[driver-model/07-资源生命周期三模式](../driver-model/docs/07-资源生命周期三模式.md)（kobject 延迟清理的机制细节）｜[locking/README](README.md)（锁族谱）｜[init 分册](../init/README.md)（boot 期的调试阶段——early_param/late_initcall 的时机）
