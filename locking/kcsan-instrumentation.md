# KCSAN 插桩面：编译器钩子与手工断言（Linux 6.6）

> 阅读目标：补全 [kcsan-encoding](kcsan-encoding.md) 只覆盖 memset 尾部的缺口——**编译器侧**（scripts/Makefile.kcsan：`-fsanitize=thread` 的 TSAN 复用 + `tsan-compound-read-before-write` 的 RMW 旗标 + `tsan-distinguish-volatile` + ★**func-entry-exit 钩子与 WEAK_MEMORY 的绑定**）；**__tsan 家族全表**（read/write/read_write × 5 尺寸的 15+15 别名 + range 变体 + volatile 族的 LWN 引证注释与 `is_atomic` 启发式）；★`__tsan_func_exit` 的重排收尾（"write without a barrier (supposed release) was last access" 的补查）；**手工 API**（`kcsan_check_*`/disable-enable 的深度计数/scoped access 的 cleanup 属性）与 **ASSERT_EXCLUSIVE 家族**（refcount 文档例）；permissive 白名单的豁免逻辑。全部对照 v6.6 当轮核对。前置：[kcsan-encoding](kcsan-encoding.md)（watchpoint 编码与检测协议）。

---

## 一、两层插桩全景

```
自动层：编译器在每个普通 load/store 前插入 __tsan_readN/__tsan_writeN 调用
        （RMW 指令插 __tsan_read_writeN；volatile 访问插 __tsan_volatile_*）
手工层：kcsan_check_read/write（指定区域的显式检查）
        ASSERT_EXCLUSIVE_*（"此处不该有并发"的断言——比检查更强）
两层汇于 check_access(ptr, size, type, ip)——[kcsan-encoding 章] 的三岔口
```

## 二、构建旗标（scripts/Makefile.kcsan）

```make
kcsan-cflags := -fsanitize=thread -fno-optimize-sibling-calls \
	$(call cc-option,$(call cc-param,tsan-compound-read-before-write=1),\
	      $(call cc-option,$(call cc-param,tsan-instrument-read-before-write=1))) \
	$(call cc-param,tsan-distinguish-volatile=1)
ifdef CONFIG_CC_IS_GCC
	kcsan-cflags += -Wno-tsan
endif
ifndef CONFIG_KCSAN_WEAK_MEMORY
	kcsan-cflags += $(call cc-option,--param tsan-instrument-func-entry-exit=0)
endif
```

★三要点：
- **`-fsanitize=thread` 的复用**——KCSAN 不是新 sanitizer 模式，是 clang TSAN 插桩 + 内核自己的运行时（__tsan_* 被内核接管）
- `tsan-compound-read-before-write=1`——RMW（读改写）指令插 **`__tsan_read_writeN`**（COMPOUND 标志的来源）
- ★**func-entry-exit 默认关**——只有 `CONFIG_KCSAN_WEAK_MEMORY`（重排模拟）才让编译器插 `__tsan_func_entry/exit`——钩子开销与功能精确绑定

## 三、__tsan 家族全表（kernel/kcsan/core.c:985-1141）

### 3.1 基础族（DEFINE_TSAN_READ_WRITE × {1,2,4,8,16}）

```c
void __tsan_read##size(void *ptr)        { check_access(ptr, size, 0, _RET_IP_); }
void __tsan_write##size(void *ptr)       { check_access(ptr, size, KCSAN_ACCESS_WRITE, _RET_IP_); }
void __tsan_read_write##size(void *ptr)  { check_access(ptr, size,
	                                     KCSAN_ACCESS_COMPOUND | KCSAN_ACCESS_WRITE, _RET_IP_); }
/* 每个 X##size 各配 __tsan_unaligned_X##size 的 __alias 别名 */
```

unaligned 别名的理由（:985-989 注释原文）："not all supported compiler versions distinguish aligned/unaligned accesses, but e.g. recent versions of Clang do. **We simply alias the unaligned version to the generic version**, which can handle both."

### 3.2 range 族（:1028-1041）

`__tsan_read_range(ptr, size)` / `__tsan_write_range`——供 `__tsan_memset/memmove/memcpy` 尾部（[kcsan-encoding 章] 已覆盖）与 GCC 旧版的无尺寸 range 插桩共用。

