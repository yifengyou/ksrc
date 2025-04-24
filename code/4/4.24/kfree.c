void kfree(const void *x)
{
	struct page *page;
	void *object = (void *)x;

	//�õ�x����slab�ĵ�һ��ҳ���page�ṹ
	page = virt_to_head_page(x);
	if (unlikely(!PageSlab(page))) //����8KB�Ķ�������ʱ��û������PG_slab��־
	{
		//������ͷ�
		//ʵ���ϵ�����__free_pages�ͷ�ҳ��
		return;
	}

	slab_free(page->slab_cache, page, x, _RET_IP_);
}
EXPORT_SYMBOL(kfree);

static __always_inline void slab_free(struct kmem_cache *s, struct page *page, void *x, unsigned long addr)
{
	void **object = (void *)x;
	struct kmem_cache_cpu *c;
	unsigned long tid;

redo:
	preempt_disable();
	c = __this_cpu_ptr(s->cpu_slab);

	tid = c->tid;
	preempt_enable();

	//�����ͷŶ������ڵ�slab�ǻslabʱ��ʹ��cas���������ͷŶ�����뵽����cpu����freelist����ͷ
	if (likely(page == c->page)) 
	{
		//......
	} 
	else
		__slab_free(s, page, x, addr);//��Էǻslab�ڵĶ����ͷ�
}

static void __slab_free(struct kmem_cache *s, struct page *page, void *x, unsigned long addr)
{
	void *prior;
	void **object = (void *)x;
	int was_frozen;
	struct page new;
	unsigned long counters;
	struct kmem_cache_node *n = NULL;
	
	
	//�����ͷŶ������slab��page->freelistָ��Ŀ��ж�������ͷ��
	do 
	{
		prior = page->freelist;//�����в�Ϊ0
		counters = page->counters;

		//ʹ���ͷŶ���ı�����һ�����ж����ֶΣ�ָ��page�ṹ��freelist�ĵ�һ�����ж���
		set_freepointer(s, object, prior);
		
		new.counters = counters;
		was_frozen = new.frozen;//������û�б���ס
		new.inuse--;//��ʱslab�Ƿǻslab��inuse��������ʹ�õĶ������

		//������slabȫ�ǿ��ж��󣬼�inuse=0����û�б���ס
		if ((!new.inuse || !prior) && !was_frozen) 
		{
			//��ȡ��Ӧ��NUMA�ڵ㻺��ṹ
	            n = get_node(s, page_to_nid(page));
				
				spin_lock_irqsave(&n->list_lock, flags);

			}
		}

	} while (!cmpxchg_double_slab(s, page,
		prior, counters,
		object, new.counters,
		"__slab_free"));

	//�����ͷŶ���������slabû�б���ס���Ҹ�slab�о��ǿ��ж��󣨸�slabӦ��λ��ĳ��node�Ĳ��ֿ�slab�����У�
	//���Ҹ�slab������node�Ĳ��ֿ������е�slab�������������ޣ������ͷŸ�slab
	if (unlikely(!new.inuse && n->nr_partial > s->min_partial))
	{
		remove_partial(n, page);

		spin_unlock_irqrestore(&n->list_lock, flags);

		//�ͷ�slab
		discard_slab(s, page);
	}
}

static inline void remove_partial(struct kmem_cache_node *n, struct page *page)
{
	list_del(&page->lru);
	n->nr_partial--;
}