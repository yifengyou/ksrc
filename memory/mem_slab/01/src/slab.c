#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>

extern struct kmem_cache *kmem_cache;

static int __init slab_init(void)
{
	printk(KERN_INFO "--------------------------------------------\n");
	printk(KERN_INFO "Loading slab Module\n");
	printk(KERN_INFO "file:%s func:%s line:%d\n",__FILE__,__func__,__LINE__);
	struct kmem_cache_cpu *c;
	struct kmem_cache *cache;
	unsigned long flags;
	int cpu;

	printk(KERN_INFO "Starting slab info module\n");

	// 获取第一个 slab 缓存
	cache = kmem_cache_next(kmem_cache);
	while (cache) {
		printk(KERN_INFO "Cache: %s\n", cache->name);
		printk(KERN_INFO "   Size: %zu\n", cache->size);
		printk(KERN_INFO "   Object size: %zu\n", cache->object_size);
		printk(KERN_INFO "   Active objects: %u\n", cache->num);
		printk(KERN_INFO "   Total allocated objects: %lu\n", cache->total_objects);

		// 遍历每个 CPU 的 slab 缓存
		for_each_possible_cpu(cpu) {
			local_irq_save(flags);
			c = per_cpu_ptr(cache->cpu_slab, cpu);
			if (c->freelist) {
				printk(KERN_INFO "   CPU %d: Free objects: %u\n", cpu, c->freelist->next);
			}
			local_irq_restore(flags);
		}

		// 获取下一个 slab 缓存
		cache = kmem_cache_next(cache);
	}

	return 0;

}

static void __exit slab_exit(void)
{
	printk(KERN_INFO "--------------------------------------------\n");
	printk(KERN_INFO "Removing slab Module\n");
	printk(KERN_INFO "file:%s func:%s line:%d\n",__FILE__,__func__,__LINE__);
}

module_init(slab_init);
module_exit(slab_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("slab Module");
MODULE_AUTHOR("nicyou");
