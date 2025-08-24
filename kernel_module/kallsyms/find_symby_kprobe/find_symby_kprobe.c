#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>

static int __init find_symby_kprobe_init(void) {
    pr_info("find_symby_kprobe: module_init\n");
    return 0;
}

static void __exit find_symby_kprobe_exit(void) {
    pr_info("find_symby_kprobe: module_exit\n");
}

module_init(find_symby_kprobe_init);
module_exit(find_symby_kprobe_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kdev");
MODULE_DESCRIPTION("A simple Linux driver");
MODULE_VERSION("0.1");
