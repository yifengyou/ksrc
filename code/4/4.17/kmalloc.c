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

	//..............

load_freelist:
	
	//��freelistָ����һ�����ж��󣬵�һ�����󷵻ظ��û�
	c->freelist = get_freepointer(s, freelist);
	
	c->tid = next_tid(c->tid);

	local_irq_restore(flags);
	
	return freelist;
	
new_slab:

	if (c->partial) 
	{
		//.....
	}
	
	freelist = new_slab_objects(s, gfpflags, node, &c);

	page = c->page;

	goto load_freelist;
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

	//........
}

//���δ�ָ���ڵ㡢Զ�˽ڵ�Ĳ��ֿ�slab��������ȡ����slab����������Ϊ�slab�ĵ�һ�����ж���ĵ�ַ
static void *get_partial(struct kmem_cache *s, gfp_t flags, int node, struct kmem_cache_cpu *c)
{
	void *object;
	int searchnode = (node == NUMA_NO_NODE) ? numa_node_id() : node;

	//��node�Ĳ��ֿ�slab�����л�ȡ����slab����������Ϊ�slab�ĵ�һ�����ж���ĵ�ַ
	object = get_partial_node(s, get_node(s, searchnode), c, flags);
	if (object || node != NUMA_NO_NODE)
		return object;

	//����NUMA���룬ѡ�������ڵ�Ĳ��ֿ�slab�����ȡslab
	return get_any_partial(s, flags, c);
}

//��node�Ĳ��ֿ�slab�����л�ȡ����slab����������Ϊ�slab�ĵ�һ�����ж���ĵ�ַ
//���õ��Ŀ��ж���������cpu_partial/2ʱ�����ٴ�node�Ĳ��ֿ������ȡslab
static void *get_partial_node(struct kmem_cache *s, struct kmem_cache_node *n, struct kmem_cache_cpu *c, gfp_t flags)
{
	struct page *page, *page2;
	void *object = NULL;
	int available = 0;
	int objects;

	//����node��slab��Դʱ����Ҫ����
	spin_lock(&n->list_lock);

	list_for_each_entry_safe(page, page2, &n->partial, lru) 
	{
		void *t;

		//�ú�����node�Ĳ��ֿ�slab������ȡ��page������slab��������page�ṹ�ֶΣ�freelist��inuse��frozen
		t = acquire_slab(s, n, page, object == NULL, &objects);
		if (!t)
			break;

		//available���ٴ�node���ֿ�slab�����У��õ��Ŀ��ж�����Ŀ
		available += objects;

		if (!object) 
		{//˵����ǰpage������slab�����������slab
			//���ñ���CPU���棬ʹ��slab��Ϊ�slab
			c->page = page;
			object = t;
		} 
		else 
		{
			//��page������slab����Ϊ�slabʱ�����뱾��CPU����Ĳ��ֿ�slab������
			put_cpu_partial(s, page, 0);
		}

		if (!kmem_cache_has_cpu_partial(s) || available > s->cpu_partial / 2)
			break;

	}
	spin_unlock(&n->list_lock);
	return object;
}