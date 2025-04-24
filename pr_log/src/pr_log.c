#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

static int __init pr_log_init(void)
{
	printk(KERN_INFO "--------------------------------------------\n");
	printk(KERN_INFO "Loading pr_log Module\n");
	printk(KERN_INFO "file:%s func:%s line:%d\n",__FILE__,__func__,__LINE__);
	pr_emerg("This is an emergency message\n");
	pr_alert("This is an alert message\n");
	pr_crit("This is a critical message\n");
	pr_err("This is an error message\n");
	pr_warn("This is a warning message\n");
	pr_notice("This is a notice message\n");
	pr_info("This is an info message\n");
	pr_cont("This is a continuation of the previous line\n");
	pr_devel("This is a development message\n");
	pr_debug("This is a debug message\n");

	return 0;
}

static void __exit pr_log_exit(void)
{
	printk(KERN_INFO "--------------------------------------------\n");
	printk(KERN_INFO "Removing pr_log Module\n");
	printk(KERN_INFO "file:%s func:%s line:%d\n",__FILE__,__func__,__LINE__);
}

module_init(pr_log_init);
module_exit(pr_log_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("pr_log Module");
MODULE_AUTHOR("nicyou");
