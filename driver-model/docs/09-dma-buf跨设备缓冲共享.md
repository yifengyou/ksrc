# dma-buf：跨设备缓冲共享（Linux 6.6）

> 阅读目标：吃透跨设备共享一块内存的通用框架——`struct dma_buf`（文件描述符即引用计数）/`struct dma_buf_attachment`（每设备的 sg_table 地址翻译）/`dma_buf_ops` 回调族与★**exporter 锁协议三组约定**（dma-buf.c:839-860 注释原文——哪些回调持锁调用/哪些不持/哪些必须自己持）；★**隐式同步规则**（dma-buf.h resv 字段 kernel-doc 原文——READ/WRITE usage 的加 fence 契约与 "may just always add a write fence" 的宽松条款）；动态导入者的 `move_notify` 免钉协议；用户态三件（`DMA_BUF_IOCTL_SYNC` 的括号语义/dma_heap 的 `/dev/dma_heap`/udmabuf 的 memfd 路线）；TTM 衔接（GPU 内存管理器如何做 exporter——`ttm_bo_validate` 的持锁断言与 `drm_gem_prime_export` 桥）。全部对照 v6.6 当轮核对。前置：[dma_fence](../../locking/dma-fence.md)（resv/fence 基础）｜[05-devres](05-devres设备资源管理.md)（生命周期思维）。

---

## 一、问题：三个设备共享一块内存的三重困难

| 困难 | 内核对策 | 对象 |
| :--- | :--- | :--- |
| **地址翻译**——每个设备看到的物理地址不同（IOMMU/直连） | attach 时向 exporter 要 sg_table，由 exporter 一次性翻译 | `dma_buf_attachment` |
| **生命周期**——谁分配谁释放？谁还在用？ | 包装成 **file**，fd 跨进程传递，引用计数自动管 | `struct dma_buf::file` |
| **同步**——设备 A 写完设备 B 才能读 | 隐式：fence 进 `dma_resv`；显式：`DMA_BUF_IOCTL_SYNC` 括号 | `dmabuf->resv` |

**导出方（exporter）**拥有底层存储；**导入方（importer）**attach 后拿到本设备视角的地址。框架本身不做分配——分配在 dma_heap/udmabuf/GEM 等具体出口。

## 二、核心对象（include/linux/dma-buf.h）

### 2.1 struct dma_buf（:302-460 逐字段）

```c
struct dma_buf {
	size_t size;                    /* 不变量：生命周期内不变（mmap 核心代查） */
	struct file *file;              /* 引用计数与跨进程共享的载体 */
	struct list_head attachments;   /* 全部已 attach 设备，受 resv 锁保护 */
	const struct dma_buf_ops *ops;
	unsigned vmapping_counter;      /* vmap 引用计数（受 @lock 保护） */
	struct iosys_map vmap_ptr;
	const char *exp_name;           /* exporter 名（调试） */
	const char *name;               /* 用户态起的名（accounting；resv 锁+name_lock 双保护） */
	spinlock_t name_lock;
	struct module *owner;           /* exporter 模块引用计数 */
	struct list_head list_node;     /* 全局链（accounting/debug） */
	void *priv;                     /* exporter 私有数据 */
	struct dma_resv *resv;          /* ★同步中枢（可由 exporter 注入共享） */
	wait_queue_head_t poll;         /* 用户态 poll 支持 */
	struct dma_buf_poll_cb_t { struct dma_fence_cb cb; wait_queue_head_t *poll;
				   __poll_t active; } cb_in, cb_out;
#ifdef CONFIG_DMABUF_SYSFS_STATS
	struct dma_buf_sysfs_entry { struct kobject kobj; struct dma_buf *dmabuf; } *sysfs_entry;
#endif
};
```

★**resv 可注入**（export_info 的 `resv` 字段）——GEM 对象把自己的 resv 直接借给 dma-buf，形成"对象与共享体共用一把 fence 账本"。

### 2.2 dma_buf_ops 回调族（:36 起）

| 回调 | 语义 | 要点 |
| :--- | :--- | :--- |
| `cache_sgt_mapping` | 若 true，框架为首个映射缓存 sg_table | 免重复翻译 |
| `attach`/`detach` | 设备能否访问的把关 | VRAM/carveout 特殊位置的 exporter 在此拒绝 |
| `map_dma_buf`/`unmap_dma_buf` | 产出/归还 sg_table | ★框架持 resv 锁调用（见 §三） |
| `release` | 底层存储回收 | 最后一个引用放下时 |
| `begin_cpu_access`/`end_cpu_access` | CPU 访问括号 | cache 维护 |
| `mmap` | 用户态映射 | kernel-doc 原话："the mapping needs to be **incoherent**, userspace is expected to bracket CPU access using the DMA_BUF_IOCTL_SYNC interface" |
| `vmap`/`vunmap` | 内核线性映射 | 高成本路径 |

