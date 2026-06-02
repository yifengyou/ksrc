# buddy

## 一、伙伴系统基础原理（理论根基）
### 1. 经典Buddy算法原理
1. 伙伴块定义：**物理页按2^n阶(Order)分组**，相邻同阶块合并为高一阶、高阶拆分出两个低阶伙伴
2. Order值域：`0~MAX_ORDER`，Order0=1页(PAGE_SIZE)、Order1=2页……
3. 伙伴判定规则：物理地址对齐、互为另一半，`(page_idx ^ (1<<order))`得到伙伴页帧号
4. 分配逻辑：
    - 按需向上取阶，对应空闲链表无空闲则向上找高阶拆分
    - 释放：相邻伙伴空闲则循环合并，直到无法合并
5. 优缺点：外部碎片少、拆分合并开销可控；小块分配内部碎片

### 2. 物理内存分区模型基础
1. 物理帧PFN、page结构体、`struct page`基础字段（flags/_count/_mapcount等与buddy关联成员）
2. 内存域（Zone）：ZONE_DMA/ZONE_DMA32/ZONE_NORMAL/ZONE_HIGHMEM/ZONE_MOVABLE分区划分逻辑，**每个Zone独立一套buddy空闲链表**
3. 物理内存布局：E820、memblock早期内存管理（buddy初始化依赖memblock）

## 二、Linux内核Buddy源码核心实现（Linux标准MM，重点）
### 1. 核心数据结构
1. `struct zone`：`free_area[MAX_ORDER]`数组，每阶对应一条空闲页链表
2. `struct free_area`：`free_list[MIGRATE_TYPES]`（**迁移类型MIGRATE分类是现代buddy核心**）、nr_free统计
3. 迁移类型（MIGRATE_UNMOVABLE/MIGRATE_MOVABLE/MIGRATE_RECLAIMABLE/MIGRATE_HIGHATOMIC等）原理：**按页可移动性分组，减少碎片化**
4. `MAX_ORDER`内核配置，不同架构(x86/arm64)默认值差异

### 2. 核心API源码实现
#### （1）分配接口
1. `__alloc_pages()`：buddy顶层入口，分配物理页核心函数
2. `alloc_pages_nodemask`、`alloc_page(gfp_t)`、`__get_free_page`系列
3. gfp_t标志位：GFP_KERNEL/GFP_ATOMIC/__GFP_HIGHMEM/__GFP_MOVABLE/__GFP_RECLAIM等对buddy分配策略影响
4. 分配流程：优先本迁移类型→备用迁移链表→备用迁移fallback列表→内存回收→OOM

#### （2）释放接口
1. `__free_pages()`、`free_page()`、`free_pages()`完整释放链路
2. 释放时伙伴查找、合并逻辑`__free_one_page`源码流程

### 3. 页拆分/合并核心函数
1. `expand()`：高阶页拆分，逐级拆分成低阶放入对应free_list
2. `__free_one_page()`：释放页，查找伙伴、循环合并order
3. `find_buddy_pfn()`伙伴PFN计算、`page_is_buddy()`伙伴有效性校验

### 4. Buddy系统初始化流程
1. memblock向buddy移交物理内存：`free_area_init()`→`free_area_init_core()`→`init_currently_empty_zone()`
2. 开机内存按zone、迁移类型加入对应free_area空闲链表
3. 预留内存(bootmem/reserve)不加入buddy空闲池规则

## 三、页迁移&防碎片机制（现代Linux Buddy高阶知识点，资深必备）
### 1. MIGRATE隔离机制
1. 可移动/不可移动页分区原理，大块连续内存预留（CMA依赖buddy迁移）
2. fallback迁移链表配置，分配失败跨迁移类型借用规则

### 2. CMA（连续内存分配器，基于Buddy）
1. CMA区域初始化：预留一段连续物理内存挂入buddy特殊迁移类型
2. CMA分配时：触发页迁移、腾出连续大块order内存，CMA与buddy交互全流程

### 3. 内存规整（Memory Compaction）
1. `compact_zone()`：规整扫描、移动可迁移页，合并零散空闲页生成高阶大块
2. 触发时机：buddy高阶分配失败、kswapd/直接回收后触发规整
3. 规整两种模式：同步规整(直接分配触发)、异步规整(kcompactd内核线程)

