#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fsnotify.h>
#include <linux/init.h>
#include <linux/path.h>
#include <linux/slab.h>
#include <linux/namei.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Simple fsnotify Monitor");

// 自定义监控组结构
struct my_fsnotify_group {
	struct fsnotify_group *group;
	struct path target_path;
	char *filename; // 存储监控的文件名
};

static struct my_fsnotify_group my_group;

/*
#define FS_ACCESS		0x00000001	/ File was accessed /
#define FS_MODIFY		0x00000002	/ File was modified /
#define FS_ATTRIB		0x00000004	/ Metadata changed /
#define FS_CLOSE_WRITE		0x00000008	/ Writtable file was closed /
#define FS_CLOSE_NOWRITE	0x00000010	/ Unwrittable file closed /
#define FS_OPEN			0x00000020	/ File was opened /
#define FS_MOVED_FROM		0x00000040	/ File was moved from X /
#define FS_MOVED_TO		0x00000080	/ File was moved to Y /
#define FS_CREATE		0x00000100	/ Subfile was created /
#define FS_DELETE		0x00000200	/ Subfile was deleted /
#define FS_DELETE_SELF		0x00000400	/ Self was deleted /
#define FS_MOVE_SELF		0x00000800	/ Self was moved /
#define FS_OPEN_EXEC		0x00001000	/ File was opened for exec /

#define FS_UNMOUNT		0x00002000	/ inode on umount fs /
#define FS_Q_OVERFLOW		0x00004000	/ Event queued overflowed /
#define FS_ERROR		0x00008000	/ Filesystem Error (fanotify) /

#define FS_IN_IGNORED		0x00008000	/ last inotify event here /

#define FS_OPEN_PERM		0x00010000	/ open event in an permission hook /
#define FS_ACCESS_PERM		0x00020000	/ access event in a permissions hook /
#define FS_OPEN_EXEC_PERM	0x00040000	/ open/exec event in a permission hook /
*/

// 事件处理回调
static int my_handle_event(struct fsnotify_group *group, u32 mask,
		const void *data, int data_type, struct inode *dir,
		const struct qstr *file_name, u32 cookie,
		struct fsnotify_iter_info *iter_info)
{
	// 处理目录事件（有文件名）
	if (file_name) {
		if (mask & FS_CREATE) {
			printk(KERN_INFO "fsnotify_demo: New file created: %.*s\n",
					file_name->len, file_name->name);
		}
		if (mask & FS_CLOSE_WRITE) {
			printk(KERN_INFO "fsnotify_demo: file closed: %.*s\n",
					file_name->len, file_name->name);
		}
		if (mask & FS_MODIFY) {
			printk(KERN_INFO "fsnotify_demo: file modified: %.*s\n",
					file_name->len, file_name->name);
		}
	}
	// 处理文件事件（无文件名，使用存储的文件名）
	else {
		if (mask & FS_MODIFY) {
			printk(KERN_INFO "fsnotify_demo: file modified: %s\n", my_group.filename);
		}
		if (mask & FS_CLOSE_WRITE) {
			printk(KERN_INFO "fsnotify_demo: file closed: %s\n", my_group.filename);
		}
	}
	pr_info("event happen!\n");
	return 0;
}

// 定义操作函数集
static const struct fsnotify_ops my_fsnotify_ops = {
	.handle_event = my_handle_event,
};

// 初始化模块
static int __init fsnotify_demo_init(void)
{
	struct path *path = &my_group.target_path;
	struct fsnotify_mark *mark;
	int ret;
	char targetpath[] = "/data/123.txt";

	// 存储文件名（用于事件处理）
	my_group.filename = kstrdup(targetpath, GFP_KERNEL);
	if (!my_group.filename) {
		printk(KERN_ERR "Failed to allocate filename\n");
		return -ENOMEM;
	}

	// 解析目标路径
	ret = kern_path(targetpath, LOOKUP_FOLLOW, path);
	if (ret) {
		printk(KERN_ERR "Path resolve failed for %s\n", targetpath);
		kfree(my_group.filename);
		return ret;
	}

	// 创建fsnotify组
	my_group.group = fsnotify_alloc_group(&my_fsnotify_ops, FSNOTIFY_GROUP_DUPS);
	if (IS_ERR(my_group.group)) {
		printk(KERN_ERR "Group alloc failed\n");
		path_put(path);
		kfree(my_group.filename);
		return PTR_ERR(my_group.group);
	}

	// 添加监控标记
	mark = kmalloc(sizeof(*mark), GFP_KERNEL);
	if (!mark) {
		printk(KERN_ERR "Failed to allocate memory for mark\n");
		fsnotify_put_group(my_group.group);
		path_put(path);
		kfree(my_group.filename);
		return -ENOMEM;
	}

	fsnotify_init_mark(mark, my_group.group);
	// mark->mask = FS_MODIFY | FS_CLOSE_WRITE | FS_CREATE; // 仅监控文件事件
	mark->mask = FS_MODIFY | FS_CLOSE_WRITE | FS_CLOSE_NOWRITE | FS_CREATE | FS_DELETE ; // 仅监控文件事件

	ret = fsnotify_add_inode_mark(mark, d_inode(path->dentry), 0);
	if (ret) {
		printk(KERN_ERR "Failed to add inode mark (error %d)\n", ret);
		fsnotify_put_mark(mark);
		fsnotify_put_group(my_group.group);
		path_put(path);
		kfree(my_group.filename);
		return ret;
	}

	printk(KERN_INFO "Monitoring %s for file events\n", targetpath);
	return 0;
}

// 清理模块
static void __exit fsnotify_demo_exit(void)
{
	path_put(&my_group.target_path);
	fsnotify_put_group(my_group.group);
	kfree(my_group.filename); // 释放文件名内存
	printk(KERN_INFO "Module unloaded\n");
}

module_init(fsnotify_demo_init);
module_exit(fsnotify_demo_exit);
