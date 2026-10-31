#include "memory_management.h"
#include "rbtree_ftns.h"
#include <linux/compat.h>

#if 0
unsigned long get_mmap_base(struct mm_struct *mm, int is_legacy)
{
#ifdef CONFIG_HAVE_ARCH_COMPAT_MMAP_BASES
	if (in_compat_syscall()) {
		return is_legacy ? mm->mmap_compat_legacy_base
				 : mm->mmap_compat_base;
	}
#endif
	return is_legacy ? mm->mmap_legacy_base : mm->mmap_base;
}

static void find_start_end(struct mm_struct *mm,
        unsigned long addr, unsigned long flags,
		unsigned long *begin, unsigned long *end)
{
	if (!in_compat_syscall() && (flags & MAP_32BIT)) {
		/* This is usually used needed to map code in small
		   model, so it needs to be in the first 31bit. Limit
		   it to that.  This means we need to move the
		   unmapped base down for this case. This can give
		   conflicts with the heap, but we assume that glibc
		   malloc knows how to fall back to mmap. Give it 1GB
		   of playground for now. -AK */
		*begin = 0x40000000;
		*end = 0x80000000;
		if (current->flags & PF_RANDOMIZE) {
			*begin = randomize_page(*begin, 0x02000000);
		}
		return;
	}

	*begin	= get_mmap_base(mm, 1);
	if (in_compat_syscall())
		*end = task_size_32bit();
	else
		*end = task_size_64bit(addr > DEFAULT_MAP_WINDOW);
}
#endif

unsigned long
mn_arch_get_unmapped_area(struct task_struct *tsk, struct file *filp, 
		unsigned long addr,	unsigned long len, unsigned long pgoff, 
		unsigned long flags)
{
	struct mm_struct *mm = tsk->mm;
	struct vm_area_struct *vma;
	struct vm_unmapped_area_info info;
	unsigned long begin, end;



	if (IS_ERR_VALUE(addr))
		return addr;

	if (flags & MAP_FIXED)
		return addr;


    begin = mm->mmap_legacy_base;
    end = TASK_SIZE;	//ALWAYS 64-bit 

	if (len > end)
		return -ENOMEM;

	if (addr) {
		addr = PAGE_ALIGN(addr);
		vma = mn_find_vma(mm, addr);
		if (end - len >= addr &&
		    (!vma || addr + len <= vm_start_gap(vma)))  // linux/mm.h
			return addr;
	}

	info.flags = 0;
	info.length = len;
	info.low_limit = begin;
	info.high_limit = end;
	info.align_mask = 0;
	info.align_offset = pgoff << PAGE_SHIFT;




	return mn_vm_unmapped_area(tsk, &info);		//linux/mm.h
}

static bool mn_mmap_address_hint_valid(unsigned long addr, unsigned long len)
{
	if (TASK_SIZE - len < addr)
		return false;

	return (addr > DEFAULT_MAP_WINDOW) == (addr + len > DEFAULT_MAP_WINDOW);
}

unsigned long
mn_arch_get_unmapped_area_topdown(struct task_struct *tsk, struct file *filp, 
		const unsigned long addr0, const unsigned long len, const unsigned long pgoff,
		const unsigned long flags)
{
	struct vm_area_struct *vma;
	struct mm_struct *mm = tsk->mm;
	unsigned long addr = addr0;
	struct vm_unmapped_area_info info;



	if (IS_ERR_VALUE(addr))
		return addr;

	/* requested length too big for entire address space */
	if (len > TASK_SIZE)
		return -ENOMEM;

	/* No address checking. See comment at mmap_address_hint_valid() */
	if (flags & MAP_FIXED)
		return addr;





	// pr_info("mn_arch_get_unmapped_area_topdown: addr: 0x%lx, len: 0x%lx, flags: 0x%lx\n",
	// 		addr, len, flags);

	/* requesting a specific address */
	if (addr) {
		addr &= PAGE_MASK;
		if (!mn_mmap_address_hint_valid(addr, len))
			goto get_unmapped_area;

		vma = mn_find_vma(mm, addr);
		if (!vma || addr + len <= vm_start_gap(vma))  // linux/mm.h
		{
			// empharically not used
			// pr_info("Use the address: 0x%lx, gap to next: 0x%lx\n",
			// 			addr, vm_start_gap(vma));
			return addr;
		}
	}
get_unmapped_area:




	info.flags = VM_UNMAPPED_AREA_TOPDOWN;
	// }
	info.length = len;
	info.low_limit = PAGE_SIZE;

    info.high_limit = mm->mmap_base;

	/*
	 * If hint address is above DEFAULT_MAP_WINDOW, look for unmapped area
	 * in the full address space.
	 *
	 * !in_compat_syscall() check to avoid high addresses for x32.
	 */

    if (addr > DEFAULT_MAP_WINDOW)  // arch/x86/include/asm/processor.h
		info.high_limit += TASK_SIZE_MAX - DEFAULT_MAP_WINDOW;

	info.align_mask = 0;
	info.align_offset = pgoff << PAGE_SHIFT;





	addr = mn_vm_unmapped_area(tsk, &info);
	if (!(addr & ~PAGE_MASK))
		return addr;
	VM_BUG_ON(addr != -ENOMEM);

// bottomup:
	/*
	 * A failed mmap() very likely causes application failure,
	 * so fall back to the bottom-up function here. This scenario
	 * can happen with large stack limits and large mmap()
	 * allocations.
	 */
	return mn_arch_get_unmapped_area(tsk, filp, addr0, len, pgoff, flags);
}


