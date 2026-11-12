# Summary

* [Introduction](README.md)

---

## 一、内核启动（init）

* [启动分册导读](init/README.md)
* [内核启动总览：从上电到 start_kernel](init/docs/01-内核启动总览.md)
* [start_kernel 详解](init/docs/02-start_kernel详解.md)
* [从 rest_init 到用户空间](init/docs/03-从rest_init到用户空间.md)
* [initcall 与模块初始化机制](init/docs/04-initcall与模块初始化机制.md)
* [async 异步基础设施](init/docs/05-async异步基础设施.md)

## 二、进程调度（scheduler）

* [调度分册导读](scheduler/README.md)
* [一页速查](scheduler/docs/00-一页速查.md)
* [调度器演进与总体架构](scheduler/docs/01-调度器演进与总体架构.md)
* [核心数据结构](scheduler/docs/02-核心数据结构.md)
* [CFS 与 EEVDF 详解](scheduler/docs/03-CFS与EEVDF详解.md)
* [调度时机与上下文切换](scheduler/docs/04-调度时机与上下文切换.md)
* [PELT 与利用率追踪](scheduler/docs/05-PELT与利用率追踪.md)
* [调度域与负载均衡](scheduler/docs/06-调度域与负载均衡.md)
* [实时与 Deadline 调度](scheduler/docs/07-实时与Deadline调度.md)
* [sched_ext 前瞻（v6.12）](scheduler/docs/08-sched_ext前瞻.md)
* [PSI 压力失速信息](scheduler/docs/09-PSI压力失速信息.md)
* [NUMA balancing](scheduler/docs/10-NUMA_balancing.md)
* [NUMA 统计接口](scheduler/docs/11-NUMA统计接口.md)
* [psi 触发器实践](scheduler/docs/12-psi触发器实践.md)
* [调度器 debugfs 全景](scheduler/docs/13-调度器debugfs全景.md)
* [调度器统计纵深 schedstat](scheduler/docs/14-调度器统计纵深schedstat.md)
* [任务延迟统计 delayacct](scheduler/docs/15-任务延迟统计delayacct.md)
* [观测扇出模式](scheduler/docs/16-观测扇出模式.md)
* [SCHED_BT 离在线混部调度（腾讯）](scheduler/sched-batch/README.md)
* [xsched 调度实践](scheduler/xsched.md)

## 三、中断子系统（interrupt）

* [中断分册导读](interrupt/README.md)
* [一页速查](interrupt/docs/00-一页速查.md)
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
* [一页速查](mm/docs/00-一页速查.md)
  * bootmem / memblock / buddy / slab / slub / slob / page / zone / node
  * page fault / cow / kmalloc / vmalloc / tlb / cache
  * hugetlbfs / thp / mmu / canonical address space
* [bootmem](mm/bootmem/bootmem.md)
* [memblock](mm/memblock/memblock.md)
* [buddy 伙伴系统](mm/buddy/buddy.md)
* [slab](mm/slab/slab.md)
* [slub](mm/slub/slub.md)
* [page fault](mm/pagefualt/pagefault.md)
* [vma 与 mmap 生命周期](mm/vma/vma与mmap生命周期.md)
* [page cache 与 writeback](mm/pagecache/pagecache与writeback.md)
* [ksm 与 zswap](mm/ksm/ksm与zswap.md)
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
* [TCP 拥塞控制框架](networking/docs/06-TCP拥塞控制框架.md)
* [XDP 与 eBPF 数据面](networking/docs/07-XDP与eBPF数据面.md)
* [netfilter 五钩子与 conntrack](networking/docs/08-netfilter五钩子与conntrack.md)
* [UDP 收包路径与 KTLS 卸载](networking/docs/09-UDP与KTLS.md)

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
  * [kexec 加载与跳转](ras/kdump/docs/01-kexec加载与跳转.md)
  * [crashkernel 预留与 elfcorehdr](ras/kdump/docs/02-crashkernel预留与elfcorehdr.md)
* [kexec 签名与 lockdown 联动](ras/kdump/docs/03-kexec签名与lockdown.md)
* [OOM](ras/oom/README.md)

## 十二、虚拟化与体系结构

* [KVM 分册](virt/kvm/README.md)
  * [vcpu 线程模型与主循环](virt/kvm/docs/01-vcpu线程模型与主循环.md)
  * [EPT 与内存虚拟化](virt/kvm/docs/02-EPT与内存虚拟化.md)
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
* [一页速查](timers/docs/00-一页速查.md)
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
* [procfs 观测索引](trace/docs/07-procfs观测索引.md)
* [sysctl 调参索引](trace/docs/08-sysctl调参索引.md)
* [debugfs 全景索引](trace/docs/09-debugfs全景索引.md)
* [eBPF 验证器入门](trace/docs/10-eBPF验证器入门.md)
* [tnum 三值数](trace/docs/11-tnum三值数.md)
* [profile 与 latencytop](trace/docs/12-profile与latencytop.md)
* [内核代码生成惯用法](trace/docs/13-内核代码生成惯用法.md)

## 十九、块设备层（block）

* [块层分册导读](block/README.md)
* [一页速查](block/docs/00-一页速查.md)
* [块层总体架构](block/docs/01-块层总体架构.md)
* [bio 与 bio_vec](block/docs/02-bio与bio_vec.md)
* [blk-mq 多队列框架](block/docs/03-blk-mq多队列框架.md)
* [plug 合并与 IO 路径](block/docs/04-plug合并与IO路径.md)
* [IO 调度器](block/docs/05-IO调度器.md)
* [完成路径与统计](block/docs/06-完成路径与统计.md)

