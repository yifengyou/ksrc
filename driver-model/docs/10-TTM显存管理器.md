# TTM：GPU 显存管理器（Linux 6.6）

> 阅读目标：独立展开 [dma-buf 章 §六](09-dma-buf跨设备缓冲共享.md) 的衔接节——**mem_type 抽象**（`TTM_PL_SYSTEM/TT/VRAM/PRIV` 四类 + `struct ttm_resource_manager` 的 lru[4]/usage/move fence）；**LRU 逐出链**（`ttm_mem_evict_first` 的遍历与 busy 等待 → `ttm_bo_evict` 的 ★**-EMULTIHOP 弹跳循环**（VRAM→TT→SYSTEM 的中继搬移）→ `ttm_bo_eviction_valuable` 的判定）；★**bulk_move**（`pos[TTM_NUM_MEM_TYPES][4]` 的 first/last 游标 + `list_bulk_move_tail` 的 **O(1) 拼接**——一整批 BO 挪 LRU 尾只动两个指针 + "resource order never changes" 契约）；`ttm_range_manager` 的 drm_mm 后端；**CPU 缺页路径**（`ttm_bo_vm_fault` 的可重试 reserve + 预取 + 热插拔 dummy page）。全部对照 v6.6（drivers/gpu/drm/ttm/ 五文件）当轮核对。前置：[09-dma-buf](09-dma-buf跨设备缓冲共享.md)（三层栈与 resv 基础）。

---

## 一、三层栈回顾（TTM 的职责边界）

```
TTM    —— 这块内存在哪（VRAM/GTT/SYSTEM？可搬可逐可换出）
GEM    —— 这是一个对象吗（handle/生命周期/dma-buf 出口）
dma-buf—— 怎么跨设备交出去（fd/attachment/resv）
TTM 的"交换空间思维"：显存不够 → 把 BO 搬到系统内存（位置对用户透明）
```

## 二、mem_type 抽象（include/drm/ttm/ttm_placement.h + ttm_resource.h）

```c
/* ttm_placement.h:51-54 */
#define TTM_PL_SYSTEM   0     /* 系统内存 */
#define TTM_PL_TT       1     /* GTT/aperture（GPU 可访问的系统内存） */
#define TTM_PL_VRAM     2     /* 显存 */
#define TTM_PL_PRIV     3     /* 驱动私有 */
/* 三钮：FLAG_CONTIGUOUS / FLAG_TOPDOWN / FLAG_TEMPORARY (:61-65) */

/* ttm_resource.h:151-170 */
struct ttm_resource_manager {
	bool use_type;  bool use_tt;                  /* 常量区 */
	struct ttm_device *bdev;
	uint64_t size;
	const struct ttm_resource_manager_func *func; /* alloc/free 回调 */
	spinlock_t move_lock;
	struct dma_fence *move;                       /* 整型搬移 fence（move_lock） */
	struct list_head lru[TTM_MAX_BO_PRIORITY];    /* ★按优先级的 LRU（lru_lock） */
	uint64_t usage;                               /* 已用量（lru_lock） */
};

/* 每个 BO 的占位凭证： */
struct ttm_resource {
	unsigned long start;  size_t size;
	uint32_t mem_type;  uint32_t placement;
	struct ttm_bus_placement bus;   /* addr/offset/is_iomem/caching */
	struct ttm_buffer_object *bo;
};
```

★`TTM_MAX_BO_PRIORITY = 4U`（ttm_resource.h:38）——**逐出的优先级旋钮**：LRU 按优先级分列，高优先级（数值小）的先被逐（`ttm_buffer_object::priority` 字段——[dma-buf 章的 ttm_buffer_object](09-dma-buf跨设备缓冲共享.md) 已见）。

## 三、LRU 逐出链（ttm_bo.c）

### 3.1 遍历者：ttm_mem_evict_first（:589-660）

```
持 bdev->lru_lock
ttm_resource_manager_for_each_res(man, &cursor, res)：      ← LRU 从头扫
  ttm_bo_evict_swapout_allowable(res->bo, ...)？             ← 三重判定
    （同 resv 共享者免锁直过 / deleted 延迟释放者可逐 /
      pin_count==0 才动——"钉住的不逐"）
  不行且 busy → 记 busy_bo（等它的票）→ continue
  可逐 → ttm_bo_get_unless_zero → 脱锁去逐
全部不行 → ttm_mem_evict_wait_busy(busy_bo...)（等忙碌 BO 的 fence）
bo->deleted → ttm_bo_cleanup_refs（清尸体而非搬移）
正常 → ttm_bo_evict(bo, ctx) → 搬回 LRU 尾
```

### 3.2 执行者：ttm_bo_evict（:423-473）——★-EMULTIHOP 弹跳循环