### 3.3 ★volatile 族（DEFINE_TSAN_VOLATILE_READ_WRITE × 5 尺寸）

头注释原文（:1043-1052）：

```
 * Use of explicit volatile is generally disallowed [1], however, volatile is
 * still used in various concurrent context, whether in low-level
 * synchronization primitives or for legacy reasons.
 * [1] https://lwn.net/Articles/233479/
 *
 * We only consider volatile accesses atomic if they are aligned and would
 * pass the size-check of compiletime_assert_rwonce_type().
```

```c
const bool is_atomic = size <= sizeof(long long) &&
		       IS_ALIGNED((unsigned long)ptr, size);
if (IS_ENABLED(CONFIG_KCSAN_IGNORE_ATOMICS) && is_atomic)
	return;                                        /* 原子化 volatile 直接跳过 */
check_access(ptr, size, is_atomic ? KCSAN_ACCESS_ATOMIC : 0, _RET_IP_);
```

★**volatile=疑似原子的启发式**：对齐且 ≤ 8 字节的 volatile 访问标 `KCSAN_ACCESS_ATOMIC`（模拟 READ_ONCE 语义——不参与 watchpoint，见 encoding 章的 should_watch）；其余 volatile 当普通访问查。

### 3.4 杂项

`__tsan_init`（:1141-1144——空实现，接口占位）；`__tsan_atomic_thread_fence/signal_fence`（:1277-1310——原子栅栏钩子，退化为屏障语义）。

## 四、★func_entry/exit：重排模拟的生命线（:1089-1138）

```
头注释两理由：
  "Function entry and exit are used to determine the validty of
   reorder_access. Reordering of the access ends at the end of the function
   scope where the access happened. This is done for two reasons:
   1. ... contain the required barriers but were missed.
   2. Simplifies generating the stack trace of the access."
```

```c
noinline void __tsan_func_entry(void *call_pc)
{
	if (!IS_ENABLED(CONFIG_KCSAN_WEAK_MEMORY))
		return;
	add_kcsan_stack_depth(1);          /* 深度计数——重排访问的"作用域标尺" */
}

noinline void __tsan_func_exit(void)
{
	...
	reorder_access = get_reorder_access(get_ctx());
	if (get_kcsan_stack_depth() <= reorder_access->stack_depth) {
		/*
		 * Access check to catch cases where write without a barrier
		 * (supposed release) was last access in function: because
		 * instrumentation is inserted before the real access, a data
		 * race due to the write giving up a c-s would only be caught if
		 * we do the conflicting access after.
		 */
		check_access(reorder_access->ptr, ...);   /* 出函数前的最后一查 */
		reorder_access->size = 0;                 /* 失效 */
		reorder_access->stack_depth = INT_MIN;
	}
out:
	add_kcsan_stack_depth(-1);
}
```

★exit 的补查动机：**插桩在真实访问之前**——若"无屏障的写"（本该是 release）是函数末访问，真实写之后的冲突要靠 exit 时再查一次才能撞上。

## 五、手工 API（include/linux/kcsan-checks.h）

| API | 行号 | 语义 |
| :--- | :--- | :--- |
| `__kcsan_check_access(ptr,size,type)` | :37 | 显式检查（type 手工给） |
| `kcsan_check_read/write(ptr,size)` | :302/:311 | 便捷包装（type=0/WRITE） |
| `kcsan_disable_current()` / `kcsan_enable_current()` | :74/:81 | **ctx 深度计数**——关闭/开启（可嵌套；boot 期默认关） |
| `kcsan_enable_current_nowarn()` | :82 | "Safe in uaccess regions."（uaccess 区豁免 warn 的变体） |
| `kcsan_begin/end_scoped_access(...)` | :172+ | **作用域检查**——区域全程按 scoped access 反复查 |
| `struct kcsan_scoped_access` | :131-155 | ★`__cleanup__(kcsan_end_scoped_access)` 属性——**作用域结束自动收** |

## 六、ASSERT_EXCLUSIVE 家族（断言比检查更强）