mmap 的 kernel-doc 还写透一个坑：要用 `unmap_mapping_range` 收 PTE 就得有独立 `address_space`，而 dma-buf 的 fd 不带——"exporters need to setup their own file (and address_space) association by setting `vma->vm_file`"（GEM 驱动可复用其 shmem file）。

### 2.3 导入方回调：dma_buf_attach_ops（:449-490）

```c
struct dma_buf_attach_ops {
	bool allow_peer2peer;    /* "must be able to handle peer resources
	                            without struct pages"（如 NVLink/P2PDMA） */
	void (*move_notify)(struct dma_buf_attachment *);  /* 动态协议核心 */
};
```

## 三、★exporter 锁协议三组约定（dma-buf.c:839-860 注释原文）

```
1. These &dma_buf_ops callbacks are invoked with unlocked dma-buf
   reservation and exporter can take the lock:
       attach() / detach() / release() /
       begin_cpu_access() / end_cpu_access() / mmap()

2. These &dma_buf_ops callbacks are invoked with locked dma-buf
   reservation and exporter can't take the lock:
       pin() / unpin() / map_dma_buf() / unmap_dma_buf() /
       vmap() / vunmap()

3. Exporters must hold the dma-buf reservation lock when calling these
   functions:
       dma_buf_move_notify()
```

★三组各自成立的原因：①组的操作慢且要睡觉（可拿锁慢慢做）；②组在 map 的热路径上、框架已持锁（再拿即死锁——对照 [dma_fence 章](../../locking/dma-fence.md)的死锁约束清单）；③组是框架反向调用通知方。这是 v6.6 定型的"动态 dma-buf 锁定"协议——旧式"map 期间钉死存储"被 move_notify 化的动态协议替代。

## 四、attach 与 map 流程

### 4.1 静态 vs 动态导入者

```
dma_buf_attach()            :968-975  "Wrapper to call
                                        dma_buf_dynamic_attach() for drivers
                                        which still use a static [attachment]"
dma_buf_dynamic_attach()    :867-965  传入 dma_buf_attach_ops
  → dma_resv_lock(dmabuf->resv) → list_add(&attach->node, &dmabuf->attachments)
  → attach->dma_map?  → ops->attach（§三的 ① 组：未持锁）
  → 若 !ops->pin 能力：默认给 importer 一个"钉住"语义
```

**动态导入者**提供 `move_notify` → exporter 得以**不钉**底层存储（"the framework can avoid pinning the backing store while mappings exists"，dma-buf.h:471-473）——GPU 驱动的 buffer 可被逐出再搬回，只要通过 move_notify 通知。

### 4.2 map：sg_table 的产出

```
dma_buf_map_attachment_unlocked（与 §三 ② 组对应——内部持 resv 锁）
  快路径：cache_sgt_mapping && attach->sgt 直接复用
  慢路径：ops->map_dma_buf(dmabuf, attach, dir) → attach->sgt
  动态导入者另附 attach->peer2peer? …
```

### 4.3 ★隐式同步规则（dma-buf.h resv 字段 kernel-doc 原文裁剪）

```
 * - Drivers must add a read fence through dma_resv_add_fence() with the
 *   DMA_RESV_USAGE_READ flag for anything the userspace API considers a
 *   read access. ...
 * - Similarly drivers must add a write fence ... DMA_RESV_USAGE_WRITE ...
 *
 * - Drivers may just always add a write fence, since that only
 *   causes unnecessary synchronization, but no correctness issues.
 *
 * - Some drivers only expose a synchronous userspace API with no
 *   pipelining across drivers. These do not set any fences for their
 *   access. An example here is v4l.
 *
 * - Driver should use dma_resv_usage_rw() when retrieving fences as
 *   dependency for implicit synchronization.
```

★**宽松条款**是设计精髓：写 fence 比读 fence 更保守（等价于把所有并发访问串行化），"只造成不必要的同步而无正确性问题"——**允许多报、禁止漏报**。v4l 例（同步 API 无流水线）是"零 fence"的合法形态。

**动态导入者附加规则**（紧随其后）：必须等 write fence signal 后才放行设备访问；"should set fences for any access that they can't disable immediately from their &dma_buf_attach_ops.move_notify callback"。

