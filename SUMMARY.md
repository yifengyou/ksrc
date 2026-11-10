# Summary

* [Introduction](README.md)

---

## 一、内核启动（init）

* [启动分册导读](init/README.md)
* [内核启动总览：从上电到 start_kernel](init/docs/01-内核启动总览.md)
* [start_kernel 详解](init/docs/02-start_kernel详解.md)
* [从 rest_init 到用户空间](init/docs/03-从rest_init到用户空间.md)
* [initcall 与模块初始化机制](init/docs/04-initcall与模块初始化机制.md)

## 二、进程调度（scheduler）

* [调度分册导读](scheduler/README.md)
* [调度器演进与总体架构](scheduler/docs/01-调度器演进与总体架构.md)
* [核心数据结构](scheduler/docs/02-核心数据结构.md)
* [CFS 与 EEVDF 详解](scheduler/docs/03-CFS与EEVDF详解.md)
* [调度时机与上下文切换](scheduler/docs/04-调度时机与上下文切换.md)
* [PELT 与利用率追踪](scheduler/docs/05-PELT与利用率追踪.md)
* [调度域与负载均衡](scheduler/docs/06-调度域与负载均衡.md)
* [实时与 Deadline 调度](scheduler/docs/07-实时与Deadline调度.md)
* [SCHED_BT 离在线混部调度（腾讯）](scheduler/sched-batch/README.md)
* [xsched 调度实践](scheduler/xsched.md)

## 三、中断子系统（interrupt）

* [中断分册导读](interrupt/README.md)
* [中断子系统总体架构](interrupt/docs/01-中断子系统总体架构.md)
* [核心数据结构](interrupt/docs/02-核心数据结构.md)
* [中断处理流程](interrupt/docs/03-中断处理流程.md)
* [irq_domain 与中断号映射](interrupt/docs/04-irq_domain与中断号映射.md)
* [softirq 与 tasklet](interrupt/docs/05-softirq与tasklet.md)
* [线程化中断与中断亲和性](interrupt/docs/06-线程化中断与中断亲和性.md)

## 四、内核同步（locking）

* [同步分册导读](locking/README.md)
* [spinlock 与 qspinlock](locking/spinlocks.md)
* [seqlock](locking/seqlock.md)
* [mutex](locking/mutex.md)
* [rwsem](locking/rwsem.md)
* [内存屏障（官方镜像 memory-barriers.txt）](memory-barriers.txt)
* [atomic_t（官方镜像）](atomic_t.txt)

## 五、内存管理（mm）

* [内存管理分册导读（含目录）](mm/README.md)
  * bootmem / memblock / buddy / slab / slub / slob / page / zone / node
  * page fault / cow / kmalloc / vmalloc / tlb / cache
  * hugetlbfs / thp / mmu / canonical address space
* [bootmem](mm/bootmem/bootmem.md)
* [memblock](mm/memblock/memblock.md)
* [buddy 伙伴系统](mm/buddy/buddy.md)
* [slab](mm/slab/slab.md)
* [slub](mm/slub/slub.md)
* [page fault](mm/pagefualt/pagefault.md)
* [kmalloc](mm/kmalloc/kmalloc.md)
* [vmalloc](mm/vmalloc/vmalloc.md)
* [thp 透明大页](mm/thp/thp.md)

## 六、RCU

* [RCU 分册导读](RCU/README.md)
* [基本概念](RCU/docs/基本概念.md)
* [tree RCU](RCU/tree_rcu.md)
* [tiny RCU](RCU/tiny_rcu.md)
* [preempt RCU](RCU/preempt_rcu.md)
* [SRCU](RCU/srcu.md)
* [tasks RCU](RCU/tasks_rcu.md)

## 七、网络子系统（networking）

* [网络分册导读](networking/README.md)
* [网络子系统总体架构](networking/docs/01-网络子系统总体架构.md)
* [sk_buff 详解](networking/docs/02-sk_buff详解.md)
* [net_device 与收发队列](networking/docs/03-net_device与收发队列.md)
* [收包路径与 NAPI](networking/docs/04-收包路径与NAPI.md)
* [发包路径与 qdisc](networking/docs/05-发包路径与qdisc.md)

