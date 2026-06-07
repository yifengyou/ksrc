#include <linux/init.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <asm/pgtable.h>
#include <asm/fixmap.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Print User + Kernel Full Address Space");

static int __init full_addr_print_init(void)
{
    printk("=====================================================\n");
    printk("    Linux 完整虚拟地址空间（从低到高）\n");
    printk("=====================================================\n");

    // ====================== 用户地址空间 ======================
    printk("[用户空间 低地址]\n");
    printk("  TASK_UNMAPPED_BASE   = 0x%lx\n", TASK_UNMAPPED_BASE);
    printk("  堆起始(大致)        = 高于 TASK_UNMAPPED_BASE\n");
    printk("  MMAP_START          = 0x%lx\n", VM_START); // mmap 区域起始
    printk("  STACK_TOP           = 0x%lx\n", STACK_TOP); // 栈顶（用户空间最高）
    printk("  TASK_SIZE (用户顶)  = 0x%lx\n", TASK_SIZE);

    // ====================== 内核地址空间 ======================
    printk("\n[内核空间 从低到高]\n");
    printk("  PAGE_OFFSET (内核起始) = 0x%lx\n", PAGE_OFFSET);
    printk("  VMALLOC_START        = 0x%lx\n", VMALLOC_START);
    printk("  VMALLOC_END          = 0x%lx\n", VMALLOC_END);
    printk("  FIXADDR_START        = 0x%lx\n", FIXADDR_START);
    printk("  FIXADDR_TOP          = 0x%lx\n", FIXADDR_TOP);
    printk("  MODULES_END          = 0x%lx\n", MODULES_END);

    printk("=====================================================\n");
    printk("地址打印完成！\n");
    return 0;
}

static void __exit full_addr_print_exit(void)
{
    printk("地址打印模块已卸载\n");
}

module_init(full_addr_print_init);
module_exit(full_addr_print_exit);

