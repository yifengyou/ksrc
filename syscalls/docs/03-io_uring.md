# io_uring：异步 IO 的终极形态（Linux 6.6）

> 阅读目标：掌握三环结构（SQ ring/SQE/CQ ring）与共享内存协议；吃透三条执行形态（默认线程池/SQPOLL/IOPOLL）的取舍；理解注册资源、multishot、linked SQE、task_work 完成收割；建立与 block/mm/workqueue/process-mgmt 各分册的连接；正视 2023 年的安全风波与部署现实。

---

## 一、动机：epoll 的税单

```
epoll（高性能服务的标配）做到了"一次等待多路复用"，但**每个就绪事件
仍要一次系统调用去读**——100K QPS 的代理服务器单是 read/write 就吃掉
数十万 syscall/秒。POSIX AIO（glibc）是用户态线程模拟的伪异步。
io_uring（5.1，Jens Axboe）的答案：
  批量化：N 个操作一次 syscall 提交
  完成收割：N 个完成一次收割（甚至零 syscall——SQPOLL）
  注册资源：buffers/files 固定 pin（省每次校验）
```

---

## 二、三环结构与内存协议

`io_uring_setup(entries, params)`（io_uring/io_uring.c，6.6 已核对 `SYSCALL_DEFINE2`）返回 fd，用户态按 `io_uring_params` 的 offsets mmap：

```
┌────────────────────────────────────────────────────────────┐
│ SQ ring（状态环）: head/tail/flags + array（SQE 下标）        │
│ SQE 环        : struct io_uring_sqe[entries]（8 字段×8B=64B）│
│ CQ ring（状态环）: head/tail/overflow + CQE 环                │
│   struct io_uring_cqe{ user_data, res, flags } —— 16 字节    │
└────────────────────────────────────────────────────────────┘
提交侧（应用）：
  取 SQE（tail 处）→ 填 opcode/fd/addr/len/off/user_data → SQ.tail++
  （tail 的写入即提交动作——内核按 head..tail 界消费）
  最后 io_uring_enter(fd, to_submit, ...) 触发收割或依赖 sqpoll
完成侧（内核）：
  CQE 写 user_data 回执 + res 结果 → CQ.tail++
  （应用读 CQ.head..CQ.tail 对账）
```

**user_data 的对账设计**：SQE 里任意 64 位应用自留，完成原样奉还——把"哪个请求完成了"的映射交给应用（常用指针/序号），内核不做协议假设。

`io_uring_params.features`（协商位，`IORING_FEAT_NODROP` 等已核对 uapi 头）：

* **NODROP**：CQ 满不再丢完成——进后备链表并置 overflow 标志，收割时回灌；
* **SQ_AFF/Single MMAP** 等其余位：老内核协商用。

---

## 三、SQE 的执行形态（三条路）

```
① 默认：io_uring_enter 触发 → 当前上下文直接执行（非阻塞部分）
   + 阻塞部分交 io-wq 内核线程池（按需伸缩——异步化的"兜底线程"）
② IORING_SETUP_SQPOLL：内核 sq 线程持续轮询 SQ.tail——
   应用纯内存写 SQE 即提交（零 syscall），空闲 sq_thread_idle 后 park
   并置 IORING_SQ_NEEDWAKEUP（应用需 enter(SQ_WAKEUP) 叫醒——
   空转与省电的折中协议）
③ IORING_SETUP_IOPOLL：NVMe 轮询队列（block 分册 POLL 队列）——
   不发完成中断，enter 时收割完成；与 SQPOLL 叠加 = 提交/完成/等待
   全程零系统调用的极限形态（DPDK 化的存储栈）
```

---

## 四、opcode 家族与高级特性

6.6 的 opcode 覆盖存储（READV/WRITEV/READ_FIXED…）、网络（SEND/RECV/ACCEPT/CONNECT…）、文件（OPENAT/CLOSE/STATX…）、杂项（NOP/timeout/…）四十余个（uapi 头 `IORING_OP_*` 至 `IORING_OP_LAST` 已核对）：

