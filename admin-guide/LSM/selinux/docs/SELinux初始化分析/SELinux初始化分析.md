# SE Linux初始化分析

## 基于LSM的基础初始化


```c

/* SELinux requires early initialization in order to label
   all processes and objects when they are created. */
DEFINE_LSM(selinux) = {
	.name = "selinux",
	.flags = LSM_FLAG_LEGACY_MAJOR | LSM_FLAG_EXCLUSIVE,
	.enabled = &selinux_enabled_boot,
	.blobs = &selinux_blob_sizes,
	.init = selinux_init,
};

```

根据 O:\include\linux\lsm_hooks.h 定义展开，很清晰，定义lsm_info结构交给lsm子系统注册、调用

```c

struct lsm_info {
	const char *name;	/* Required. */
	enum lsm_order order;	/* Optional: default is LSM_ORDER_MUTABLE */
	unsigned long flags;	/* Optional: flags describing LSM */
	int *enabled;		/* Optional: controlled by CONFIG_LSM */
	int (*init)(void);	/* Required. */
	struct lsm_blob_sizes *blobs; /* Optional: for blob sharing. */
};

#define DEFINE_LSM(lsm)							\
	static struct lsm_info __lsm_##lsm				\
		__used __section(".lsm_info.init")			\
		__aligned(sizeof(unsigned long))

```


## 全局变量

### selinux_state

```c

struct selinux_state selinux_state;

```

```c
// O:\security\selinux\include\security.h
struct selinux_policy;

struct selinux_state {
#ifdef CONFIG_SECURITY_SELINUX_DEVELOP
	bool enforcing; // 决定 SELinux 是处于强制模式 (Enforcing) 还是 宽容模式 (Permissive)
#endif
	bool initialized; // 标记 SELinux 子系统是否已完成初始化（即策略是否已加载）防止还未初始化就进行安全检查
	bool policycap[__POLICYDB_CAP_MAX]; // 策略能力位图（Policy Capabilities）

	struct page *status_page; // 状态页与锁机制，基于页的同步机制
	struct mutex status_lock;

	struct selinux_policy __rcu *policy; // 指向当前SELinux 策略对象。包含策略数据库、符号表、访问向量缓存的引用等核心数据。
	struct mutex policy_mutex; // 互斥锁，保护策略的加载和替换过程
} __randomize_layout; //对抗内存破坏漏洞。每次内核编译，成员的顺序都可能不同，增加利用难度.
```


初步看了下，调用很多