```c
	bdev->funcs->evict_flags(bo, &placement);   /* 驱动给"逐到哪"的建议 */
	...
	ret = ttm_bo_mem_space(bo, &placement, &evict_mem, ctx);
	...
	do {
		ret = ttm_bo_handle_move_mem(bo, evict_mem, true, ctx, &hop);
		if (ret != -EMULTIHOP)
			break;
		/* ★弹跳：目标不可直达（如 VRAM 满且 TT 也满）→
		   先腾中继站再走第二跳 */
		ret = ttm_bo_bounce_temp_buffer(bo, &evict_mem, ctx, &hop);
	} while (!ret);
```

——**多跳逐出**：VRAM 的 BO 想去 SYSTEM 但 TT 中转站也满时，`handle_move_mem` 返回 `-EMULTIHOP` + `hop`（建议的中继位置），`bounce_temp_buffer` 腾出中继再来一跳——死循环由逐出递归收敛（每跳腾出空间）。

### 3.3 值不值：ttm_bo_eviction_valuable（:481-497）

```c
	if (bo->resource->mem_type == TTM_PL_SYSTEM)
		return true;          /* SYSTEM 的 BO 逐了白逐？不——占的是 GTT 槽 */
	/* Don't evict this BO if it's outside of the requested placement range */
	return ttm_resource_intersects(bdev, res, place, bo->base.size);
```

## 四、★bulk_move：一整批 BO 的 O(1) LRU 挪移（ttm_resource.c:46-140）

### 4.1 结构：first/last 游标矩阵

```c
struct ttm_lru_bulk_move_pos {          /* :242 */
	struct ttm_resource *first;     /* 区间头 */
	struct ttm_resource *last;      /* 区间尾 */
};
struct ttm_lru_bulk_move {              /* :255 */
	struct ttm_lru_bulk_move_pos pos[TTM_NUM_MEM_TYPES][TTM_MAX_BO_PRIORITY];
};
```

### 4.2 一次性拼接（ttm_lru_bulk_move_tail，:53-79）

```c
void ttm_lru_bulk_move_tail(struct ttm_lru_bulk_move *bulk)
{
	for (i = 0; i < TTM_NUM_MEM_TYPES; ++i)
		for (j = 0; j < TTM_MAX_BO_PRIORITY; ++j) {
			pos = &bulk->pos[i][j];
			if (!pos->first)
				continue;
			man = ttm_manager_type(pos->first->bo->bdev, i);
			list_bulk_move_tail(&man->lru[j], &pos->first->lru,
					    &pos->last->lru);   /* ★O(1) 区间拼接 */
		}
}
```

### 4.3 契约与维护

```
头注释原文："Bulk move BOs to the LRU tail, only valid to use when
 driver makes sure that resource order never changes."
断言三连：lockdep_assert_held(lru_lock) + 首尾 BO 各 dma_resv_assert_held
维护（ttm_lru_bulk_move_pos_tail/:113-123）：新资源进批时只挪 last 指针
  （list_move 到 last 后）——批内序不变是 splice 的前提
ttm_bo_set_bulk_move（:97+）把一批 BO 挂上同一个 bulk——
  "making sure that resources are added/removed as well"
```

★收益：GPU 驱动常一次提交几百个 BO（一次 execbuf 的全家桶）——逐个 `list_move_tail` 是 O(n) 锁操作，bulk 把**整批**压成每 (mem_type, priority) 格一次拼接——**两次指针改写挪一批**。

## 五、默认后端：ttm_range_manager（ttm_range_manager.c，240 行）

```c
struct ttm_range_manager {              /* :46 */
	struct ttm_resource_manager manager;   /* 内嵌基类 */
	struct drm_mm mm;                      /* ★区间分配器（addr 语义） */
	spinlock_t lock;
};
```

alloc 即 `drm_mm_insert_node_in_range`（:85）——**线性地址空间的管理器**（VRAM 的 mmio 视图/GTT 槽位都长这样）；`FLAG_TOPDOWN` 直通 drm_mm 的插入模式。大驱动（amdgpu 的 VRAM）自写 manager 时仍以 range 为底加偏移/带宽策略。

## 六、系统内存侧：ttm_tt 与 ttm_pool

```
ttm_tt（ttm_tt.c）——BO 的"系统内存影子页表"：
  TTM_PL_SYSTEM/TT 的页集合 + 换出状态（swap）——ttm_bo_validate 落
  SYSTEM/TT 时建立；逐回 VRAM 时可保留（下次搬回免分配）
ttm_pool（ttm_pool.c 807 行）——分配页的池：
  struct ttm_pool { dev, nid, use_dma_alloc, use_dma32, ... }
  （[dma-buf 章] §六 已验）——逐出页热缓存，反复搬家不进伙伴系统
```

## 七、CPU 缺页路径（ttm_bo_vm.c:321-345）

