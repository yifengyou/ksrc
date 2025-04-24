static __always_inline void *kmalloc(size_t size, gfp_t flags)
{
	//������ռ����8KBʱ��ֱ�ӵ���__get_free_pages���϶��룬����ҳ��
	if (size > KMALLOC_MAX_CACHE_SIZE)//8K
		return kmalloc_large(size, flags);

	//�ں˷�����ά����14����������������index��0��13
	int index = kmalloc_index(size);

	if (!index)
		return ZERO_SIZE_PTR;//((void *)16)

	return kmem_cache_alloc_trace(kmalloc_caches[index], flags, size);
}

static __always_inline int kmalloc_index(size_t size)
{
	if (!size)
		return 0;

	if (size > 64 && size <= 96)
		return 1;

	if (size > 128 && size <= 192)
		return 2;

	if (size <=          8) return 3;
	if (size <=         16) return 4;
	if (size <=         32) return 5;
	if (size <=         64) return 6;
	if (size <=        128) return 7;
	if (size <=        256) return 8;
	if (size <=        512) return 9;
	if (size <=       1024) return 10;
	if (size <=   2 * 1024) return 11;
	if (size <=   4 * 1024) return 12;
	if (size <=   8 * 1024) return 13;
	
	return -1;
}

void *kmem_cache_alloc_trace(struct kmem_cache *s, gfp_t gfpflags, size_t size)
{
	void *ret = slab_alloc(s, gfpflags, _RET_IP_);
	return ret;
}
EXPORT_SYMBOL(kmem_cache_alloc_trace);

static __always_inline void *slab_alloc(struct kmem_cache *s,
										gfp_t gfpflags, unsigned long addr)
{
	return slab_alloc_node(s, gfpflags, NUMA_NO_NODE, addr);
}

static __always_inline void *slab_alloc_node(struct kmem_cache *s,
		gfp_t gfpflags, int node, unsigned long addr)
{
	void **object;
	struct kmem_cache_cpu *c;
	struct page *page;
	unsigned long tid;

	preempt_disable();
	
	c = __this_cpu_ptr(s->cpu_slab);
	
	tid = c->tid;
	
	preempt_enable();

	object = c->freelist;
	page = c->page;

	if (unlikely(!object || !node_match(page, node)))
	{
		//����ǰCPU�Ļslabû�п��ж��󣬻���page�����ڸ�node��
		//���ߵ�ǰCPU������û�лslab����������º�����
		object = __slab_alloc(s, gfpflags, node, addr, c);
	}
	
	//......................

	return object;
}

static void *__slab_alloc(struct kmem_cache *s, gfp_t gfpflags, int node,
			  unsigned long addr, struct kmem_cache_cpu *c)
{
	void *freelist;
	struct page *page;
	unsigned long flags;

	//��Ȼ��������жϣ���������û�����kmallocʱ����˯�ߣ�gfpflags��������__GFP_WAIT����
	//�����allocate_slab�����п��жϣ�Ȼ�����buddy����ҳ��֮���ٹ��ж�
	//���ԾͿ��ܳ���˯�Ѻ󣬱�������������CPU����
	local_irq_save(flags);

#ifdef CONFIG_PREEMPT
	/*
	 * We may have been preempted and rescheduled on a different
	 * cpu before disabling interrupts. Need to reload cpu area
	 * pointer.
	 */
	//�����ں���ռ��ԭ����Ҫ���»�ȡ��ǰCPU���档����һ������ͬ��ȫ�µؽ��з������
	c = this_cpu_ptr(s->cpu_slab);
#endif

	page = c->page;
	if (!page)
		goto new_slab;

	//..................

load_freelist:
	
	//��freelistָ����һ�����ж��󣬵�һ�����󷵻ظ��û�
	c->freelist = get_freepointer(s, freelist);
	
	c->tid = next_tid(c->tid);

	local_irq_restore(flags);
	
	return freelist;

new_slab:

	if (c->partial) 
	{
		//.............
	}

	freelist = new_slab_objects(s, gfpflags, node, &c);

	page = c->page;

	goto load_freelist;
	
	//....................
}

static inline void *get_freepointer(struct kmem_cache *s, void *object)
{
	return *(void **)(object + s->offset);
}

