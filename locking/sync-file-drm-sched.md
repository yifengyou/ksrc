# sync_file 与 drm_sched：fence 的两张消费面孔（Linux 6.6）

> 阅读目标：吃透 dma_fence 的两大消费生态——**sync_file**（把 fence 包装成**文件描述符**的用户态之窗：`struct sync_file` 恒持单 fence 指针（merge 即换 `dma_fence_array`）、poll 的 EPOLLIN 语义与首挂回调协议、`SYNC_IOC_MERGE/FILE_INFO` 两 ioctl、sw_sync 的测试时间线模型）与 **drm_sched**（GPU 作业管线：三文件分工（sched_main/sched_entity/sched_fence）、entity 的 **spsc_queue** 与 `last_scheduled` 串行依赖、★**双 fence**（scheduled/finished 两阶段——finished 进 dma_resv 作写 fence）、kthread 主循环的 `sched_set_fifo_low`、TDR 超时与 ★**karma** 连坐机制（"jobs from it will not be scheduled further" + KERNEL RQ 豁免原话））。全部对照 v6.6 当轮核对。前置：[dma-fence](dma-fence.md)｜[dma-buf](../driver-model/docs/09-dma-buf跨设备缓冲共享.md)。

---

## 一、生态位：fence 的四个消费现场

| 消费者 | 角色 | 已讲 |
| :--- | :--- | :--- |
| `dma_resv` | fence 的**容器**（READ/WRITE 多槽账本） | [dma-fence 章](dma-fence.md) |
| dma-buf | **跨设备共享**的同步中枢（隐式同步规则） | [dma-buf 章](../driver-model/docs/09-dma-buf跨设备缓冲共享.md) |
| **sync_file** | fence→**fd** 的用户态之窗（显式同步） | 本章 §二 |
| **drm_sched** | GPU **作业管线**的 fence 生产者 | 本章 §四 |

## 二、sync_file：把 fence 包装成 fd（drivers/dma-buf/sync_file.c，372 行）

### 2.1 结构（include/linux/sync_file.h:35-53）

```c
struct sync_file {
	struct file		*file;
	char			user_name[32];   /* 用户起的名（merge 时）；驱动生成则全 0 */
#ifdef CONFIG_DEBUG_FS
	struct list_head	sync_file_list;   /* 全局链——/sys/kernel/debug/sync/info 用 */
#endif
	wait_queue_head_t	wq;
	unsigned long		flags;
	struct dma_fence	*fence;           /* ★恒为单指针——merge 时换成 dma_fence_array */
	struct dma_fence_cb	cb;              /* poll 首挂的回调 */
};
#define POLL_ENABLED 0
```

★**单指针设计**：sync_file 不管"一组 fence"——需要合流时 `sync_file_merge()`（sync_file.c:159，静态）把两 fence 包成 **dma_fence_array** 再整体换掉 `->fence`。组合逻辑全押在 fence-array 原语上（[dma-fence 章](dma-fence.md)的三种组合器之一）。

### 2.2 生命周期与 poll 协议

```
sync_file_create(fence)（:65）→ 内核生成 fd → 用户态持 fd 跨进程传递
                                  （安卓 explicit sync 的标准载体）

sync_file_poll（:191-204）：
  poll_wait(file, &sync_file->wq, wait)
  首次 poll：test_and_set_bit(POLL_ENABLED) 后 dma_fence_add_callback(fence, &cb, ...)
    add 失败（已 signal）→ wake_up_all(&wq)   /* 迟到者立刻放行 */
  return dma_fence_is_signaled(fence) ? EPOLLIN : 0;   /* ★EPOLLIN=就绪 */

sync_file_release（:179-189）：
  挂过回调则 dma_fence_remove_callback → dma_fence_put(fence) → kfree
```

与 dma-buf 自带的 poll（dma_buf.cb_in/cb_out）对照：**dma-buf poll 等 resv 里全部隐式 fence**；**sync_file poll 等单个显式 fence**——隐式/显式两个世界各自的用户态等待窗。

### 2.3 UAPI（include/uapi/linux/sync_file.h:79-89）

