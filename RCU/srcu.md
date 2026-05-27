# SRCU

在 Linux 内核 6.6 中，**SRCU（Sleepable RCU）** 是 RCU（Read-Copy-Update）机制的一个变种，专为**允许在读端临界区中睡眠（sleep）**的场景设计。这与传统 RCU 不同，后者要求读端必须是原子上下文（不可阻塞、不可调度）。以下是对 SRCU 在 Linux 6.6 内核中的详细介绍：

---

## 一、SRCU 的核心特点

- **可睡眠读端**：SRCU 允许读者在 `srcu_read_lock()` 和 `srcu_read_unlock()` 之间调用可能引起睡眠的函数（如 `mutex_lock()`、`kmalloc(GFP_KERNEL)` 等）。
- **独立的同步域**：每个 `struct srcu_struct` 实例代表一个独立的 SRCU 域，不同域之间互不影响。
- **显式初始化/清理**：需要通过 `init_srcu_struct()` 初始化，并在不再使用时调用 `cleanup_srcu_struct()`。
- **回调宽限期（Grace Period）等待**：更新者需调用 `synchronize_srcu()` 或 `call_srcu()` 来等待所有当前读者完成。

---

## 二、关键数据结构（Linux 6.6）

### 1. `struct srcu_struct`
定义在 `include/linux/srcu.h`，包含两个计数器数组（用于双缓冲机制）和锁等字段：

```c
struct srcu_struct {
    struct srcu_data __percpu *sda;   // per-CPU 数据
    unsigned long srcu_idx;           // 当前活动的计数器索引（0 或 1）
    spinlock_t lock;
    ...
};
```

### 2. `struct srcu_data`
每个 CPU 拥有一个 `srcu_data`，记录该 CPU 上的读者数量和宽限期状态。

---

## 三、主要 API（基于 Linux 6.6）

| 函数 | 说明 |
|------|------|
| `DEFINE_STATIC_SRCU(name)` / `init_srcu_struct()` | 静态或动态初始化一个 `srcu_struct` |
| `srcu_read_lock(struct srcu_struct *ssp)` | 进入 SRCU 读端临界区，返回 cookie（用于 unlock） |
| `srcu_read_unlock(struct srcu_struct *ssp, int idx)` | 退出读端临界区 |
| `synchronize_srcu(struct srcu_struct *ssp)` | 同步等待所有当前读者完成（阻塞调用） |
| `call_srcu(struct srcu_struct *ssp, struct rcu_head *head, rcu_callback_t func)` | 异步注册宽限期完成后的回调 |
| `cleanup_srcu_struct(struct srcu_struct *ssp)` | 清理资源（必须在无活跃读者/回调后调用） |

---

## 四、实现机制简述

1. **双缓冲计数器**：
   - 使用两个计数器（index 0 和 1），交替记录读者。
   - 写者切换“当前索引”并等待旧索引上的读者全部退出。

2. **宽限期检测**：
   - `synchronize_srcu()` 触发宽限期开始。
   - 内核通过 per-CPU 的 `srcu_data` 检查所有 CPU 是否已无读者（旧索引计数为 0）。

3. **回调处理**：
   - 类似于 RCU 的 `call_rcu()`，`call_srcu()` 将回调加入链表，在宽限期结束后执行。

---

## 五、使用场景

- **文件系统**：如在 inode 或 dentry 的遍历中可能需要睡眠。
- **设备驱动**：某些驱动在读取设备状态时需等待硬件响应。
- **模块卸载**：确保没有读者正在访问即将释放的数据结构。

> ⚠️ 注意：SRCU 的宽限期通常比普通 RCU 更长，且开销更大，应仅在确实需要睡眠时使用。

---

## 六、源码位置（Linux 6.6）

- 头文件：`include/linux/srcu.h`
- 实现文件：`kernel/locking/srcu.c`
- 测试代码：`tools/testing/selftests/rcu/`

