# ww_rt_mutex：PREEMPT_RT 变体与多重包含惯用法（Linux 6.6）

> 阅读目标：吃透 ww_mutex 在 PREEMPT_RT 下的底座替换——`ww_rt_mutex.c`（101 行）如何用**多重包含惯用法**（`#define RT_MUTEX_BUILD_MUTEX` + `#include "rtmutex.c"`——与 [trace/13](../trace/docs/13-内核代码生成惯用法.md) 的 X-macro 家族同族）复用 1833 行的 rtmutex 模板；`build_ww_mutex()` 的编译期分派（wound/wait 逻辑仅在 ww 构建时编入）；`rt_mutex_slowlock` 的 PI（优先级继承）链如何与 wound/wait 的 `-EDEADLK` 回退共存。全部对照 v6.6（kernel/locking/ww_rt_mutex.c、kernel/locking/rtmutex.c、kernel/locking/spinlock_rt.c、include/linux/ww_mutex.h）。前置：[ww-mutex](ww-mutex.md)（非 RT 基线的完整机制）。

---

## 一、底座替换：WW_MUTEX_BASE 的条件编译

```c
/* include/linux/ww_mutex.h:30-37 */
#ifndef CONFIG_PREEMPT_RT
#define WW_MUTEX_BASE         mutex
#define ww_mutex_base_init(l,n,k)  __mutex_init(l,n,k)
#define ww_mutex_base_is_locked(b) mutex_is_locked((b))
#else
#define WW_MUTEX_BASE         rt_mutex
#define ww_mutex_base_init(l,n,k)  __rt_mutex_init(l,n,k)
#define ww_mutex_base_is_locked(b) rt_mutex_base_is_locked(&(b)->rtmutex)
#endif
```

```
非 RT：struct ww_mutex { struct mutex base; struct ww_acquire_ctx *ctx; }
   RT：struct ww_mutex { struct rt_mutex base; struct ww_acquire_ctx *ctx; }

★上层 API 完全不变（ww_mutex_lock/ww_mutex_unlock 对用户透明）——
  底座差异被 WW_MUTEX_BASE 宏全部吸收
```

## 二、多重包含：rtmutex.c 的四重身份

```
kernel/locking/rtmutex.c（1833 行）是一个"模板"文件——被四个
wrapper 各自定义不同宏后 #include：

| wrapper 文件 | 定义的宏 | 生成的产物 |
| :--- | :--- | :--- |
| rtmutex_api.c | RT_MUTEX_BUILD_MUTEX | 标准 rtmutex API |
| ww_rt_mutex.c | RT_MUTEX_BUILD_MUTEX + WW_RT | ww_mutex 的 RT 实现 |
| spinlock_rt.c | RT_MUTEX_BUILD_SPINLOCKS | RT 下的 spinlock 替身 |
| rwbase_rt.c | （类似） | RT 下的 rwsem 基座 |

★这正是 [trace/13](../trace/docs/13-内核代码生成惯用法.md) 的
"多重包含式"（TRACE_HEADER_MULTI_READ）在 locking 子系统的应用——
一份清单（rtmutex 的核心算法）→ 多种产物（四个锁 API）
```

## 三、build_ww_mutex()：编译期分派

```c
/* kernel/locking/rtmutex.c:31-32/:60 */
#ifndef WW_RT
# define build_ww_mutex()  (false)   /* 普通 rtmutex 构建——wound/wait 不编入 */
#else
# define build_ww_mutex()  (true)    /* ww 构建——wound/wait 逻辑编入 */
#endif
```

rtmutex.c 中所有 ww 相关代码都用 `build_ww_mutex() && ww_ctx` 门控：

```c
/* rtmutex.c:1603-1607 —— slowlock 的等待循环内 */
if (build_ww_mutex() && ww_ctx) {
	ret = __ww_mutex_check_kill(rtm, waiter, ww_ctx);
	if (ret)          /* -EDEADLK：事务被 wound → 回退 */
		break;
}
```

★`build_ww_mutex()` 是编译期常量（false/true）——非 ww 构建时整个 `if` 被**死代码消除**，零运行时开销。这与 tracepoint 的 `static_key_false` 语义对齐（[trace/02]），但用**编译期**而非运行期实现。

## 四、ww_rt_mutex.c 的 lock 路径（:48-86）

```c
static int __sched
__ww_rt_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ww_ctx,
		   unsigned int state, unsigned long ip)
{
	struct rt_mutex *rtm = &lock->base;

	if (ww_ctx) {
		if (unlikely(ww_ctx == READ_ONCE(lock->ctx)))
			return -EALREADY;
		/* 注释原话："Reset the wounded flag after a kill.
		   No other process can race and wound us here, since
		   they can't have a valid owner pointer if we don't
		   have any locks held." */
		if (ww_ctx->acquired == 0)
			ww_ctx->wounded = 0;
	}

	/* 快路径：cmpxchg 空闲→current */
	if (likely(rt_mutex_cmpxchg_acquire(&rtm->rtmutex, NULL, current))) {
		if (ww_ctx)
			ww_mutex_set_context_fastpath(lock, ww_ctx);
		return 0;
	}

	/* 慢路径：rt_mutex_slowlock（含 PI 链 + wound/wait 检查） */
	ret = rt_mutex_slowlock(&rtm->rtmutex, ww_ctx, state);
	...
}
```