```c
#define SYNC_IOC_MAGIC		'>'
#define SYNC_IOC_MERGE		_IOWR(SYNC_IOC_MAGIC, 3, struct sync_merge_data)
#define SYNC_IOC_FILE_INFO	_IOWR(SYNC_IOC_MAGIC, 4, struct sync_file_info)
```

- **MERGE**（sync_file.c:207-235）：`fence3 = sync_file_merge(data.name, sync_file, fence2)`——两 fd 合流成第三个（fence_array）
- **FILE_INFO**（:279+）：两段式回读——`num_fences==0` 时先问数量，再带缓冲回读 `struct sync_fence_info` 数组（ctx/seqno/status/timeline 名）——可枚举 fd 里每个成员 fence 的状态

### 2.4 sw_sync：测试时间线（drivers/dma-buf/sw_sync.c:36-60）

```
SW_SYNC_IOC_MAGIC 'W'
SW_SYNC_IOC_CREATE_FENCE（cmd 0，struct sw_sync_create_fence_data）
  —— 在时间线上建一个"到 N 才 signal"的 fence
SW_SYNC_IOC_INC（cmd 1，__u32）
  —— 头注释原话："To increment the timeline counter, SW_SYNC_IOC_INC
     ioctl should be used"
时间线计数单调递增——纯软件 fence，供测试/用户态驱动模拟时序
（CONFIG_SW_SYNC；Android 的 sync framework 调试孪生）
```

配套观测：`/sys/kernel/debug/sync/info`（sync_debug.c:176 `debugfs_create_dir("sync")` + :183 `info` 文件 0444）——**全部 timeline/fence/sync_file 的实时快照**，排显式同步死锁的第一现场。

## 三、drm_sched：GPU 作业管线（drivers/gpu/drm/scheduler/）

### 3.1 三文件分工（v6.6 实况）

| 文件 | 职责 |
| :--- | :--- |
| `sched_main.c`（1208 行） | 调度器本体：kthread 主循环、TDR、karma、job 生命周期 |
| `sched_entity.c`（596 行） | 提交侧：entity/runqueue/依赖串行 |
| `sched_fence.c`（240 行） | 双 fence 原语（scheduled/finished） |

### 3.2 struct drm_gpu_scheduler（include/drm/gpu_scheduler.h:498-519）

```c
struct drm_gpu_scheduler {
	const struct drm_sched_backend_ops	*ops;   /* prepare/run/timedout/free_job */
	uint32_t			hw_submission_limit;   /* 在飞上限 */
	long				timeout;           /* TDR 窗口（job->timeout 的默认） */
	const char			*name;
	struct drm_sched_rq		sched_rq[DRM_SCHED_PRIORITY_COUNT];  /* 按优先级的 rq 数组 */
	wait_queue_head_t		wake_up_worker;    /* 唤醒 kthread */
	wait_queue_head_t		job_scheduled;
	atomic_t			hw_rq_count;
	atomic64_t			job_id_count;
	struct workqueue_struct		*timeout_wq;   /* TDR 的执行 wq */
	struct delayed_work		work_tdr;      /* ★TDR 定时器 */
	struct task_struct		*thread;       /* ★kthread（非 workqueue） */
	struct list_head		pending_list;  /* 在飞作业（job_list_lock） */
	spinlock_t			job_list_lock;
	int				hang_limit;    /* ★连坐上限——见 karma */
	atomic_t			*score;		/* 外部计分板（fe 后端聚合） */
	atomic_t			_score;
	bool				ready;
	bool				free_guilty;   /* timedout 后真完成的 guilt 走手工 free */
	struct device			*dev;
};
```

优先级四档（gpu_scheduler.h:65-72）：`MIN < NORMAL < HIGH < KERNEL`——KERNEL 档留给驱动内部必需作业（页表更新等），karma 豁免由此而来（§3.5）。

### 3.3 entity：提交侧（sched_entity.c）

