#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/version.h>
#include <linux/kallsyms.h>
#include <linux/string.h>
#include <asm/cacheflush.h>
#include <asm/text-patching.h>

// 禁用BTF验证
MODULE_INFO(btf, "disabled");

#if LINUX_VERSION_CODE != KERNEL_VERSION(6,6,0)
#error "This module is designed for Linux kernel version 6.6.0 only"
#endif

typedef int (*module_sig_check_t)(struct module *mod);
typedef void (*text_poke_bp_p)(void *addr, const void *opcode, size_t len, const void *emulate);

static unsigned long module_sig_check_addr = 0;
static text_poke_bp_p text_poke_bp_addr;
static char original_instructions[20];
static bool is_hooked = false;

// 汇编代码：xor eax, eax; ret
// 对应机器码：31 c0 c3
static unsigned char patch_code[] = { 0x31, 0xC0, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90 };

// 安全的内存写入函数
static void safe_memcpy(void *dst, const void *src, size_t size)
{

	// ffffffff8c99ec70
	text_poke_bp_addr = 0xffffffff9f99ec70;
	text_poke_bp_addr(dst, src, size, NULL);
	/*
	unsigned long cr0;


	preempt_disable();
	// 保存当前CR0并禁用写保护
	cr0 = read_cr0();
	write_cr0(cr0 & ~X86_CR0_WP);

	// 执行内存复制
	memcpy(dst, src, size);

	// 恢复写保护
	write_cr0(cr0);
	preempt_enable();
	*/
}

// 刷新指令缓存（兼容性处理）
static void my_flush_icache_range(unsigned long start, unsigned long end)
{
#if defined(CONFIG_X86)
	// 对于x86架构，使用clflush指令刷新缓存
	unsigned long addr;
	for (addr = start; addr < end; addr += boot_cpu_data.x86_clflush_size)
		clflushopt((void *)addr);

	// 内存屏障确保操作完成
	mb();
#else
	// 其他架构使用标准函数
	flush_icache_range(start, end);
#endif
}

// 获取符号地址的安全方式
static unsigned long safe_kallsyms_lookup_name(const char *name)
{
	unsigned long addr;

	// 方法1: 直接使用kallsyms_lookup_name（如果可用）
	addr = kallsyms_lookup_name(name);
	if (addr)
		return addr;

	// 方法2: 通过/proc/kallsyms解析（备用方案）
	printk(KERN_WARNING "kallsyms_lookup_name failed, trying alternative method\n");

	// 这里可以添加通过读取/proc/kallsyms来查找符号的代码
	// 但为了简单起见，我们暂时返回硬编码地址

	// 如果是module_sig_check，返回已知地址
	if (strcmp(name, "module_sig_check") == 0) {
		return 0xffffffff9fbd9440;
	}

	return 0;
}

static int __init patch_init(void)
{
	printk(KERN_INFO "Patching module_sig_check...\n");

	// 获取 module_sig_check 函数地址
	// module_sig_check_addr = safe_kallsyms_lookup_name("module_sig_check");
	module_sig_check_addr = 0xffffffff9edd9440;
	if (!module_sig_check_addr) {
		printk(KERN_ERR "Failed to find module_sig_check symbol\n");
		return -ENOENT;
	}

	printk(KERN_INFO "module_sig_check found at: 0x%lx\n", module_sig_check_addr);

	// 保存原始指令
	safe_memcpy(original_instructions, (void *)module_sig_check_addr, sizeof(patch_code));

	printk(KERN_INFO "Original instructions: %02x %02x %02x %02x %02x %02x %02x %02x\n",
			original_instructions[0], original_instructions[1],
			original_instructions[2], original_instructions[3],
			original_instructions[4], original_instructions[5],
			original_instructions[6], original_instructions[7]);

	// 应用补丁
	safe_memcpy((void *)module_sig_check_addr, patch_code, sizeof(patch_code));

	// 刷新指令缓存
	my_flush_icache_range(module_sig_check_addr, module_sig_check_addr + sizeof(patch_code));

	is_hooked = true;
	printk(KERN_INFO "module_sig_check patched successfully\n");

	// 验证补丁是否生效
	printk(KERN_INFO "Patched instructions: %02x %02x %02x %02x %02x %02x %02x %02x\n",
			((unsigned char *)module_sig_check_addr)[0],
			((unsigned char *)module_sig_check_addr)[1],
			((unsigned char *)module_sig_check_addr)[2],
			((unsigned char *)module_sig_check_addr)[3],
			((unsigned char *)module_sig_check_addr)[4],
			((unsigned char *)module_sig_check_addr)[5],
			((unsigned char *)module_sig_check_addr)[6],
			((unsigned char *)module_sig_check_addr)[7]);

	return 0;
}

static void __exit patch_exit(void)
{
	if (is_hooked) {
		printk(KERN_INFO "Restoring original module_sig_check...\n");

		// 恢复原始指令
		safe_memcpy((void *)module_sig_check_addr, original_instructions, sizeof(patch_code));

		// 刷新指令缓存
		my_flush_icache_range(module_sig_check_addr, module_sig_check_addr + sizeof(patch_code));

		printk(KERN_INFO "Original module_sig_check restored\n");
	}
}

module_init(patch_init);
module_exit(patch_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Patch module_sig_check to always return 0");
MODULE_VERSION("1.0");
