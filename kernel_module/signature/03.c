#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/version.h>
//#include <linux/kallsyms.h>


typedef int (*module_sig_check_t)(struct module *mod);

static unsigned long module_sig_check_addr = 0;
static char original_instructions[20];
static bool is_hooked = false;

// 汇编代码：xor eax, eax; ret
// 对应机器码：31 c0 c3
static unsigned char patch_code[] = { 0x31, 0xC0, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90 };

static int __init patch_init(void)
{
	//struct module *mod;
	unsigned long cr0;

	printk(KERN_INFO "Patching module_sig_check...\n");

	// 获取 module_sig_check 函数地址
	//module_sig_check_addr = kallsyms_lookup_name("module_sig_check");
	// ffffffff9fbd9440
	module_sig_check_addr = 0xffffffff9fbd9440;
	if (!module_sig_check_addr) {
		printk(KERN_ERR "Failed to find module_sig_check symbol\n");
		return -ENOENT;
	}

	printk(KERN_INFO "module_sig_check found at: 0x%lx\n", module_sig_check_addr);

	// 保存原始指令
	memcpy(original_instructions, (void *)module_sig_check_addr, sizeof(patch_code));

	// 禁用写保护
	cr0 = read_cr0();
	write_cr0(cr0 & ~0x10000);

	// 应用补丁
	memcpy((void *)module_sig_check_addr, patch_code, sizeof(patch_code));

	// 恢复写保护
	write_cr0(cr0);

	// 刷新指令缓存
	//flush_icache_range(module_sig_check_addr, module_sig_check_addr + sizeof(patch_code));

	is_hooked = true;
	printk(KERN_INFO "module_sig_check patched successfully\n");

	return 0;
}

static void __exit patch_exit(void)
{
	unsigned long cr0;

	if (is_hooked) {
		printk(KERN_INFO "Restoring original module_sig_check...\n");

		// 禁用写保护
		cr0 = read_cr0();
		write_cr0(cr0 & ~0x10000);

		// 恢复原始指令
		memcpy((void *)module_sig_check_addr, original_instructions, sizeof(patch_code));

		// 恢复写保护
		write_cr0(cr0);

		// 刷新指令缓存
		//flush_icache_range(module_sig_check_addr, module_sig_check_addr + sizeof(patch_code));

		printk(KERN_INFO "Original module_sig_check restored\n");
	}
}

module_init(patch_init);
module_exit(patch_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Patch module_sig_check to always return 0");
MODULE_VERSION("1.0");
MODULE_INFO(btf, "disabled");
