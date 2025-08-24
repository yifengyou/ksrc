#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/kfifo.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/uidgid.h>

// 自定义结构体定义
typedef struct ctforge_audit_event {
	u8 audit_type;
	ktime_t timestamp;
	pid_t pid;
	uid_t uid;
	char comm[TASK_COMM_LEN];
	unsigned int data_size;
	char *data;  // 动态分配的指针成员
} ctforge_audit_event_t;

#define FIFO_SIZE 64  // 队列容量（必须是2的幂次）
static DEFINE_KFIFO(event_fifo, ctforge_audit_event_t, FIFO_SIZE);  // 静态声明kfifo
static DEFINE_SPINLOCK(fifo_lock);  // 自旋锁用于SMP同步

// 创建并填充一个审计事件
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

// 释放事件内存
static void free_audit_event(ctforge_audit_event_t *event) {
	if (event->data)
		kfree(event->data);
	kfree(event);
}

// 线程安全的入队操作（生产者）
static int enqueue_event(u8 type, const char *data, unsigned int data_len) {
	ctforge_audit_event_t *event = create_audit_event(type, data, data_len);
	if (!event)
		return -ENOMEM;

	// 使用spinlocked版本保证原子性
	if (kfifo_in_spinlocked(&event_fifo, event, 1, &fifo_lock)) {
		free_audit_event(event);  // 队列已拷贝数据，释放临时结构体
		return 0;
	} else {
		free_audit_event(event);
		return -ENOSPC;  // 队列已满
	}
}

// 线程安全的出队操作（消费者）
static ctforge_audit_event_t *dequeue_event(void) {
	ctforge_audit_event_t *event = kmalloc(sizeof(*event), GFP_KERNEL);
	if (!event)
		return NULL;

	if (kfifo_out_spinlocked(&event_fifo, event, 1, &fifo_lock)) {
		return event;
	} else {
		kfree(event);
		return NULL;  // 队列为空
	}
}

// 示例模块初始化
static int __init kfifo_example_init(void) {
	printk(KERN_INFO "Initializing audit event FIFO\n");

	// 测试入队
	const char *test_data = "Sensitive operation detected";
	if (enqueue_event(1, test_data, strlen(test_data) + 1) != 0) {
		printk(KERN_ERR "Failed to enqueue event\n");
		return -EFAULT;
	}

	// 测试出队
	ctforge_audit_event_t *event = dequeue_event();
	if (event) {
		printk(KERN_INFO "Dequeued event: type=%d, pid=%d, data='%s'\n",
				event->audit_type, event->pid, event->data);
		free_audit_event(event);
	} else {
		printk(KERN_ERR "No events in queue\n");
	}
	return 0;
}

// 模块退出时清理资源
static void __exit kfifo_example_exit(void) {
	ctforge_audit_event_t event;
	spin_lock(&fifo_lock);
	while (kfifo_out(&event_fifo, &event, 1)) {  // 非spinlocked版本（已持有锁）
		if (event.data)
			kfree(event.data);
	}
	spin_unlock(&fifo_lock);
	printk(KERN_INFO "Audit event FIFO module unloaded\n");
}

module_init(kfifo_example_init);
module_exit(kfifo_example_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("nicyou");
MODULE_DESCRIPTION("Thread-safe kfifo for ctforge_audit_event_t");

