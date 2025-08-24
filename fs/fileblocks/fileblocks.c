#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/namei.h>
#include <linux/blkdev.h>

// 在这里设置要分析的文件路径
#define TARGET_FILE "/bin/ls"

static int __init inode_blocks_init(void)
{
	struct path p;
	struct inode *inode;
	sector_t block;
	int error;

	pr_info("Inode blocks module loaded\n");
	pr_info("Analyzing file: %s\n", TARGET_FILE);

	// 解析文件路径获取inode
	error = kern_path(TARGET_FILE, LOOKUP_FOLLOW, &p);
	if (error) {
		pr_err("Failed to resolve path: %s, error: %d\n", TARGET_FILE, error);
		return error;
	}

	inode = d_inode(p.dentry);
	if (!inode) {
		pr_err("Failed to get inode\n");
		path_put(&p);
		return -ENOENT;
	}

	pr_info("Inode: %lu\n", inode->i_ino);
	pr_info("File size: %lld bytes\n", i_size_read(inode));
	pr_info("Block size: %lu bytes\n", inode->i_sb->s_blocksize);

	if (!inode->i_mapping->a_ops->bmap) {
		pr_err("Filesystem doesn't support bmap operation\n");
		path_put(&p);
		return -EOPNOTSUPP;
	}

	pr_info("Blocks mapping:\n");

	// 计算文件包含的块数
	unsigned long num_blocks = (i_size_read(inode) + inode->i_sb->s_blocksize - 1) >> 
		inode->i_sb->s_blocksize_bits;

	for (block = 0; block < num_blocks; block++) {
		sector_t phys_block;

		phys_block = inode->i_mapping->a_ops->bmap(inode->i_mapping, block);
		if (phys_block) {
			pr_info("Logical block %llu -> Physical block %llu\n", 
					(unsigned long long)block, (unsigned long long)phys_block);
		} else {
			pr_info("Logical block %llu -> hole\n", (unsigned long long)block);
		}
	}

	path_put(&p);
	pr_info("File blocks analysis completed\n");
	return 0;
}

static void __exit inode_blocks_exit(void)
{
	pr_info("Inode blocks module unloaded\n");
}

module_init(inode_blocks_init);
module_exit(inode_blocks_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kdev");
MODULE_DESCRIPTION("Display blocks allocation for a fixed file");
MODULE_VERSION("1.0");



