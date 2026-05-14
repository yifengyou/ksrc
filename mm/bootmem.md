# bootmem

Bootmem 是 Linux 内核早期版本中使用的一种**引导内存分配器**。它的核心作用是在内核启动的初始阶段，为那些需要在主内存管理系统（如伙伴系统）初始化之前就必须分配内存的代码提供服务。

简单来说，当内核开始运行时，复杂的伙伴系统等内存管理机制还未准备就绪，但内核自身又需要内存来构建数据结构。Bootmem 就是一个简单的、临时的“急救包”，用于满足这段空窗期的内存需求。一旦伙伴系统初始化完成，bootmem 就会将剩余的内存移交出去，其自身的使命也就结束了。

在现代 Linux 内核中，更先进的 `memblock` 分配器已经逐渐取代了 `bootmem`，但理解 `bootmem` 对于学习内核内存管理的演进非常有帮助。

## 🧠 核心数据结构

Bootmem 的设计思想非常简洁，其核心是使用一个位图（bitmap）来追踪物理内存页的使用状态。所有相关信息都封装在 `bootmem_data_t` 结构体中，每个 NUMA 节点都有一个该结构体的实例。

```c
// include/linux/bootmem.h
typedef struct bootmem_data {
    unsigned long node_min_pfn;   // 该节点管理的起始物理页帧号 (Page Frame Number)
    unsigned long node_low_pfn;   // 该节点管理的结束物理页帧号
    void *node_bootmem_map;       // 指向位图的指针，每一位代表一个物理页的使用情况
    unsigned long last_end_off;   // 上次分配的内存块结束位置的字节偏移，用于优化小内存分配
    unsigned long hint_idx;       // 上次分配的内存块之后的页索引，作为下次分配的起点提示
    struct list_head list;        // 用于将所有节点的 bootmem_data 连接成一个全局链表
} bootmem_data_t;
```

*   **`node_bootmem_map`**: 这是 bootmem 的灵魂。它是一个位图，其中**每一个比特（bit）对应一个物理内存页**。如果某个比特位被置为 `1`，表示对应的物理页已被分配；如果为 `0`，则表示该页空闲。
*   **`last_end_off` 和 `hint_idx`**: 这两个字段是一种优化策略。它们记录了上一次分配结束的位置，使得下一次分配可以优先从上次结束的地方继续寻找，避免了每次都从头扫描位图，提高了分配效率。

这个 `bootmem_data_t` 结构体本身被嵌入到每个内存节点的描述符 `pglist_data` 中。

```c
// include/linux/mmzone.h
typedef struct pglist_data {
    // ... 其他成员 ...
#ifndef CONFIG_NO_BOOTMEM
    struct bootmem_data *bdata;  // 指向该节点的 bootmem 数据
#endif
    // ... 其他成员 ...
} pg_data_t;
```

## ⚙️ 工作原理与源码分析

Bootmem 的工作流程可以分为初始化和内存分配两个主要部分，核心代码位于 `mm/bootmem.c` 文件中。

### 1. 初始化 (`init_bootmem_core`)

在内核启动时，会通过 `init_bootmem_core` 函数来初始化 bootmem 分配器。

```c
// mm/bootmem.c
static unsigned long __init init_bootmem_core(pg_data_t *pgdat, unsigned long mapstart, unsigned long start, unsigned long end)
{
    bootmem_data_t *bdata = pgdat->bdata;
    unsigned long mapsize;

    // 1. 计算位图所需的大小：(总页数 + 7) / 8，即每8个页用一个字节表示
    mapsize = ((end - start) + 7) / 8;
    mapsize = ALIGN(mapsize, sizeof(long)); // 对齐到 long 类型大小，提高访问效率

    // 2. 将位图的虚拟地址存入 bdata 结构
    bdata->node_bootmem_map = phys_to_virt(mapstart << PAGE_SHIFT);

    // 3. 记录该节点管理的物理内存范围
    bdata->node_boot_start = (start << PAGE_SHIFT);
    bdata->node_low_pfn = end;

    // 4. 将此节点的 bdata 加入全局链表 bdata_list，并按起始地址排序
    link_bootmem(bdata);

    // 5. 【关键步骤】将所有位图的比特位设置为 1 (0xff)，意味着所有页面初始状态都是“已保留/已占用”
    memset(bdata->node_bootmem_map, 0xff, mapsize);

    return mapsize;
}
```

这里有一个非常重要的点：**内核在初始化 bootmem 时，会先将所有物理内存页都标记为“已占用”**。之后，内核会通过调用 `free_bootmem()` 函数，将真正可用的空闲内存区域（RAM）显式地释放给 bootmem 管理。这样做的好处是，任何未被显式释放的内存都会被自动保留，防止被意外使用。

### 2. 内存分配 (`alloc_bootmem_bdata`)

当有组件需要申请内存时，会调用 `alloc_bootmem` 等接口，最终会进入核心的分配函数 `alloc_bootmem_bdata`。

其基本算法是**最先适配（First-Fit）**：

