# async 异步基础设施与异步探测（Linux 6.6）

> 阅读目标：吃透 kernel/async.c 的 **sequence cookie 协议**——"乱序执行、顺序退休"如何用一个 u64 单调计数 + 双链表 + `wait_event` 不等式实现全局顺序保证；理解 initcall 层级里 `1s` 同步段的链接序编排与 `async_synchronize_full()` 在 free_initmem 前的**深层理由**；掌握异步探测的三层开关（`probe_type` 三值 / `driver_async_probe=` cmdline / 模块 `async_probe=1`）与 `current_is_async()` 的死锁警戒线。全部对照 v6.6（kernel/async.c、include/linux/async.h、init/main.c、include/linux/init.h、drivers/base/dd.c、kernel/module/kmod.c）。前置：[04-initcall 与模块初始化机制](04-initcall与模块初始化机制.md)（层级基础）。

---

## 一、设计目标：乱序执行，顺序退休（async.c:10-40 注释）

```
async.c: Asynchronous function calls for boot performance
（Arjan van de Ven，Intel，2009）

"The primary goal of this feature is to reduce the kernel boot time,
 by doing various independent hardware delays and discovery operations
 decoupled and not strictly serialized."

——慢速硬件探测（PCI 扫描、SCSI 发现、固件加载）不再串行堵住启动，
  但"外部可见的部分"（注册设备号等）仍然保序：

"not unlike how out-of-order CPUs retire their instructions in order"
```

**协议契约**（:31-37 注释原话）：异步函数在执行**全局可见操作前**（如注册设备号），必须先 `async_synchronize_cookie(自己的 cookie)`——保证所有先于自己调度的异步操作全部完成。

## 二、cookie 协议与核心结构（async.c:58-107）

```c
typedef u64 async_cookie_t;                 /* async.h:16 */
typedef void (*async_func_t)(void *data, async_cookie_t cookie);
                                            /* ★函数拿到自己的 cookie */

static async_cookie_t next_cookie = 1;      /* async.c:58 单调分配器 */
#define MAX_WORK        32768               /* :60 全局在途上限 */
#define ASYNC_COOKIE_MAX ULLONG_MAX         /* :61 "infinity cookie" */
static LIST_HEAD(async_global_pending);     /* :63 所有 registered 域的并集 */
static ASYNC_DOMAIN(async_dfl_domain);      /* :64 缺省域 */
static DECLARE_WAIT_QUEUE_HEAD(async_done); /* :77 唤醒点 */
static atomic_t entry_count;                /* :79 全局在途计数 */

struct async_entry {                        /* :67-77 双挂链 */
	struct list_head domain_list;   /* 挂 domain->pending */
	struct list_head global_list;   /* 挂 async_global_pending（registered 域）*/
	struct work_struct work;
	async_cookie_t cookie;
	async_func_t func; void *data;
	struct async_domain *domain;
};
```

**顺序性来源**：两条链都是 `list_add_tail` 按分配序入队——`lowest_in_progress()`（:87）只需看链表**头节点的 cookie**（最小未完成者）；运行完成后 `async_run_entry_fn`（:115）把自己从两条链同时摘除并 `wake_up(&async_done)`。

## 三、调度路径：async_schedule_node_domain（async.c:165-213）

```
async_schedule(func, data)                → 缺省域，NUMA_NO_NODE
async_schedule_domain(func, data, dom)    → 指定域
async_schedule_dev(func, dev)             → dev_to_node(dev) 就近调度
async_schedule_dev_domain(func, dev, dom) → 二者组合
（以上均为 async.h:52-110 的 inline 包装，落点同一个函数）
        │
        ▼
async_schedule_node_domain(:165)：
  ① kzalloc(GFP_ATOMIC)   ← 允许原子上下文调用
  ② ★过载回落：!entry || entry_count > MAX_WORK(32768)
       → 不排队，同步就地执行 func(data, newcookie) 并直接返回
       （注释原话 "low on memory.. run synchronously"——内存耗尽/
         积压过深时优雅退化，绝不失败）
  ③ 持 async_lock 分配 cookie（next_cookie++）、
     list_add_tail 入 domain->pending（+ registered 域再入 global）
  ④ queue_work_node(node, system_unbound_wq, &entry->work)
```

