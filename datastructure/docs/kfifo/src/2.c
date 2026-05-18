#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/kfifo.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>
#include <linux/sched.h>

// 自定义结构体定义
typedef struct ctforge_audit_event {
	u8 audit_type;
	ktime_t timestamp;
	pid_t pid;
	uid_t uid;
	char comm[TASK_COMM_LEN];
	unsigned int data_size;
	char *data;
} ctforge_audit_event_t;

#define FIFO_SIZE 64  // 队列容量（必须是2的幂次）
static DEFINE_KFIFO(event_fifo, ctforge_audit_event_t, FIFO_SIZE);  // 静态声明kfifo
static DEFINE_SPINLOCK(fifo_lock);  // 自旋锁用于SMP同步

// 初始化并填充一个事件结构体
static ctforge_audit_event_t *create_audit_event(u8 type, const char *user_data, unsigned int data_len) {
	ctforge_audit_event_t *event = kmalloc(sizeof(*event), GFP_KERNEL);
	if (!event)
		return NULL;

	event->audit_type = type;
	event->timestamp = ktime_get();
	event->pid = current->pid;
	event->uid = current_uid().val;
	get_task_comm(event->comm, current);
	event->data_size = data_len;

	// 动态分配并拷贝数据
	if (data_len > 0) {
		event->data = kmalloc(data_len, GFP_KERNEL);
		if (!event->data) {
			kfree(event);
			return NULL;
		}
		memcpy(event->data, user_data, data_len);
	} else {
		event->data = NULL;
	}
	return event;
}

// 释放事件结构体内存
static void free_audit_event(ctforge_audit_event_t *event) {
	if (event->data)
		kfree(event->data);
	kfree(event);
}

// 入队操作（生产者）
static int enqueue_event(u8 type, const char *data, unsigned int data_len) {
	ctforge_audit_event_t *event = create_audit_event(type, data, data_len);
	if (!event)
		return -ENOMEM;

	spin_lock(&fifo_lock);
	if (kfifo_avail(&event_fifo) > 0) {
		kfifo_put(&event_fifo, *event);  // 结构体拷贝入队
		spin_unlock(&fifo_lock);
		free_audit_event(event);  // 释放临时结构体（队列已保存副本）
		return 0;
	} else {
		spin_unlock(&fifo_lock);
		free_audit_event(event);
		return -ENOSPC;  // 队列已满
	}
}

// 出队操作（消费者）
static ctforge_audit_event_t *dequeue_event(void) {
	ctforge_audit_event_t *event = kmalloc(sizeof(*event), GFP_KERNEL);
	if (!event)
		return NULL;

	spin_lock(&fifo_lock);
	if (!kfifo_is_empty(&event_fifo)) {
		kfifo_get(&event_fifo, *event);  // 结构体拷贝出队
		spin_unlock(&fifo_lock);
		return event;
	} else {
		spin_unlock(&fifo_lock);
		kfree(event);
		return NULL;  // 队列为空
	}
}

static int __init kfifo_example_init(void) {
	printk(KERN_INFO "Enqueuing test event...\n");
	const char *test_data = "Security audit payload";
	enqueue_event(1, test_data, strlen(test_data) + 1);

	ctforge_audit_event_t *event = dequeue_event();
	if (event) {
		printk(KERN_INFO "Dequeued event: type=%d, pid=%d, data='%s'\n",
				event->audit_type, event->pid, event->data);
		free_audit_event(event);
	}
	return 0;
}

static void __exit kfifo_example_exit(void) {
	ctforge_audit_event_t event;
	spin_lock(&fifo_lock);
	while (kfifo_get(&event_fifo, &event)) {
		if (event.data)
			kfree(event.data);
	}
	spin_unlock(&fifo_lock);
	printk(KERN_INFO "Module unloaded\n");
}

module_init(kfifo_example_init);
module_exit(kfifo_example_exit);
MODULE_LICENSE("GPL");
