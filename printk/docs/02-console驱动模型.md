# console 驱动模型（Linux 6.6）

> 阅读目标：吃透 prb 之外的第二半——console 子系统的搬运协议：`register_console` 与 boot console 的交接、`console_unlock` 的输出循环（格式化与 write 调用）、console_loglevel 过滤、以及"printk 在原子上下文搬运"的历史争议与 RT 树的处理。掌握排障 console 卡顿的完整工具箱。

---

## 一、struct console 与注册（include/linux/console.h，已核对 6.6）

```c
struct console {
	char		name[16];
	void		(*write)(struct console *, const char *s, unsigned count);
	int		(*read)(struct console *, char *s, unsigned count);
	struct tty_driver *(*device)(struct console *, int *index);
	void		(*unblank)(void);
	int		(*setup)(struct console *, char *options);   /* console= 参数 */
	int		(*exit)(struct console *);
	int		(*match)(struct console *, char *name, int idx, char *options);
	short		flags;      /* CON_PRINTBUFFER/CON_ANYTIME/CON_BOOT/
	                           CON_BRL(盲文)... */
	short		index;
	u64		seq;           /* ★本 console 已输出到哪（追赶游标）*/
	unsigned long	dropped;       /* 本 console 丢行计数 */
	struct hlist_node	node;
};
```

注册链（kernel/printk/printk.c）：`register_console` → 名称与 `console=` 启动参数匹配（`__add_preferred_console` 的注册表 + match 回调）→ **boot console 与真 console 的交接**：首个非 CON_BOOT console 注册时，先 flush 再 unregister 全部 boot console（earlycon 的生命周期终点）。`console_flush_on_panic` 是 panic 路径的强制冲刷（ras 分册的 panic 后"最后一眼"）。

## 二、搬运主循环：console_unlock

```
vprintk() 写入 prb 后（第一章）：
  若可立即输出（非 atomic/非 NMI/未 suspended）：
  console_trylock → 成功则 console_unlock 自旋搬运；失败则 wake klogd/
  defer（printk_deferred：在 irq_work 里补做——中断路径写者的延迟通道）

console_unlock() 的循环（已核对结构）：
  while (console_seq != prb next_seq) {
    ① prb_read_valid（复制下一条）
    ② msg_print_text（按 console_loglevel 过滤 + 前缀级别格式化）
    ③ 本地关中断段（CON_ANYTIME? 否则 write 在关中断下调用——历史约定，
       部分驱动因此不能睡眠）
    ④ console->write(con, text, len)   ← 慢设备（串口 115200≈1ms/百字节）
       在这里决定系统命运：持 console_lock 且（可能）关中断地慢写
    ⑤ console_seq++（记录到 con->seq）
  }
```

`con->dropped`：输出跟不上、环被回收时按 console 记丢行数（dmesg 里 `** N printk messages dropped **` 的来源）。

## 三、过滤与控制

```
/proc/sys/kernel/printk 四值：console_loglevel default_message_loglevel
                              minimum_console_loglevel default_console_loglevel
printk 的 level（<N> 前缀）> console_loglevel → 只进 prb 不出 console
dmesg -n / dmesg -D(-r)     运行时调级/静默/恢复
ignore_loglevel（boot 参数）强开全量（内核调试标配）
boot_delay_ms               每条延迟（慢 console 下排查早期崩溃）
echo l > /proc/sys/kernel/sysrq → sysrq-loglevel 快捷通道
```

## 四、争议与 RT：搬运为什么要离线出原子路径

```
症状史：串口 console + 高频 printk → 系统被搬运卡到毫秒级抖动
（write 关中断慢速输出 = 定时中断丢失 = NMI watchdog 报 watchdog lockup）
PREEMPT_RT 的答案：console 输出全部踢出原子上下文（RT 树的 deferred/
kthread 化搬运——主线 6.6 尚无 printk kthreads，6.8+ 才引入；RT 侧早已先行）
no_console_suspend（power 分册 suspend 章）：suspend 期间保留 console 的
调试开关（代价：睡不下去/唤醒路径死锁风险）
console_suspend_on=0 / sysrq 音量等属于同族开关
```

**主线 6.6 的现实**：原子路径搬运仍在（trylock 失败退 defer），"printk 抖动"仍是 6.6 的已知痛点——笔记如实记录这一时点状态，6.8 的 kthread 化留给后续版本对照。

## 五、观测与排障

```
/proc/consoles             已注册 console 表（flags/占用/seq 游标）
cat /dev/kmsg              实时流（含被 console 过滤掉的行——对照
                            console_loglevel 语义）
dmesg | grep -c dropped    丢行统计
echo 8 > /proc/sys/kernel/printk   全量开放
# console 卡顿三板斧：
① /proc/consoles 看 con->seq 与 dmesg 尾部的差距（追赶滞后量化）
② 串口场景评估波特率上限（115200≈11KB/s——printk 风暴必卡）
③ PREEMPT_RT 树或 6.8+ kthread 化方案的对照测试
```

## 六、思考题

1. `console_trylock` 失败后的 defer 路径为什么用 irq_work 而不是直接 workqueue？（提示：workqueue 依赖的内存分配可能又失败——递归雪崩防线。）
2. boot console 与真 console 并存时输出为什么可能重复？（对照交接协议：flush 时序。）
3. `con->seq` 与全局 `console_seq` 为什么分开维护？（多 console 不同速。）
4. printk 在关中断段调用 `console->write`——驱动在此睡眠会发生什么？历史上哪些驱动因此爆雷？
5. RT 树把搬运挪出原子上下文的代价是什么？（对照 panic 路径：原子上下文里无 kthread 可用——`console_flush_on_panic` 的存在意义。）

---

> printk 分册至此 2 章完整。回看：[01 printk-ringbuffer](01-printk-ringbuffer.md)｜[02 console驱动模型](02-console驱动模型.md)