```shell

# rg -w selinux_state
status.c
47:	mutex_lock(&selinux_state.status_lock);
48:	if (!selinux_state.status_page) {
49:		selinux_state.status_page = alloc_page(GFP_KERNEL|__GFP_ZERO);
51:		if (selinux_state.status_page) {
52:			status = page_address(selinux_state.status_page);
68:	result = selinux_state.status_page;
69:	mutex_unlock(&selinux_state.status_lock);
83:	mutex_lock(&selinux_state.status_lock);
84:	if (selinux_state.status_page) {
85:		status = page_address(selinux_state.status_page);
95:	mutex_unlock(&selinux_state.status_lock);
108:	mutex_lock(&selinux_state.status_lock);
109:	if (selinux_state.status_page) {
110:		status = page_address(selinux_state.status_page);
121:	mutex_unlock(&selinux_state.status_lock);

include/security.h
92:struct selinux_state {
108:extern struct selinux_state selinux_state;
113:	return smp_load_acquire(&selinux_state.initialized);
119:	smp_store_release(&selinux_state.initialized, true);
125:	return READ_ONCE(selinux_state.enforcing);
130:	WRITE_ONCE(selinux_state.enforcing, value);
151:	return READ_ONCE(selinux_state.policycap[POLICYDB_CAP_NETPEER]);
156:	return READ_ONCE(selinux_state.policycap[POLICYDB_CAP_OPENPERM]);
161:	return READ_ONCE(selinux_state.policycap[POLICYDB_CAP_EXTSOCKCLASS]);
166:	return READ_ONCE(selinux_state.policycap[POLICYDB_CAP_ALWAYSNETWORK]);
171:	return READ_ONCE(selinux_state.policycap[POLICYDB_CAP_CGROUPSECLABEL]);
177:		selinux_state.policycap[POLICYDB_CAP_NNP_NOSUID_TRANSITION]);
183:		selinux_state.policycap[POLICYDB_CAP_GENFS_SECLABEL_SYMLINKS]);
189:		selinux_state.policycap[POLICYDB_CAP_IOCTL_SKIP_CLOEXEC]);

ima.c
59:		rc = strlcat(buf, selinux_state.policycap[i] ? on : off,
77:	lockdep_assert_held(&selinux_state.policy_mutex);
115:	lockdep_assert_not_held(&selinux_state.policy_mutex);
117:	mutex_lock(&selinux_state.policy_mutex);
119:	mutex_unlock(&selinux_state.policy_mutex);

selinuxfs.c
374:	mutex_lock(&selinux_state.policy_mutex);
404:	mutex_unlock(&selinux_state.policy_mutex);
408:	mutex_unlock(&selinux_state.policy_mutex);
597:	mutex_lock(&selinux_state.policy_mutex);
635:	mutex_unlock(&selinux_state.policy_mutex);
1219:	mutex_lock(&selinux_state.policy_mutex);
1238:	mutex_unlock(&selinux_state.policy_mutex);
1245:	mutex_unlock(&selinux_state.policy_mutex);
1270:	mutex_lock(&selinux_state.policy_mutex);
1294:	mutex_unlock(&selinux_state.policy_mutex);
1325:	mutex_lock(&selinux_state.policy_mutex);
1346:	mutex_unlock(&selinux_state.policy_mutex);

hooks.c
109:struct selinux_state selinux_state;
7293:	memset(&selinux_state, 0, sizeof(selinux_state));
7296:	mutex_init(&selinux_state.status_lock);
7297:	mutex_init(&selinux_state.policy_mutex);

ss/services.c
247:	policy = rcu_dereference(selinux_state.policy);
767:	policy = rcu_dereference(selinux_state.policy);
866:	policy = rcu_dereference(selinux_state.policy);
1031:	policy = rcu_dereference(selinux_state.policy);
1112:	policy = rcu_dereference(selinux_state.policy);
1168:	policy = rcu_dereference(selinux_state.policy);
1298:	policy = rcu_dereference(selinux_state.policy);
1347:	policy = rcu_dereference(selinux_state.policy);
1542:	policy = rcu_dereference(selinux_state.policy);
1736:	policy = rcu_dereference(selinux_state.policy);
2115:	for (i = 0; i < ARRAY_SIZE(selinux_state.policycap); i++)
2116:		WRITE_ONCE(selinux_state.policycap[i],
2154:	struct selinux_state *state = &selinux_state;
2178:	struct selinux_state *state = &selinux_state;
2246:	struct selinux_state *state = &selinux_state;
2399:	policy = rcu_dereference(selinux_state.policy);
2451:	policy = rcu_dereference(selinux_state.policy);
2503:	policy = rcu_dereference(selinux_state.policy);
2555:	policy = rcu_dereference(selinux_state.policy);
2620:	policy = rcu_dereference(selinux_state.policy);
2725:	policy = rcu_dereference(selinux_state.policy);
2886:		policy = rcu_dereference(selinux_state.policy);
2926:	policy = rcu_dereference(selinux_state.policy);
3021:	struct selinux_state *state = &selinux_state;
3102:	policy = rcu_dereference(selinux_state.policy);
3171:	policy = rcu_dereference(selinux_state.policy);
3290:	policy = rcu_dereference(selinux_state.policy);
3442:	policy = rcu_dereference(selinux_state.policy);
3457:	policy = rcu_dereference(selinux_state.policy);
3482:	policy = rcu_dereference(selinux_state.policy);
3507:	struct selinux_state *state = &selinux_state;
3632:	struct selinux_state *state = &selinux_state;
3828:	policy = rcu_dereference(selinux_state.policy);
3897:	policy = rcu_dereference(selinux_state.policy);
3953:	struct selinux_state *state = &selinux_state;
3983:	struct selinux_state *state = &selinux_state;

```


