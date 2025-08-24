#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/fcntl.h>
#include <linux/stat.h>
#include <linux/mnt_idmapping.h>

#define MODULE_NAME "kernel_file_ops"
#define TEST_FILE "/tmp/kernel_test_file"
#define TEST_DIR "/tmp/kernel_test_dir"
#define TEST_NEW_FILE "/tmp/kernel_test_file_renamed"

// 全局变量用于存储操作结果
static int operation_result = 0;

// 创建目录
static int create_directory(const char *path)
{
	struct dentry *dentry;
	struct path dir_path;
	int err;
	struct mnt_idmap *idmap;
	struct file *file;

	// 检查路径是否已存在
	if (kern_path(path, LOOKUP_DIRECTORY, &dir_path) == 0) {
		path_put(&dir_path);
		pr_info("Directory %s already exists\n", path);
		return -EEXIST;
	}

	// 创建目录
	dentry = kern_path_create(AT_FDCWD, path, &dir_path, LOOKUP_DIRECTORY);
	if (IS_ERR(dentry)) {
		pr_err("Failed to create dentry for %s: %ld\n", path, PTR_ERR(dentry));
		return PTR_ERR(dentry);
	}

	// 创建一个临时文件来获取idmap
	file = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		pr_err("Failed to create temp file for idmap: %d\n", err);
		done_path_create(&dir_path, dentry);
		return err;
	}

	idmap = file_mnt_idmap(file);
	err = vfs_mkdir(idmap, d_inode(dir_path.dentry), dentry, 0755);
	filp_close(file, NULL);
	done_path_create(&dir_path, dentry);

	if (err) {
		pr_err("Failed to create directory %s: %d\n", path, err);
		return err;
	}

	pr_info("Directory %s created successfully\n", path);
	return 0;
}

// 创建文件
static int create_file(const char *path)
{
	struct file *filp;
	int ret = 0;

	// 创建文件 (O_CREAT | O_RDWR | O_TRUNC)
	filp = filp_open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (IS_ERR(filp)) {
		ret = PTR_ERR(filp);
		pr_err("Failed to create file %s: %d\n", path, ret);
		goto out;
	}

	pr_info("File %s created successfully\n", path);

	// 关闭文件
	filp_close(filp, NULL);

out:
	return ret;
}

// 写入文件
static int write_to_file(const char *path, const char *data, size_t len)
{
	struct file *filp;
	loff_t pos = 0;
	int ret = 0;

	// 打开文件 (O_RDWR)
	filp = filp_open(path, O_RDWR, 0);
	if (IS_ERR(filp)) {
		ret = PTR_ERR(filp);
		pr_err("Failed to open file %s for writing: %d\n", path, ret);
		goto out;
	}

	// 写入数据
	ret = kernel_write(filp, data, len, &pos);
	if (ret < 0) {
		pr_err("Failed to write to file %s: %d\n", path, ret);
	} else {
		pr_info("Wrote %d bytes to file %s\n", ret, path);
	}

	// 关闭文件
	filp_close(filp, NULL);

out:
	return ret;
}

// 读取文件
static int read_from_file(const char *path, char *buf, size_t buf_size)
{
	struct file *filp;
	loff_t pos = 0;
	int ret = 0;

	// 打开文件 (O_RDONLY)
	filp = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(filp)) {
		ret = PTR_ERR(filp);
		pr_err("Failed to open file %s for reading: %d\n", path, ret);
		goto out;
	}

	// 读取数据
	ret = kernel_read(filp, buf, buf_size, &pos);
	if (ret < 0) {
		pr_err("Failed to read from file %s: %d\n", path, ret);
	} else {
		buf[ret] = '\0'; // 确保字符串终止
		pr_info("Read %d bytes from file %s: %s\n", ret, path, buf);
	}

	// 关闭文件
	filp_close(filp, NULL);

out:
	return ret;
}

// 获取文件信息
static int get_file_info(const char *path)
{
	struct kstat stat;
	int ret;
	struct path file_path;

	ret = kern_path(path, LOOKUP_FOLLOW, &file_path);
	if (ret) {
		pr_err("Failed to get path for %s: %d\n", path, ret);
		return ret;
	}

	ret = vfs_getattr(&file_path, &stat, STATX_BASIC_STATS, AT_STATX_SYNC_AS_STAT);
	path_put(&file_path);

	if (ret) {
		pr_err("Failed to get file info for %s: %d\n", path, ret);
		return ret;
	}

	pr_info("File info for %s:\n", path);
	pr_info("  Size: %lld bytes\n", stat.size);
	pr_info("  Mode: %o\n", stat.mode);
	pr_info("  UID: %d\n", stat.uid.val);
	pr_info("  GID: %d\n", stat.gid.val);
	pr_info("  Last access: %lld\n", stat.atime.tv_sec);
	pr_info("  Last modification: %lld\n", stat.mtime.tv_sec);
	pr_info("  Last status change: %lld\n", stat.ctime.tv_sec);

	return 0;
}

