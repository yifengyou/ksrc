#define virt_addr_valid(kaddr) __virt_addr_valid((unsigned long) (kaddr))
bool __virt_addr_valid(unsigned long x)
{
	//��x<__START_KERNEL_mapʱ��y>x��
	//��x>=__START_KERNEL_mapʱ��x>y
	unsigned long y = x - __START_KERNEL_map;

	if (unlikely(x > y)) 
	{
		//��ʱ�¼��������x�������ַ
		x = y + phys_base;

		//���Ե�ַ�������ں�ӳ��ӳ��ķ�Χ
		if (y >= KERNEL_IMAGE_SIZE)
			return false;
	} 
	else
	{
		//__START_KERNEL_map - PAGE_OFFSET = 0x77FF80000000
		//x<0xffff8800_00000000ʱ���¼���ó���x>y��ֱ�ӷ���false
		//x>=0xffff8800_00000000ʱ,�¼���ó���x<y
		//�¼���ó���xʵ���Ͼ����������x��Ӧ�������ַ
		x = y + (__START_KERNEL_map - PAGE_OFFSET);

		//�ж������ַx�Ƿ񳬳���ǰcpu���֧�ֵ������ַ��Χ
		if ((x > y) || !phys_addr_valid(x))
			return false;
	}   

	//�����Ǹ���section����������ж�
	return pfn_valid(x >> PAGE_SHIFT);
}
#define KERNEL_IMAGE_SIZE (512 * 1024 * 1024)

//�ж������ַaddr�Ƿ񳬳���ǰcpu���֧�ֵ������ַ��Χ
static inline int phys_addr_valid(resource_size_t addr)
{
	//x86_phys_bits������ǰcpu�������ַλ����������ϲ��Ϊ40
	return !(addr >> boot_cpu_data.x86_phys_bits);
}

static inline int pfn_valid(unsigned long pfn)
{
	//pfn��Ӧ��section���Ƿ񳬹����section��
	if (pfn_to_section_nr(pfn) >= NR_MEM_SECTIONS)
		return 0;

	return valid_section(__nr_to_section(pfn_to_section_nr(pfn)));
}

//���ж�section���Լ�SECTION_HAS_MEM_MAPλ��bit1���Ƿ������ж��Ƿ���Ч
static inline int valid_section(struct mem_section *section)
{
	return (section && (section->section_mem_map & SECTION_HAS_MEM_MAP));
}