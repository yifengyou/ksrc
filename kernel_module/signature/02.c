#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/version.h>
#include <linux/memory.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <asm/text-patching.h>
#include <asm/cacheflush.h>

#define MODULE_NAME "sig_check_patcher"

typedef int (*module_sig_check_t)(struct module *mod, const void *sig, unsigned long sig_len);

static unsigned long module_sig_check_addr = 0;
static u8 original_instructions[20];
static bool is_patched = false;

static u8 patch_code[] = { 0x31, 0xC0, 0xC3 };

// 从/proc/kallsyms读取符号地址
static unsigned long get_symbol_address(const char *symbol_name)
{
	struct file *file;
	char buf[256];
	loff_t pos = 0;
	ssize_t ret;
	unsigned long addr;
	char *p, *sym;

	file = filp_open("/proc/kallsyms", O_RDONLY, 0);
	if (IS_ERR(file)) {
		pr_err("Failed to open /proc/kallsyms\n");
		return 0;
	}

	while ((ret = kernel_read(file, buf, sizeof(buf) - 1, &pos)) > 0) {
		buf[ret] = '\0';
		p = buf;

		while (*p) {
			if (sscanf(p, "%lx %*s %ms", &addr, &sym) == 2) {
				if (strcmp(sym, symbol_name) == 0) {
					kfree(sym);
					filp_close(file, NULL);
					return addr;
				}
				kfree(sym);
			}

			// 移动到下一行
			p = strchr(p, '\n');
			if (!p) break;
			p++;
		}
	}

	filp_close(file, NULL);
	return 0;
}

// 手动修改页面保护
static void make_rw(unsigned long address)
{
	unsigned int level;
	pte_t *pte = lookup_address(address, &level);

	if (pte) {
		pte->pte |= _PAGE_RW;
	}
}

static void make_ro(unsigned long address)
{
	unsigned int level;
	pte_t *pte = lookup_address(address, &level);

	if (pte) {
		pte->pte &= ~_PAGE_RW;
	}
}

static int __init sig_patcher_init(void)
{
	// 获取符号地址
	//module_sig_check_addr = get_symbol_address("module_sig_check");
	pr_info("init skip checker\n");
	module_sig_check_addr = 0xffffffff837d9440;
	if (!module_sig_check_addr) {
		pr_err("Failed to find module_sig_check symbol\n");
		return -ENOENT;
	}

	pr_info("Found module_sig_check at %pL\n", (void *)module_sig_check_addr);

	// 保存原始指令
	memcpy(original_instructions, (void *)module_sig_check_addr, sizeof(patch_code));

	// 修改页面保护为可写
	make_rw(module_sig_check_addr);

	// 应用补丁
	memcpy((void *)module_sig_check_addr, patch_code, sizeof(patch_code));

	// 恢复页面保护
	make_ro(module_sig_check_addr);

	// 刷新CPU指令缓存
	flush_icache_range(module_sig_check_addr, module_sig_check_addr + sizeof(patch_code));

	is_patched = true;
	pr_info("Successfully patched module_sig_check\n");
	return 0;
}

static void __exit sig_patcher_exit(void)
{
	if (is_patched) {
		// 修改页面保护为可写
		make_rw(module_sig_check_addr);

		// 恢复原始指令
		memcpy((void *)module_sig_check_addr, original_instructions, sizeof(patch_code));

		// 恢复页面保护
		make_ro(module_sig_check_addr);

		// 刷新CPU指令缓存
		flush_icache_range(module_sig_check_addr, module_sig_check_addr + sizeof(patch_code));

		pr_info("Successfully restored original module_sig_check\n");
	}
}

module_init(sig_patcher_init);
module_exit(sig_patcher_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Patch module_sig_check to always return 0");
MODULE_VERSION("0.1");