执行体 `async_run_entry_fn`（:115-141）：`pr_debug("calling  %lli_%pS @ %i\n", ...)` → `entry->func(entry->data, entry->cookie)` → 摘双链 → `kfree` → `atomic_dec` → `wake_up(&async_done)`。与 init/main.c 的 `calling  %pS @ %i` 报告格式同族——boot 分析工具（bootgraph）两者通吃。

## 四、汇合路径：一个不等式搞定全部（async.c:267-279）

```c
void async_synchronize_cookie_domain(async_cookie_t cookie,
				     struct async_domain *domain)
{
	wait_event(async_done, lowest_in_progress(domain) >= cookie);
}
```

```
async_synchronize_full()            → cookie = ∞，domain = NULL（全链）
async_synchronize_full_domain(dom)  → cookie = ∞，指定域
async_synchronize_cookie(c)         → 缺省域
async_synchronize_cookie_domain(c,dom) → 原语（其余三个全是它的马甲）

数学：lowest_in_progress(domain) 返回链表头的 cookie（最小未完成）。
     "最小未完成 >= 我的 cookie" ⟺ 所有 cookie <= 我的都已退休
     （乱序 CPU 的"顺序退休"窗口指针！）
```

醒着的人只有一件事：等 `async_done` 队列上的 `wake_up`——每个 entry 完成都会广播，等待者重算不等式决定是否继续睡。

## 五、域的两种注册身份（async.h:18-36）

```c
struct async_domain {
	struct list_head pending;
	unsigned registered:1;      /* 参与全局汇合与否 */
};

#define ASYNC_DOMAIN(_name)          /* registered = 1 */
#define ASYNC_DOMAIN_EXCLUSIVE(_name) /* registered = 0，注释原话：
	"domain is free to go out of scope as soon as all pending work is
	 complete, this domain does not participate in async_synchronize_full" */
```

EXCLUSIVE 域**不进 `async_global_pending`**——`async_synchronize_full()` 看不见它；域对象可以随其属主（如模块）一起离开作用域。实例闭环：固件加载的 `fw_cache_domain`（`ASYNC_DOMAIN_EXCLUSIVE(fw_cache_domain)`，见 [driver-model/04](../../driver-model/docs/04-firmware加载与request_firmware生命周期.md)）——suspend 缓存用 `async_synchronize_full_domain(&fw_cache_domain)` 自己汇合，不搅局全局屏障。

## 六、initcall 层级的 async 编排（init.h + main.c + vmlinux.lds.h）

### 6.1 `s` 段的链接序机关（vmlinux.lds.h:908-911 逐字）

```
#define INIT_CALLS_LEVEL(level)					\
	__initcall##level##_start = .;				\
	KEEP(*(.initcall##level##.init))			\   ← 本层普通段
	KEEP(*(.initcall##level##s.init))			\   ← ★s 段殿后
```

每个层级里 `core_initcall_sync`（`.initcall1s`）排在所有 `core_initcall`（`.initcall1`）**之后**：普通 initcall 里 `async_schedule` 完就返回（快），需要"等本层 async 尘埃落定"的代码用 `_sync` 变体注册——同层内天然屏障。

### 6.2 do_initcalls 的朴素与 1439 行的深意（main.c）

```c
do_initcalls(:1297) → 8 级循环 → do_initcall_level(:1282)
                     → do_one_initcall(initcall_from_entry(fn))
```

6.6 的主循环**没有任何 async 调用**——汇合责任全部在两处：① 各 `_sync` initcall 自行决定（async.c 头注释的建议：*"need to do a full synchronization with the async_synchronize_full() function, before returning from their init function"*）；② **总闸门**（main.c:1437-1439）：

