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

redo:
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
	else 
	{//��ǰCPU����Ļslab�п��ж���
	 //��ʱ��ȡ��ǰ�slab�еĵ�һ�����ж���
	 //��freelistָ��Ķ���
		
		//��ȡ��һ�����ж���ĵ�ַ
		void *next_object = get_freepointer_safe(s, object);

		//ʧ�ܵ�ԭ������ǣ�
		//����û�й��жϣ���ǰ���̿��ܱ�����������ռ����ʱ��������Ҳ�ڸûslab����
		//������������������CPU����ûslab�ͷŶ����
		if (unlikely(!this_cpu_cmpxchg_double(
				s->cpu_slab->freelist, s->cpu_slab->tid,
				object, tid,
				next_object, next_tid(tid)))) 
			goto redo;
		
		prefetch_freepointer(s, next_object);
	}

	if (unlikely(gfpflags & __GFP_ZERO) && object)
		memset(object, 0, s->object_size);

	return object;
}

static inline void *get_freepointer_safe(struct kmem_cache *s, void *object)
{
	void *p;

	p = get_freepointer(s, object);

	return p;
}

static inline void *get_freepointer(struct kmem_cache *s, void *object)
{
	return *(void **)(object + s->offset);
}