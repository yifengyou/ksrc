# KFENCE：采样式内存错误检测（Linux 6.6）

> 阅读目标：吃透三巨头的第三员——**采样 + 页保护**的低开销检测器（KASAN=全量 shadow、KCSAN=竞态、KFENCE=每个采样间隔看住**一个**对象）：池布局（`(255+1)×2 页`、**奇偶页**分工对象/红区）；★采样门的双层设计（static key + atomic gate——`toggle_allocation_gate` 的"武装→等一次分配→卸"协议与 ★"2 IPIs to all CPUs" 注释原文）；对象布局的**左右随机**（`random_right_allocate`）；三态状态机与 `kfence_metadata`（含 ★trylock 的递归分配逃生注释——"printk calls kmalloc() which ends up in kfence_alloc() and tries to grab the same object that we're reporting on"）；故障分类器（奇偶页判定 + 双侧邻居距离 + UAF 再分配竞态注释）；canary 双侧填充；四种报告格式。全部对照 v6.6（mm/kfence/ 四文件 + include/linux/kfence.h + lib/Kconfig.kfence）当轮核对。前置：[KASAN_VMALLOC](../kasan/kasan-vmalloc.md)｜[kcsan-encoding](../../locking/kcsan-encoding.md)。

---

## 一、三巨头的分工矩阵

| | KASAN | KCSAN | KFENCE |
| :--- | :--- | :--- | :--- |
| 抓什么 | 越界/用后释放（**访问级**） | 数据竞态 | 越界/用后释放/双重释放（**页错误级**） |
| 手段 | 1:8 shadow 全量插桩 | watchpoint 采样插桩 | **页保护 + 采样**（无插桩！） |
| 常驻开销 | 内存（shadow）+ 每访问 | 每访问（近零）+ 槽 | ★**~0**（静态键关着）+ 池 2MB |
| 检测率 | 全量 | 按间隔采样 | 每间隔**一个**对象 |

——KFENCE 的哲学：**把"逐访问检查"降为"逐页错误检查"**——硬件 MMU 当免费检测器，代价是采样窗口内只有一个对象受保护。

## 二、池布局（include/linux/kfence.h:27 + core.c:1161+）

```c
#define KFENCE_POOL_SIZE ((CONFIG_KFENCE_NUM_OBJECTS + 1) * 2 * PAGE_SIZE)
```

```
__kfence_pool（memblock 预留，2MB 级）
 ┌────────┬────────┬────────┬────────┬─── ... ───┐
 │ 对象页0 │ 红区页  │ 对象页1 │ 红区页  │  …（255 个对象页+1 尾红区）
 └────────┴────────┴────────┴────────┴─── ... ──┘
   even     odd      even     odd            ★page_index 奇偶即角色

对象页内：
 [左侧 canary…][对象（随机靠左/右）][…右侧 canary]
 红区页 = unmapped（触之即 #PF → kfence_handle_page_fault）
```

★奇偶页是故障分类的地基（§六）：`page_index % 2` 非零 = 红区 = **越界**；零 = 对象页（已释放态）= **用后释放**。

## 三、★采样门：双层与武装-等待-卸协议（core.c:811-840）

```c
static void toggle_allocation_gate(struct work_struct *work)
{
	if (!READ_ONCE(kfence_enabled))
		return;

	atomic_set(&kfence_allocation_gate, 0);       /* ①开门：本间隔的一次 */
#ifdef CONFIG_KFENCE_STATIC_KEYS
	static_branch_enable(&kfence_allocation_key); /* ②武装静态键 */

	wait_event_idle(allocation_wait,              /* ③★等分配消费掉 gate */
			atomic_read(&kfence_allocation_gate));

	static_branch_disable(&kfence_allocation_key);/* ④卸装 */
#endif
	queue_delayed_work(system_unbound_wq, &kfence_timer,
			   msecs_to_jiffies(kfence_sample_interval));  /* ⑤重排 */
}
```

头注释原文（★设计权衡的完整自述）：

```
 * We need to use a work queue (rather than a simple timer), since enabling
 * and disabling a static key cannot be done from an interrupt.
 *
 * Note: Toggling a static branch currently causes IPIs, and here we'll end
 * up with a total of 2 IPIs to all CPUs. If this ends up a problem in
 * future (with more aggressive sampling intervals), we could get away with
 * a variant that avoids IPIs, at the cost of not immediately capturing
 * allocations if the instructions remain cached.
```

**分配侧的双层门**（include/linux/kfence.h:111-124）：

```c
static __always_inline void *kfence_alloc(struct kmem_cache *s, size_t size, gfp_t flags)
{
	if (!static_branch_unlikely(&kfence_allocation_key))  /* 第一层：周期外=零开销 */
		return NULL;
	if (likely(atomic_read(&kfence_allocation_gate)))     /* 第二层：本周期已被消费 */
		return NULL;
	return __kfence_alloc(s, size, flags);                /* 消费并唤醒 timer 的 wait */
}
```