### 4.4 poll：隐式同步的用户态观测口

`cb_in`/`cb_out` 挂 `dma_fence_cb`——用户态 `poll(dmabuf_fd)` 等价于"隐式同步的 fence 全部 signal"。

## 五、用户态接口三件

### 5.1 DMA_BUF_IOCTL_SYNC（uapi/linux/dma-buf.h:26-51）

```
struct dma_buf_sync { __u32 flags; }   /* START|END × READ|WRITE|RW */

uapi 注释原文（裁剪）：
 * Prior to accessing the map, the client must call DMA_BUF_IOCTL_SYNC
 * with DMA_BUF_SYNC_START ... Once the access is complete ...
 * DMA_BUF_SYNC_END and the same read/write flags.
 * The synchronization provided via DMA_BUF_IOCTL_SYNC only provides
 * cache coherency——不解决设备并发（那是 fence 的活）
```

### 5.2 dma_heap：分配出口之一（CONFIG_DMABUF_HEAPS）

```
/dev/dma_heap/<heap_name>            # 每 heap 一个字符设备
  ioctl(DMA_HEAP_IOCTL_ALLOC)        # uapi dma-heap.h:50-51
    struct dma_heap_allocation_data { __u64 len; __u32 fd; __u32 fd_flags; ... }
内核 heap 实现（v6.6 携带两个）：
  heaps/system_heap.c :420-421  struct dma_heap_ops { .allocate = system_heap_allocate }
  heaps/cma_heap.c             （CMA 大块连续物理内存）
出口物即 dma-buf fd——分配器多了一个统一用户态面（Android 主推，ION 的继任）
```

### 5.3 udmabuf：memfd 路线（drivers/dma-buf/udmabuf.c）

用户先在 **memfd** 里备好页（flock 圈定范围），`UDMAHB_CREATE(_LIST)` 把这些页包装成 dma-buf（:202-242 的 `fget(list[i].memfd)`→`memfd->f_mapping`）——"用 memfd 当底层存储"的零拷贝出口，VM 测试与 virtio 场景常客。

### 5.4 sysfs 统计（CONFIG_DMABUF_SYSFS_STATS）

`/sys/kernel/dmabuf/buffers/<inode_number>/{exporter_name,size,...}`（dma-buf-sysfs-stats.c:36-42）——内存账本：谁 export 了多少 buffer 各多大，一眼清。

## 六、TTM 衔接：GPU 内存管理器如何做 exporter

TTM（Texture Memory Manager 的历史名）是 DRM 的显存管理器——**交换空间思维**：显存不够时把 BO（buffer object）逐出到系统内存，位置对导入设备透明。

### 6.1 struct ttm_buffer_object（include/drm/ttm/ttm_bo.h:98-135+）

```c
struct ttm_buffer_object {
	struct drm_gem_object base;      /* ★内嵌 GEM——DRM 生态的通用对象层 */
	struct ttm_device *bdev;
	enum ttm_bo_type type;           /* ttm_bo_type_device/placement/sg */
	void (*destroy)(struct ttm_buffer_object *);
	struct kref kref;
	/* resv 锁保护组： */
	struct ttm_resource *resource;   /* 当前占的显存/GTT 槽位 */
	struct ttm_tt *ttm;              /* 系统内存侧页表（swap 目标） */
	bool deleted;
	struct ttm_lru_bulk_move *bulk_move;
	unsigned priority;               /* 逐出优先级旋钮 */
	unsigned pin_count;              /* 钉住计数（不许逐出） */
	struct work_struct delayed_delete;
	struct sg_table *sg;
	struct dma_resv *resv;           /* :184——自带 fence 账本 */
};
```

### 6.2 validate：三步走（drivers/gpu/drm/ttm/ttm_bo.c:890-896）

```c
int ttm_bo_validate(struct ttm_buffer_object *bo,
		    struct ttm_placement *placement,
		    struct ttm_operation_ctx *ctx)
{
	...
	dma_resv_assert_held(bo->base.resv);   /* ★入口即断言持 resv 锁——
	                                          §三 锁协议的 TTM 侧镜像 */
```

流程：`ttm_bo_reserve`（ttm_bo.h:238 的 inline；失败 `-EAGAIN` 走 :264 的 `ttm_bo_reserve_slowpath`——"we backed off"）→ placement 逐项试位 → 无空间则**沿 LRU 逐出**别人（`ttm_bo_evict`）→ 搬运（CPU copy 或引擎 copy，`ttm_bo_util.c`）。`ttm_pool.c` 的页池（`struct ttm_pool { dev, nid, use_dma_alloc, use_dma32 }`，ttm_pool.h:69）让逐出页热缓存，避免反复进伙伴系统。

