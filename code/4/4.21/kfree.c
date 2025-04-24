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

//�õ�x����slab�ĵ�һ��ҳ���page�ṹ
static inline struct page *virt_to_head_page(const void *x)
{
	//�õ�����x����Ӧ��page�ṹ
	struct page *page = virt_to_page(x);

	//�õ�x����slab�ĵ�һ��ҳ���page�ṹ
	return compound_head(page);
}

//���page����ҳ���ĵ�һ��ҳ���page�ṹ
static inline struct page *compound_head(struct page *page)
{
	//page�ṹ�Ƿ�������PG_tailλ
	if (unlikely(PageTail(page)))
		return page->first_page;

	return page;
}

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
		//ʹ���ͷŶ���ı�����һ�����ж����ֶΣ�ָ��freelist�ĵ�һ�����ж���
		set_freepointer(s, object, c->freelist);

		if (unlikely(!this_cpu_cmpxchg_double(
				s->cpu_slab->freelist, s->cpu_slab->tid,
				c->freelist, tid,
				object, next_tid(tid)))) 
		{
			goto redo;
		}
	} 
	else
		__slab_free(s, page, x, addr);//��Էǻslab�ڵĶ����ͷ�
}