```c
kernel_init_freeable();
/* need to finish all async __init code before freeing the memory */
async_synchronize_full();
system_state = SYSTEM_FREEING_INITMEM;
```

★**深层理由**：异步 worker 执行的代码本体（initcall 函数与其 `__init` 依赖）住在 `.init` 段——不等全部退休就 `free_initmem()` 会让还在跑的 worker **执行已释放的内存**。这是"异步初始化"与"`__init` 内存回收"之间不可协商的时序约束。

### 6.3 do_one_initcall 的卫生检查（main.c:1222-1252）

```c
if (preempt_count() != count) {
	sprintf(msgbuf, "preemption imbalance ");   /* 抢占计数失衡 */
	preempt_count_set(count);
}
if (irqs_disabled()) {
	strlcat(msgbuf, "disabled interrupts ", sizeof(msgbuf));  /* 带着关中断返回 */
	local_irq_enable();
}
WARN(msgbuf[0], "initcall %pS returned with %s\n", fn, msgbuf);
```

另：`initcall_blacklist=` cmdline（:1104-1171）按函数名拉黑（`-EPERM`）；`initcall_debug`（:787-788，core_param）注册 tracepoint 回调打印 **"calling  %pS @ %i"**（:1178）与 **"initcall %pS returned %d after %lld usecs"**（:1188）——`@PID` 一眼识别 async worker（PID 是 kworker 的）。bootgraph/initcall graph 均以此为原料。

## 七、异步探测：三层开关与一条警戒线

### 7.1 probe_type 三值（include/linux/device/driver.h:38-50）

```c
enum probe_type {
	PROBE_DEFAULT_STRATEGY,      /* 同步异步皆可 */
	PROBE_PREFER_ASYNCHRONOUS,   /* 慢设备主动加入 */
	PROBE_FORCE_SYNCHRONOUS,     /* 顺序敏感者强制同步 */
};
/* 头注释："the end goal is to switch the kernel to use asynchronous
   probing by default, so annotating drivers with
   %PROBE_PREFER_ASYNCHRONOUS is a temporary measure" */
```

### 7.2 判定函数与两路分发（drivers/base/dd.c）

```c
static bool driver_allows_async_probing(struct device_driver *drv)  /* :868 */
{
	switch (drv->probe_type) {
	case PROBE_PREFER_ASYNCHRONOUS: return true;
	case PROBE_FORCE_SYNCHRONOUS:   return false;
	default:
		if (cmdline_requested_async_probing(drv->name))  /* :850 */
			return true;
		if (module_requested_async_probing(drv->owner))  /* module.h:739 */
			return true;
		return false;
	}
}
```

**设备侧重扫**（device_attach）：同步 pass 未绑定且 `have_async` → `async_schedule_dev(__device_attach_async_helper, dev)`（:1053；helper :964 内 `check_async=true, want_async=true` 再扫一遍——两遍扫的注释原话 :902-907：*"we can't shoot asynchronous..."* 不能对同步 pass 里已绑一半的设备开火）。

**驱动侧重扫**（`__driver_attach` :1156）：match 后 `driver_allows_async_probing(drv)` → 持 device_lock 占坑 `dev->p->async_driver = drv` → `async_schedule_dev(__driver_attach_async_helper, dev)`（:1194-1207，helper :1139）。

### 7.3 三层开关矩阵

```
层 ① 驱动代码：  .probe_type = PROBE_PREFER_ASYNCHRONOUS
层 ② cmdline：   driver_async_probe=drv1,drv2 或 driver_async_probe=*
                 （dd.c:866 __setup；"*" 打开 async_probe_default :862）
层 ③ 模块参数：  modprobe xxx async_probe=1
                 （module/main.c:2772-2782 unknown_module_param_cb →
                  mod->async_probe_requested）
```

### 7.4 ★警戒线：async 上下文禁止同步 request_module（kmod.c:143-148）

