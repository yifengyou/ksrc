# crashkernel 预留与 elfcorehdr（Linux 6.6）

> 阅读目标：掌握 kdump 的内存布局（`crashkernel=` 三种语法、crashk_res/crashk_low_res 双预留区）与**证据链构造**（elfcorehdr 的双层构造：load 时 prepare_elf_headers、panic 时 per-CPU notes + VMCOREINFO；6.6 的 crash hotplug）、purgatory 的 sha256 自校验、capture kernel 侧 `/proc/vmcore` 的还原。前置：[kexec 加载与跳转](01-kexec加载与跳转.md)、[printk ringbuffer](../../printk/docs/01-printk-ringbuffer.md)（panic 路径的邻居）。

---

## 一、crashkernel= 三种语法（kernel/crash_core.c，已核对 6.6）

```
① simple： crashkernel=512M 或 512M@0x8000000（尺寸[@起点]）
② range：  crashkernel=2G-4G:256M,4G-8G:384M   ← 按物理内存总量选档
③ high/low（x86）：crashkernel=2G,high + crashkernel=256M,low
   high 部分放任意高位；low 部分落在 DMA32（<4G）——老设备 DMA 与
   swiotlb 需要低位内存；arm64 也有 low 语义（crashkernel=...,low）
解析入口：parse_crashkernel()/_high()/_low() →
   parse_crashkernel_simple/_mem/_suffix 分派（函数名已核对）
```

## 二、预留落地（kernel/kexec_core.c，已核对 6.6）

```
struct resource crashk_res      = { "Crash kernel", ... };      /* 主区 */
struct resource crashk_low_res  = { "Crash kernel low", ... };  /* 低位区 */
启动期：parse → memblock 预留（对齐 CRASH_ALIGN）→
        insert_resource 挂 iomem_resource 树
观测：cat /proc/iomem | grep -i crash
      cat /sys/kernel/kexec_crash_size       # 预留区字节数
约束：crash 镜像的所有段只能装进预留区（kimage_alloc_crash 走
      crashk_res 边界内的 hole 搜索）——"搬运者不能搬走自己脚下
      的地板"：capture kernel 自身内存绝不能覆盖被 dump 对象
```

## 三、elfcorehdr：证据链的双层构造

### 层 1：load 时——骨架（arch/x86/kernel/crash.c，已核对 6.6）

```
kexec_file_load(crash) → crash image 段构造：
  prepare_elf_headers(image, ...) →
  crash_prepare_elf64_headers(cmem, ...):        (crash_core.c)
    Elf64_Ehdr（p_offset=e_phoff 等字段已核对）
    + 每 system RAM 区间一个 PT_LOAD Phdr（dump 的内存范围声明）
    + VMCOREINFO note Phdr（p_filesz=VMCOREINFO_NOTE_SIZE）
    + x86 kernel_map note
  存进 image->elf_headers / elf_headers_sz，作为 crash 镜像的一个段
6.6 新特性（CONFIG_CRASH_HOTPLUG）：
  arch_crash_get_elfcorehdr_size()：phdr 数按
    2 + CONFIG_NR_CPUS_DEFAULT [+ CONFIG_CRASH_MAX_MEMORY_RANGES]
  预留余量；CPU/内存热插拔时 arch_crash_handle_hotplug_event()
  重写 elfcorehdr（image->update_elfcorehdr）——不再需要重新加载
  crash 镜像（老内核的痛点：热插拔后 kdump 静默失效）
```

### 层 2：panic 时——血肉（kexec_core.c + crash.c，已核对 6.6）

