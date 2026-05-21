# list

## 说在前

在 Linux 6.6 内核里，`struct list_head` 是最核心、最基础的数据结构之一。

你需要掌握：
* 理解它的设计哲学
* 理解它与 cache / 并发 / 内存布局 的关系
* 能从 crash dump、RCU、调度器、VFS、网络栈中读懂复杂链表
* 能发现 list corruption
* 能设计高性能 intrusive list
* 能分析内核 exploit 中的 list 利用方式

struct list_head 不仅是核心数据结构，更是理解内核设计哲学、内存管理、并发控制乃至安全攻防的基石。掌握它，意味着你拿到了通往内核深层逻辑的钥匙。

---

**必须达到的层次**

你至少要达到下面 6 个层次：

| 层次         | 要求                                          |
| ---------- | ------------------------------------------- |
| 1. API 熟练  | 熟练使用所有 list API                             |
| 2. 宏与底层实现  | 理解 container_of / offsetof / intrusive list |
| 3. 内存布局理解  | 理解 cacheline、对象生命周期                         |
| 4. 并发与锁    | 理解 list + spinlock + RCU                    |
| 5. 调试与崩溃分析 | 能定位 list corruption                         |
| 6. 内核子系统实践 | 理解 scheduler / VFS / net / mm 中的 list       |

重点是后 4 个。

---

## list_head 本质

核心定义：

```c
struct list_head {
    struct list_head *next, *prev;
};
```

这是： 双向循环侵入式链表（intrusive circular doubly linked list）

三个关键词极重要：

1. **侵入式（Intrusive）**：这是 Linux 链表的灵魂。与普通链表“链表拥有对象”（节点包含数据指针）不同，Linux 链表是“对象拥有链表”（数据结构中嵌入链表节点）。例如在 `task_struct` 中直接嵌入 `struct list_head tasks`。这种设计实现了零额外内存分配、极强的 Cache Locality（缓存局部性），并且无需泛型开销，是 Linux 极致性能哲学的体现。
2. **循环（Circular）**：链表首尾相连，空链表的 `next` 和 `prev` 均指向自身。这种设计消灭了 NULL 指针判断的特殊情况，使得插入、删除等操作在 O(1) 时间复杂度内高度统一，且对分支预测极其友好。
3. **双向（Doubly Linked）**：每个节点同时保存前后指针，支持高效的双向遍历与删除操作。

---

### 侵入式（Intrusive）

这是 Linux list 的灵魂。

1. **内存布局与缓存局部性**（Cache Locality）

在传统链表中（如你提到的普通链表）：

```c
struct node {
    void *data;   // 指向堆上分配的对象
    struct node *next;
};
```

- `data` 是一个指针，指向另一个内存位置。解引用 data 指针去拿业务数据时，由于 data 的内存地址是随机分配的，极大概率不在当前的缓存行中。
- 遍历链表时，CPU 需要不断跳转到不同内存地址去读取实际数据，导致 **缓存未命中**（cache miss）频繁。
- 内存访问模式不连续，破坏了空间局部性。触发 Cache Miss（缓存未命中），CPU 必须暂停，花费几百个时钟周期去主存里把数据搬过来

```c
struct node {
    int data;
    struct node *next;
    struct node *pre;
};
```

- 数据与链表强耦合：无法复用。每种数据类型都需要重新定义一个链表结构
- 无法支持“一对象多链表”。一个对象只能在一个链表中！要加入第二个链表？必须再包装一层，或者复制数据
- 破坏缓存局部性（Cache Locality）。链表指针（next/pre）可能（分开）和常用字段不在同一个 cache line。
  - 将 list_head 嵌入到宿主结构体中，确实能从根本上保证链表指针（next/pre）与业务数据在物理内存上的紧密相邻，从而极大提升缓存局部性。
- 生命周期管理混乱。链表节点和数据是一体的，删除节点 = 释放整个 struct node。但如果这个对象还被其他子系统引用（比如还在另一个哈希表中），直接 free 就会导致 UAF（Use-After-Free）。
- 不通用。无法使用内核通用链表 API


而在 Linux 的侵入式链表中：

```c
struct task_struct {
    ...
    struct list_head tasks;  // 链表节点直接嵌入结构体
    ...
};
```

- 链表节点 `tasks` 是 `task_struct` 的一部分，二者在内存中 **紧邻存放**。
- 遍历时，CPU 加载 `list_head` 的同时很可能也加载了 `task_struct` 的其他字段（因为在一个 cache line 内）。
- **极强的缓存局部性** → 更高的 CPU 缓存命中率 → 更快的遍历速度。