1.  **扫描位图**: 从 `hint_idx` 提示的位置开始，扫描 `node_bootmem_map` 位图。
2.  **寻找连续零**: 寻找一段连续的、值为 `0` 的比特位，其长度足以满足请求的页数。
3.  **标记占用**: 找到合适的空闲区域后，将这段区域内对应的所有比特位全部置为 `1`，表示这些页现在已被分配。
4.  **更新提示**: 更新 `last_end_off` 和 `hint_idx`，为下一次分配做准备。
5.  **返回地址**: 根据找到的页帧号计算出实际的物理地址并返回。

这个过程保证了分配的简单性和高效性。

## ✨ 演变与现状

随着硬件架构日益复杂，特别是像 IA64、ARM64 等新平台的出现，`bootmem` 的设计显得有些力不从心。因此，Linux 内核社区开发了更强大、更灵活的 `memblock` 分配器来替代它。

*   **`memblock` 的优势**: `memblock` 不再依赖位图，而是使用一组“区域（region）”列表来管理内存，可以更高效地处理稀疏内存和复杂的内存布局。
*   **过渡期**: 为了保证平滑过渡和向后兼容，内核曾提供一个名为 `nobootmem` 的兼容层。当启用 `CONFIG_NO_BOOTMEM` 配置选项时，`memblock` 就会接管原本由 `bootmem` 负责的早期内存分配工作，同时对外提供与 `bootmem` 相同的 API。
*   **当前状态**: 在现代 Linux 内核中（尤其是 ARM64 架构），`bootmem` 已经被完全移除，`memblock` 成为了标准的引导内存分配器。




随着硬件架构的演进，特别是 IA64、ARM64 等新平台的出现，`bootmem` 分配器的设计局限性日益凸显。其根本原因在于，`bootmem` 基于位图（bitmap）的核心机制无法适应现代复杂硬件环境下的内存管理需求。

具体原因可以从以下几个方面详细阐述：

## 🧱 核心设计的固有缺陷：位图机制

`bootmem` 使用一个全局的位图来追踪物理内存页的使用状态，其中**每一个比特（bit）代表一个物理页**。这种设计在早期内存布局相对简单的系统上尚可工作，但在面对新架构时暴露了严重问题：

*   **稀疏内存效率低下**：现代服务器和移动设备通常配备大容量的非连续物理内存（即稀疏内存）。对于 `bootmem` 来说，即使物理内存之间存在巨大的空洞，它也必须为整个从最低到最高的地址范围分配位图空间。这导致了大量内存被浪费在表示不存在的“空洞”页面上，初始化过程也变得冗长且低效。
*   **初始化依赖复杂**：位图本身需要一块连续的内存来存放。在内核启动的最早期阶段，找到这样一块足够大的连续内存本身就是一项挑战，增加了初始化的复杂性。

## 🔢 寻址能力的瓶颈：32位限制

这是 `bootmem` 最致命的弱点之一。

*   **无法支持大物理地址扩展 (LPAE/PAE)**：许多 32 位架构（如 ARMv7-A）通过 PAE 或 LPAE 技术支持超过 4GB 的物理内存。在这种情况下，物理内存的起始地址就可能超出 4GB。然而，`bootmem` 的核心 API 和数据类型（如 `unsigned long`）在很多架构上是 32 位的，根本无法正确表示和操作这些高位地址。这直接导致 `bootmem` 在这些平台上完全失效。
*   **对比优势**：而新的 `memblock` 分配器从一开始就采用 64 位物理地址类型 (`phys_addr_t`)，天然地解决了这个问题，能够无缝支持从低端到高端的全部物理内存范围。

## 🧩 缺乏对 NUMA 架构的原生支持

非统一内存访问（NUMA）是现代多路服务器系统的标准配置。

*   **管理方式僵化**：虽然 `bootmem` 后期通过 `bootmem_data_t` 结构体为每个 NUMA 节点维护了独立的数据，但其底层依然是基于位图的。这使得它在处理跨节点的内存分配和管理时显得非常笨拙和低效。
*   **抽象层次不足**：`memblock` 则采用了更高级的抽象，它将物理内存视为一组“区域（region）”的集合，并为每个区域关联了 NUMA 节点 ID (`nid`)。这种基于区域的列表管理方式，比基于位图的逐页管理要灵活得多，能更优雅、高效地描述和管理复杂的 NUMA 拓扑结构。

## 📜 历史包袱与代码冗余

`bootmem` 是 Linux 内核发展早期的产物，承载了大量针对特定旧硬件的历史代码。随着时间推移，这些代码变得难以理解和维护。引入 `memblock` 并逐步淘汰 `bootmem`，也是 Linux 内核社区进行代码清理和现代化重构的一部分，旨在用一个更简洁、更通用的框架来取代一个充满补丁和特例的旧系统。

总而言之，`bootmem` 因其基于位图的简单设计，在面对现代硬件的大容量、稀疏性、高地址和 NUMA 等复杂特性时，表现出了性能低下、功能缺失和维护困难等问题。因此，被设计更先进、适应性更强的 `memblock` 所取代是必然的技术演进结果。