### 6.3 出口成 dma-buf（drivers/gpu/drm/drm_prime.c:233-248）

```
drm_gem_prime_export()  "This wraps dma_buf_export() for use by generic
                         GEM drivers"（:235-236）→ dma_buf_export()（:248）
  ops = &drm_gem_prime_dmabuf_ops（:781）
  exp_info.resv = GEM 对象的 resv——§2.1 的"注入"由此发生
GEM 对象缓存导出的 dmabuf 指针——同一对象反复 export 只出一个 dma_buf
```

**整条链闭环**：TTM 管"这块内存在哪"（可搬可逐出）→ GEM 管"这是不是一个对象"（handle/生命周期）→ dma-buf 管"怎么跨设备/跨进程交出去"（fd/attachment/resv）——三层各司其职，resv 一以贯之。

## 七、观测实战

```bash
# 1) 内存账本（CONFIG_DMABUF_SYSFS_STATS）
ls /sys/kernel/dmabuf/buffers/        # 每个 dma-buf 一目录（以 inode 号命名）
cat /sys/kernel/dmabuf/buffers/*/exporter_name /sys/kernel/dmabuf/buffers/*/size

# 2) 进程持有哪些 dma-buf
ls -l /proc/<pid>/fd | grep dmabuf    # fd 名含 "dmabuf"（alloc_anon_inode 于
                                      # dma-buf.c:530 的私有 dma_buf_mnt）

# 3) 分配出口
ls /dev/dma_heap/                     # system/cma（CONFIG_DMABUF_HEAPS）
cat /proc/devices | grep -i heap

# 4) fence 侧（配合 [dma_fence 章]）
cat /sys/kernel/debug/sync/sw_sync? 2>/dev/null   # CONFIG_SW_SYNC 调试口
#   隐式同步卡顿：poll 超时 → resv 里谁的 fence 未 signal
#   （fence 的 timestamp/context 见 locking/dma-fence.md 的观测节）
```

## 八、思考题

1. `dmabuf->resv` 允许 exporter 注入自己的 resv——GEM 这样做了。若两个独立 dma-buf 共享一个 resv，隐式同步的语义会怎样？（提示：一次 resv 锁覆盖两块内存的 fence 账本——粗粒度但安全；对照"may just always add a write fence"的允许多报精神。）
2. §三 的 ② 组回调"框架持锁、exporter 不能再拿"——如果 map_dma_buf 里必须睡眠分配页怎么办？（提示：TTM 的做法——`ttm_bo_validate` 入口即 `dma_resv_assert_held`，内部逐出/搬运全部在持锁下推进，慢路径靠 `ttm_bo_reserve_slowpath` 的 backoff 换公平。）
3. `DMA_BUF_IOCTL_SYNC` 的注释强调"only provides cache coherency"——漏掉哪类问题？（提示：设备与 CPU 的**并发**不受此 ioctl 管制——那要靠 fence（§4.3）；ioctl 只管 CPU 侧 cache/架构可见性。）
4. 动态导入者为什么"should set fences for any access that they can't disable immediately from move_notify"？（提示：move_notify 到达时设备可能正在飞——不能立刻叫停的访问必须留 fence，否则搬运窗口内设备还写旧位置——数据损坏而非同步问题。）
5. dma_heap 与 udmabuf 是两种出口——底层存储的"出身"差异会传导到哪个回调？（提示：udmabuf 的页是用户 memfd 页（可控可回收）；system_heap 是 buddy 页；cma 是连续物理——attach 时 exporter 的可搬运性承诺不同，move_notify 的可行性也不同。）
6. TTM 的 LRU 逐出与 `pin_count`——什么场景必须 pin？（提示：被 GPU 正在访问的 BO（fence 未 signal）若被逐出=正确性事故；pin_count 显式声明"不可动"，fence 则是隐式"暂时不可动"。）

---

> 回看：[dma_fence](../../locking/dma-fence.md)（resv 里放的是什么——本章同步侧的全部前置）｜[05-devres](05-devres设备资源管理.md)（设备资源生命周期三模式——dma-buf 是"共享资源"模式）｜[02-总线-设备-驱动匹配](02-总线-设备-驱动匹配.md)（attachment 与总线模型的类比）｜[driver-model/README](../README.md)（分册导读）