> 在高频操作（如调度器遍历任务队列）中，这种优化至关重要。


2. **零额外内存分配**（No Extra Allocation）

传统链表：
- 每插入一个对象，需 **两次 malloc**：一次为对象，一次为链表节点。
- 增加内存碎片、分配开销、失败风险。

侵入式链表：
- 对象本身已包含链表节点，**无需额外分配**。
- 插入/删除仅修改指针，无内存管理开销。
- 在内核中尤其重要：**不能随意调用 malloc**（kmalloc 有上下文限制，且昂贵）。

---

3. **无泛型开销，类型安全**（通过宏实现“泛型”）

C 语言没有模板，但 Linux 用宏巧妙实现“泛型”链表：

```c
#define list_entry(ptr, type, member) \
    container_of(ptr, type, member)
```

- 通过 `container_of` 宏，从 `list_head` 指针反推出宿主结构体（如 `task_struct`）的地址。
- 编译时确定类型，**无运行时开销**。
- 虽然使用宏，但配合良好命名和封装（如 `list_for_each_entry`），代码可读性高。

> 这是一种“编译期多态”，比 void* + 强转更安全、高效。

---

4. **对象生命周期与链表解耦**

- 链表节点是对象的一部分，**对象的生命周期决定节点的存在**。
- 不会出现“链表节点还在，但对象已被释放”的悬空问题（前提是正确管理对象生命周期）。
- 删除节点时，只需从链表中摘除，**不负责释放对象内存**——由对象所有者处理。

这符合内核中“谁分配，谁释放”的原则。

---

5. **一个对象可属于多个链表**

例如，一个 `task_struct` 可同时在：

- 全局任务链表（`tasks`）
- 所属 CPU 的运行队列
- 所属 cgroup 的任务列表
- 等待 I/O 的睡眠队列

只需在结构体中嵌入多个 `struct list_head`：

```c
struct task_struct {
    ...
    struct list_head tasks;      // 全局链表
    struct list_head run_list;   // 运行队列
    struct list_head io_wait;    // I/O 等待队列
    ...
};
```

每个链表独立管理，互不干扰。这是非侵入式链表难以高效实现的。

---

6. **哲学意义：面向对象的 C 实践**

虽然 C 不是面向对象语言，但 Linux 内核大量使用 **“将行为与数据绑定”** 的思想：

- 链表不是独立容器，而是对象“能力”的一部分。
- 对象“主动参与”链表组织，而非被动被容器管理。
- 这种设计更贴近真实世界的建模：**实体自身具备关系属性**。


总结：为什么这是“Linux 性能哲学的一部分”？

| 维度 | 传统链表 | Linux 侵入式链表 |
|------|--------|----------------|
| 内存开销 | 额外节点 + 指针 | 零额外内存 |
| 缓存效率 | 差（指针跳转） | 极佳（数据连续） |
| 分配次数 | 多次 malloc | 一次分配对象即可 |
| 类型安全 | void*，易出错 | 宏+container_of，编译期安全 |
| 多链表支持 | 困难 | 天然支持 |
| 生命周期管理 | 容器管理对象 | 对象自主管理 |

> 正如 Linus Torvalds 曾批评过“不会用指针的人不该写内核代码”，侵入式链表正是这种 **贴近硬件、极致高效** 设计思维的体现。


这意味着：

* 没有额外 malloc
* cache locality 极强
* 无泛型开销
* 不需要 data 指针
* 零额外内存

这是 Linux 整体性能哲学的一部分。

专家必须深入理解：

“对象拥有链表节点” 而不是：  “链表拥有对象” ，这是 Linux 内核设计思维。

---

### 为什么是 circular（循环）

初始化：

```c
LIST_HEAD(mylist);
```

实际上：

```c
mylist.next = &mylist;
mylist.prev = &mylist;
```

空链表：

```text
head
 ↑  ↓
 └──┘
```

优点：

* 无 NULL 判断
* 插入删除统一
* O(1)
* 分支预测友好

专家必须理解：

Linux 非常强调：消灭特殊情况（eliminate special cases）

循环链表就是典型体现。

---

## container_of 必须彻底掌握

这是最重要的宏之一。

```c
#define container_of(ptr, type, member)
```

本质：

已知成员地址，反推对象地址。

例子：

```c
struct task_struct {
    int pid;
    struct list_head tasks;
};
```

如果：

```c
struct list_head *p = &task->tasks;
```

那么：

```c
task = container_of(p, struct task_struct, tasks);
```

