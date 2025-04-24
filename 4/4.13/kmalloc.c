static __always_inline void *kmalloc(size_t size, gfp_t flags)
{
	//������ռ����8KBʱ��ֱ�ӵ���__get_free_pages����2��n�η����룬����ҳ��
	if (size > KMALLOC_MAX_CACHE_SIZE)//8K
		return kmalloc_large(size, flags);
	
	//..............
}

//ֱ�ӵ���__get_free_pages���϶��룬����ҳ��
static __always_inline void *kmalloc_large(size_t size, gfp_t flags)
{
	//����size����buddy���õ�order
	unsigned int order = get_order(size);
	return kmalloc_order_trace(size, flags, order);
}

void *kmalloc_order_trace(size_t size, gfp_t flags, unsigned int order)
{
	void *ret = kmalloc_order(size, flags, order);
	return ret;
}
EXPORT_SYMBOL(kmalloc_order_trace);

static __always_inline void* kmalloc_order(size_t size, gfp_t flags, unsigned int order)
{
	void *ret;

	flags |= (__GFP_COMP | __GFP_KMEMCG);
	ret = (void *) __get_free_pages(flags, order);
	return ret;
}