#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/fsnotify.h>
#include <linux/namei.h>
#include <linux/path.h>

static int fsnotify_callback(struct notifier_block *nb,
		unsigned long action, void *data)
{
	struct fsnotify_event *event = data;
	struct inode *inode;
	struct dentry *dentry;
	char *pathname;

	if (!event || !event->inode)
		return NOTIFY_DONE;

	inode = event->inode;
	dentry = d_find_alias(inode);
	if (!dentry)
		return NOTIFY_DONE;

	pathname = kasprintf(GFP_KERNEL, "%s", dentry->d_name.name);
	if (!pathname) {
		dput(dentry);
		return NOTIFY_DONE;
	}

	switch (action) {
		case FS_CREATE:
			pr_info("File created: %s\n", pathname);
			break;
		case FS_MODIFY:
			pr_info("File modified: %s\n", pathname);
			break;
		case FS_DELETE:
			pr_info("File deleted: %s\n", pathname);
			break;
		case FS_ACCESS:
			pr_info("File accessed: %s\n", pathname);
			break;
		default:
			break;
	}

	kfree(pathname);
	dput(dentry);
	return NOTIFY_DONE;
}

static struct notifier_block fsnotify_nb = {
	.notifier_call = fsnotify_callback,
};

static int __init fs_watcher_init(void)
{
	int ret;

	pr_info("FS Watcher module loaded\n");

	ret = fsnotify_add_notifier(&fsnotify_nb);
	if (ret) {
		pr_err("Failed to register fsnotify notifier: %d\n", ret);
		return ret;
	}

	pr_info("Registered fsnotify notifier\n");
	return 0;
}

static void __exit fs_watcher_exit(void)
{
	fsnotify_remove_notifier(&fsnotify_nb);
	pr_info("FS Watcher module unloaded\n");
}

module_init(fs_watcher_init);
module_exit(fs_watcher_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("File system event watcher using fsnotify");