底层：

```c
(type *)((char *)ptr - offsetof(type, member))
```

专家必须掌握：

* offsetof 原理
* pointer arithmetic
* 类型推导
* typeof
* 编译器优化
* UB 风险
* GCC extension

这是 Linux OOP 风格核心。

---

## 必须掌握的 API

Linux 内核中的链表支持以下操作：
- 初始化：LIST_HEAD、LIST_HEAD_INIT、INIT_LIST_HEAD
- 查询链表：list_is_last、list_empty、list_is_singular、list_entry、list_first_entry、list_last_entry、list_first_entry_or_null、list_next_entry、list_prev_entry
- 插入元素：__list_add、list_add
- 删除元素：__list_del、list_del
- 替换元素：list_replace
- 遍历链表：list_for_each、list_for_each_prev、list_for_each_entry、list_for_each_entry_reverse、list_prepare_entry、list_for_each_entry_continue、list_for_each_entry_continue_reverse、list_for_each_entry_from、list_for_each_entry_from_reverse
- 单个链表操作：list_rotate_left、__list_cut_position、list_cut_position
- 多个链表操作：list_move、list_move_tail、__list_splice、list_splice、list_splice_tail、list_splice_init、list_splice_tail_init


### 1. 初始化 - LIST_HEAD


```c
#define LIST_HEAD(name) \
    struct list_head name = LIST_HEAD_INIT(name)
```


### 1. 初始化 - LIST_HEAD_INIT


```c
#define LIST_HEAD_INIT(name) { &(name), &(name) }
```

### 1. 初始化 - INIT_LIST_HEAD


```c
// K:\include\linux\list.h
/**
 * INIT_LIST_HEAD - Initialize a list_head structure
 * @list: list_head structure to be initialized.
 *
 * Initializes the list_head to point to itself.  If it is a list header,
 * the result is an empty list.
 */
static inline void INIT_LIST_HEAD(struct list_head *list)
{
WRITE_ONCE(list->next, list);
WRITE_ONCE(list->prev, list);
}
```






---

### 2. 插入

```c
list_add()
list_add_tail()
```

必须理解：

```text
head <-> A <-> B
```

插入时 prev/next 如何修改。

专家必须能手写。

---

### 3. 删除

```c
list_del()
list_del_init()
```

重点：list_del 不会清空对象。这是大量 UAF 来源。

```c
entry->next = LIST_POISON1;
entry->prev = LIST_POISON2;
```

专家必须知道：poison pointer 调试机制

---

### 4. 遍历

```c
list_for_each()
list_for_each_entry()
```

必须理解：

* 为什么 entry 更常用
* container_of 如何嵌套
* 预取优化

---

### 安全遍历

```c
list_for_each_entry_safe()
```

这是删除场景必须掌握的。

否则：

```c
pos = pos->next
```

会 UAF。

专家必须理解： 为什么 safe 要保存 next

---

## 必须掌握的底层实现

真正专家必须能手写：

```c
static inline void __list_add(
    struct list_head *new,
    struct list_head *prev,
    struct list_head *next)
{
    next->prev = new;
    new->next = next;
    new->prev = prev;
    prev->next = new;
}
```

必须理解： 为什么写入顺序这样安排

涉及：

* 并发可见性
* transient corruption
* lockless traversal

---

## 必须掌握 list corruption

这是内核调试核心能力。


### 1. double add

同一节点插入两次：

```c
list_add(&obj->list, &head);
list_add(&obj->list, &head);
```

导致：

* 环损坏
* 无限循环

---

### 2. double del

```c
list_del(&obj->list);
list_del(&obj->list);
```

可能：

* poison dereference
* panic

---

### 3. UAF

对象释放：

```c
kfree(obj);
```

但 list 仍引用。

---

### 4. 并发损坏

无锁：

```c
CPU0 list_add
CPU1 list_del
```

导致：

* prev/next mismatch

---

## 必须掌握调试技术

## 1. CONFIG_DEBUG_LIST

开启：

```text
Kernel hacking
  -> Debug list operations
```

会检查：

* next->prev
* prev->next
* double add

专家必须会用。

---

## 2. crash 分析

必须会：

```bash
crash> struct list_head
```

遍历：

```bash
crash> list task_struct.tasks
```

---

## 3. drgn

现代内核调试神器。

专家应掌握：

```python
    for task in list_for_each_entry(...):
```

---

## 必须掌握并发模型

这是“高级”和“专家”的分水岭。

---

### 1. spinlock + list

最常见：