static inline void *new_slab_objects(struct kmem_cache *s, gfp_t flags, int node, struct kmem_cache_cpu **pc)
{
	void *freelist;
	struct kmem_cache_cpu *c = *pc;
	struct page *page;

	//���δ�ָ���ڵ㡢Զ�˽ڵ�Ĳ��ֿ�slab��������ȡ����slab
	//��������Ϊ�slab�ĵ�һ�����ж���ĵ�ַ
	freelist = get_partial(s, flags, node, c);
	if (freelist)
		return freelist;

	page = new_slab(s, flags, node);
	if (page) 
	{
		//��Ȼ��������жϣ���������û�����kmallocʱ����˯�ߣ�gfpflags��������__GFP_WAIT����
		//�����allocate_slab�����У�new_slab���õģ����жϣ�Ȼ�����buddy����ҳ��֮���ٹ��ж�
		//���ԾͿ��ܳ���˯�Ѻ󣬱�������������CPU����
		//���������ٴζ�ȡ����cpu���档���������������cpu����c->page������Ч��
		//��ʱ����slab����������node�Ĳ��ֿ�slab����ȫ��slab�������߹黹��buddy
		//��Ȼ�������뵽��slab���ܲ����ڽ������node
		c = __this_cpu_ptr(s->cpu_slab);
		if (c->page)
			flush_slab(s, c);

		/*
		 * No other reference to the page yet so we can
		 * muck around with it freely without cmpxchg
		 */
		freelist = page->freelist;
		page->freelist = NULL;

		c->page = page;
		*pc = c;
	} 
	else
		freelist = NULL;

	return freelist;
}

static struct page *new_slab(struct kmem_cache *s, gfp_t flags, int node)
{
	struct page *page;
	void *start;
	void *last;
	void *p;
	int order;

	//����slab�������������ҳ�򣬲�����һ��ҳ��page�ṹ��objects�ֶΣ���Ϊ��slab�����Ķ������
	page = allocate_slab(s, flags & (GFP_RECLAIM_MASK | GFP_CONSTRAINT_MASK), node);
	if (!page)
		goto out;

	//�·����slab����Ӧ��kmem_cache_node��nr_slabs++,total_objects������Ӧ�Ķ�����
	inc_slabs_node(s, page_to_nid(page), page->objects);

	//��page�ṹ��slab_cache�ֶΣ���ָ���������ṹkmem_cache
	page->slab_cache = s;

	start = page_address(page);

	//����ÿ������ı�����һ�����ж����ַ���ֶΣ�
	//ʹ��ʵ����ָ���������ڵĸߵ�ַ���ж���
	last = start;
	for_each_object(p, s, start, page->objects) 
	{
		setup_object(s, page, last);
		set_freepointer(s, last, p);
		last = p;
	}
	setup_object(s, page, last);
	set_freepointer(s, last, NULL);

	//����page�ṹ��freelist��ʹ��ָ��slab�еĵ�һ�����ж���
	page->freelist = start;
	page->inuse = page->objects;
	page->frozen = 1;

out:
	return page;
}

static struct page *allocate_slab(struct kmem_cache *s, gfp_t flags, int node)
{
	struct page *page;
	struct kmem_cache_order_objects oo = s->oo;
	gfp_t alloc_gfp;

	//����û�����kmallocʱ����˯�ߣ���Ὺ�жϣ�Ȼ�����buddy����ҳ��֮���ٹ��ж�
	if (flags & __GFP_WAIT)
		local_irq_enable();

	flags |= s->allocflags;

	//����ʹ��kmem_cache��oo�й涨��order������ҳ��
	//��ʧ�ܣ���ʹ��kmem_cache��min�й涨��order����ҳ��
	page = alloc_slab_page(alloc_gfp, node, oo);
	if (unlikely(!page)) 
	{
		oo = s->min;
		page = alloc_slab_page(flags, node, oo);
	}

	if (flags & __GFP_WAIT)
		local_irq_disable();

	page->objects = oo_objects(oo);

	return page;
}

static inline struct page *alloc_slab_page(gfp_t flags, int node, struct kmem_cache_order_objects oo)
{
	int order = oo_order(oo);

	if (node == NUMA_NO_NODE)
		return alloc_pages(flags, order);
	else
		return alloc_pages_exact_node(node, flags, order);
}