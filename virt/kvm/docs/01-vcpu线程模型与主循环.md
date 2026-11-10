# KVM vcpu 线程模型与主循环（Linux 6.6）

> 阅读目标：建立 KVM 的执行模型——宿主机视角下 **vcpu 只是一个普通线程**在 `ioctl(KVM_RUN)` 里跑循环；掌握 `vcpu_run` 主循环（`vcpu_enter_guest`/`vcpu_block`）、KVM request 位图协议、自适应 halt 轮询、以及 **Posted Interrupt**（中断免 VM-exit 投递）。与目录内官方镜像 `vcpu-requests.rst`/`halt-polling.rst` 互补。前置：[scheduler 分册](../../scheduler/README.md)（vcpu 就是调度器眼里的任务）、[interrupt 分册](../../interrupt/README.md)。

---

## 一、执行模型：ioctl 驱动的世界

```
QEMU 线程 ──ioctl(KVM_RUN)──▶ 内核:
  kvm_vcpu_ioctl → kvm_arch_vcpu_ioctl_run → vcpu_run(vcpu)
  循环跑 guest 直到需要用户态接手（exit reason 写入共享页）
  struct kvm_vcpu（include/linux/kvm_host.h，已核对）:
    struct kvm_run *run        ← 与用户态共享的出口信息页
    int mode;  u64 requests;   ← IN_GUEST_MODE/EXITING_GUEST_MODE + 请求位图
    struct pid *pid            ← 当前"vcpu 线程"身份（可换线程接管）
    unsigned int halt_poll_ns  ← 本 vcpu 的自适应轮询值
  注：vcpu 没有专属内核线程——谁 ioctl 谁就是 vcpu（vcpu_load 时
      preempt_notifier 钉住；vcpu->pid 追踪线程归属）
```

## 二、主循环 vcpu_run（arch/x86/kvm/x86.c，已核对 6.6）

```
for (;;) {
    if (kvm_vcpu_running(vcpu))
        r = vcpu_enter_guest(vcpu);     ← 进 guest
    else
        r = vcpu_block(vcpu);           ← halt 睡眠
    if (r <= 0) break;                  ← 交给用户态（KVM_EXIT_*）
    kvm_clear_request(KVM_REQ_UNBLOCK, vcpu);
    kvm_xen_inject_pending_events / kvm_inject_pending_timer_irqs …
}
vcpu_enter_guest 出口（static_call(kvm_x86_vcpu_run)→vmx_vcpu_run 的
vmlaunch/vmresume 返回后）按 exit_reason 查表分发：
  [EXIT_REASON_EPT_VIOLATION] = handle_ept_violation …（vmx.c 出口表已核对）
```

## 三、KVM request：vcpu 间异步工作协议（kvm_host.h，已核对）

```c
#define KVM_REQ_TLB_FLUSH  (0 | KVM_REQUEST_WAIT | KVM_REQUEST_NO_WAKEUP)
#define KVM_REQ_VM_DEAD    (1 | KVM_REQUEST_WAIT | KVM_REQUEST_NO_WAKEUP)
#define KVM_REQ_UNBLOCK    2
#define KVM_REQ_DIRTY_RING_SOFT_FULL 3
#define KVM_REQUEST_ARCH_BASE 8    ← 架构请求号从 8 起
kvm_make_request(req, vcpu) / kvm_check_request(req, vcpu)
  ——requests 位图 + smp_mb 配对（源码注释原样：make 侧发布请求内容，
    check 侧 after_atomic 保证可见性）；KVM_REQUEST_WAIT 让发起者
    等待目标 vcpu 认领（TLB flush 一致性的地基）
```

消费点在 `vcpu_enter_guest` 入口与循环各处——这是官方 `vcpu-requests.rst` 的机制本体：**"request = 跨 vcpu 的延迟函数调用"**。

## 四、halt：自适应轮询与睡眠（virt/kvm/kvm_main.c，已核对）

```
guest 执行 hlt → kvm_vcpu_halt() → kvm_vcpu_block():
  先忙等 halt_poll_ns（期待中断马上来——省一次睡眠/唤醒），
  超时则 schedule() 睡眠（kvm_vcpu_wake 被中断唤醒路径调用）
自适应（grow/shrink_halt_poll_ns 已核对）：
  唤醒"赶上了轮询窗口"→ grow（默认翻倍 ×2，halt_poll_ns_grow）
  唤醒"远超窗口"→ shrink（halt_poll_ns_shrink）
  上限 halt_poll_ns_max；全局起点 halt_poll_ns（模块参数 0644）
```

## 五、Posted Interrupt（APICv）：免 VM-exit 的中断投递

```
vmx_deliver_posted_interrupt（vmx.c，已核对）：
  中断到达正在 guest 里的 vcpu → 只置 PIR（Posted Interrupt
  Descriptor 的请求位图）与 ON 位 → 若 vcpu 在 guest 中，发
  notification 中断（IPI 特殊向量）；vmentry 时/通知中断里
  硬件把 PIR 原子搬进 vIRR（源码注释："hardware will sync PIR
  to vIRR atomically"）——全程无 VM-exit
  对照：无 APICv 时中断投递必须 VM-exit（EXIT_REASON_EXTERNAL_INTERRUPT）
嵌套场景（vmx->nested.posted_intr_nv）：L1 给 L2 的 posted intr 向量
  直通（vmx.c 已核对）——nested virtualization 的中断性能保命符
```

## 六、观测与实战

```
kvm_stat（1s 间隔的 exit 分布/VM-EXIT 原因榜）
perf: kvm:kvm_entry / kvm:kvm_exit / kvm:kvm_vcpu_wakeup tracepoints
/sys/kernel/debug/kvm/                  # 每 VM 的 mmu/vcpu 计数
latency 排障链：kvm_exit 高频 EPT_MISCONFIG → MMU 章；
  EXTERNAL_INTERRUPT 占比高 → APICv 未开；HLT 长停留 → halt_poll 调参
  （halt_polling.rst 的 grow/shrink 曲线复现）
```

## 七、思考题

1. vcpu 没有专属内核线程——QEMU 把 vcpu 线程 `pthread` 迁到别的 CPU 会发生什么？（对照 vcpu->pid 与 preempt_notifier 的追踪语义。）
2. `KVM_REQ_TLB_FLUSH` 带 WAIT 但 NO_WAKEUP——为什么 flush 不能把睡着的 vcpu 拉起来，却要等它下次进 guest 前自己认领？（提示：TLB 的有效期边界是 vmentry，不是"立刻"。）
3. halt 轮询窗口越大越好吗？（推演 CPU 空烧与虚拟化中断延迟的 trade-off，对照 idle injection。）
4. Posted Interrupt 的 ON 位是谁置、谁清？（硬件/软件分工的边界画在哪？）
5. `struct kvm_run` 为什么放在共享页而不是 ioctl 返回值？（提示：高频出口的拷贝成本与 mmap 只读权限。）

---

> 下一章：[EPT 与内存虚拟化](02-EPT与内存虚拟化.md)