```c
vm_fault_t ttm_bo_vm_fault(struct vm_fault *vmf)
{
	ret = ttm_bo_vm_reserve(bo, vmf);     /* ★可重试的 resv 锁
	          （VM_FAULT_RETRY 语义——持锁期间不阻塞更重要的路径） */
	if (ret)
		return ret;
	if (drm_dev_enter(ddev, &idx)) {
		ret = ttm_bo_vm_fault_reserved(vmf, prot, TTM_BO_VM_NUM_PREFAULT);
		                                  /* 预取窗口一次填多页 */
		drm_dev_exit(idx);
	} else {
		ret = ttm_bo_vm_dummy_page(vmf, prot);   /* ★热插拔后：
		                              映射哑页保存活性（dev 已 unplugged） */
	}
	if (ret == VM_FAULT_RETRY && !(vmf->flags & FAULT_FLAG_RETRY_NOWAIT))
		return ret;                       /* 交还内核带锁重试 */
	dma_resv_unlock(bo->base.resv);
	return ret;
}
```

配套：`ttm_bo_vm_fault_idle`（:41-51）先做 *"Quick non-stalling check"*（`dma_resv_test_signaled(bo->base.resv, DMA_RESV_USAGE_KERNEL)`——**GPU 还在用就别急着映射**，返回 RETRY 等下一次缺页）；vm_open/close 是 bo 的 get/put 对。

## 八、观测实战

```bash
# 1) 各 mem_type 的用量与 LRU 深度（驱动 debugfs 通用口）
ls /sys/kernel/debug/dri/0/                       # amdgpu 等各有 ttm 相关节点
#   官方 debug 钩子：ttm_resource_manager::func->debug 打 usage/LRU 概况

# 2) 逐出风暴问诊
dmesg | grep -i 'eviction failed\|out of memory' # ttm_bo_evict 的
                                                 # "Buffer eviction failed"
#   峰值显存占用 → cat /sys/kernel/debug/dri/0/amdgpu_vram_mm 等驱动节点

# 3) bulk_move 的收益面（驱动侧）
#   一次 execbuf 的 BO 数即为批大小——amdgpu/amdkfd 的 LRU 挪移路径
grep -rn 'ttm_lru_bulk_move' drivers/gpu/drm/amd/amdgpu/ | head -4

# 4) CPU 缺页路径的微观测
perf record -e dummy -e page-faults ./gpu_app     # fault 聚簇=预取不足或 idle 等待多
```

## 九、思考题

1. LRU 为什么按**优先级分列**（lru[4]）而不是单列加优先级字段？（提示：逐出扫描要"先看高优先级列再看低"——分列即排序，遍历免比较；单列每次扫全表比 prio 是 O(n·p)。）
2. -EMULTIHOP 弹跳为什么不无限循环？（提示：每跳都在**腾出空间**（中继站的占用者被继续逐）——单调减的资源占用保证收敛；hop 由驱动给的 busy_placement 决定中继位置。）
3. bulk_move 的"序不变"契约如果被驱动违反会发生什么？（提示：splice 的是 [first,last] 区间——若批内资源被单独 list_move 出去再 splice，区间会**裹进外人**——别人的 BO 被错误挪尾（年轻化）或反之（错误老化）。断言只防锁序，不防这个。）
4. `ttm_bo_eviction_valuable` 对 SYSTEM 恒真——逐 SYSTEM 的 BO 图什么？（提示：TT（GTT）槽位与 SYSTEM 共用 ttm_tt——SYSTEM BO 可能占着 GTT 映射；"白逐"是对 VRAM 逐出而言的，GTT 视角它有价值。）
5. vm_fault 的 dummy_page（热插拔分支）为什么映射**真页**而不是直接失败？（提示：进程持有的映射语义必须保持（SIGBUS 会杀无辜进程且下次缺页还是失败）；哑页让读拿到垃圾、写丢进黑洞——设备已拔，"成功"即安全降级。）
6. ttm_pool 逐出页不还给伙伴系统——与 [vmalloc 的 lazy purge](../../mm/vmalloc/vmalloc.md) 的候审树有何同构性？（提示：都是"回收有成本（TLB/分配器）→ 先挂自家池子等复用"——vmalloc 等 TLB 批刷、ttm 等 GPU 搬回；爆池才下沉给伙伴系统。）

---

> 回看：[09-dma-buf](09-dma-buf跨设备缓冲共享.md)（TTM 在三层栈的位置与 resv 集成）｜[drm_sched](../../locking/sync-file-drm-sched.md)（GPU 作业管线——TTM 的邻居）｜[buddy](../../mm/buddy/buddy.md)（ttm_pool 的下游）｜[vmalloc 惰性释放](../../mm/vmalloc/vmalloc.md)（池化思维的同构）｜[driver-model/README](../README.md)（分册导读）
