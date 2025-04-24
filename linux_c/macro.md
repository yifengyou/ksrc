# 宏定义

## 常见kernel宏

### EXPORT_SYMBOL

```c
/* For every exported symbol, place a struct in the __ksymtab section */
#define ___EXPORT_SYMBOL(sym, sec)					\
	extern typeof(sym) sym;						\
	__CRC_SYMBOL(sym, sec)						\
	static const char __kstrtab_##sym[]				\
	__attribute__((section("__ksymtab_strings"), used, aligned(1)))	\
	= #sym;								\
	__KSYMTAB_ENTRY(sym, sec)



#if defined(__DISABLE_EXPORTS)

/*
 * Allow symbol exports to be disabled completely so that C code may
 * be reused in other execution contexts such as the UEFI stub or the
 * decompressor.
 */
#define __EXPORT_SYMBOL(sym, sec)

#elif defined(__KSYM_DEPS__)

/*
 * For fine grained build dependencies, we want to tell the build system
 * about each possible exported symbol even if they're not actually exported.
 * We use a string pattern that is unlikely to be valid code that the build
 * system filters out from the preprocessor output (see ksym_dep_filter
 * in scripts/Kbuild.include).
 */
#define __EXPORT_SYMBOL(sym, sec)	=== __KSYM_##sym ===

#elif defined(CONFIG_TRIM_UNUSED_KSYMS)

#include <generated/autoksyms.h>

#define __EXPORT_SYMBOL(sym, sec)				\
	__cond_export_sym(sym, sec, __is_defined(__KSYM_##sym))
#define __cond_export_sym(sym, sec, conf)			\
	___cond_export_sym(sym, sec, conf)
#define ___cond_export_sym(sym, sec, enabled)			\
	__cond_export_sym_##enabled(sym, sec)
#define __cond_export_sym_1(sym, sec) ___EXPORT_SYMBOL(sym, sec)
#define __cond_export_sym_0(sym, sec) /* nothing */

#else
#define __EXPORT_SYMBOL ___EXPORT_SYMBOL
#endif

#define EXPORT_SYMBOL(sym)					\
	__EXPORT_SYMBOL(sym, "")

#define EXPORT_SYMBOL_GPL(sym)					\
	__EXPORT_SYMBOL(sym, "_gpl")

#define EXPORT_SYMBOL_GPL_FUTURE(sym)				\
	__EXPORT_SYMBOL(sym, "_gpl_future")
```


导出符号放在特定节，提供符号地址信息，这样便能提供给ko模块调用访问

```shell
[root@gc ~]# objdump -j __ksymtab -t -x /usr/lib/debug/lib/modules/4.19.90-2102.2.0.0062.ctl2.x86_64/vmlinux   | more

/usr/lib/debug/lib/modules/4.19.90-2102.2.0.0062.ctl2.x86_64/vmlinux:     file format elf64-x86-64
/usr/lib/debug/lib/modules/4.19.90-2102.2.0.0062.ctl2.x86_64/vmlinux
architecture: i386:x86-64, flags 0x00000113:
HAS_RELOC, EXEC_P, HAS_SYMS, D_PAGED
start address 0x0000000001000000

Program Header:
    LOAD off    0x0000000000200000 vaddr 0xffffffff81000000 paddr 0x0000000001000000 align 2**21
         filesz 0x00000000011a3000 memsz 0x00000000011a3000 flags r-x
    LOAD off    0x0000000001400000 vaddr 0xffffffff82200000 paddr 0x0000000002200000 align 2**21
         filesz 0x00000000005d8000 memsz 0x00000000005d8000 flags rw-
    LOAD off    0x0000000001a00000 vaddr 0x0000000000000000 paddr 0x00000000027d8000 align 2**21
         filesz 0x0000000000025000 memsz 0x0000000000025000 flags rw-
    LOAD off    0x0000000001bfd000 vaddr 0xffffffff827fd000 paddr 0x00000000027fd000 align 2**21
         filesz 0x0000000000236000 memsz 0x000000000082f000 flags rwx
    NOTE off    0x0000000000e031d4 vaddr 0xffffffff81c031d4 paddr 0x0000000001c031d4 align 2**2
         filesz 0x00000000000001bc memsz 0x00000000000001bc flags ---

Sections:
Idx Name          Size      VMA               LMA               File off  Algn
  5 __ksymtab     00009448  ffffffff82157c10  0000000002157c10  01357c10  2**3
                  CONTENTS, ALLOC, LOAD, RELOC, READONLY, DATA
SYMBOL TABLE:
ffffffff82157c10 l    d  __ksymtab	0000000000000000 __ksymtab
ffffffff8215de50 l       __ksymtab	0000000000000000 __ksymtab_pgdir_shift
ffffffff8215e3b0 l       __ksymtab	0000000000000000 __ksymtab_ptrs_per_p4d
ffffffff8215d6a0 l       __ksymtab	0000000000000000 __ksymtab_page_offset_base
ffffffff82160968 l       __ksymtab	0000000000000000 __ksymtab_vmalloc_base
ffffffff821609a8 l       __ksymtab	0000000000000000 __ksymtab_vmemmap_base
ffffffff8215fc50 l       __ksymtab	0000000000000000 __ksymtab_system_state
ffffffff8215e978 l       __ksymtab	0000000000000000 __ksymtab_reset_devices
ffffffff8215c7f8 l       __ksymtab	0000000000000000 __ksymtab_loops_per_jiffy
ffffffff8215bbb0 l       __ksymtab	0000000000000000 __ksymtab_init_task
ffffffff8215b388 l       __ksymtab	0000000000000000 __ksymtab_get_ibs_caps
ffffffff82160ba8 l       __ksymtab	0000000000000000 __ksymtab_xen_vcpu_id
ffffffff8215c810 l       __ksymtab	0000000000000000 __ksymtab_machine_to_phys_mapping
ffffffff8215c818 l       __ksymtab	0000000000000000 __ksymtab_machine_to_phys_nr
ffffffff82160ba0 l       __ksymtab	0000000000000000 __ksymtab_xen_start_flags
ffffffff82160b78 l       __ksymtab	0000000000000000 __ksymtab_xen_arch_register_cpu
ffffffff82160b80 l       __ksymtab	0000000000000000 __ksymtab_xen_arch_unregister_cpu
ffffffff8215c198 l       __ksymtab	0000000000000000 __ksymtab_irq_stat
ffffffff8215c168 l       __ksymtab	0000000000000000 __ksymtab_irq_regs
ffffffff8215e2f0 l       __ksymtab	0000000000000000 __ksymtab_profile_pc
ffffffff821584e0 l       __ksymtab	0000000000000000 __ksymtab___register_nmi_handler

```