```
panic() → __crash_kexec(regs):            （STACK_FRAME_NON_STANDARD
                                           标注：不可被 unwind）
  ① kexec_trylock（防并发 load 替换镜像）
  ② crash_save_vmcoreinfo()：往 VMCOREINFO note 填运行时真相——
     VMCOREINFO_OSRELEASE/BUILD_ID/PAGESIZE/SYMBOL(name)... 宏族
     （include/linux/crash_core.h）：crash 工具用它把符号/结构布局
     对上被 dump 内核（= vmcore 的"ABI 契约"）
  ③ x86: kdump_nmi_shootdown_cpus() → nmi_shootdown_cpus
     (kdump_nmi_callback) → 每 CPU crash_save_cpu：把 pt_regs 写成
     NT_PRSTATUS ELF note（主 CPU 在 machine_crash_shutdown 里
     crash_save_cpu(safe_smp_processor_id())）
  ④ machine_kexec(kexec_crash_image) → 跳转进 capture kernel
可调：crash_kexec_post_notifiers=1 时先跑 panic notifiers/kmsg_dump
      再 crash_kexec（kexec_core.c 已核对该分支注释）
```

### 中间人：purgatory 的 sha256 自检

```
两个内核之间执行的自包含小段（kernel/kexec_file.c 内置）：
  crypto_alloc_shash("sha256") 对所有段算 purgatory_sha256_regions
  摘要，跳转前写入；capture 前自校验——搬运途中内存损坏则原地
  循环（拒绝带病起飞）；kexec_purgatory_get_symbol_addr 可查符号
```

## 四、capture kernel 侧：/proc/vmcore 还原（fs/proc/vmcore.c，已核对 6.6）

```
capture kernel 启动参数 elfcorehdr=<addr>（kexec-tools 从段表提取）→
vmcore_init()：
  elfcorehdr_read() 读 Elf64_Ehdr（弱符号钩子，架构可换实现）
  → 遍历 Phdr：PT_LOAD 段 remap 旧内核物理内存（只读映射）；
    PT_NOTE 段 elfcorehdr_read_notes() 解析 VMCOREINFO 与
    per-CPU PRSTATUS
  → 注册 /proc/vmcore（ET_CORE ELF 文件视图）
消费：cp /proc/vmcore core.dump 或 makedumpfile（--strip 无效页/
  过滤用户页/压缩）——crash utility 依据 VMCOREINFO 的 SYMBOL/
  结构偏移解析现场
```

## 五、观测与实战

```
启动验证：dmesg | grep -i crashkernel（"Reserving XXXMB"行）
布局验证：cat /proc/iomem | grep -A1 -i crash；kexec -p -l 后
          cat /sys/kernel/kexec_crash_loaded 应为 1
演练：    echo c > /proc/sysrq-trigger（全程在 ras/panic 视测下）
6.6 热插拔联动：CPU online/offline 后无需重载 crash 镜像——
          dmesg 里 arch_crash_handle_hotplug_event 的重写记录
排障：vmcore 只有部分内存段 → 检查 range 语法与 high/low 组合；
      capture 内核起不来 → 预留区过小/low 区缺失；
      VMCOREINFO 不匹配（crash 工具报 bad magic）→ kexec-tools
      与 kernel 版本错配（BUILD_ID 核对）
```

## 六、思考题

1. VMCOREINFO 里为什么要放 SYMBOL/结构偏移而不是让 crash 工具带符号表？（提示：kdump 机器与工具机内核版本可不同；note 是被 dump 内核的"自我描述"。）
2. `crashk_low_res` 单独存在的 DMA32 理由在哪些设备上仍然成立？（对照 [block 分册 DMA](../../block/docs/01-块层总体架构.md) 与 swiotlb。）
3. `__crash_kexec` 为什么标 STACK_FRAME_NON_STANDARD？（对照：跳转后栈回溯语义已不存在。）
4. nmi_shootdown_cpus 抓 PRSTATUS 时，正在持自旋锁的 CPU 会留下什么现场？（推演：crash 工具里 backtrace 卡在某锁的 spin——为什么这恰好是证据。）
5. 6.6 的 crash hotplug 把"重载镜像"变成"重写 elfcorehdr"——省掉的那次 kexec_file_load 原本要付出什么代价？（提示：镜像段的 hole 搜索与运行系统内存压力。）

---

> 回看：[kexec 加载与跳转](01-kexec加载与跳转.md)｜mm 分册 [page fault](../../mm/pagefualt/pagefault.md)｜printk 分册 [ringbuffer](../../printk/docs/01-printk-ringbuffer.md)
