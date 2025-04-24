//struct page����С��0x40
struct page 
{
	unsigned long flags;

	union 
	{
		//����ҳ���ٻ����������ʹ��
		struct address_space *mapping;  /* If low bit clear, points to
										* inode address_space, or NULL.
										* If page mapped as anonymous
										* memory, low bit is set, and
										* it points to anon_vma object:
										* see PAGE_MAPPING_ANON below.
										*/
		void *s_mem;/* slab first object *///������slab����slub����ʾslab�е�һ������ĵ�ַ�������Ƿ����ڱ�ʹ��
	};  

	struct 
	{
		union 
		{
			//����ǰpage�ṹ��Ӧ��ҳ����pgd����index����pgd���������̵�mm�ṹ��ָ��
			pgoff_t index;          /* Our offset within mapping. */
			void *freelist;         /* sl[aou]b first free object *///slub�е�һ�����ж���ĵ�ַ������slab�ǻslab�ģ���ֵΪ0�������������ָ��slab�еĵ�һ�����ж���
			bool pfmemalloc;        /* If set by the page allocator,
									* ALLOC_NO_WATERMARKS was set
									* and the low watermark was not
									* met implying that the system
									* is under some pressure. The
									* caller should try ensure
									* this page is only used to
									* free other pages.
									*/
		};  

		union 
		{
			/* Used for cmpxchg_double in slub */
			unsigned long counters;

			struct 
			{
				union 
				{
					/* Count of ptes mapped in mms, to show when page is mapped & limit reverse map searches.
					Used also for tail pages refcounting instead of _count. Tail pages cannot be mapped 
					and keeping the tail page _count zero at all times guarantees get_page_unless_zero() 
					will never succeed on tail pages. 
					ҳ����ҳ������Ŀ
					*/
					atomic_t _mapcount;

					struct 
					{ /* SLUB */
						unsigned inuse:16;//����������ʹ�õĶ������
						unsigned objects:15;//һ��slab�����ֳɵĶ��������������Ķ�����Ŀ
						unsigned frozen:1;//��slabλ�ڱ���cpu����ʱ����Ϊ�slab��λ�ڱ���cpu���沿�ֿ�slab����ʱ�������ڶ�ס״̬�����ֶ�Ϊ1
					};

					int units;      /* SLOB *///slob�п��е�Ԫ�ĸ�����4Kҳ�棬һ����Ԫ�Ĵ�С��2�ֽڣ�64K����ҳ���Сʱ��һ����Ԫ�Ĵ�С��4�ֽ�
				};
				//ҳ���ü����������ֶ�Ϊ-1�����ʾ��Ӧ��ҳ����С�
				//����page_count�����ֶΣ�����0��ʾҳ�����
				atomic_t _count;    /* Usage count, see below. */
			};

			unsigned int active;    /* SLAB *///��Ե���slab����slub�����ֶ���slab�е�һ�����ж�����±�
		};
	};

	union 
	{
		//��buddyϵͳ�У����ڹ��ɿ���ҳ������
		//����ǰ������ҳ����pgd�����ʱlru�ֶ��������pgd_list����
		//������slub�������У�node�Ĳ��ֿ�slab����node��ȫ��slab����
		struct list_head lru;   // Pageout list, eg. active_list protected by zone->lru_lock !

		struct 
		{                
			/* slub per cpu partial pages */
			//���ڱ���CPU�����еĲ��ֿ�slab����
			struct page *next;      /* Next partial slab *///���ɱ���CPU�����в��ֿ�slab�����ָ��
			int pages;      /* Nr of partial slabs left *///����CPU�����в��ֿ�slab����������slab������ֻ������ͷ��page�ṹ�ĸ��ֶβ�������
			int pobjects;   /* Approximate # of objects */
		};

		struct list_head list;  /* slobs list of pages *///������������slob����֮һ
		struct slab *slab_page; /* slab fields */
		struct rcu_head rcu_head;       // Used by SLAB when destroying via RCU

		pgtable_t pmd_huge_pte; /* protected by page->ptl */
	};

	union 
	{
		//��page�ṹλ��buddyϵͳ��������ʱ������page�ṹΪĳ������ҳ���ĵ�һ��ҳ���page�ṹ����private�ֶδ��order�����ÿ���ҳ���Ĵ�СΪ2��order�η���
		unsigned long private; /* Mapping-private opaque data: usually used for buffer_heads
							   * if PagePrivate set; used for swp_entry_t if PageSwapCache;
							   * indicates order in the buddy system if PG_buddy is set.*/
		spinlock_t ptl;

		struct kmem_cache *slab_cache;  /* SL[AU]B: Pointer to slab *///slub�еĵ�һ��page��page�ṹ��slab_cache�ֶΣ���ָ��slub�����Ļ���������kmem_cache�ṹ
		struct page *first_page;        /* Compound tail pages *///����ҳ��ʱ������__GFP_COMP��־��������budyyϵͳ����һ��ҳ���ʱ����ҳ�������зǵ�һ��page�ṹ�ĸ��ֶζ�ָ���һ��page�ṹ
	};

	/*
	* On machines where all RAM is mapped into kernel address space,
	* we can simply calculate the virtual address. On machines with
	* highmem some memory is mapped into kernel virtual memory
	* dynamically, so we need a place to store that address.
	* Note that this field could be 16 bits on x86 ... ;)
	*
	* Architectures with slow multiplication can define
	* WANT_PAGE_VIRTUAL in asm/page.h
	*/
	//ҳ���Ӧ���ں����Ե�ַ
	void *virtual;/* Kernel virtual address (NULL if not kmapped, ie. highmem) */
};