## 二十、电源管理（power）

* [电源管理分册导读](power/README.md)
* [一页速查](power/docs/00-一页速查.md)
* [电源管理总体框架](power/docs/01-电源管理总体框架.md)
* [cpufreq 调频子系统](power/docs/02-cpufreq.md)
* [cpuidle 空闲管理](power/docs/03-cpuidle.md)
* [runtime PM 与设备电源管理](power/docs/04-runtime-PM.md)
* [suspend 与 s2idle 全流程](power/docs/05-suspend与s2idle.md)
* [thermal 框架](power/docs/06-thermal框架.md)
* [devfreq 与 pm_qos](power/docs/07-devfreq与pm_qos.md)
* [HWP 与 EPP 调优](power/docs/08-HWP与EPP调优.md)
* [idle 注入与 powerclamp](power/docs/09-idle注入与powerclamp.md)

## 二十一、工作队列（workqueue）

* [工作队列分册导读](workqueue/README.md)
* [一页速查](workqueue/docs/00-一页速查.md)
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
* [namespace 全景](process-mgmt/docs/06-namespace全景.md)
* [信号深入](process-mgmt/docs/07-信号深入.md)

## 二十三、cgroup 资源治理（cgroup）

* [cgroup 分册导读](cgroup/README.md)
* [一页速查](cgroup/docs/00-一页速查.md)
* [v2 层级与线程模式](cgroup/docs/01-v2层级与线程模式.md)
* [cpu 控制器](cgroup/docs/02-cpu控制器.md)
* [memory 控制器](cgroup/docs/03-memory控制器.md)
* [io 控制器与 cpuset](cgroup/docs/04-io控制器与cpuset.md)
* [memcg 回收深挖](cgroup/docs/05-memcg回收深挖.md)
* [uclamp 深挖](cgroup/docs/06-uclamp深挖.md)
* [css_set 深挖](cgroup/docs/07-css_set深挖.md)
* [v1 淘汰史](cgroup/docs/08-v1淘汰史.md)
* [v1 到 v2 迁移手册](cgroup/docs/09-v1到v2迁移手册.md)

## 二十四、系统调用机制（syscalls）

* [系统调用分册导读](syscalls/README.md)
* [一页速查](syscalls/docs/00-一页速查.md)
* [系统调用入口与分发](syscalls/docs/01-系统调用入口与分发.md)
* [seccomp](syscalls/docs/02-seccomp.md)
* [io_uring](syscalls/docs/03-io_uring.md)
* [LSM 与安全钩子顺序](syscalls/docs/04-LSM与安全钩子顺序.md)
* [io-wq 与取消状态机](syscalls/docs/05-iowq与取消状态机.md)
* [vDSO 与 clock_gettime](syscalls/docs/06-vDSO与clock_gettime.md)
* [seccomp 过滤器与 BPF 验证](syscalls/docs/07-seccomp过滤器与BPF验证.md)

## 三十、Rust for Linux（rust）

* [Rust 分册导读](rust/README.md)
* [6.6 现状导读](rust/docs/01-RustForLinux现状导读.md)
* [Rust 抽象层深挖](rust/docs/02-抽象层深挖.md)

## 二十五、模块机制（module）

* [模块分册导读](module/README.md)
* [模块布局与加载器](module/docs/01-模块布局与加载器.md)
* [符号解析与 kallsyms](module/docs/02-符号解析与kallsyms.md)
* [KO 构建与 BTF](module/docs/03-KO构建与BTF.md)
* [模块签名链](module/docs/04-模块签名链.md)
* [kernel_read_file 加载基础设施](module/docs/05-kernel_read_file加载基础设施.md)
* [usermodehelper 机制](module/docs/06-usermodehelper机制.md)
* [idempotent 模块加载](module/docs/07-idempotent模块加载.md)
* [umh 消费者全景](module/docs/08-umh消费者全景.md)

## 二十六、设备模型（driver-model）

* [设备模型分册导读](driver-model/README.md)
* [kobject 与 sysfs](driver-model/docs/01-kobject与sysfs.md)
* [总线-设备-驱动匹配](driver-model/docs/02-总线-设备-驱动匹配.md)
* [设备树与 ACPI](driver-model/docs/03-设备树与ACPI.md)
* [firmware 加载与 request_firmware 生命周期](driver-model/docs/04-firmware加载与request_firmware生命周期.md)
* [devres 设备资源管理](driver-model/docs/05-devres设备资源管理.md)
* [pcim 与 PCI 管理资源](driver-model/docs/06-pcim与PCI管理资源.md)
* [资源生命周期三模式](driver-model/docs/07-资源生命周期三模式.md)

## 二十七、printk 子系统（printk）

* [printk 分册导读](printk/README.md)
* [printk-ringbuffer](printk/docs/01-printk-ringbuffer.md)
* [console 驱动模型](printk/docs/02-console驱动模型.md)

## 二十八、内核热补丁（livepatch）

* [livepatch 分册导读](livepatch/README.md)
* [klp 核心结构与打补丁流](livepatch/docs/01-klp核心结构与打补丁流.md)
* [一致性模型与 transition](livepatch/docs/02-一致性模型与transition.md)

## 二十九、热插拔（hotplug）

* [热插拔分册导读](hotplug/README.md)
* [cpuhp 状态机](hotplug/docs/01-cpuhp状态机.md)
* [内存热插拔 memhp](hotplug/docs/02-内存热插拔memhp.md)

---

> 备注：各分册目录下另有对应 `SUMMARY.md` 提供该分册完整目录；大量 `*.rst` 文件为 Linux 内核官方文档镜像，可作原文参考。
