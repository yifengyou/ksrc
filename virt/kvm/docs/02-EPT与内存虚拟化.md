# EPT 与内存虚拟化（Linux 6.6）

> 阅读目标：掌握 x86 KVM 的两维分页（TDP/EPT）实现——EPT violation 出口到 `direct_page_fault` 的完整链路、`struct kvm_mmu`/`kvm_mmu_page`/tdp_mmu 的分工、宿主侧反向同步（mmu notifier）、以及 shadow paging 与 TDP 的分野。与官方镜像 `locking.rst`/`review-checklist.rst` 互补。前置：[01-vcpu线程模型](01-vcpu线程模型与主循环.md)、[mm 分册 vma](../../../mm/vma/vma与mmap生命周期.md)（宿主侧的 mmap 与 memslot）。

---

## 一、两条路线：shadow paging vs TDP/EPT

```
guest 视角:  gVA --CR3(guest 页表)--> gPA --EPTP--> hPA
shadow paging（SPT，无硬件 TDP 的老路）:
  KVM 为 guest 每张页表建"影子"，把 gVA→hPA 一步直达；
  代价：guest 改自己的页表（CR3 写/invlpg）都会 VM-exit → 同步风暴
TDP/EPT（Intel EPT / AMD NPT，硬件两维分页，6.6 x86 默认）:
  guest 页表原样跑（不再 exit），KVM 只维护第二维 EPT（gPA→hPA）
  ——EPT violation（guest 首次触碰该 gPA 页）才是缺页出口
```

## 二、结构分工（已核对 6.6）

```
struct kvm_mmu（per-vcpu 的 MMU 上下文）:
  root_hpa（EPT 根指针，vmentry 装进 EPTP）/ page_fault 回调
  （TDP 路线填 kvm_tdp_page_fault）……
struct kvm_mmu_page（arch/x86/kvm/mmu/mmu_internal.h:51，已核对）:
  link + hash_link（挂 mmu 页缓存哈希）
  tdp_mmu_page（是否 tdp_mmu 域）/ unsync（shadow 路线的脏页标记）
  tdp_mmu_scheduled_root_to_zap / nx_huge_page_disallowed
tdp_mmu（arch/x86/kvm/mmu/tdp_mmu.c）:
  独立的 EPT 页表域——RCU 无锁读遍历（zap 与 page fault 并行），
  每-VM 的 tdp_mmu_roots 链 + per-CPU 的 tdp_mmu_pages 池
```

## 三、缺页链路：EPT violation → SPTE 建立（已核对 6.6 全链）

```
guest 访问未映射 gPA → VM-exit（EXIT_REASON_EPT_VIOLATION=48）
  → handle_ept_violation（vmx.c 出口表项）
  → kvm_mmu_page_fault：把 exit qualification 解码进
     struct kvm_page_fault（已核对字段：error_code/exec/write/user/
     is_tdp/gfn/pfn——pfn 由 kvm_faultin_pfn 填）
  → kvm_tdp_page_fault（mmu.c:4976）
  → direct_page_fault（mmu.c:4846）:
     ① gfn → kvm_memory_slot（memslot 表：gfn↔hva 昘区间）
     ② hva → hva_to_pfn（kvm_main.c：pin 宿主页，跟随 vma 的
        hugetlb/regular/大页属性）
     ③ 检测可映射大页（host 大页+内存对齐+exec 权限交互）→
        逐级填 EPT 项（4K/2M/1G）；nx_huge_page 恢复机制防
        ITLB 别信道滥用大页
     ④ 失败方向：MMIO 区（无 pfn）→ 走 MMIO 仿写（device 分册联动）
```

## 四、反向同步：宿主动了，EPT 怎么办（kvm_main.c，已核对）

```
宿主侧回收/迁移/换出 guest 正在用的页 → mmu_notifier 回调：
  kvm_mmu_notifier_ops.invalidate_range_start（已核对 770/921 行）:
  摘除区间内所有相关 SPTE → 对应 vcpu 发 KVM_REQ_TLB_FLUSH
  （vcpu 分册的 request 协议——flush 带 WAIT|NO_WAKEUP 的原因见彼）
换页/迁移的配合：KVM 先 pin（hva_to_pfn 的 refcount）后建表；
  notifier 摘表后宿主才能回收——两方靠 notifier 的 start/end
  区间做无窗口同步
TLB 失效：vmentry 用 EPTP/VPID 的 tag；改表后 invept（按上下文
  粒度）+ 上述 request 广播
```

## 五、观测与实战

```
kvm_stat（含 EPT violation 计数、mmu_cache_miss 类指标）
/sys/kernel/debug/kvm/<vm>/: mmu 页缓存命中率、tdp_mmu 页数
perf -e kvm:kvm_page_fault,kvm:kvm_mmu_page_fault / kvm:kvm_tdp_page_fault
典型调优：
  EPT violation 风暴（首次触碰多）→ 预热/THP 对齐 guest 大页
  （host 大页让 EPT 直接建 2M 项：madvise/HugeTLBFS）
  zap 风暴（迁移/NUMA balance 抖动）→ 关闭自动 numa balancing
  观察宿主侧 mmu notifier 频次
排障链：exit 榜 EPT_VIOLATION 高 → kvm_stat 看 mmu 命中 →
  memslot/hugepage 布局 → notifier 源头（谁在抖 vma）
```

## 六、思考题

1. shadow paging 为什么在 guest 内核启动（页表频繁重建）时最痛？（对照 CR3 写 exit 与 TDP 的"页表零介入"。）
2. `struct kvm_page_fault` 的 `is_tdp` 由 `page_fault == kvm_tdp_page_fault` 推出（mmu.c 已核对）——为什么不存成字段？（提示：同一 vcpu 可挂两套 mmu，nested/smm 切换。）
3. EPT 大页（2M/1G）带来什么与什么风险？（对照 nx_huge_page 的存在理由——执行权限与大页的交互侧信道。）
4. `invalidate_range_start` 若在摘表前让宿主先回收了页，会看到什么？（推演：EPT 指向已释放 pfn 的 UAF 面——KVM 的 zapping 顺序为何必须先于回收。）
5. tdp_mmu 用 RCU 做无锁读遍历——zap 与缺页并发时谁可能"白干"？（对照 [RCU 分册](../../../RCU/README.md) 的宽限期语义。）

---

> 回看：[01-vcpu线程模型与主循环](01-vcpu线程模型与主循环.md)｜官方镜像：[vcpu-requests](../vcpu-requests.rst)、[locking](../locking.rst)｜[mm 分册 vma](../../../mm/vma/vma与mmap生命周期.md)
