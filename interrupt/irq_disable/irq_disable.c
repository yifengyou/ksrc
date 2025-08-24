#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/hardirq.h>

#define DRIVER_AUTHOR "nicyou"
#define DRIVER_DESC   "Kernel module demonstrating interrupt disabling techniques"

// 用于保存中断状态的变量
static unsigned long irq_flags;

// 用于单个IRQ控制的变量
static unsigned int test_irq = 1; // 默认使用IRQ1 (键盘中断)作为示例
module_param(test_irq, uint, 0644);
MODULE_PARM_DESC(test_irq, "IRQ number to test disable/enable functions");

// 用于嵌套中断禁止的变量
static unsigned long nested_flags1, nested_flags2;

// 简单的中断处理函数
static irqreturn_t dummy_handler(int irq, void *dev_id)
{
	pr_info("Dummy handler called for IRQ %d\n", irq);
	return IRQ_NONE;
}

// 禁止所有中断（最基础的方式）
static void demo_basic_disable(void)
{
	pr_info("=== Basic interrupt disable demo ===\n");

	// 禁止中断
	local_irq_disable();
	pr_info("Interrupts disabled\n");

	// 在这里执行需要原子性的关键操作
	pr_info("Critical section: interrupts are disabled\n");
	mdelay(2); // 模拟耗时操作

	// 重新启用中断
	local_irq_enable();
	pr_info("Interrupts enabled\n");
}

// 保存和恢复中断状态
static void demo_save_restore(void)
{
	pr_info("=== Save/restore interrupt state demo ===\n");

	// 保存当前中断状态并禁止中断
	local_irq_save(irq_flags);
	pr_info("Interrupts disabled (state saved)\n");

	// 关键操作
	pr_info("Critical section: interrupts are disabled\n");
	mdelay(1);

	// 恢复之前的中断状态
	local_irq_restore(irq_flags);
	pr_info("Interrupt state restored\n");
}

// 嵌套中断禁止
static void demo_nested_disable(void)
{
	pr_info("=== Nested interrupt disable demo ===\n");

	// 第一层禁止
	local_irq_save(nested_flags1);
	pr_info("Level 1: interrupts disabled\n");

	// 第二层禁止
	local_irq_save(nested_flags2);
	pr_info("Level 2: interrupts still disabled\n");

	// 关键操作
	pr_info("Critical section: nested interrupt disable\n");
	mdelay(1);

	// 第一层恢复
	local_irq_restore(nested_flags2);
	pr_info("Level 2: restored - interrupts still disabled\n");

	// 第二层恢复
	local_irq_restore(nested_flags1);
	pr_info("Level 1: restored - interrupts now enabled\n");
}

// 单个IRQ禁止
static void demo_single_irq_disable(void)
{
	int ret;

	pr_info("=== Single IRQ disable demo (IRQ %d) ===\n", test_irq);

	// 首先尝试注册一个处理函数（如果IRQ未被占用）
	ret = request_irq(test_irq, dummy_handler, IRQF_SHARED, "test_irq", (void *)demo_single_irq_disable);
	if (ret) {
		pr_warn("Could not register handler for IRQ %d (may be in use)\n", test_irq);
	} else {
		pr_info("Registered dummy handler for IRQ %d\n", test_irq);
	}

	// 禁止特定IRQ
	disable_irq(test_irq);
	pr_info("IRQ %d disabled\n", test_irq);

	// 模拟操作
	pr_info("Critical section: IRQ %d is disabled\n", test_irq);
	mdelay(2);

	// 重新启用IRQ
	enable_irq(test_irq);
	pr_info("IRQ %d enabled\n", test_irq);

	// 释放处理函数（如果注册成功）
	if (!ret) {
		free_irq(test_irq, (void *)demo_single_irq_disable);
		pr_info("Dummy handler for IRQ %d unregistered\n", test_irq);
	}
}

// 禁止中断与自旋锁结合
static void demo_spinlock_irq(void)
{
	DEFINE_SPINLOCK(test_lock);

	pr_info("=== Spinlock + interrupt disable demo ===\n");

	// 获取自旋锁并禁止中断
	spin_lock_irqsave(&test_lock, irq_flags);
	pr_info("Spinlock acquired, interrupts disabled\n");

	// 关键操作
	pr_info("Critical section: spinlock held, interrupts disabled\n");
	mdelay(1);

	// 释放自旋锁并恢复中断
	spin_unlock_irqrestore(&test_lock, irq_flags);
	pr_info("Spinlock released, interrupts enabled\n");
}

// 禁止下半部（软中断）
static void demo_bh_disable(void)
{
	pr_info("=== Bottom half (softirq) disable demo ===\n");

	// 禁止下半部
	local_bh_disable();
	pr_info("Bottom halves disabled\n");

	// 关键操作
	pr_info("Critical section: bottom halves are disabled\n");
	mdelay(1);

	// 启用下半部
	local_bh_enable();
	pr_info("Bottom halves enabled\n");
}

// 检查中断上下文
static void demo_check_context(void)
{
	pr_info("=== Interrupt context check demo ===\n");

	if (in_interrupt()) {
		pr_info("We are in interrupt context\n");
		pr_info("Hard interrupt context: %s\n", in_irq() ? "yes" : "no");
		pr_info("Soft interrupt context: %s\n", in_softirq() ? "yes" : "no");
	} else {
		pr_info("We are in process context\n");
	}

	if (in_nmi()) {
		pr_info("We are in NMI context\n");
	}
}

// 模块初始化函数
static int __init irq_demo_init(void)
{
	pr_info("Interrupt Disable Demo Module Loaded\n");

	// 演示各种中断禁止技术
	demo_basic_disable();
	demo_save_restore();
	demo_nested_disable();
	demo_single_irq_disable();
	demo_spinlock_irq();
	demo_bh_disable();
	demo_check_context();

	return 0;
}

// 模块退出函数
static void __exit irq_demo_exit(void)
{
	pr_info("Interrupt Disable Demo Module Unloaded\n");
}

module_init(irq_demo_init);
module_exit(irq_demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_VERSION("1.0");