## 八、内核数据结构（datastructure）

* [数据结构分册导读](datastructure/README.md)
* [list 双向链表](datastructure/docs/list/list.md)
* [hlist 哈希链表](datastructure/docs/hlist/hlist.md)
* [rbtree 红黑树](datastructure/docs/rbtree/rbtree.md)
* [xarray](datastructure/docs/xarray/xarray.md)
* [bitmap](datastructure/docs/bitmap/bitmap.md)
* [kfifo](datastructure/docs/kfifo/kfifo.md)
* [klist](datastructure/docs/klist/klist.md)
* [sbitmap](datastructure/docs/sbitmap/sbitmap.md)

## 九、安全子系统（security / LSM）

* [SELinux 分册（导读+目录）](admin-guide/LSM/selinux/README.md)
  * LSM 框架 / TE-RBAC-MLS-MCS / 初始化 / Hook 点 / 关键路径 / 实践用例
* [Landlock 分册](admin-guide/LSM/landlock/README.md)
* [lua-ebpf-lsm（欧拉）](security/lua-ebpf-lsm/README.md)

## 十、BPF 生态（bpf）

* [BPF 分册导读](bpf/README.md)
* [libbpf](bpf/libbpf/README.md)
* [bpfman](bpf/bpfman/README.md)
* [lsm-ebpf](bpf/lsm-ebpf/README.md)
* [vmlinux.h](bpf/vmlinuxh/README.md)

## 十一、RAS（可靠性/可用性/可维护性）

* [RAS 分册导读](ras/README.md)
* [panic](ras/panic/README.md)
* [oops](ras/oops/README.md)
* [kdump](ras/kdump/README.md)
* [OOM](ras/oom/README.md)

## 十二、虚拟化与体系结构

* [KVM 分册](virt/kvm/README.md)
* [arm64 MPAM 资源监控](arch/arm64/mpam.md)

## 十三、存储与外设

* [SMART 与磁盘健康监测](storage/smart/README.md)

## 十四、密码算法（国密）

* [国密算法分册](crypto/sm/README.md)
* [SM2 / SM3 / SM4 / SM9](crypto/sm/SUMMARY.md)

## 十五、文件系统（filesystems）

* [文件系统分册导读](filesystems/README.md)
* [mount 如何探测文件系统类型](filesystems/docs/mount如何探测文件系统类型/mount如何探测文件系统类型.md)

## 十六、其他

* [uestc-lilin 内核技术问答](uestc-lilin/README.md)

## 十七、时间子系统（timers）

* [时间分册导读](timers/README.md)
* [时间子系统总体架构](timers/docs/01-时间子系统总体架构.md)
* [clocksource 与时钟硬件](timers/docs/02-clocksource与时钟硬件.md)
* [timekeeping 与 clock_gettime](timers/docs/03-timekeeping与clock_gettime.md)
* [时间轮与低分辨率定时器](timers/docs/04-时间轮与低分辨率定时器.md)
* [hrtimer 高精度定时器](timers/docs/05-hrtimer高精度定时器.md)
* [tick 与 NOHZ](timers/docs/06-tick与NOHZ.md)
* [posix 定时器](timers/docs/07-posix定时器.md)

## 十八、内核观测（trace）

* [观测分册导读](trace/README.md)
* [内核观测体系总览](trace/docs/01-内核观测体系总览.md)
* [tracepoint 机制](trace/docs/02-tracepoint机制.md)
* [ring buffer 设计](trace/docs/03-ring-buffer设计.md)
* [kprobes 与 uprobes](trace/docs/04-kprobes与uprobes.md)
* [tracer 家族](trace/docs/05-tracer家族.md)
* [perf 与 BPF 观测](trace/docs/06-perf与BPF观测.md)

