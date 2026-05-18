#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>

#define MOD_NAME "list_demo01"
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("A kernel module demonstrating comprehensive Linux kernel list operations");

// 1. 定义包含链表节点的数据结构
struct my_data {
	int value;
	struct list_head node;
};

// 定义全局链表头
static struct list_head main_list;
static struct list_head temp_list;

static int __init list_demo_init(void)
{
	struct my_data *entry, *tmp, *pos;

	printk(KERN_INFO MOD_NAME ": --- Initializing List APIs Demo ---\n");

	// 【初始化操作】
	// 动态初始化主链表
	INIT_LIST_HEAD(&main_list);
	// 静态初始化临时链表
	LIST_HEAD(temp_list_head);
	// 将全局指针指向静态初始化的链表头
	temp_list = temp_list_head;

	// 【插入元素操作】
	// 向主链表添加 5 个节点 (10, 20, 30, 40, 50)
	for (int i = 1; i <= 5; i++) {
		entry = kmalloc(sizeof(*entry), GFP_KERNEL);
		if (!entry) return -ENOMEM;
		entry->value = i * 10;
		// list_add 是头插法（插在 head 后面），所以链表顺序会是 50->40->30->20->10
		list_add(&entry->node, &main_list);
	}
	printk(KERN_INFO MOD_NAME ": Added 5 elements to main_list using list_add.\n");

	// 【遍历链表 & 查询链表】
	printk(KERN_INFO MOD_NAME ": Traversing main_list (list_for_each_entry):\n");
	list_for_each_entry(entry, &main_list, node) {
		// 顺便测试 list_is_last 和 list_is_singular
		if (list_is_last(&entry->node, &main_list))
			printk(KERN_INFO MOD_NAME ":  Value: %d (This is the last node)\n", entry->value);
		else
			printk(KERN_INFO MOD_NAME ":  Value: %d\n", entry->value);
	}
	// 测试 list_empty 和 list_is_singular
	if (!list_empty(&main_list) && !list_is_singular(&main_list)) {
		printk(KERN_INFO MOD_NAME ": main_list is not empty and has more than one entry.\n");
	}

	// 【查询链表：获取特定位置的 entry】
	// 获取第一个和最后一个节点
	struct my_data *first = list_first_entry(&main_list, struct my_data, node);
	struct my_data *last = list_last_entry(&main_list, struct my_data, node);
	printk(KERN_INFO MOD_NAME ": First entry value: %d, Last entry value: %d\n", first->value, last->value);

	// 测试 list_first_entry_or_null
	struct my_data *first_or_null = list_first_entry_or_null(&main_list, struct my_data, node);
	if (first_or_null)
		printk(KERN_INFO MOD_NAME ": list_first_entry_or_null got: %d\n", first_or_null->value);

	// 测试 list_next_entry 和 list_prev_entry
	struct my_data *next_of_first = list_next_entry(first, node);
	struct my_data *prev_of_last = list_prev_entry(last, node);
	printk(KERN_INFO MOD_NAME ": Next of first(%d) is %d, Prev of last(%d) is %d\n", 
			first->value, next_of_first->value, last->value, prev_of_last->value);

	// 【替换元素操作】
	// 将第一个节点（值为50）替换为一个新节点（值为99）
	struct my_data *new_entry = kmalloc(sizeof(*new_entry), GFP_KERNEL);
	new_entry->value = 99;
	list_replace(&first->node, &new_entry->node);
	kfree(first); // 替换后释放旧节点内存
	printk(KERN_INFO MOD_NAME ": Replaced first entry (50) with new entry (99) using list_replace.\n");

	// 【删除元素操作】
	// 删除值为 40 的节点（当前在第二个位置）
	struct my_data *to_delete = list_next_entry(list_first_entry(&main_list, struct my_data, node), node);
	list_del(&to_delete->node);
	kfree(to_delete);
	printk(KERN_INFO MOD_NAME ": Deleted the second entry (40) using list_del.\n");

	// 【移动节点操作】
	// 将最后一个节点移动到临时链表中
	struct my_data *to_move = list_last_entry(&main_list, struct my_data, node);
	list_move(&to_move->node, &temp_list);
	printk(KERN_INFO MOD_NAME ": Moved last entry (%d) from main_list to temp_list using list_move.\n", to_move->value);

	// 将临时链表的节点移动回主链表的尾部
	list_move_tail(temp_list.next, &main_list);
	printk(KERN_INFO MOD_NAME ": Moved entry back to the tail of main_list using list_move_tail.\n");

	// 【多个链表操作：拼接 (Splice)】
	// 再向 temp_list 添加一些数据用于拼接演示
	for (int i = 1; i <= 3; i++) {
		entry = kmalloc(sizeof(*entry), GFP_KERNEL);
		entry->value = 100 + i;
		list_add_tail(&entry->node, &temp_list);
	}
	// 将 temp_list 的所有节点拼接到 main_list 的头部
	list_splice_init(&temp_list, &main_list);
	printk(KERN_INFO MOD_NAME ": Spliced temp_list into main_list using list_splice_init.\n");

	// 【单个链表操作：切割 (Cut)】
	// 将 main_list 从第一个节点处切开，切出的部分放入 temp_list
	struct my_data *cut_pos = list_first_entry(&main_list, struct my_data, node);
	list_cut_position(&temp_list, &main_list, &cut_pos->node);
	printk(KERN_INFO MOD_NAME ": Cut main_list at value %d into temp_list using list_cut_position.\n", cut_pos->value);

	// 【旋转链表操作】
	// 将主链表的第一个节点移到链表末尾
	list_rotate_left(&main_list);
	printk(KERN_INFO MOD_NAME ": Rotated main_list to the left using list_rotate_left.\n");

	// 【遍历链表：反向遍历 & 安全遍历删除】
	printk(KERN_INFO MOD_NAME ": Final main_list content (Reverse traversal with list_for_each_entry_reverse):\n");
	// 使用安全遍历宏，防止在遍历中删除节点导致崩溃
	list_for_each_entry_safe_reverse(entry, tmp, &main_list, node) {
		printk(KERN_INFO MOD_NAME ":  Value: %d\n", entry->value);
		// 演示 list_for_each_entry_continue 的准备工作（这里仅打印，不做实际跳转）
		// list_prepare_entry(entry, &main_list, node);
	}

	// 【清理工作：释放所有内存】
	// 释放主链表
	list_for_each_entry_safe(entry, tmp, &main_list, node) {
		list_del(&entry->node);
		kfree(entry);
	}
	// 释放临时链表（切割出来的部分）
	list_for_each_entry_safe(entry, tmp, &temp_list, node) {
		list_del(&entry->node);
		kfree(entry);
	}

	printk(KERN_INFO MOD_NAME ": All lists cleaned up. Module loaded successfully.\n");
	return 0;
}

static void __exit list_demo_exit(void)
{
	printk(KERN_INFO MOD_NAME ": Module exiting. Goodbye!\n");
}

module_init(list_demo_init);
module_exit(list_demo_exit);