③ 的唤醒链：`__kfence_alloc` 消费 gate 后经 **irq_work**（`wake_up_kfence_timer_work`，core.c:804-808）安全地从中断上下文叫醒 timer 任务——静态键只在"有活干"的窗口开着。

## 四、对象布局：左右随机（kfence_guarded_alloc，core.c:390-475）

```c
	const bool random_right_allocate = get_random_u32_below(2);
	...
	if (random_right_allocate) {
		/* Allocate on the "right" side, re-calculate address. */
		meta->addr += PAGE_SIZE - size;
		meta->addr = ALIGN_DOWN(meta->addr, cache->align);
	}
```

- **左放**：对象贴页首——右侧 canary+红区页接住**右越界**；**右放**：贴页尾——左侧接住左越界——随机化让两个方向的 partial-OOB 都有机会被抓
- RNG 未就绪注释（:437-443 原文）："for allocations made before RNG initialization, will always return zero...The only downside is that the out-of-bounds accesses detected are deterministic for such allocations."——早开 KFENCE 仍值得（确定性放置只是定向）
- `slab->slab_cache = cache; slab->objects = 1`（:462-466）——**伪装成 slab 对象**：kfree/virt_to_slab 等通用路径不需特判即可工作
- canary 填充（`set_canary`，:326-343）：对象两侧的页内空隙用 `KFENCE_CANARY_PATTERN_U64` 填——**页内**越界（不触发 #PF 的部分）靠 canary 兜底（free 时逐字节校验，`check_canary_byte` 报 KFENCE_ERROR_CORRUPTION）

## 五、状态机与元数据（kfence.h:37-104 + core.c:277-306）

```c
enum kfence_object_state {
	KFENCE_OBJECT_UNUSED,		/* Object is unused. */
	KFENCE_OBJECT_ALLOCATED,	/* Object is currently allocated. */
	KFENCE_OBJECT_FREED,		/* Object was allocated, and then freed. */
};

struct kfence_metadata {
	struct list_head list;		/* freelist 节点 */
	struct rcu_head rcu_head;	/* 延迟释放 */
	raw_spinlock_t lock;		/* ★竞态三方注释："since the following
					   may execute concurrently: __kfence_alloc(),
					   __kfence_free(), kfence_handle_page_fault()" */
	enum kfence_object_state state;	/* WRITE_ONCE 与三处 READ_ONCE 配对 */
	unsigned long addr;		/* "cannot be calculated from size, because
					   of alignment requirements"+
					   "Invariant: ALIGN_DOWN(addr, PAGE_SIZE) is
					   constant" */
	...
	struct kfence_track alloc_track, free_track;  /* pid/cpu/ts/栈 */
};
```

★**trylock 递归逃生**（core.c:417-431 注释原文）——KFENCE 最精彩的工程细节：

```
	 * This is extremely unlikely -- we are reporting on a use-after-free,
	 * which locked meta->lock, and the reporting code via printk calls
	 * kmalloc() which ends up in kfence_alloc() and tries to grab the same
	 * object that we're reporting on. While it has never been observed,
	 * lockdep does report that there is a possibility of deadlock. Fix it
	 * by using trylock and bailing out gracefully.
```

——**报告路径自己会分配内存**（printk→kmalloc→kfence_alloc→恰好抢到正在报告的对象）——锁序闭环死锁；trylock+退回 freelist 优雅化解。

## 六、故障分类器（kfence_handle_page_fault，core.c:1161-1232）

```
addr 落池外？→ return false（交还内核正常 #PF 流）
运行时被禁用？→ kfence_unprotect(addr) 放行（"... unprotect and proceed"）
page_index 奇（红区页）→ 越界：
  ★双侧邻居判定：addr-PAGE_SIZE 与 addr+PAGE_SIZE 各找 metadata
  （注释 "Data race ok; distance calculation approximate."）
  取距离更近者为 to_report → KFENCE_ERROR_OOB
  （注释："If the object was freed before we took the lock we can still
   report this as an OOB -- the report will simply show the stacktrace of
   the free as well."）
page_index 偶（对象页）→ KFENCE_ERROR_UAF：
  （注释："We may race with __kfence_alloc(), and it is possible that a
   freed object may be reallocated. We simply report this as a
   use-after-free, with the stack trace showing the place where the
   object was re-allocated."）
  meta->unprotected_page = addr（解锁红区让报告路径能走）
```

## 七、报告（mm/kfence/report.c:217-244）

```
pr_err("BUG: KFENCE: out-of-bounds %s in %pS\n\n", ...)
  "Out-of-bounds %s at 0x%p (%luB %s of kfence-#%td):"   ← 带方向与距离
"BUG: KFENCE: use-after-free %s in %pS\n\n"
"BUG: KFENCE: memory corruption in %pS\n\n"              ← canary 型
"BUG: KFENCE: invalid %s in %pS\n\n"                     ← invalid-free
每份含：当前访问栈 + alloc 栈 + free 栈（三栈合璧）+ 对象编号/大小
```