```
ASSERT_EXCLUSIVE_ACCESS(var)       —— 本访问点无任何并发访问（读写都禁）
ASSERT_EXCLUSIVE_WRITER(var)       —— 无并发写（读者随意）
ASSERT_EXCLUSIVE_READ(var)         —— 无并发读？
ASSERT_EXCLUSIVE_ACCESS_BITS(var, mask) —— 指定位段无并发访问
★各配 _SCOPED 变体（作用域全程断言——"if applicable, performs more
  thorough checking if a clear scope where no concurrent accesses are
  expected exists" :443-444）
```

文档例（:434-439 原文）——**refcount 复用检查的黄金场景**：

```c
	if (refcount_dec_and_test(&obj->refcnt)) {
		ASSERT_EXCLUSIVE_ACCESS(*obj);
		do_some_cleanup(obj);
		release_for_reuse(obj);
	}
```

——归零后到复用前，对象该处于"无人再看"的独占窗；任何并发访问= use-after-free 竞态。

## 七、permissive 白名单（kernel/kcsan/permissive.h）

```c
static __always_inline bool kcsan_ignore_address(const volatile void *ptr)
{
	/* "Data-racy bitops on current->flags are too common, ignore
	   completely for now." */
	return ptr == &current->flags;      /* CONFIG_KCSAN_PERMISSIVE */
}
```

——**白名单先于报告**：`kcsan_ignore_address`（encoding 章 §4.3 的 racer 侧过滤）在 CONFIG_KCSAN_PERMISSIVE 下豁免已知噪声源；`kcsan_ignore_data_race`（size/type/old/new/diff 五元判定）按值变化模式筛掉无害竞态。

## 八、观测实战

```bash
# 1) 构建期确认插桩
grep -E 'CONFIG_KCSAN(_WEAK_MEMORY|_PERMISSIVE|_IGNORE_ATOMICS)?=' /boot/config-$(uname -r)
#   WEAK_MEMORY 决定 func_entry/exit 与重排模拟是否存在

# 2) 反汇编看插桩点（每条 load/store 前的 __tsan_read4 调用）
objdump -d vmlinux --disassemble=<func> | grep -B3 'tsan'

# 3) 运行时断言失败样貌
dmesg | grep -A6 'ASSERT_EXCLUSIVE'    # "no concurrent accesses" 违例报告

# 4) 压测与白名单调参
#   CONFIG_KCSAN_PERMISSIVE=y 的降噪对比（正式 CI 关、扫雷时评估再开）
cat /sys/kernel/debug/kcsan             # counters（encoding 章观测节）
```

## 九、思考题

1. KCSAN 复用 `-fsanitize=thread` 而非新模式——好处与代价？（提示：编译器零新代码即支持；代价=继承 TSAN 的全部钩子形状（__tsan_atomic_* 等只得空壳/退化实现）。）
2. `__tsan_read_writeN` 的 `COMPOUND|WRITE` 双标志——encoding 章的哪些规则消费它？（提示：found_watchpoint 的 access_mask 分支与 check_access 的 `!(type & KCSAN_ACCESS_WRITE)` 方向过滤——RMW 按写算。）
3. volatile 的 `is_atomic` 启发式为什么限定"对齐且 ≤8 字节"？（提示：模拟 READ_ONCE 的合法性边界——compiletime_assert_rwonce_type 的尺寸约束（不对齐/过大的 volatile 不具备单指令原子性）。）
4. func_exit 的补查（"instrumentation is inserted before the real access"）——不补会漏哪类 bug？（提示：函数末尾的无屏障写（本意 release）——真实写后到函数返回前若有冲突访问，插桩点在写**之前**错过它；exit 的再查补上这最后一段。）
5. `ASSERT_EXCLUSIVE_ACCESS` 与 watchpoint 检查的本质差异？（提示：检查=被动等撞（可能漏）；断言=**主动宣布不变式**——任何并发访问（即使无值变化）都违规——更强但要求写对断言位置。）
6. scoped_access 的 `__cleanup__` 属性为什么比手工 begin/end 对？（提示：早退/异常路径不漏 end——cleanup 是编译器保证的 scope-exit 钩子；手工配对在复杂控制流下易漏。）

---

> 回看：[kcsan-encoding](kcsan-encoding.md)（check_access 之后的检测协议与 watchpoint 编码）｜[sanitizers](sanitizers.md)（三巨头总览）｜[debug-adversarial](debug-adversarial.md)（上刑家族——KCSAN 是"并发刑"）｜[locking/README](README.md)（锁族谱）