```c
spin_lock(&lock);
list_add(...)
spin_unlock(&lock);
```

必须理解：

* 原子性
* cache bouncing
* lock contention

---

### 2. RCU list

真正核心。

必须掌握：

```c
list_add_rcu()
list_del_rcu()
list_for_each_entry_rcu()
```

以及：

```c
rcu_read_lock()
synchronize_rcu()
```

专家标准：能解释 RCU 为什么允许 lockless read

---

### 3. memory barrier

必须理解：

```c
WRITE_ONCE()
READ_ONCE()
smp_wmb()
```

与 list 的关系。

---

## 必须理解内核里真实应用

### 1. task_struct

进程链表：

```c
task_struct.tasks
```

全系统任务循环。

---

### 2. VFS

inode/dentry：

```c
inode->i_sb_list
```

---

### 3. 网络栈

```c
sk_buff
```

大量 list 操作。

---

### 4. slab allocator

slub freelist。

---

### 5. timer wheel

时间管理。

---

## list_head 与 cache

专家必须理解：

链表为什么 cache locality 差。

---

普通链表缺点

```text
node1 -> node2 -> node3
```

随机跳转：

* cache miss
* TLB miss

---

Linux 为什么仍大量使用

因为：

* O(1) 插删
* intrusive 节省内存
* 内核对象生命周期复杂

---

专家需要知道替代方案

Linux 里：

| 数据结构       | 场景         |
| ---------- | ---------- |
| xarray     | page cache |
| maple tree | mm         |
| rbtree     | VMA        |
| hlist      | hash       |
| plist      | priority   |
| llist      | lockless   |

---

## 必须掌握 hlist

这是面试高频。

```c
struct hlist_head
struct hlist_node
```

特点：

* 单指针 head
* hash table 优化

大量用于：

* dcache
* netfilter
* inode hash

---

## 必须掌握 list_sort

Linux 有：

```c
list_sort()
```

归并排序。

专家需要理解：

* 为什么不用 quicksort
* 稳定性
* 链表适合 merge sort

---

## 必须掌握 exploit 相关

现代内核漏洞大量涉及 list。

必须理解： unlink attack

历史上：

```c
prev->next = next;
next->prev = prev;
```

被利用实现 arbitrary write。

后来加入：

* hardening
* sanity check
* poison

---

## 内核必须关注的新点

Linux 6.x 更强调：

* hardened list
* KASAN
* KFENCE
* DEBUG_LIST
* UBSAN

专家必须会结合：

```text
CONFIG_KASAN
CONFIG_DEBUG_LIST
CONFIG_SLUB_DEBUG
```

分析链表问题。

---

## 真正专家的标准

真正专家不是“会 API”。

而是：

看到一个 list_head：

立即能想到：

* 内存布局
* 生命周期
* 锁保护
* cacheline
* RCU
* corruption 风险
* exploitability

---

## 建议学习路线（非常重要）

### 第一阶段

必须手写：

* add
* del
* splice
* rotate

---

### 第二阶段

读源码：

```text
include/linux/list.h
```

逐行理解。

---

### 第三阶段

读：

```text
kernel/sched/core.c
```

看 task list。

---

### 第四阶段

读：

```text
net/core/skbuff.c
```

---

### 第五阶段

调试：

* crash
* drgn
* gdb vmlinux

---

### 第六阶段（专家）

研究：

* RCU list
* lockless list
* exploit
* memory ordering

---

##必须能达到的能力（最终目标）

你应该能：

1. 手写完整 intrusive list。
2. 看汇编理解 list_add。
3. 分析 list corruption panic。
4. 设计 SMP-safe list。
5. 分析 RCU list race。
6. 从 vmcore 恢复整个链表。

---

## 二十、推荐源码（非常关键）

Linux 6.6：

核心头文件

```text
include/linux/list.h
```

RCU

```text
include/linux/rculist.h
```

hlist

```text
include/linux/list_bl.h
```

---

## 推荐阅读顺序

建议：

```text
list_head
 -> hlist
 -> rbtree
 -> xarray
 -> maple tree
 -> RCU
```

这是 Linux 内核数据结构主线。

---

## 一句话总结

如果只是“会用 list_add/list_for_each”，只能算初级。

真正 Linux 内核专家，需要掌握的是：

* intrusive 数据结构思想
* container_of
* 并发与 RCU
* memory ordering
* corruption 调试
* cache 与性能
* exploit 与 hardening
* 子系统真实实现

这才是 Linux 内核中 `list_head` 的完整专家级知识体系。

---