## 十九、块设备层（block）

* [块层分册导读](block/README.md)
* [块层总体架构](block/docs/01-块层总体架构.md)
* [bio 与 bio_vec](block/docs/02-bio与bio_vec.md)
* [blk-mq 多队列框架](block/docs/03-blk-mq多队列框架.md)
* [plug 合并与 IO 路径](block/docs/04-plug合并与IO路径.md)
* [IO 调度器](block/docs/05-IO调度器.md)
* [完成路径与统计](block/docs/06-完成路径与统计.md)

## 二十、电源管理（power）

* [电源管理分册导读](power/README.md)
* [电源管理总体框架](power/docs/01-电源管理总体框架.md)
* [cpufreq 调频子系统](power/docs/02-cpufreq.md)
* [cpuidle 空闲管理](power/docs/03-cpuidle.md)
* [runtime PM 与设备电源管理](power/docs/04-runtime-PM.md)
* [suspend 与 s2idle 全流程](power/docs/05-suspend与s2idle.md)

## 二十一、工作队列（workqueue）

* [工作队列分册导读](workqueue/README.md)
* [cmwq 设计与 worker pool](workqueue/docs/01-cmwq设计与worker-pool.md)
* [work 生命周期与 flush/cancel](workqueue/docs/02-work生命周期与flush-cancel.md)
* [unbound 池与 affinity scope](workqueue/docs/03-unbound池与affinity-scope.md)
* [使用规范与死锁分析](workqueue/docs/04-使用规范与死锁分析.md)

## 二十二、进程生命周期（process-mgmt）

* [进程生命周期分册导读](process-mgmt/README.md)
* [fork 与 clone 路径全解](process-mgmt/docs/01-fork与clone路径全解.md)
* [execve 与 mm 替换](process-mgmt/docs/02-execve与mm替换.md)
* [exit 与僵尸孤儿](process-mgmt/docs/03-exit与僵尸孤儿.md)
* [信号子系统](process-mgmt/docs/04-信号子系统.md)
* [ptrace 与 coredump](process-mgmt/docs/05-ptrace与coredump.md)

## 二十三、cgroup 资源治理（cgroup）

* [cgroup 分册导读](cgroup/README.md)
* [v2 层级与线程模式](cgroup/docs/01-v2层级与线程模式.md)
* [cpu 控制器](cgroup/docs/02-cpu控制器.md)
* [memory 控制器](cgroup/docs/03-memory控制器.md)
* [io 控制器与 cpuset](cgroup/docs/04-io控制器与cpuset.md)

## 二十四、系统调用机制（syscalls）

* [系统调用分册导读](syscalls/README.md)
* [系统调用入口与分发](syscalls/docs/01-系统调用入口与分发.md)
* [seccomp](syscalls/docs/02-seccomp.md)
* [io_uring](syscalls/docs/03-io_uring.md)

## 二十五、模块机制（module）

* [模块分册导读](module/README.md)
* [模块布局与加载器](module/docs/01-模块布局与加载器.md)
* [符号解析与 kallsyms](module/docs/02-符号解析与kallsyms.md)
* [KO 构建与 BTF](module/docs/03-KO构建与BTF.md)

## 二十六、设备模型（driver-model）

* [设备模型分册导读](driver-model/README.md)
* [kobject 与 sysfs](driver-model/docs/01-kobject与sysfs.md)
* [总线-设备-驱动匹配](driver-model/docs/02-总线-设备-驱动匹配.md)
* [设备树与 ACPI](driver-model/docs/03-设备树与ACPI.md)

## 二十七、printk 子系统（printk）

* [printk 分册导读](printk/README.md)
* [printk-ringbuffer](printk/docs/01-printk-ringbuffer.md)
* [console 驱动模型](printk/docs/02-console驱动模型.md)

---

> 备注：各分册目录下另有对应 `SUMMARY.md` 提供该分册完整目录；大量 `*.rst` 文件为 Linux 内核官方文档镜像，可作原文参考。