### selinux_enforcing_boot

* 带有 __initdata 标记，初始化之后回收内存


```c

#ifdef CONFIG_SECURITY_SELINUX_DEVELOP
static int selinux_enforcing_boot __initdata;

static int __init enforcing_setup(char *str)
{
	unsigned long enforcing;
	if (!kstrtoul(str, 0, &enforcing))
		selinux_enforcing_boot = enforcing ? 1 : 0;
	return 1;
}
__setup("enforcing=", enforcing_setup);
#else
#define selinux_enforcing_boot 1
#endif
```

### selinux_avc

```c
struct selinux_avc {
	unsigned int avc_cache_threshold;
	struct avc_cache avc_cache;
};

static struct selinux_avc selinux_avc;

```


## selinux_init初始化


```c

static __init int selinux_init(void)
{
	pr_info("SELinux:  Initializing.\n");

	memset(&selinux_state, 0, sizeof(selinux_state));
	enforcing_set(selinux_enforcing_boot);
	selinux_avc_init();
	mutex_init(&selinux_state.status_lock);
	mutex_init(&selinux_state.policy_mutex);

	/* Set the security state for the initial task. */
	cred_init_security();

	default_noexec = !(VM_DATA_DEFAULT_FLAGS & VM_EXEC);
	if (!default_noexec)
		pr_notice("SELinux:  virtual memory is executable by default\n");

	avc_init();

	avtab_cache_init();

	ebitmap_cache_init();

	hashtab_cache_init();

	security_add_hooks(selinux_hooks, ARRAY_SIZE(selinux_hooks), "selinux");

	if (avc_add_callback(selinux_netcache_avc_callback, AVC_CALLBACK_RESET))
		panic("SELinux: Unable to register AVC netcache callback\n");

	if (avc_add_callback(selinux_lsm_notifier_avc_callback, AVC_CALLBACK_RESET))
		panic("SELinux: Unable to register AVC LSM notifier callback\n");

	if (selinux_enforcing_boot)
		pr_debug("SELinux:  Starting in enforcing mode\n");
	else
		pr_debug("SELinux:  Starting in permissive mode\n");

	fs_validate_description("selinux", selinux_fs_parameters);

	return 0;
}

```

### selinux_enforcing_boot初始化

```c
enforcing_set(selinux_enforcing_boot);

#ifdef CONFIG_SECURITY_SELINUX_DEVELOP
static inline bool enforcing_enabled(void)
{
	return READ_ONCE(selinux_state.enforcing);
}

static inline void enforcing_set(bool value)
{
	WRITE_ONCE(selinux_state.enforcing, value);
}
#else
static inline bool enforcing_enabled(void)
{
	return true;
}

static inline void enforcing_set(bool value)
{
}
#endif

```

* CONFIG_SECURITY_SELINUX_DEVELOP 的作用：这是一个编译开关。宏定义了内核是“开发构建”还是“生产构建”
   - 开启时（开发版）：允许运行时动态切换模式（即支持 setenforce 0/1 命令）。此时 enforcing_set 会真正修改内存中的状态变量。用户可以通过 /sys/fs/selinux/enforce 文件或 setenforce 命令随时切换模式。
   - 关闭时（生产版/正式版）：为了安全性和性能优化，内核被编译为强制固定在 Enforcing 模式。此时 enforcing_set 是一个空函数，任何尝试关闭 SELinux 的操作（包括启动参数试图设为 permissive）都会被忽略，确保系统始终处于最安全的强制模式。即使攻击者获得了 root 权限，也无法通过 setenforce 0 关闭 SELinux 保护


