# vDSO 与 clock_gettime：零系统调用的读钟（Linux 6.6）

> 阅读目标：掌握 vDSO 的时间服务全链——`struct vdso_data` 数据页与更新协议（seq 锁）、读侧的无锁计算（`do_hres` 与硬件计数器分发）、**time namespace 的"卡 seq"重定向技巧**、以及 6.6 的现实勘误：x86 早已没有手写汇编 vDSO。前置：[timers/01-clocksource](../../timers/docs/02-clocksource与时钟硬件.md)、[timers/02-timekeeping](../../timers/docs/03-timekeeping与clock_gettime.md)、[namespace 全景](../../process-mgmt/docs/06-namespace全景.md)（time ns）。

---

## 一、为什么：读钟是第一大系统调用

```
gettimeofday/clock_gettime 频率冠绝所有 syscall（监控/日志/网络
时间戳/条件变量全在读钟）——每次都走 entry/exit（几十 ns 级）不可接受
vDSO：内核把"计算下一秒"需要的全部状态写进用户态只读页（vvar），
  用户态库直接算，零特权级切换
```

## 二、数据页：struct vdso_data（include/vdso/datapage.h，已核对 6.6）

```c
struct vdso_data {
	u32 seq;          /* 序号锁：奇数=写中；读侧自旋/重试 */
	s32 clock_mode;   /* VDSO_CLOCKMODE_{NONE,TSC,PVCLOCK,HVCLOCK} */
	u64 cycle_last, mask;   u32 mult, shift;   /* timekeeping 同款换算参数 */
	union {
		struct vdso_timestamp basetime[VDSO_BASES];  /* 各 clock 基准 */
		struct timens_offset offset[VDSO_BASES];     /* timens 页：偏移表 */
	};
	s32 tz_minuteswest, tz_dsttime;  u32 hrtimer_res;
};   /* VDSO_BASES = CLOCK_TAI+1；CS_HRES_COARSE=0/CS_RAW=1 两份实例 */
```

**双页布局**：`vvar` 页（本 time ns 的真实数据）+ **timens 页**（固定内容：`seq` 卡在 1、`clock_mode = VDSO_CLOCKMODE_TIMENS`、`offset[]` 填本 ns 的偏移）。

## 三、读侧：无锁协议与 timens 陷阱（lib/vdso/gettimeofday.c，已核对）

```
do_hres(vd, clk, ts):
  do {
    while (READ_ONCE(vd->seq) & 1) {          /* 奇数=写中 */
      if (vd->clock_mode == VDSO_CLOCKMODE_TIMENS)
        return do_hres_timens(vd, clk, ts);   /* ★卡 seq 的"死循环"
                                                 其实是 timens 的正门：
                                                 本 ns 页永远 seq=1，
                                                 读侧在这里被引去偏移版 */
      cpu_relax();
    }
    smp_rmb();
    cycles = __arch_get_hw_counter(vd->clock_mode, vd);  /* TSC/PVCLOCK/
                                                            HVCLOCK 各自读 */
    ts = basetime[clk] + ((cycles - cycle_last) * mult) >> shift;  /* 与
      timekeeping 完全同款公式（timers/02 章） */
  } while (vdso_read_retry(vd, seq));
do_coarse：无 hw counter 版（低精度直接读 basetime）
fallback：clock_mode == VDSO_CLOCKMODE_NONE（时钟源不支持 vDSO，
  如 HPET）→ clock_gettime_fallback() 真正走系统调用（gettimeofday.c:261）
```

`do_hres_timens`：拿 **timens 页的 offset[clk]** 加到 **宿主 vvar 页（CS_RAW 实例）** 的结果上——一个 ns 一套时间观感，落盘零拷贝。

## 四、x86 6.6 现实勘误：没有手写汇编

```
arch/x86/entry/vdso/vclock_gettime.c（已核对，全部正文一行）：
  #include "../../../../lib/vdso/gettimeofday.c"
  ——泛型 C 实现（CONFIG_GENERIC_GETTIMEOFDAY）直接编进 vDSO；
  __vdso_clock_gettime → __cvdso_clock_gettime；glibc 符号
  clock_gettime 以 weak alias 挂 __vdso_clock_gettime
  （i386 侧另有 __cvdso_clock_gettime32/64 双 ABI）
老资料里"vsyscall 页手写汇编"早已成历史（legacy vsyscall 仅剩
  EMULATE 模式，x86_64 默认 none——[ras 分册] 的 ABI 历史注脚）
```

## 五、更新侧：clocksource 的 vdso_clock_mode 驱动一切

```
timekeeping 每次 adjust（timers/02）→ update_vsyscall
  （kernel/time/vsyscall.c，已核对）:
  vdata[CS_HRES_COARSE/CS_RAW].{cycle_last,mask,mult,shift} ← tkr_mono/raw
  basetime[] 各 clock 基准值
  clock_mode = tk->tkr_mono.clock->vdso_clock_mode   ← ★每 clocksource
      自带该字段（arch/x86/kernel/tsc.c: .vdso_clock_mode =
      VDSO_CLOCKMODE_TSC；kvmclock.c: VDSO_CLOCKMODE_PVCLOCK）
  seq++ 两轮（写前/写后）完成发布
换时钟源（TSC→HPET）后 clock_mode 变 NONE → 读侧自动回落
  fallback 系统调用——无需通知任何用户态
```

## 六、观测与实战

```
cat /proc/self/maps | grep vdso       # 映射基址（AT_SYSINFO_EHDR 同源）
vdso(7) man 手册 / ldd --list         # 库侧符号解析
strace -c -f ./app | grep clock       # 若见 clock_gettime 系统调用：
  ① clocksource 不支持（clock_mode=NONE，如某些 VM 的 HPET 态）
  ② 老 glibc 未走 vDSO ABI（升级库）
  ③ TIMENS 进程的 RAW 类请求（少见路径）
perf stat -e dummy:u 粗测读钟成本（vDSO ~20ns vs syscall ~60ns+）
容器迁移一致性：docker/kata 里检查 /proc/self/timens_offset（timens
  域内 monotonic 从 0 起——checkpoint/restore 的地基，process-mgmt 06 章）
```

## 七、思考题

1. `seq` 用"卡在 1"表达 timens——为什么不为 timens 单开一个 clock_mode 正常走查表？（提示：offset 需要与宿主页计算结果混合，两个读来源；卡 seq 让复用路径零分支成本。）
2. `mult/shift` 换算必须在用户态复刻——timekeeping 调整 mult（NTP 微调）时 vDSO 读者会看到什么？（对照 seq 重试协议。）
3. KVM guest 的 PVCLOCK 模式下 `__arch_get_hw_counter` 读的是什么？（对照 [virt/kvm 分册](../../virt/kvm/README.md)：pvclock 页与稳定 TSC 语义。）
4. `clock_gettime_fallback` 存在却几乎不出现——什么场景必然踩它？（HPET 时钟源 + 哪类应用？）
5. timens 的 offset 只加在 MONOTONIC 族、BOOTTIME 不变——为什么 CLOCK_MONOTONIC 可偏而 CLOCK_BOOTTIME 语义上不该偏？（对照 timers 分册 posix 定时器章的 clock 分层。）

---

> 回看：[03-io_uring](03-io_uring.md)｜[05-io-wq 与取消状态机](05-iowq与取消状态机.md)｜timers 分册 [timekeeping](../../timers/docs/03-timekeeping与clock_gettime.md)｜[namespace 全景](../../process-mgmt/docs/06-namespace全景.md)
