#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/file.h>
#include <linux/namei.h>

#define FILE_PATH "/data/123.txt"

static int __init file_ops_init(void)
{
	struct file *filp;
	loff_t pos = 0;
	char buf[32] = {0};
	ssize_t ret;

	printk(KERN_INFO "File operations module loaded\n");

	// 创建文件 (O_CREAT | O_WRONLY | O_TRUNC)
	filp = filp_open(FILE_PATH, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (IS_ERR(filp)) {
		printk(KERN_ERR "Failed to create file: %ld\n", PTR_ERR(filp));
		return PTR_ERR(filp);
	}
	filp_close(filp, NULL);

	// 打开文件准备写入
	filp = filp_open(FILE_PATH, O_WRONLY, 0);
	if (IS_ERR(filp)) {
		printk(KERN_ERR "Failed to open file for writing: %ld\n", PTR_ERR(filp));
		return PTR_ERR(filp);
	}

	// 写入数据
	ret = kernel_write(filp, "hello", 5, &pos);
	if (ret < 0) {
		printk(KERN_ERR "Failed to write to file: %zd\n", ret);
		filp_close(filp, NULL);
		return ret;
	}
	printk(KERN_INFO "Wrote %zd bytes to file\n", ret);
	filp_close(filp, NULL);

	// 打开文件准备读取
	filp = filp_open(FILE_PATH, O_RDONLY, 0);
	if (IS_ERR(filp)) {
		printk(KERN_ERR "Failed to open file for reading: %ld\n", PTR_ERR(filp));
		return PTR_ERR(filp);
	}

	// 读取数据
	pos = 0;
	ret = kernel_read(filp, buf, sizeof(buf), &pos);
	if (ret < 0) {
		printk(KERN_ERR "Failed to read from file: %zd\n", ret);
		filp_close(filp, NULL);
		return ret;
	}

	printk(KERN_INFO "Read %zd bytes from file: %s\n", ret, buf);
	filp_close(filp, NULL);

	return 0;
}

static void __exit file_ops_exit(void)
{
	printk(KERN_INFO "File operations module unloaded\n");
}

module_init(file_ops_init);
module_exit(file_ops_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("yifengyou");
MODULE_DESCRIPTION("Kernel file operations example");
