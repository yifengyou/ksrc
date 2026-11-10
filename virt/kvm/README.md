# Introduction — KVM 分册（virt/kvm）

基于 **Linux 6.6** 的 KVM 深度笔记（2 章）。与目录内 `.rst` 官方镜像（api/vcpu-requests/halt-polling/locking 等）互补：rst 讲协议与用法，本笔记讲内部机制与数据流。

## 章节导读

| 章 | 文件 | 一句话 |
| :--- | :--- | :--- |
| vcpu 线程模型与主循环 | [01-vcpu线程模型与主循环](docs/01-vcpu线程模型与主循环.md) | ioctl 驱动模型（vcpu 无专属内核线程）、vcpu_run 主循环、KVM request 位图协议（WAIT/NO_WAKEUP 语义）、自适应 halt 轮询（grow×2/shrink）、Posted Interrupt 的 PIR/ON 免 exit 投递 |
| EPT 与内存虚拟化 | [02-EPT与内存虚拟化](docs/02-EPT与内存虚拟化.md) | shadow vs TDP 分野、EPT violation→direct_page_fault 全链、kvm_page_fault 上下文、tdp_mmu 无锁域、mmu_notifier 反向同步、EPT 大页与 nx_huge_page |

## 与其他分册的联系

- **scheduler 分册**：vcpu 就是普通任务（PELT/halt 轮询的调度交互）
- **interrupt 分册**：Posted Interrupt 与 APICv 的硬件联动
- **mm 分册**：memslot↔hva、mmu_notifier 与 vma 生命周期
- **RCU 分册**：tdp_mmu 的无锁遍历
- **hotplug 分册**：vCPU 热插拔走 cpuhp 联动

## 源码索引（Linux 6.6）

```
virt/kvm/kvm_main.c             通用层（request/halt/mmu_notifier/hva_to_pfn）
include/linux/kvm_host.h        struct kvm_vcpu/KVM_REQ_* 定义
arch/x86/kvm/x86.c              vcpu_run/vcpu_enter_guest
arch/x86/kvm/vmx/vmx.c          出口表/posted interrupt
arch/x86/kvm/mmu/mmu.c          kvm_tdp_page_fault/direct_page_fault
arch/x86/kvm/mmu/mmu_internal.h kvm_mmu_page/kvm_page_fault
arch/x86/kvm/mmu/tdp_mmu.c      TDP MMU 无锁域
```