```c
struct drm_sched_entity {                  /* gpu_scheduler.h:88-203 */
	struct list_head	list;           /* 挂进 rq->entities */
	struct drm_sched_rq	*rq;            /* 头注释彩蛋——"FIXME: Locking is
	                                           very unclear for this." */
	enum drm_sched_priority	priority;       /* 可运行时调档 */
	struct spsc_queue	job_queue;      /* ★单生产单消费队列——提交者/调度 kthread 两端无锁 */
	atomic_t		*guilty;            /* ★外部原子指针——驱动轮询连坐状态 */
	struct dma_fence __rcu	*last_scheduled;   /* ★同实体作业串行的锚点 */
	...
};
```

★**依赖模型极简**：一个 job 只显式等待 `entity->last_scheduled`（同实体上一个作业的 finished fence）——**实体内部串行、实体间并行**；跨实体依赖由驱动自己 `dma_fence_chain`/array 组合后挂 job（drm-amdgpu 的依赖列表模式）。

### 3.4 ★双 fence：一个作业的两个里程碑（sched_fence.c）

```c
struct drm_sched_fence {                   /* gpu_scheduler.h:267+ */
	struct dma_fence	scheduled;  /* "signaled by the scheduler
	                                     when the job is scheduled" */
	struct dma_fence	finished;   /* "...when the job is completed"——
	                                     ★进 dma_resv 作写 fence 的就是它 */
	...
};
```

两阶段的意义：**scheduled** 让等待方知道"已被硬件接单"（可提前做准备工作），**finished** 才是数据可读的承诺。amdgpu 等把 finished 塞进 bo 的 resv（DMA_RESV_USAGE_WRITE）——回到 [dma-buf 章的隐式同步规则](../driver-model/docs/09-dma-buf跨设备缓冲共享.md)。

### 3.5 主循环与 TDR（sched_main.c）

```
drm_sched_main（:1005-1075，kthread）：
  sched_set_fifo_low(current)                    /* ★SCHED_FIFO 低档——实时但不抢驱动 */
  while (!kthread_should_stop()) {
    wait_event_interruptible(wake_up_worker,
        (cleanup_job = drm_sched_get_cleanup_job()) ||      /* ①先清尸体 */
        (!drm_sched_blocked && (entity = drm_sched_select_entity())) || /* ②按优先级选实体 */
        kthread_should_stop())
    sched_job = drm_sched_entity_pop_job(entity)
    drm_sched_job_begin(sched_job)               /* 挂进 pending_list + 起 TDR */
    fence = ops->run_job(sched_job)              /* 交给驱动喂硬件 */
    drm_sched_fence_scheduled(s_fence, fence)    /* 里程碑一 */
    dma_fence_add_callback(fence, &job->cb, drm_sched_job_done_cb)
      → 硬件完成 → drm_sched_job_done(s_job, fence->error)  /* 里程碑二 */
  }

TDR：drm_sched_job_timedout（:383-426，delayed_work）：
  job = list_first_entry(pending_list)           /* 最老的在飞作业=嫌疑犯 */
  list_del_init(&job->list)                      /* 摘出防并发 free——注释：
    "It will be reinserted back after sched->thread is parked" */
  status = ops->timedout_job(job)                /* 驱动决定：GPU reset / 换 ring */
  free_guilty 时 ops->free_job(job)              /* "Guilty job did complete and
                                                    hence needs to be manually removed" */
  status != ENODEV → drm_sched_start_timeout()   /* 重新武装 TDR */
```

### 3.6 ★karma：连坐机制（:1167-1208，头注释原文）

```
 * drm_sched_increase_karma - Update sched_entity guilty flag
 * Increment on every hang caused by the 'bad' job. If this exceeds the
 * hang limit of the scheduler then the respective sched entity is marked
 * guilty and jobs from it will not be scheduled further
```

```
实现三步：
  ① bad->karma++（与 sched->hang_limit 比对）
  ② 沿各优先级 rq 找 fence_context 匹配的 entity：
       bad->s_fence->scheduled.context == entity->fence_context
  ③ atomic_set(entity->guilty, 1)——★guilty 是外部指针，驱动（如 amdgpu）
     轮询它拒绝后续提交——"jobs from it will not be scheduled further"

KERNEL RQ 豁免（注释原话）：
  "don't change @bad's karma if it's from KERNEL RQ, because sometimes
   GPU hang would cause kernel jobs (like VM updating jobs) corrupt but
   keep in mind that kernel jobs always considered good."
```