* WRITE_ONCE 的作用：绝不是为了调试。它是 Linux 内核并发编程的标准原语，用于保证多线程/多核环境下的内存可见性和指令重排安全性。
    - SELinux 的状态检查（enforcing_enabled）可能在高频路径上被无数进程同时读取。
    - WRITE_ONCE 确保写入操作是原子的，且不会被编译器优化掉或乱序执行，防止其他 CPU 核心读到脏数据（例如读到一半的布尔值），从而导致安全策略判断错误（比如本该拦截的攻击被放行了）。


如果没有 WRITE_ONCE：
1. 编译器优化：编译器可能认为 selinux_state.enforcing 在当前函数上下文中没变，就使用了寄存器里的旧值，导致切换不及时。
2. 指令重排：CPU 可能乱序执行写入，导致状态更新不完整。
3. 部分写入（Tearing）：虽然在 bool 或 int 对齐情况下较少见，但在某些架构上，非原子写入可能导致读取端读到中间状态。

有了 WRITE_ONCE：
它强制编译器生成一条直接的存储指令，禁止对该变量的访问进行重排序，并确保其他 CPU 核心能立即看到这个变化（配合适当的内存屏障，虽然 WRITE_ONCE 主要解决编译器和单条指令原子性问题，完整的 SMP 安全通常还需要 smp_store_release 等，但在简单的 bool 标志位上 WRITE_ONCE 是标准做法）。

| 特性 | 开发/调试内核 (`CONFIG_SECURITY_SELINUX_DEVELOP` 开启) | 生产/发布内核 (`CONFIG_SECURITY_SELINUX_DEVELOP` 关闭) |
| :--- | :--- | :--- |
| `enforcing_set` 行为 | 真正修改内存状态 | 空函数 (无效) |
| 能否动态关闭 SELinux | 能 (`setenforce 0`) | 不能 (始终强制) |
| 启动参数 `enforcing=0` | 生效 (进入 Permissive) | 通常无效 (保持 Enforcing) |
| `WRITE_ONCE` 作用 | 保证多核切换状态时的数据安全 | (代码被优化掉，不存在此调用) |
| 主要目的 | 方便开发者调试策略和驱动 | 最大化安全性，防止运行时被绕过 |



### selinux_avc_init初始化

```c
// O:\security\selinux\avc.c

#define AVC_CACHE_SLOTS			512
#define AVC_DEF_CACHE_THRESHOLD		512
#define AVC_CACHE_RECLAIM		16

struct avc_cache {
	struct hlist_head	slots[AVC_CACHE_SLOTS]; /* head for avc_node->list */
	spinlock_t		slots_lock[AVC_CACHE_SLOTS]; /* lock for writes */
	atomic_t		lru_hint;	/* LRU hint for reclaim scan */
	atomic_t		active_nodes;
	u32			latest_notif;	/* latest revocation notification */
};

struct selinux_avc {
	unsigned int avc_cache_threshold;
	struct avc_cache avc_cache;
};

static struct selinux_avc selinux_avc; // 静态全局变量

void selinux_avc_init(void)
{
	int i;

	selinux_avc.avc_cache_threshold = AVC_DEF_CACHE_THRESHOLD;
	for (i = 0; i < AVC_CACHE_SLOTS; i++) {
		INIT_HLIST_HEAD(&selinux_avc.avc_cache.slots[i]); // 初始化空链表
		spin_lock_init(&selinux_avc.avc_cache.slots_lock[i]); // 初始化每把锁
	}
	atomic_set(&selinux_avc.avc_cache.active_nodes, 0); // 清零计数器
	atomic_set(&selinux_avc.avc_cache.lru_hint, 0); // 清零计数器
}

```