## 五、PI 链与 wound/wait 的共存

```
rt_mutex_slowlock 的等待循环：
  ① try_to_take_rt_mutex（尝试获取）
  ② signal/timeout 检查
  ③ __ww_mutex_check_kill（wound/wait 判定——如被 wound 则 -EDEADLK 退出）
  ④ rt_mutex_slowlock_block（真正睡眠——进入 PI 等待）

PI（优先级继承）在 ④ 中发生：
  当前任务作为 waiter 挂入 rt_mutex 的 waiter 树（按 priority 排序）
  → 任务优先级传播给锁持有者（boosting）
  → 持有者以高优先级尽快释放
  → waiter 被唤醒后重新尝试 ①

★wound/wait 与 PI 的交互：
  wound = "老者要求新者释放"——RT 下这不仅是标记，还会通过 PI
  让老者（若优先级更高）**实际加速**新者的释放
  （新者的 lock 持有会被老者的优先级继承 boosting 促使尽快完成）
```

## 六、RT spinlock 家族对照

```
spinlock_rt.c 的头注释原话（:10-18）：
  "The RT substitutions explicitly disable migration and take
   rcu_read_lock() across the lock held section."

| 家族 | 非 RT | RT | 语义差异 |
| :--- | :--- | :--- | :--- |
| spinlock | qspinlock | rt_mutex 包装 | RT 版可睡眠（PI 参与） |
| rwlock | qrwlock | rwbase_rt | 同上 |
| mutex | mutex | rt_mutex 包装 | 语义最接近 |
| ww_mutex | mutex 底座 | rt_mutex 底座 | wound/wait 逻辑不变 |
```

## 七、观测实战

```bash
# 1) 当前构建是否 RT
uname -v | grep -i preempt     # "PREEMPT_RT" 字样
grep PREEMPT_RT /boot/config-$(uname -r)

# 2) rtmutex 的 PI 链观测
cat /proc/lockdep_stats | grep rt_mutex

# 3) ww_ctx 的 -EDEADLK 在 RT 下的行为（DRM 用例）
bpftrace -e 'kprobe:__ww_mutex_check_kill { @[comm] = count(); }'

# 4) RT 下 spinlock 变成可睡眠的验证
#   spinlock_rt 的持有者可被抢占（而非非 RT 的禁抢占）
cat /sys/kernel/debug/sched/preempt
```

## 八、思考题

1. `WW_MUTEX_BASE` 条件编译让 `struct ww_mutex` 的**布局**在 RT 与非 RT 间不同——模块跨构建的兼容性如何？（提示：PREEMPT_RT 与非 RT 是**互斥的内核构建**，不存在混合——模块 ABI 天然隔离。）
2. `build_ww_mutex()` 返回编译期常量——为什么不做成 `IS_ENABLED(CONFIG_*)` 风格？（提示：WW_RT 不是 CONFIG 选项——它由 **wrapper 文件的 #define** 决定（同一内核中 rtmutex.c 被两种方式包含）。）
3. PI boosting 加速 wound 者释放的机制——如果 wound 者优先级**不高于**新者呢？（提示：PI 只在优先级反转时 boosting；同优先级下 wound/wait 靠**回退重做**而非加速。）
4. RT spinlock "可睡眠"对中断处理的影响？（提示：spinlock_rt 的持有者可被抢占——中断上下文必须用 raw_spinlock（不进入 RT 替换）。）
5. rtmutex.c 被 include 四次——编译后的二进制大小是"膨胀"了吗？（提示：四个 wrapper 各自 `#include` 后生成**独立的 .o**——但 `static` 函数与 `__always_inline` 保证无符号冲突。）
6. ww_rt_mutex.c 的 `ww_mutex_trylock` 与非 RT 版的差别？（提示：底座换成 `__rt_mutex_trylock`——但 wound/wait 的 trylock 语义（不排 GP/不等 stamp）不变。）

---

> 回看：[ww-mutex](ww-mutex.md)（非 RT 基线的完整 wound/wait 机制）｜[mutex](mutex.md)（非 RT 底座）｜[trace/13-内核代码生成惯用法](../trace/docs/13-内核代码生成惯用法.md)（多重包含的 TRACE_EVENT 本家——rtmutex 的四重包含同族）｜[concession](concession.md)（让步家族——ww 是第四形态）
