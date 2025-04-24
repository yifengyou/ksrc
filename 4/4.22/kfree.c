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
		prior = page->freelist;//������Ϊ0
		counters = page->counters;

		//ʹ���ͷŶ���ı�����һ�����ж����ֶΣ�ָ��page�ṹ��freelist�ĵ�һ�����ж���
		set_freepointer(s, object, prior);
		
		new.counters = counters;
		was_frozen = new.frozen;//������û�б���ס
		new.inuse--;//��ʱslab�Ƿǻslab��inuse��������ʹ�õĶ������

		//�����ͷŶ���������slabû�б���ס���Ҹ�slab�о��ǿ��ж��󣨸�slabӦ��λ��ĳ��node�Ĳ��ֿ�slab�����У�
		//�����ͷŶ���������slabû�б���ס����page->freelistΪ0������ǣ���slab֮ǰ�ǻslab�����������󣬼��޿��ж���󣬲����ǻslab������cache��page�ֶβ�ָ�����ˣ���Ҳ�����κβ��ֿ�������
		if ((!new.inuse || !prior) && !was_frozen) 
		{
			
			if (!prior)
			{
				//���ͷŶ���������slab����֮ǰ�ǻslab�����������󣬼��޿��ж���󣬲����ǻslab��
				//����cache��page�ֶβ�ָ�����ˣ���Ҳ�����κβ��ֿ�������
				//��slab���ᱻ���뱾��cpu����Ĳ��ֿ�slab������
				new.frozen = 1;
			}
			else 
			{ 
				//.................
			}
		}

	} while (!cmpxchg_double_slab(s, page,
		prior, counters,
		object, new.counters,
		"__slab_free"));

	if (new.frozen && !was_frozen) 
	{
		//��slab���ᱻ���뱾��cpu����Ĳ��ֿ�slab������
		put_cpu_partial(s, page, 1);
	}
}

//��page��Ӧ��slab�����뵽����CPU����Ĳ��ֿ�slab�����ͷ���������޸���page�ṹ��pages��pobjects�ֶΡ�
static void put_cpu_partial(struct kmem_cache *s, struct page *page, int drain)
{
	struct page *oldpage;
	int pages;
	int pobjects;

	do 
	{
		pages = 0;
		pobjects = 0;
		oldpage = this_cpu_read(s->cpu_slab->partial);

		if (oldpage) 
		{
			pobjects = oldpage->pobjects;
			pages = oldpage->pages;
			
			//������CPU���ֿ�slab��������Ķ�������������
			if (drain && pobjects > s->cpu_partial) 
			{
				unsigned long flags;
				
				local_irq_save(flags);

				//������cpu�����в��ֿ�slab�����е�����slab����������node�Ĳ��ֿ�slab�����С�
				//�������ޣ�����ȫ�յ�slab���ỹ��buddy
				unfreeze_partials(s, this_cpu_ptr(s->cpu_slab));

				local_irq_restore(flags);

				oldpage = NULL;
				pobjects = 0;
				pages = 0;
			}
		}

		pages++;
		pobjects += page->objects - page->inuse;

		page->pages = pages;
		page->pobjects = pobjects;
		page->next = oldpage;

	} while (this_cpu_cmpxchg(s->cpu_slab->partial, oldpage, page) != oldpage);
}