SELinux 的核心逻辑是：当进程 A 想访问资源 B 时，内核需要查询策略数据库（Policy Database），判断是否允许。
1. 没有 AVC：每次访问都要遍历庞大的策略树（可能包含数万条规则），耗时极长（微秒级甚至毫秒级）。
2. 有 AVC：将“允许/拒绝”的结果缓存起来。第一次慢，后续相同请求直接命中缓存（纳秒级）。


这段看似简单的初始化代码，实际上定义了 SELinux 在系统中的性能上限和并发能力：
1. 决定了系统快慢：它建立的 512 槽位哈希表，让 SELinux 的权限检查从  O(N) （遍历策略）变成了接近 O(1) （哈希查找）。没有它，开启 SELinux 的服务器 IOPS 可能会下降 50% 以上。
2. 决定了多核扩展性：通过初始化 512 把独立的自旋锁，它确保了在 64 核甚至 128 核的服务器上，SELinux 不会成为锁竞争的瓶颈。
3. 保证了策略一致性：通过 latest_notif 的初始化，为后续动态热加载安全策略（不重启系统更新规则）打下了基础。

一句话概括： 这不是普通的初始化，这是在构建一个高并发、线程安全、支持动态失效的高速决策缓存引擎，它是 SELinux 能够在生产环境中“无感”运行的秘密武器。



### cred_init_security

```c

extern struct lsm_blob_sizes selinux_blob_sizes;
static inline struct task_security_struct *selinux_cred(const struct cred *cred)
{
	return cred->security + selinux_blob_sizes.lbs_cred;
}

/*
 * initialise the security for the init task
 */
static void cred_init_security(void)
{
	struct task_security_struct *tsec;

	tsec = selinux_cred(unrcu_pointer(current->real_cred));
	tsec->osid = tsec->sid = SECINITSID_KERNEL;
}

```

为 Linux 系统的第一个进程（init 任务，即 PID 1）赋予初始的 SELinux 安全上下文（Security Context），使其成为内核信任的“根”主体

如果没有这个函数，系统启动后的第一个用户空间进程将没有合法的 SELinux 身份，导致后续所有进程创建、文件访问等安全检查全部失败，系统无法正常运行。

在 SELinux 的安全模型中，每一个进程（Task）和每一个对象（文件、Socket等）都必须有一个 SID (Security Identifier)。
* 系统刚启动时，内核本身是受信的，但即将运行的第一个用户空间进程（通常是 /sbin/init 或 systemd）还没有被分配 SID。
* 这个函数的作用就是手动给这个“始祖进程”打上标签：SECINITSID_KERNEL。
* 含义：这表示该进程是由内核直接创建的，拥有最高的初始信任级别。它是后续所有进程安全上下文转换的起点（例如，init 进程随后会加载策略，并根据配置文件将自己切换到 init_t 或其他域）。


在 SELinux 策略加载中，有一个特殊的 SID 叫 SECINITSID_KERNEL。
1. 策略加载前的特权：在用户空间加载完整的 SELinux 策略二进制文件之前，内核必须能够执行一些操作（如挂载文件系统、打开设备）。将这些操作标记为 KERNEL SID，允许它们在“无策略”或“最小策略”状态下通过检查。
2. 策略加载的触发者：只有拥有 SECINITSID_KERNEL 身份的进程（即 init），才被允许执行 security_load_policy 系统调用来加载真正的策略文件。
3. 上下文转换的源头：一旦策略加载完成，init 进程会根据策略规则（通常在 file_contexts 中定义），将自己的 SID 从 SECINITSID_KERNEL 转换为具体的域（如 unconfined_t 或 init_t）。此后它生成的子进程才会继承正确的上下文。

SECINITSID_KERNEL 不是一个硬编码的固定数值（比如写死的 #define SECINITSID_KERNEL 1），而是一个动态生成的索引值。

它的值是在 SELinux 策略加载过程中，根据策略配置文件（security_contexts 文件，通常位于 /etc/selinux/<policy>/contexts/security_contexts）中的顺序动态确定的。





























































































































