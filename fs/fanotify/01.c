#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/fanotify.h>
#include <linux/slab.h>
#include <linux/audit.h>
#include <linux/printk.h>

static int __init fs_watcher_init(void)
{
	struct fanotify_event_metadata *event;
	struct fanotify_event_info_fid *fid;
	int fan_fd;
	char buf[4096];
	ssize_t len;
	char pathname[PATH_MAX];
	struct file_handle *handle;
	int ret;

	pr_info("FS Watcher module loaded\n");

	fan_fd = fanotify_init(FAN_CLASS_NOTIF | FAN_REPORT_DFID_NAME, O_RDONLY);
	if (fan_fd < 0) {
		pr_err("Failed to initialize fanotify: %d\n", fan_fd);
		return fan_fd;
	}

	ret = fanotify_mark(fan_fd, FAN_MARK_ADD | FAN_MARK_FILESYSTEM,
			FAN_CREATE | FAN_MODIFY | FAN_CLOSE_WRITE,
			AT_FDCWD, "/");
	if (ret < 0) {
		pr_err("Failed to add mark: %d\n", ret);
		goto out_close;
	}

	while (1) {
		len = read(fan_fd, buf, sizeof(buf));
		if (len < 0) {
			if (signal_pending(current))
				break;
			pr_err("Failed to read event: %zd\n", len);
			continue;
		}

		event = (struct fanotify_event_metadata *)buf;
		while (FAN_EVENT_OK(event, len)) {
			if (event->mask & FAN_CREATE) {
				fid = (struct fanotify_event_info_fid *)(event + 1);
				handle = (struct file_handle *)fid->handle;
				ret = fanotify_event_get_path(handle, pathname, sizeof(pathname));
				if (ret < 0) {
					pr_err("Failed to get path: %d\n", ret);
				} else {
					pr_info("File created: %s\n", pathname);
				}
			} else if (event->mask & FAN_MODIFY) {
				fid = (struct fanotify_event_info_fid *)(event + 1);
				handle = (struct file_handle *)fid->handle;
				ret = fanotify_event_get_path(handle, pathname, sizeof(pathname));
				if (ret < 0) {
					pr_err("Failed to get path: %d\n", ret);
				} else {
					pr_info("File modified: %s\n", pathname);
				}
			} else if (event->mask & FAN_CLOSE_WRITE) {
				fid = (struct fanotify_event_info_fid *)(event + 1);
				handle = (struct file_handle *)fid->handle;
				ret = fanotify_event_get_path(handle, pathname, sizeof(pathname));
				if (ret < 0) {
					pr_err("Failed to get path: %d\n", ret);
				} else {
					pr_info("File written and closed: %s\n", pathname);
				}
			}
			event = FAN_EVENT_NEXT(event, len);
		}
	}

out_close:
	close_fd(fan_fd);
	return ret;
}

static void __exit fs_watcher_exit(void)
{
	pr_info("FS Watcher module unloaded\n");
}

module_init(fs_watcher_init);
module_exit(fs_watcher_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("File system event watcher");