```c
/*
 * We don't allow synchronous module loading from async.  Module
 * init may invoke async_synchronize_full() which will end up
 * waiting for this task which already is waiting for the module
 * loading to complete, leading to a deadlock.
 */
WARN_ON_ONCE(wait && current_is_async());
```

死锁环：async worker `request_module(wait=true)` → modprobe 加载新模块 → 新模块 init 调 `async_synchronize_full()` → **等所有 async 包括这个卡在 modprobe 里的 worker 自己**。`current_is_async()`（async.c:299-305）判定法：`worker->current_func == async_run_entry_fn`。驱动侧的自查调用者：DRM intel_fbdev（:619）与 cxlflash（:697）用它在 probe 里走不同路径。

## 八、观测实战

```bash
# 1) boot 全程 initcall 计时（含 async worker 的调用）
dmesg | grep -E 'calling|initcall.*returned'
# async worker 的特征：@ 后面是 kworker 的 PID，
# 且 initcall 名形如 "%lli_%pS"（cookie_函数名，async_run_entry_fn 的 pr_debug）

# 2) 强制指定驱动异步探测（免改代码的实验通道）
#    内核命令行：driver_async_probe=e1000e,nvme
#    或全开：driver_async_probe=*

# 3) 模块级
modprobe mydrv async_probe=1

# 4) bootgraph 可视化（采 dmesg 时间戳）
dmesg > boot.log && \
  scripts/bootgraph.pl boot.log > boot.svg   # 源码树自带工具

# 5) initcall 拉黑（定位问题 initcall 的二分法）
initcall_blacklist=init_i915,foo_bar_init

# 6) 异步积压上限观察（过载回落阈值 MAX_WORK=32768 极难触达；
#    正常系统 entry_count 长期接近 0——每完成即摘链递减）
```

## 九、思考题

1. `lowest_in_progress` 只看链表头就敢声称"最小未完成"——哪条不变式支撑这个断言？（提示：cookie 分配序 = 入队序 = 双链单调性；谁维护它？）
2. 过载回落把 func 改成同步执行——为什么这不是语义破坏？（提示：async API 的契约是"完成顺序"而非"执行时机"；cookie 照发意味着什么？）
3. `ASYNC_DOMAIN_EXCLUSIVE` 的域对象为什么可以随模块离开作用域，而 registered 域不行？（提示：async_global_pending 里还挂着谁的指针？）
4. `async_synchronize_full()` 在 free_initmem 前的总闸门为什么不可协商？如果某 async worker 里又 `async_schedule` 了新工作，闸门语义还成立吗？（提示：新工作的 cookie 更大；`lowest >= ∞` 永远为假会怎样——再读 wait_event 的条件。）
5. 两遍扫描（同步 pass + async pass）里，`dev->p->async_driver` 的"占坑"为什么必须在 device_lock 下做？（提示：device_attach 与 driver_attach 是两条独立入口，同一设备可能同时被扫两遍。）
6. `WARN_ON_ONCE(wait && current_is_async())` 只警告不阻止——如果真发生了死锁，系统会以什么形态僵住？（提示：modprobe 用户态进程、kworker、以及各自的等待队列；谁等谁？）

---

> 关联：[04-initcall 与模块初始化机制](04-initcall与模块初始化机制.md)（层级与段机制基础）｜[02-start_kernel 详解](02-start_kernel详解.md)（do_basic_setup 位置）｜[driver-model/02-总线-设备-驱动匹配](../../driver-model/docs/02-总线-设备-驱动匹配.md)（really_probe 同步主干）｜[driver-model/04-firmware 加载](../../driver-model/docs/04-firmware加载与request_firmware生命周期.md)（fw_cache_domain 的 EXCLUSIVE 实例）｜[workqueue 分册](../../workqueue/README.md)（system_unbound_wq 与 worker 结构）｜[module/01-模块布局与加载器](../../module/docs/01-模块布局与加载器.md)（kmod 并发闸）