* **linked SQE**（`IOSQE_IO_LINK`）：前驱成功才执行后继——把"读→处理→写"的微管线压进一次提交；
* **multishot**（6.1+）：一次提交持续产出多次完成（accept 的"来一个连接回一个 CQE"）；
* **注册资源**：`io_uring_register`（`SYSCALL_DEFINE4` 已核对）的 BUFFERS/FILES——固定 pin，每次请求省 pin/unpin 与 fdget；
* **提供的缓冲环**（provided buffer rings，6.0+）：网络接收的"内核挑缓冲"模式（配合 multishot recv——io_uring 版的零拷贝接收）。

---

## 五、完成收割与跨分册的机器

```
完成的最后一跳：CQE 写入后如何让应用知道？
  阻塞收割：enter(GETEVENTS) 睡到 min_complete——eventfd 异步通知
            （IORING_REGISTER_EVENTFD）是"应用侧 epoll 集成"的桥
内核侧收割（sqpoll/线程池模式）用 task_work：
  目标上下文的返回用户态路径收割 CQE（workqueue 分册的 task_work
  与 process-mgmt 分册 exit_task_work 的 io_uring TWA 清理——
  进程退出时未决请求的取消/强释放都在那里）
```

跨分册连接图：

```
io_uring ─┬─ block 分册：plug 合并（io_uring 的 need_plug 按 max_ios>2）、
          │             blk-mq 的 POLL 队列、iocost/blkcg 记账
          ├─ mm 分册：registered buffers 的 pin（pin_user_pages）
          ├─ net 分册：multishot accept/send_zc 的 data 面整合
          ├─ workqueue 分册：io-wq 线程池
          └─ syscalls 分册本册：三入口（setup/enter/register）
```

---

## 六、安全风波（工程现实）

2023 年起 Google（ChromeOS/Android）、Fedora 等相继**默认禁用 io_uring**：其攻击面（大量 opcode 与文件系统/网络子系统深度耦合 + 与 cred 生命周期的交互）在内核漏洞统计中占比畸高。现实姿势：

```
sysctl kernel.io_uring_disabled（新装默认关在部分发行版）
容器 seccomp 配置默认拒绝 io_uring_setup（上一章的实践直接应用）
持有威胁模型的部署（高价值多租户）应默认关闭，按白名单开
```

**笔记立场**：机制值得深学（异步 IO 的最优设计教科书），部署按威胁模型决策——两条线不要混。

---

## 七、观测与实战

```
# 最小骨架（伪码；生产用 liburing）
fd = io_uring_setup(64, &p);
mmap 三块；io_uring_register(files/buffers)（可选）
sqe = get_sqe(); 填 opcode=READV/fd/off/len/user_data=ticket; SQ.tail++
io_uring_enter(fd, 1, 1, IORING_ENTER_GETEVENTS);   # 或交给 sqpoll
for each cqe: 按 user_data 对账 res

# 压测对照
fio --ioengine=io_uring --sqthread_poll=1   vs   --ioengine=libaio
# 观测：perf trace 看三个 syscall 频率；iou-mgt 线程的 CPU 占比（sqpoll 税）
```

---

## 八、思考题

1. SQ 与 CQ 的 head/tail 各自由谁写、谁读？写者搞反会在哪种内存序下先出问题？（对照 release/acquire 的配对方向与 `smp_store_release` 的使用点。）
2. NODROP 前的"完成即丢"语义为什么不可接受？后备链表把丢事件的点移到了哪里？（提示：收割时 CQ 空间仍不足的路径。）
3. SQPOLL 的 `NEEDWAKEUP` 协议里应用忘了叫醒，最坏观察是什么？为什么不是死锁？（对照 sq 线程的 park 条件。）
4. registered buffers 把 pin 常驻——代价是什么？哪类负载反而变慢？（联系 cgroup 分册 memory 控制器与回收的交互。）
5. 对比三形态（默认/io-wq、SQPOLL、IOPOLL）在"syscall 数/延迟下界/CPU 占用"三维的曲线，各举一个最优场景。

---

> 本分册 3 章完整。回看：[01 系统调用入口与分发](01-系统调用入口与分发.md)｜[02 seccomp](02-seccomp.md)｜[03 io_uring](03-io_uring.md)