另有 `alloc_covered_add(alloc_stack_hash,...)`（core.c:458）——按分配栈哈希记账覆盖面（stats 的 alloc_covered 指标——防同一热点分配栈独占采样）。

## 八、集成与配置

```
分配挂点（双分配器）：mm/slab.c:3225/:3466 + mm/slub.c:3464/:3950
  ——slab_alloc 路径先问 kfence_alloc（§三的双层门），中了直接返回池对象
释放/校验：kfree→is_kfence_address→__kfence_free；ksize→kfence_ksize
  ★is_kfence_address 的性能契约（kfence.h:46-49 注释）："This function may
  be used in fast-paths, and is performance critical...we want to avoid
  introducing another load and therefore need to keep KFENCE_POOL_SIZE a
  constant"
shutdown_cache 的"僵尸分配"（kfence.h:88-96）："making allocated objects
  'zombie allocations'. Objects may then still be used or freed (which is
  handled gracefully), but usage will result in showing KFENCE error
  reports"

Kconfig（lib/Kconfig.kfence）：
  KFENCE_SAMPLE_INTERVAL   default 100（ms；0=启动即禁）
  KFENCE_NUM_OBJECTS       default 255
  KFENCE_DEFERRABLE        （timer 可延迟——省空闲唤醒）
  KFENCE_STATIC_KEYS       （静态键优化）
  KFENCE_STRESS_TEST_FAULTS（故障注入）
```

## 九、观测实战

```bash
# 1) 运行时开关（sysctl）
cat /sys/kernel/debug/kfence/stats          # counters 全表（BUGs/分配/覆盖/跳过）
sysctl kernel.kfence.sample_interval=10     # 10ms 激进采样

# 2) 报告样貌
dmesg | grep -A16 'BUG: KFENCE'             # 三栈合璧的报告

# 3) 采样命中率调优
#   alloc_covered 低 + 同栈重复 → 提高间隔的随机化或降低 interval
#   SKIP_CAPACITY 高（core.c:409-411 计数）→ 池耗尽（255 活对象上限）

# 4) 与 KASAN 的共存
#   可同开（不同机制不同目标）；KFENCE 常开在生产（开销~0），KASAN 实验室
grep -E 'CONFIG_KFENCE|CONFIG_KASAN' /boot/config-$(uname -r)

# 5) 自检
modprobe kfence_test                        # CONFIG_KFENCE_KUNIT_TEST
```

## 十、思考题

1. 采样门为什么要等 `wait_event_idle`（③）而不直接武装后定时卸？（提示：不等的话静态键空转整个间隔——白付 IPI 与缓存污染；等一次真实消费保证**每次武装必有收获**。）
2. 池布局为什么奇偶交替而不是"对象页全排前面"？（提示：故障分类靠 page_index 奇偶**一次除法**完成；若连续排列需查元数据区间才能分辨红区/对象——fast-path 契约（kfence.h:46）不容许。）
3. trylock 逃生（§五）与"报告路径不许拿被报告对象的锁"是同一件事的两面吗？（提示：前者是**取对象**（freelist 侧）的递归，后者是**锁**的递归——printk 的 kmalloc 可能撞上同对象同锁；两个 trylock 各断一条环。）
4. 左右随机放置为什么"RNG 未就绪时仍然值得开"？（提示：确定性放置只牺牲方向覆盖的一半（固定左/右）；早开捕捉 boot 期的早期 bug 收益大于方向损失——工程取舍原文自述。）
5. KFENCE 抓不住什么？（提示：窗口外的绝大多数对象；页内 canary 检查只在 free 时发生——**活着期间**的页内越界要等到 free 才报；无插桩=抓不到"读了错值但地址合法"类。）
6. `slab->slab_cache = cache; slab->objects = 1` 的伪装让哪些通用路径免特判？（提示：kfree/virt_to_head_page/ksize/SLUB debug——KFENCE 对象"长得像"单对象 slab 页；代价是这页永进不了伙伴系统的常规 slab 管理。）

---

> 回看：[KASAN_VMALLOC](../kasan/kasan-vmalloc.md)（全量 shadow 的对照极）｜[kcsan-encoding](../../locking/kcsan-encoding.md)（采样插桩的对照极——同为采样，一个查访问一个查竞态）｜[sanitizers](../../locking/sanitizers.md)（三巨头总览）｜[debug-adversarial](../../locking/debug-adversarial.md)（上刑家族谱系——KFENCE 是"零成本常开刑"）｜[buddy](../buddy/buddy.md)（池预留的 memblock/伙伴语境）｜[mm/README](../README.md)（分册导读）
