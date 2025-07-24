#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/atomic.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("nicyou");
MODULE_DESCRIPTION("Atomic operations demonstration module");

// 定义原子变量
static atomic_t my_atomic = ATOMIC_INIT(0);
static atomic_t my_atomic2 = ATOMIC_INIT(10);

static int __init atomic_demo_init(void)
{
	int old_val, new_val, ret;

	printk(KERN_INFO "Atomic Demo Module Loaded\n");

	// 1. 基本原子操作
	printk(KERN_INFO "Initial value: %d\n", atomic_read(&my_atomic));

	atomic_set(&my_atomic, 5);
	printk(KERN_INFO "After atomic_set: %d\n", atomic_read(&my_atomic));

	atomic_add(3, &my_atomic);
	printk(KERN_INFO "After atomic_add(3): %d\n", atomic_read(&my_atomic));

	atomic_sub(2, &my_atomic);
	printk(KERN_INFO "After atomic_sub(2): %d\n", atomic_read(&my_atomic));

	atomic_inc(&my_atomic);
	printk(KERN_INFO "After atomic_inc: %d\n", atomic_read(&my_atomic));

	atomic_dec(&my_atomic);
	printk(KERN_INFO "After atomic_dec: %d\n", atomic_read(&my_atomic));

	// 2. 带返回值的原子操作
	old_val = atomic_add_return(4, &my_atomic);
	printk(KERN_INFO "atomic_add_return(4): old=%d, new=%d\n", 
			old_val, atomic_read(&my_atomic));

	old_val = atomic_sub_return(2, &my_atomic);
	printk(KERN_INFO "atomic_sub_return(2): old=%d, new=%d\n", 
			old_val, atomic_read(&my_atomic));

	old_val = atomic_inc_return(&my_atomic);
	printk(KERN_INFO "atomic_inc_return: old=%d, new=%d\n", 
			old_val, atomic_read(&my_atomic));

	old_val = atomic_dec_return(&my_atomic);
	printk(KERN_INFO "atomic_dec_return: old=%d, new=%d\n", 
			old_val, atomic_read(&my_atomic));

	// 3. 测试类原子操作
	ret = atomic_add_negative(5, &my_atomic);
	printk(KERN_INFO "atomic_add_negative(5): result=%d (is negative? %s), value=%d\n",
			ret, ret ? "yes" : "no", atomic_read(&my_atomic));

	ret = atomic_sub_and_test(3, &my_atomic);
	printk(KERN_INFO "atomic_sub_and_test(3): result=%d (is zero? %s), value=%d\n",
			ret, ret ? "yes" : "no", atomic_read(&my_atomic));

	ret = atomic_inc_and_test(&my_atomic);
	printk(KERN_INFO "atomic_inc_and_test: result=%d (is zero? %s), value=%d\n",
			ret, ret ? "yes" : "no", atomic_read(&my_atomic));

	ret = atomic_dec_and_test(&my_atomic);
	printk(KERN_INFO "atomic_dec_and_test: result=%d (is zero? %s), value=%d\n",
			ret, ret ? "yes" : "no", atomic_read(&my_atomic));

	// 4. 高级原子操作
	// atomic_cmpxchg - 比较并交换
	old_val = atomic_read(&my_atomic);
	new_val = 42;
	ret = atomic_cmpxchg(&my_atomic, old_val, new_val);
	printk(KERN_INFO "atomic_cmpxchg(expected=%d, new=%d): old=%d, success=%s, now=%d\n",
			old_val, new_val, ret, (ret == old_val) ? "yes" : "no", atomic_read(&my_atomic));

	// 失败的cmpxchg尝试
	old_val = atomic_read(&my_atomic) - 1; // 故意设置错误期望值
	new_val = 100;
	ret = atomic_cmpxchg(&my_atomic, old_val, new_val);
	printk(KERN_INFO "atomic_cmpxchg(expected=%d, new=%d): old=%d, success=%s, now=%d\n",
			old_val, new_val, ret, (ret == old_val) ? "yes" : "no", atomic_read(&my_atomic));

	// atomic_xchg - 交换
	old_val = atomic_read(&my_atomic);
	new_val = 7;
	ret = atomic_xchg(&my_atomic, new_val);
	printk(KERN_INFO "atomic_xchg(new=%d): old=%d, now=%d\n",
			new_val, ret, atomic_read(&my_atomic));

	// 5. atomic_add_unless (不常见但有用的操作)
	atomic_set(&my_atomic, 5);
	atomic_set(&my_atomic2, 10);

	ret = atomic_add_unless(&my_atomic, 3, 5); // 当值不等于5时才加3
	printk(KERN_INFO "atomic_add_unless(&my_atomic, 3, 5): added? %s (value=%d)\n",
			ret ? "no" : "yes", atomic_read(&my_atomic));

	ret = atomic_add_unless(&my_atomic2, 3, 5); // 当值不等于5时才加3
	printk(KERN_INFO "atomic_add_unless(&my_atomic2, 3, 5): added? %s (value=%d)\n",
			ret ? "yes" : "no", atomic_read(&my_atomic2));

	// 6. atomic_inc_not_zero (不常见但有用的操作)
	atomic_set(&my_atomic, 0);
	ret = atomic_inc_not_zero(&my_atomic);
	printk(KERN_INFO "atomic_inc_not_zero on zero: success=%s, value=%d\n",
			ret ? "yes" : "no", atomic_read(&my_atomic));

	atomic_set(&my_atomic, 1);
	ret = atomic_inc_not_zero(&my_atomic);
	printk(KERN_INFO "atomic_inc_not_zero on non-zero: success=%s, value=%d\n",
			ret ? "yes" : "no", atomic_read(&my_atomic));

	return 0;
}

static void __exit atomic_demo_exit(void)
{
	printk(KERN_INFO "Atomic Demo Module Unloaded\n");
}

module_init(atomic_demo_init);
module_exit(atomic_demo_exit);

