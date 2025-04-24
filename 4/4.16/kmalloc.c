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
	
	//�����ں���ռ��ԭ����Ҫ���»�ȡ��ǰCPU���档����һ������ͬ��ȫ�µؽ��з������
	c = this_cpu_ptr(s->cpu_slab);
#endif

	page = c->page;
	if (!page)
		goto new_slab;

redo:
	//..................

	//��ǰpage������slab�Ѿ�û�п��ж����ˣ���kmalloc��ʼ���̽������е��˴���
	//�ս��뺯��ʱ������cpu����page�ֶλ�ָ���page��page->freelist=0���ұ���ס
	//����get_freelist�󣬸�slab���ⶳ����page�ṹ��frozen��Ϊ0
	freelist = get_freelist(s, page);

	if (!freelist) {
		c->page = NULL;
		goto new_slab;
	}

load_freelist:
	
	//��freelistָ����һ�����ж��󣬵�һ�����󷵻ظ��û�
	c->freelist = get_freepointer(s, freelist);
	
	c->tid = next_tid(c->tid);

	local_irq_restore(flags);
	
	return freelist;

new_slab:

	if (c->partial) 
	{
		page = c->page = c->partial;
		c->partial = page->next;
		c->freelist = NULL;
		goto redo;
	}
	
	//....................
}

//��ǰpage������slab�Ѿ�û�п��ж����ˣ���kmalloc��ʼ���̽������е��˴���
//�ս��뺯��ʱ������cpu����page�ֶλ�ָ���page��page->freelist=0���ұ���ס
//����get_freelist�󣬸�slab���ⶳ����page�ṹ��frozen��Ϊ0
static inline void *get_freelist(struct kmem_cache *s, struct page *page)
{
	struct page new;
	unsigned long counters;
	void *freelist;

	do {
		freelist = page->freelist;
		counters = page->counters;

		new.counters = counters;
		VM_BUG_ON(!new.frozen);

		new.inuse = page->objects;
		new.frozen = freelist != NULL;

	} while (!__cmpxchg_double_slab(s, page,
		freelist, counters,
		NULL, new.counters,
		"get_freelist"));

	return freelist;
}

static inline void *get_freepointer(struct kmem_cache *s, void *object)
{
	return *(void **)(object + s->offset);
}