### 4. 页碎片预防：页面防碎片化(anti-fragmentation)、min_free_kbytes、watermark水位线
1. zone三大水位：min/low/high，水位低于阈值触发直接页回收，影响buddy空闲量
2. 预留原子页池MIGRATE_HIGHATOMIC：防止GFP_ATOMIC分配无内存

## 四、周边子系统联动（Buddy不是孤立，专家需掌握联动逻辑）
### 1. 与slub/slob/slram分配器联动
1. SLUB依赖buddy获取整阶物理页做缓存池，`kmem_cache_alloc`底层最终走到buddy
2. SLUB页回收、释放回buddy的流程

### 2. 与页回收(kswapd)、OOM联动
1. kswapd回收匿名页/文件页→回收页放回buddy空闲链表，补充空闲页
2. buddy空闲不足→直接页面回收(direct_reclaim)→回收失败触发OOM killer

### 3. 与HugePage（大页）联动
1. 透明巨页THP：需要buddy提供连续高阶物理页(order=9~11等)，分配失败触发规整
2. 静态HugePage：开机从buddy预留大块连续页，脱离buddy空闲池

### 4. 内存热插拔、NUMA架构下Buddy
1. NUMA：每个node多个zone，**每个node独立buddy free_area**，numa内存优先本地node分配
2. 内存热插拔：新内存上线加入对应zone的buddy空闲链表、下线反向回收

### 5. memfd、DMA、设备驱动
1. DMA连续内存：DMA32 zone buddy分配，CMA用于大DMA连续内存
2. 驱动`dma_alloc_coherent`底层依赖buddy/CMA

## 五、调试、统计、性能优化&故障定位（资深专家落地能力）
### 1. Buddy系统统计与查看接口
1. /proc/meminfo：MemFree、HighFree、LowFree、PageFree各阶空闲统计
2. /proc/pagetypeinfo：**各zone+迁移类型+order空闲页明细（碎片分析核心）**
3. /sys/kernel/mm/compact、/sys/kernel/mm/hugepages 相关节点
4. `vmstat`、slabtop、内核tracepoint：`trace_alloc_pages/trace_free_pages`追踪buddy分配释放

### 2. 常见故障与问题
1. **高阶内存碎片化**：大页/THP/CMA分配失败，pagetypeinfo分析碎片来源
2. GFP_ATOMIC分配失败：原子池不足、min水位配置不合理
3. 直接回收频繁：buddy空闲水位偏低，内存泄漏(页未free回buddy)
4. 内存规整开销高：频繁compact导致CPU抖动，优化迁移分组

### 3. 内核参数调优项
1. min_free_kbytes：系统最小预留空闲页，影响zone水位
2. compact_memory、kcompactd配置、THP开关(always/madvise/never)
3. cma_size配置、MIGRATE分区预留比例配置
4. sysctl.vm.watermark_scale_factor 水位缩放因子

### 4. 内核调试手段
1. crash工具：查看struct zone/free_area、page结构体、空闲链表
2. 内核dump_stack追踪异常页泄漏（__free_pages漏调用导致buddy空闲不增长）
3. 内核boot参数：`page_alloc.debug`开启buddy分配释放调试检测

## 六、不同内核版本Buddy演进（资深专家跨版本兼容必备）
1. Linux2.6早期：无MIGRATE迁移分类，全全局链表，碎片严重
2. Linux3.x：引入MIGRATE迁移分组、内存规整compaction、CMA落地
3. Linux4.x：MIGRATE_HIGHATOMIC、kcompactd独立线程、THP精细化管理
4. Linux5.x~6.x：buddy分页优化、NUMA规整优化、ZONE_MOVABLE增强、离散页跟踪
5. 嵌入式/Android内核裁剪：MAX_ORDER裁剪、关闭CMA/规整的定制修改

## 七、拓展：特殊架构下Buddy差异
1. ARM32/ARM64：高端内存ZONE_HIGHMEM在ARM64废弃，全线性映射
2. 大页架构、PAE(x86-32)开启后zone布局与buddy变化
3. 嵌入式小内存系统：裁剪MAX_ORDER、精简迁移类型的定制buddy实现

需要我按「面试考点精简版」或「源码学习路线顺序版」再压缩一版吗？