// 重命名文件
static int rename_file(const char *old_path, const char *new_path)
{
	struct renamedata rd;
	struct path old_path_struct, new_dir_path;
	struct dentry *old_dentry, *new_dentry;
	struct mnt_idmap *idmap;
	struct file *file;
	int ret;

	// 获取旧路径
	ret = kern_path(old_path, LOOKUP_FOLLOW, &old_path_struct);
	if (ret) {
		pr_err("Failed to find old path %s: %d\n", old_path, ret);
		return ret;
	}

	old_dentry = old_path_struct.dentry;

	// 获取新路径的父目录
	ret = kern_path(new_path, LOOKUP_PARENT, &new_dir_path);
	if (ret) {
		pr_err("Failed to find parent of new path %s: %d\n", new_path, ret);
		path_put(&old_path_struct);
		return ret;
	}

	// 创建新dentry
	new_dentry = lookup_one_len(new_path + strlen(new_dir_path.dentry->d_name.name) + 1,
			new_dir_path.dentry,
			strlen(new_path) - strlen(new_dir_path.dentry->d_name.name) - 1);
	if (IS_ERR(new_dentry)) {
		ret = PTR_ERR(new_dentry);
		pr_err("Failed to create new dentry for %s: %d\n", new_path, ret);
		path_put(&old_path_struct);
		path_put(&new_dir_path);
		return ret;
	}

	// 获取idmap
	file = filp_open(old_path, O_RDONLY, 0);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		pr_err("Failed to open file for idmap: %d\n", ret);
		dput(new_dentry);
		path_put(&old_path_struct);
		path_put(&new_dir_path);
		return ret;
	}

	idmap = file_mnt_idmap(file);
	filp_close(file, NULL);

	// 设置renamedata结构体
	rd.old_mnt_idmap = idmap;
	rd.old_dir = d_inode(old_path_struct.dentry->d_parent);
	rd.old_dentry = old_dentry;
	rd.new_mnt_idmap = idmap;
	rd.new_dir = d_inode(new_dir_path.dentry);
	rd.new_dentry = new_dentry;
	rd.flags = 0;

	// 执行重命名
	ret = vfs_rename(&rd);

	dput(new_dentry);
	path_put(&old_path_struct);
	path_put(&new_dir_path);

	if (ret) {
		pr_err("Failed to rename %s to %s: %d\n", old_path, new_path, ret);
	} else {
		pr_info("Successfully renamed %s to %s\n", old_path, new_path);
	}

	return ret;
}

// 删除文件
static int delete_file(const char *path)
{
	struct path file_path;
	struct mnt_idmap *idmap;
	struct file *file;
	int ret;

	ret = kern_path(path, LOOKUP_FOLLOW, &file_path);
	if (ret) {
		pr_err("Failed to find file %s: %d\n", path, ret);
		return ret;
	}

	if (!d_is_reg(file_path.dentry)) {
		pr_err("%s is not a regular file\n", path);
		path_put(&file_path);
		return -EINVAL;
	}

	// 获取idmap
	file = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		pr_err("Failed to open file for idmap: %d\n", ret);
		path_put(&file_path);
		return ret;
	}

	idmap = file_mnt_idmap(file);
	filp_close(file, NULL);

	ret = vfs_unlink(idmap, d_inode(file_path.dentry->d_parent), file_path.dentry, NULL);
	path_put(&file_path);

	if (ret) {
		pr_err("Failed to delete file %s: %d\n", path, ret);
	} else {
		pr_info("Successfully deleted file %s\n", path);
	}

	return ret;
}

// 删除目录
static int delete_directory(const char *path)
{
	struct path dir_path;
	struct mnt_idmap *idmap;
	struct file *file;
	int ret;

	ret = kern_path(path, LOOKUP_DIRECTORY, &dir_path);
	if (ret) {
		pr_err("Failed to find directory %s: %d\n", path, ret);
		return ret;
	}

	if (!d_is_dir(dir_path.dentry)) {
		pr_err("%s is not a directory\n", path);
		path_put(&dir_path);
		return -EINVAL;
	}

	// 获取idmap
	file = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		pr_err("Failed to open directory for idmap: %d\n", ret);
		path_put(&dir_path);
		return ret;
	}

	idmap = file_mnt_idmap(file);
	filp_close(file, NULL);

	ret = vfs_rmdir(idmap, d_inode(dir_path.dentry->d_parent), dir_path.dentry);
	path_put(&dir_path);

	if (ret) {
		pr_err("Failed to delete directory %s: %d\n", path, ret);
	} else {
		pr_info("Successfully deleted directory %s\n", path);
	}

	return ret;
}

// 模块初始化函数
static int __init file_ops_init(void)
{
	char *test_data = "This is a test string written from kernel space!";
	char read_buf[256];
	int ret;

	pr_info("Kernel file operations module loaded\n");

	// 1. 创建测试目录
	ret = create_directory(TEST_DIR);
	if (ret && ret != -EEXIST) {
		operation_result = ret;
		return ret;
	}

	// 2. 创建测试文件
	ret = create_file(TEST_FILE);
	if (ret) {
		operation_result = ret;
		return ret;
	}

	// 3. 写入测试文件
	ret = write_to_file(TEST_FILE, test_data, strlen(test_data));
	if (ret < 0) {
		operation_result = ret;
		return ret;
	}

	// 4. 读取测试文件
	memset(read_buf, 0, sizeof(read_buf));
	ret = read_from_file(TEST_FILE, read_buf, sizeof(read_buf)-1);
	if (ret < 0) {
		operation_result = ret;
		return ret;
	}

	// 5. 获取文件信息
	ret = get_file_info(TEST_FILE);
	if (ret) {
		operation_result = ret;
		return ret;
	}

	// 6. 重命名文件
	ret = rename_file(TEST_FILE, TEST_NEW_FILE);
	if (ret) {
		operation_result = ret;
		return ret;
	}

	// 7. 删除重命名后的文件
	ret = delete_file(TEST_NEW_FILE);
	if (ret) {
		operation_result = ret;
		return ret;
	}

	// 8. 删除测试目录
	ret = delete_directory(TEST_DIR);
	if (ret) {
		operation_result = ret;
		return ret;
	}

	operation_result = 0;
	return 0;
}

// 模块退出函数
static void __exit file_ops_exit(void)
{
	// 清理操作（如果需要）
	pr_info("Kernel file operations module unloaded with result: %d\n", operation_result);
}

module_init(file_ops_init);
module_exit(file_ops_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Kernel module demonstrating file operations in kernel space");