★设计哲学：**不惩罚嫌疑人，惩罚惯犯**——单次超时可能是巧合（邻居扰流），hang_limit 次连坐才拉黑整个提交上下文；页表更新这类"坏了大家一起完"的内核作业永不连坐。

## 四、两案合流：显式同步的现代形态

```
隐式（dma-buf resv）：驱动替用户管 fence——应用无感知，但调试是黑盒
显式（sync_file fd）：应用自己拿 fd 排序——Vulkan/Android 的 explicit sync

drm_sched 横跨两界：
  对下（硬件）：生产 scheduled/finished 两阶段 fence
  对上（应用）：finished fence 既能进 resv（隐式），也能
                sync_file_create() 包装成 fd 交应用（显式）
sw_sync 则是显式世界的"假硬件"——CI 里模拟任意时序
```

## 五、观测实战

```bash
# 1) 显式同步全景快照（CONFIG_DEBUG_FS + sync_file 的 sync_file_list）
cat /sys/kernel/debug/sync/info      # 全部 timeline/fence/sync_file 状态表

# 2) 造一条假时间线（CONFIG_SW_SYNC 调试口）
#   fd = open("/dev/sw_sync", ...); CREATE_FENCE(seqno) / INC(cnt)
#   ——复现死锁剧本：sync_file_merge 出环 → /sys/kernel/debug/sync/info 验尸

# 3) drm_sched 侧
dmesg | grep -i 'timedout\|hang'     # TDR 触发与 ops->timedout_job 的决策日志
#   tracepoint：drm:drm_run_job（sched_main.c:1034 trace_drm_run_job）
echo 1 > /sys/kernel/debug/tracing/events/drm/enable

# 4) karma 连坐排查
#   hang_limit 次超时后 entity->guilty=1 → 驱动侧拒绝提交
#   （amdgpu: dmesg 的 "guilty" 上下文；ctx 经 amdgpu_ctx 的 guilty 位拉黑）

# 5) fence 通用（[dma-fence 章] 观测节：trace_dma_fence_* 全家）
```

## 六、思考题

1. sync_file 的 `fence` 恒为单指针——merge 两次（A+B 再 +C）后的结构是什么？（提示：fence_array 套 fence_array——两层展开；`dma_fence_unwrap`（dma-fence-unwrap.c）就是为这种嵌套 flatten 而生。）
2. `sync_file_poll` 里 `dma_fence_add_callback` 返回负值时 `wake_up_all`——什么场景会走到？（提示：add 失败=已 signal（-ENOENT）；poll_wait 已挂表，若不主动唤醒则 poll 语义仍对但唤醒时机靠设备事件——为什么这里非要立刻唤醒？）
3. entity 的 `job_queue` 用 spsc_queue 而非普通链表+锁——两端的"单生产/单消费"分别是谁？（提示：生产端=提交线程（可能不同进程上下文），消费端=调度 kthread——天然两点，锁退化为内存序。）
4. 双 fence 的 scheduled 里程碑有什么实际消费者？（提示：等待方拿到 scheduled 即知"队列位置已锁定"——可提前 buffer 目标切换；resv 里放 finished 而非 scheduled 是因为数据承诺只在完成时兑现。）
5. karma 为什么按 `fence_context` 反查 entity 而不在 job 里存反向指针？（提示：job 可能已被 free（超时时坏作业存活期不定）；fence_context 是稳定标识——连坐判定宁慢勿悬空。）
6. TDR 把"最老的 pending 作业"当嫌疑犯——为什么不是最新提交的？（提示：硬件按序执行——最老未完成=最先卡住的位置；最新作业可能只是排在后面的受害者。）

---

> 回看：[dma-fence](dma-fence.md)（fence 原语与三种组合器——本章的 vocabulary）｜[dma-buf](../driver-model/docs/09-dma-buf跨设备缓冲共享.md)（隐式同步规则——drm_sched finished fence 的下游）｜[preempt-rt-matrix](preempt-rt-matrix.md)（sched_set_fifo_low 的 FIFO 家族）｜[locking/README](README.md)（锁族谱）
