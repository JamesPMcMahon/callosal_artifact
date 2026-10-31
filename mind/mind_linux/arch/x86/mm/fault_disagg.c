// PARTIALLY DUPLICATED FROM original fault.c //
#include <linux/sched.h>		/* test_thread_flag(), ...	*/
#include <linux/sched/task_stack.h>	/* task_stack_*(), ...		*/
#include <linux/kdebug.h>		/* oops_begin/end, ...		*/
#include <linux/extable.h>		/* search_exception_tables	*/
#include <linux/bootmem.h>		/* max_low_pfn			*/
#include <linux/kprobes.h>		/* NOKPROBE_SYMBOL, ...		*/
#include <linux/mmiotrace.h>		/* kmmio_handler, ...		*/
#include <linux/perf_event.h>		/* perf_sw_event		*/
#include <linux/hugetlb.h>		/* hstate_index_to_shift	*/
#include <linux/prefetch.h>		/* prefetchw			*/
#include <linux/context_tracking.h>	/* exception_enter(), ...	*/
#include <linux/uaccess.h>		/* faulthandler_disabled()	*/
#include <linux/mm.h>
#include <linux/oom.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/delay.h>
#include <linux/mmu_notifier.h>	/* ptep_clear_flush_notify ... */
#include <linux/random.h>

#include <linux/smp.h>

#include <asm/cpufeature.h>		/* boot_cpu_has, ...		*/
#include <asm/traps.h>			/* dotraplinkage, ...		*/
#include <asm/pgalloc.h>		/* pgd_*(), ...			*/
#include <asm/fixmap.h>			/* VSYSCALL_ADDR		*/
#include <asm/vsyscall.h>		/* emulate_vsyscall		*/
#include <asm/vm86.h>			/* struct vm86			*/
#include <asm/mmu_context.h>		/* vma_pkey()			*/
#include <asm-generic/memory_model.h>




#include "mm_internal.h"
#include <disagg/config.h>
#include <disagg/network_disagg.h>
#include <disagg/network_fit_disagg.h>
#include <disagg/fault_disagg.h>
#include <disagg/exec_disagg.h>
#include <disagg/cnthread_disagg.h>
#include <disagg/kshmem_disagg.h>
#include <disagg/print_disagg.h>
#include <disagg/profile_points_disagg.h>
#include <disagg/futex_disagg.h>
#include <disagg/range_lock_disagg.h>

extern noinline void    // was static
bad_area_nosemaphore(struct pt_regs *regs, unsigned long error_code,
		     unsigned long address, u32 *pkey);

extern noinline void    // was static
bad_area(struct pt_regs *regs, unsigned long error_code, unsigned long address);

extern noinline void    // was static
bad_area_access_error(struct pt_regs *regs, unsigned long error_code,
		      unsigned long address, struct vm_area_struct *vma);

extern inline int    // was static
access_error(unsigned long error_code, struct vm_area_struct *vma);

extern noinline void    // was static
no_context(struct pt_regs *regs, unsigned long error_code,
	   unsigned long address, int signal, int si_code);

extern noinline void    // was static
mm_fault_error(struct pt_regs *regs, unsigned long error_code,
	       unsigned long address, u32 *pkey, unsigned int fault);

extern void DEBUG_print_vma(struct mm_struct *mm);

DEFINE_PROFILE_POINT(FH_fault_handler)
DEFINE_PROFILE_POINT(FH_fault_handler_tot_user)
DEFINE_PROFILE_POINT(FH_fault_handler_tot_kern)
DEFINE_PROFILE_POINT(FH_fault_handler_tot_test_futex_messaging)
DEFINE_PROFILE_POINT(FH_fault_handler_tot_test_mem_acc)
DEFINE_PROFILE_POINT(FH_fault_handler_tot_test_lock)
DEFINE_PROFILE_POINT(FH_fault_handler_abort_user)
DEFINE_PROFILE_POINT(FH_fault_handler_abort_kern)
DEFINE_PROFILE_POINT(FH_fault_handler_abort_test_futex_messaging)
DEFINE_PROFILE_POINT(FH_fault_handler_abort_test_mem_acc)
DEFINE_PROFILE_POINT(FH_fault_handler_abort_test_lock)
DEFINE_PROFILE_POINT_EVAL(FH_fetch_remote_tot)
DEFINE_PROFILE_POINT(FH_pte_lock)
DEFINE_PROFILE_POINT(FH_prepare_data)
DEFINE_PROFILE_POINT(FH_restore_data)
DEFINE_PROFILE_POINT(FH_add_waiting_node)
DEFINE_PROFILE_POINT_EVAL(FH_ack_waiting_node)
DEFINE_PROFILE_POINT(FH_ownership_optimization)
DEFINE_PROFILE_POINT(FH_pre_process)
DEFINE_PROFILE_POINT(FH_find_pte)
DEFINE_PROFILE_POINT(FH_DATA_update_cache)
DEFINE_PROFILE_POINT(FH_DATA_pte)
DEFINE_PROFILE_POINT(FH_DATA_copy)
DEFINE_PROFILE_POINT(FH_nack_and_retry)
DEFINE_PROFILE_POINT(FH_nack_and_retry_delay)
DEFINE_PROFILE_POINT(FH_nack_for_restore)
DEFINE_PROFILE_POINT(FH_nack_for_inval)
DEFINE_PROFILE_POINT(FH_nack_for_inval_w)
DEFINE_PROFILE_POINT(FH_DATA_rw_copy)
DEFINE_PROFILE_POINT(FH_latency_10us)
DEFINE_PROFILE_POINT(FH_latency_50us)
DEFINE_PROFILE_POINT(FH_latency_100us)
DEFINE_PROFILE_POINT(FH_latency_250us)
DEFINE_PROFILE_POINT(FH_latency_500us)
DEFINE_PROFILE_POINT(FH_latency_1000us)
DEFINE_PROFILE_POINT(FH_warn_net_lat)
DEFINE_PROFILE_POINT(FH_warn_wait_ack)
DEFINE_PROFILE_POINT(FH_succ_net_lat)
DEFINE_PROFILE_POINT(FH_shared_net_lat)
DEFINE_PROFILE_POINT(FH_modified_net_lat)

#ifdef MIND_VERIFY_PAGE_CHKSUM
static void print_page_checksum(void *data_ptr, unsigned long addr, unsigned long pfn, unsigned long ret_dma_addr, unsigned long dma_addr, struct cnthread_page *cnpage)
{
	unsigned long checksum = 0, *itr;
	for (itr = data_ptr; (char *)itr != ((char *)data_ptr + PAGE_SIZE); ++itr)
		checksum += *itr;
	pr_pgfault("pfault addr[%lx] checksum[%lx] pfn[%lx] retdmaaddr[%lx] dmaaddr[%lx] cnreq[%llx]\n", addr, checksum, pfn, ret_dma_addr, dma_addr, (u64)cnpage);
}
#endif

static int is_kern_ip(u64 ip) {
	return ip > 0xffffffffffff ? 1 : 0;
}

//__always_inline static int send_pfault_to_mn(
__always_inline int send_pfault_to_mn(
		struct task_struct *tsk, unsigned long error_code,
		unsigned long address, int reset_req,
		struct fault_reply_struct *reply)
{
	PROFILE_POINT_TIME_EVAL(FH_fetch_remote_tot)
	struct fault_msg_struct payload;
	int res = -1;
	u32 tot_size = sizeof(struct fault_reply_struct);
	PROFILE_START_EVAL(FH_fetch_remote_tot);

	payload.tgid = is_kshmem_address(address) ? DISAGG_KERN_TGID : tsk->tgid;

	payload.address = address;
	payload.error_code = error_code;
	payload.flags = reset_req;



	res = send_msg_to_memory_rdma(DISSAGG_PFAULT, &payload, sizeof(payload),
								  reply, tot_size);

	pr_pgfault("\tsend tgid[%d] addr[%lx] errcode[%lx] flags[%x]; recv ret[%d]",
			   is_kshmem_address(address) ? DISAGG_KERN_TGID : tsk->tgid,
			   address, error_code, reset_req, (reply)->ret);

	if(likely(res >= 0))
	{
		res = (reply)->ret;
	}
	PROFILE_LEAVE_EVAL(FH_fetch_remote_tot);
	return res;
}
EXPORT_SYMBOL(send_pfault_to_mn);

struct mm_struct * get_init_mm(){
      return &init_mm;
}
EXPORT_SYMBOL(get_init_mm);

static pte_t *find_pte_from_reg(unsigned long address)
{
	pgd_t *pgd;
	unsigned int level;

	pgd = __va(read_cr3_pa());
	pgd += pgd_index(address);
	return lookup_address_in_pgd(pgd, address, &level);
}

static pte_t *find_pte_from_mm(struct mm_struct *mm, unsigned long address)
{
	pgd_t *pgd;
	unsigned int level;
	if (mm)
	{
		pgd = pgd_offset(mm, address);
		return lookup_address_in_pgd(pgd, address, &level);
	}else{
		return NULL;
	}
}

void print_pgfault_error(struct task_struct *tsk, unsigned long error_code, 
	unsigned long address, struct vm_area_struct *vma, int is_prefetch)
{
	// pte_t *pte = find_pte_from_reg(address);
	pte_t *pte = NULL;
	if (!is_prefetch)
		pte = find_pte_from_mm(is_kshmem_address(address) ? &init_mm : tsk->mm, address);

	printk(KERN_DEFAULT "CN: fault handler - (tgid: %d, pid: %d, W: %d, pR/W: %d/%d, an: %d, pte_fl: 0x%03lx, pfn: 0x%06lx, err: 0x%02lx) - addr: 0x%lx, vma: 0x%lx - 0x%lx [0x%lx], st[0x%lx]\n",
		(int)tsk->tgid, (int)tsk->pid,
		(error_code & X86_PF_WRITE) ? 1 : 0, // 0 means read
		vma ? ((vma->vm_flags & VM_READ) ? 1 : 0) : -1,
		vma ? ((vma->vm_flags & VM_WRITE) ? 1 : 0) : -1,
		vma ? (vma_is_anonymous(vma) ? 1 : 0) : -1,
		pte ? ((unsigned long)pte_flags(*pte) & 0xfff) : 0xfff,
		pte ? ((unsigned long)pte->pte >> PAGE_SHIFT) & 0xffffff : 0x000000,
		error_code, address,
		vma ? vma->vm_start : 0,
		vma ? vma->vm_end : 0,
		vma ? vma->vm_flags : 0,
		tsk->mm ? tsk->mm->start_stack : 0);
}

static pte_t *find_pte(unsigned long address)
{
	unsigned int level;
	return lookup_address(address, &level);
}

static int bad_address(void *p)
{
	unsigned long dummy;
	return probe_kernel_address((unsigned long *)p, dummy);
}

pte_t *find_pte_target(struct mm_struct *mm, unsigned long address)
{
	pgd_t *pgd = pgd_offset(mm, address);
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte = NULL;

	if (!pgd || (bad_address(pgd)) || pgd_none(*pgd))
		return NULL;
	p4d = p4d_offset(pgd, address);
	barrier();

	if (!p4d || (bad_address(p4d)) || p4d_none(*p4d))
		return NULL;
	pud = pud_offset(p4d, address);
	barrier();

	if (!pud || (bad_address(pud)) || pud_none(*pud))
		return NULL;
	pmd = pmd_offset(pud, address);
	barrier();

	if (pmd && likely(!bad_address(pmd)) && !pmd_none(*pmd))
	{
		pte = pte_offset_map(pmd, address);
		barrier();
		if ((bad_address(pte)))
			return NULL;
	}
	return pte;
}

/* We assume that it will be called only during the page fault handling */
pte_t *find_pte_target_lock(struct mm_struct *mm, unsigned long address, spinlock_t **ptl_ptr)
{
	pgd_t *pgd = pgd_offset(mm, address);
	p4d_t *p4d = NULL;
	pud_t *pud = NULL;
	pmd_t *pmd = NULL;
	pte_t *pte = NULL;
	*ptl_ptr = NULL;
	barrier();

	if (!pgd || (bad_address(pgd)) || pgd_none(*pgd))
		return NULL;
	p4d = p4d_offset(pgd, address);
	barrier();
	
	if (!p4d || (bad_address(p4d)) || p4d_none(*p4d))
		return NULL;
	pud = pud_offset(p4d, address);
	barrier();

	if (!pud || (bad_address(pud)) || pud_none(*pud))
		return NULL;
	pmd = pmd_offset(pud, address);
	barrier();
	
	if (pmd && likely(!bad_address(pmd)) && !pmd_none(*pmd))
	{
		pte = pte_offset_map(pmd, address);
		barrier();
		if ((bad_address(pte)))
			return NULL;

		*ptl_ptr = pte_lockptr(mm, pmd);
	}

	return pte;
}

__always_inline pte_t *ensure_pte(struct mm_struct *mm, unsigned long address,
	spinlock_t **ptl_ptr)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	// 1) Find pmd
	// See how to allocate pgd, pud, pmd @ memory.c / __handle_mm_fault()
	pgd = pgd_offset(mm, address);
	p4d = p4d_alloc(mm, pgd, address);
	if (!p4d)
		return NULL;

	pud = pud_alloc(mm, p4d, address);
	barrier();
	if (!pud)
		return NULL;
	pmd = pmd_alloc(mm, pud, address);
	barrier();
	if (!pmd)
		return NULL;

	// 2) Check pte
	// Maybe there is pte but not presented
	pte = find_pte(address);
	// Now we need page aligned address
	address &= PAGE_MASK;
	if (pte){
		goto return_pte;
	}
	
	// 3) Allocate pte
	if (pte_alloc(mm, pmd, address)){
		return NULL;
	}

return_pte:

	pte = pte_offset_map(pmd, address);

	*ptl_ptr = pte_lockptr(mm, pmd);
	return pte;
}
EXPORT_SYMBOL(ensure_pte);

extern int find_vma_links(struct mm_struct *mm, unsigned long addr,
		unsigned long end, struct vm_area_struct **pprev,
		struct rb_node ***rb_link, struct rb_node **rb_parent);

static //__always_inline
void *prepare_data_page(
	unsigned int tgid, struct mm_struct *mm, pte_t *entry,
	unsigned long address,
	unsigned long vm_flags,
	struct vm_area_struct **_vma,
	struct cnthread_page **new_cnpage_ptr,
	int *existing_page,
	int *hold_vma_rlock_when_return)
{
	struct page *page = NULL;
	struct vm_area_struct *vma = *_vma;
	struct cnthread_page *cnpage = NULL;
	void *new_data = NULL;
	unsigned long vm_start, vm_end;

	PROFILE_POINT_TIME(FH_DATA_copy)
	// 1) try to get existing pte
	// There is pte but it can be un-presented
	address &= PAGE_MASK;
	vm_start = address;
	vm_end = vm_start + PAGE_SIZE;

	if (!vma && tgid != DISAGG_KERN_TGID)
	{
		struct vm_area_struct *prev;
		struct rb_node **rb_link, *rb_parent;

		if (unlikely(vm_start <= 0 || vm_end <= vm_start))
		{
			printk(KERN_ERR "VMA ERROR: 0x%lx - 0x%lx\n", vm_start, vm_end);

			BUG();

		}

		// Adjust existing VMAs to be not overlapping
		if (unlikely(find_vma_links(mm, vm_start, vm_end, &prev, &rb_link, &rb_parent)))
		{	
			// Find approporiate size: we asseme that there is no overlapping VMA
			vma = find_vma(mm, address);
			if (likely(vma))
			{
				if (vma->vm_prev && vma->vm_prev->vm_end > vm_start)
				{
					vm_start = vma->vm_prev->vm_end;
				}
				if (vma->vm_start < vm_end)
				{
					vm_end = vma->vm_start;
				}
			}

			// Check whether the adjustment solved the problem
			if (find_vma_links(mm, vm_start, vm_end, &prev, &rb_link, &rb_parent))
			{
				printk(KERN_DEFAULT "(-1) VMA is overlapping: 0x%lx - 0x%lx [0x%lx - 0x%lx]\n",
					   vm_start, vm_end, vma ? vma->vm_start : 0, vma ? vma->vm_end : 0);
				BUG(); // overlapping VMA detected
			}
		}
		up_read(&mm->mmap_sem);
		barrier();
		pr_info_ratelimited("** VMA is needed [0x%lx]: 0x%lx - 0x%lx\n", address, vm_start, vm_end);

		pr_locks(KERN_DEFAULT "Try down_write mm->mmap_sem | addr[0x%lx]\n", address);

		if (unlikely(!down_write_trylock(&mm->mmap_sem)))
		{

			if (address == 0x1000)
				BUG();
			if (!down_read_trylock(&mm->mmap_sem)) {
				*hold_vma_rlock_when_return = 0;
			} else {
				*hold_vma_rlock_when_return = 1;
			}
			pr_locks(KERN_DEFAULT "Fail down_write mm->mmap_sem | addr[0x%lx]\n", address);
			goto under_eviction;
		}

		pr_pgfault(KERN_DEFAULT "down_write mm->mmap_sem\n");

		pr_locks(KERN_DEFAULT "Get down_write mm->mmap_sem | addr[0x%lx]\n", address);

		//
		if (likely(!IS_ERR_VALUE(mmap_region(NULL, vm_start, vm_end - vm_start, vm_flags, 0, NULL))))
		{
			pr_pgfault(KERN_DEFAULT "goes into mmap_region!\n");
			vma = find_vma(mm, address);
			*_vma = vma;
			if (unlikely(vma && vma->vm_start > address))
				vma = NULL;
			else
			{
				vma->vm_flags &= ~(VM_READ | VM_EXEC); // Remove permission
													   // (VM_WRITE was already removed,
													   // when prepare_data_page() was called)
			}

			if (likely(vma))
			{
				if (unlikely(anon_vma_prepare(vma)))
					BUG();

				pr_pgfault(KERN_DEFAULT "0) VMA is created [0x%lx]: 0x%lx - 0x%lx [0x%lx - 0x%lx, 0x%lx]\n",
						   address, vm_start, vm_end, vma->vm_start, vma->vm_end, vma->vm_flags);
			}
		}
		pr_pgfault(KERN_DEFAULT "downgrade_write mm->mmap_sem\n");
		downgrade_write(&mm->mmap_sem);
	}
	/*
	 *	Here we prepare page only for unpresented pte
	 */
	if (!pte_present(*entry))
	{
		pr_pgfault("\tpte not present addr: %lx, pte: %llx, pte_val: %lx, mm: %llx\n", address, (u64)entry, pte_val(*entry), (u64)mm);
		cnpage = cnthread_get_new_page(tgid, address, mm, existing_page); //&is_modified_cache);
		if (!cnpage) {
			pr_pgfault("Not present: no cnreq\n");
			goto under_eviction;
		}

		page = cnpage->kpage;

		if (unlikely(!page))
		{
			printk(KERN_DEFAULT "Cannot allocate new page at 0x%lx\n", address);
			BUG();
		}

		if (new_cnpage_ptr)
			*new_cnpage_ptr = cnpage;

		new_data = cnpage->kmap;
	}
	else if (pte_present(*entry) && !pte_write(*entry))
	{
		pr_pgfault("\tpte present & not writable addr: %lx, pte: %llx, pte_val: %lx, mm: %llx\n", address, (u64)entry, pte_val(*entry), (u64)mm);
		cnpage = cnthread_get_page(tgid, address, mm, existing_page); // get existing or new
		if (!cnpage)
			goto under_eviction;

		if (new_cnpage_ptr)
			*new_cnpage_ptr = cnpage;
	}else{
		BUG();
	}
	// printk(KERN_DEFAULT "CN: (pre) fault handler - pte: 0x%p, val: 0x%lx, page: 0x%lx, addr: 0x%lx, ecode: 0x%lx, page(%d)\n",
	// 	   (void *)entry, (unsigned long)entry->pte, (unsigned long)page, address, error_code, need_new_page);
	return new_data;

under_eviction:
	pr_pgfault("\tpage[%lx] under eviction or invalidation\n", address);
	*new_cnpage_ptr = NULL;
	return NULL;
}


__always_inline int wait_ack_from_ctrl(struct cache_waiting_node *node, spinlock_t *ptl,struct rw_semaphore *mmap_sem, struct cnthread_page *cnpage)
{


	int ret = wait_until_counter(node, ptl, mmap_sem, cnpage);
	kfree(node);
	return ret;
}
EXPORT_SYMBOL(wait_ack_from_ctrl);

static __always_inline pte_t *restore_data_page(
	struct task_struct *cur_tsk, pte_t *entry,
	unsigned long address, unsigned long error_code,
	unsigned long vm_flags,
	struct vm_area_struct **_vma, unsigned long data_dma_addr,
	struct cnthread_page *cnpage,
	int existing_page, int is_prefetch)
{
	pte_t pte_val, prev_pte_val;
	struct page *page = NULL, *old_page = NULL;
	pgprot_t prot;
	struct vm_area_struct *vma = *_vma;
	void *old_data, *new_data;
	int need_new_page = 0, err = 0;
	PROFILE_POINT_TIME(FH_DATA_pte)
	PROFILE_POINT_TIME(FH_DATA_rw_copy)

	// Retrieve received data into the memory
	// 1) try to get existing pte
	// Maybe there is pte but not presented
	address &= PAGE_MASK;

	// ** THIS PART MUST BE DONE BEFORE REQUEST DATA FETCH OVER NETWORK ** //
	/*
	 *	Possible cases
	 *  1) new_page is not required (it was read-only but become write-able)
	 *  2) new_page is required and there is data to write (fetched from memory)
	 *  3) new_page is required but there is no data to write -> error
	 *  4) pte is already presented--it may already recovered by another threads -> skip
	 */
	if (pte_present(*entry) && !pte_write(*entry)) // Case 1
	{
		// Use the old page
		page = pte_page(*entry);
		PROFILE_START(FH_DATA_rw_copy);
		if (unlikely(page && atomic_read(&page->_refcount) > 3))
		{
			// Create new page if there are multiple mappings 
			// — THIS SHOULD NOT HAPPEN AS WE MANUALLY HANDLE FORK 
			//	 (only happens when copying data from non-remote process to remote process,
			//	  e.g., start the first remote process)
			if (likely(cnpage))
			{
				old_page = page;
				page = cnpage->kpage;
			}
			else
			{
				BUG();
			}

			if (unlikely(!page))
			{
				printk(KERN_DEFAULT "Cannot allocate new page at 0x%lx\n", address);
				BUG();
			}

			old_data = kmap_atomic(old_page);
			new_data = cnpage->kmap;
			memcpy(new_data, old_data, PAGE_SIZE);
			kunmap_atomic(old_data);
		}
		PROFILE_LEAVE(FH_DATA_rw_copy);
	}
	else if (!pte_present(*entry) && data_dma_addr) // Case 2
	{
		if (unlikely(!cnpage))
		{
			printk(KERN_DEFAULT "Error - cnreq was not allocated (%d) with pte: 0x%lx\n",
				   need_new_page, (unsigned long)entry->pte);
			print_pgfault_error(cur_tsk, error_code, address, vma, is_prefetch);
			BUG();
		}

		page = cnpage->kpage;
		if (unlikely(!page))
		{
			printk(KERN_DEFAULT "Error - page was not allocated (%d) with pte: 0x%lx\n",
				   need_new_page, (unsigned long)entry->pte);
			print_pgfault_error(cur_tsk, error_code, address, vma, is_prefetch);
			BUG();
		}
		need_new_page = 1;
	}else{	// Case 3: errous case
		printk(KERN_DEFAULT "Error on need_new_page (%d) with pte: 0x%lx\n",
				need_new_page, (unsigned long)entry->pte);
		print_pgfault_error(cur_tsk, error_code, address, vma, is_prefetch);
		BUG();
	}
	prev_pte_val = *entry;
	err = cnthread_add_pte_to_list_with_cnpage(entry, address, vma, cnpage, !existing_page);
	if (unlikely(err))
	{
		return NULL;
	}

	smp_wmb();
	__set_bit(PG_uptodate, &page->flags);

	// Update mm for user process
	if (!is_kshmem_address(address))
	{
		// TRY to skip flush cache (we are just adding empty page...)
		if(unlikely(check_stable_address_space(cur_tsk->mm)))
		{
			printk(KERN_DEFAULT "CN: fault handler - not stable address space\n");
			BUG();
		}

		if (need_new_page)
		{
			inc_mm_counter(cur_tsk->mm, MM_FILEPAGES);
		}
	}

	PROFILE_START(FH_DATA_pte);
	prot = vm_get_page_prot(vm_flags);
	pte_val = mk_pte(page, prot);
	pte_val = pte_set_flags(pte_val, _PAGE_PRESENT);
	if (error_code & X86_PF_WRITE)
	{
		pte_val = pte_mkwrite(pte_mkdirty(pte_val));

	}
	pte_val = pte_mkyoung(pte_val);

	if (is_kshmem_address(address))
        pte_val = pte_clear_flags(pte_val, _PAGE_USER);
	smp_wmb();
	set_pte_at_notify(is_kshmem_address(address) ? &init_mm : cur_tsk->mm, address, entry, pte_val);
	mmu_notifier_invalidate_range_only_end(is_kshmem_address(address) ? &init_mm : cur_tsk->mm,
										   address, address + PAGE_SIZE);
#ifdef MIND_VERIFY_PAGE_CHKSUM
	//print checksum
	if (page) {
		void *tmp_data = kmap_atomic(page);
		print_page_checksum(tmp_data, address, pte_pfn(pte_val), data_dma_addr, cnpage->dma_addr, cnpage);
		kunmap_atomic(tmp_data);
	} else {
		pr_err("can not find page for checksum, va: %lx\n", address);
	}
#endif

	if (old_page)
	{
		page_remove_rmap(old_page, false);
		put_page(old_page);


	}
	PROFILE_LEAVE(FH_DATA_pte); // ~ 500 ns: generate PTE, write PTE, and flush TLB
	return entry;
}

// Sync counter once per 64 page faults
#ifndef TASK_RSS_EVENTS_THRESH
#define TASK_RSS_EVENTS_THRESH	(64)
#endif

static void check_sync_rss_stat(struct task_struct *tsk)
{
	if (unlikely(tsk != current))
		return;
	if (tsk->mm && unlikely(tsk->rss_stat.events++ > TASK_RSS_EVENTS_THRESH))
		sync_mm_rss(tsk->mm);
}

static void help_invalidation_and_sleep(int back_off_cnt)
{
	unsigned long tmp_jiff = jiffies, target_wait_time = 0;
	int i = 0, cpu;
	unsigned char randval = 0;
	for (i = 0; i < DISAGG_QP_NUM_INVAL_BUF; i++)
		try_invalidation_lookahead(PROFILE_CNTHREAD_INV_ACK_SERV_FROM_PRMPT, SERVE_ACK_PER_NACK);
	cpu = get_cpu();
	put_cpu();
	// Back-off routine
	get_random_bytes(&randval, 1);
	target_wait_time = (DISAGG_NACK_RETRY_IN_USEC) + (back_off_cnt * (DISAGG_NACK_RETRY_IN_USEC * (unsigned int)randval) / 256);
	tmp_jiff = jiffies_to_usecs(jiffies - tmp_jiff);
	if ((tmp_jiff >= 0) && (tmp_jiff < DISAGG_NACK_RETRY_IN_USEC * back_off_cnt))
		udelay(DISAGG_NACK_RETRY_IN_USEC * back_off_cnt - tmp_jiff);
	else if (tmp_jiff == 0)
		udelay(DISAGG_NACK_RETRY_IN_USEC * back_off_cnt);
}

enum {
	FH_ALREADY_EVICT = 1,
	FH_NACK_NORMAL = 2,
	FH_NACK_FROM_RESTORE = 3,
	FH_ALREADY_EVICT_WAIT = 10,
};

void
_do_disagg_page_fault(struct task_struct *tsk, struct pt_regs *regs,
    unsigned long error_code, unsigned long address, unsigned int flags, int is_prefetch)
{
    struct mm_struct *mm = tsk->mm;
    struct vm_area_struct *vma;
    int fault, major = 0, reset_req = 0, cpu_id;
    u32 pkey;
	struct fault_reply_struct ret_buf;
	void *pre_alloc_data = NULL;
	int existing_page = 0;
	int try_inval_proc = 0;
	volatile int return_code = 0;
	static volatile unsigned long return_cnt = 0;
	struct cache_waiting_node *wait_node = NULL;
	unsigned int is_bad_area = 0;
	volatile unsigned long start_jiff = 0, net_jiff = 0, wait_jiff = 0;
	unsigned int back_off_cnt = 1;
	int clear_responsibility = 0;
	spinlock_t *ptl_ptr = NULL;
	struct task_struct *cur_tsk = NULL;
	int is_kern_shared_mem = 0;
	int hold_vma_rlock;
	PROFILE_POINT_TIME(FH_fault_handler)
	PROFILE_POINT_TIME(FH_fault_handler_tot_user)
	PROFILE_POINT_TIME(FH_fault_handler_tot_kern)
	PROFILE_POINT_TIME(FH_fault_handler_tot_test_futex_messaging)
	PROFILE_POINT_TIME(FH_fault_handler_tot_test_mem_acc)
	PROFILE_POINT_TIME(FH_fault_handler_tot_test_lock)
	PROFILE_POINT_TIME(FH_fault_handler_abort_user)
	PROFILE_POINT_TIME(FH_fault_handler_abort_kern)
	PROFILE_POINT_TIME(FH_fault_handler_abort_test_futex_messaging)
	PROFILE_POINT_TIME(FH_fault_handler_abort_test_mem_acc)
	PROFILE_POINT_TIME(FH_fault_handler_abort_test_lock)
	PROFILE_POINT_TIME(FH_pte_lock)
	PROFILE_POINT_TIME(FH_restore_data)
	PROFILE_POINT_TIME(FH_prepare_data)
	PROFILE_POINT_TIME(FH_add_waiting_node)
	PROFILE_POINT_TIME_EVAL(FH_ack_waiting_node)
	PROFILE_POINT_TIME(FH_nack_and_retry)
	PROFILE_POINT_TIME(FH_nack_and_retry_delay)
	PROFILE_POINT_TIME(FH_nack_for_restore)
	PROFILE_POINT_TIME(FH_nack_for_inval)
	PROFILE_POINT_TIME(FH_nack_for_inval_w)
	PROFILE_POINT_TIME(FH_ownership_optimization)
	PROFILE_POINT_TIME(FH_pre_process)
	PROFILE_POINT_TIME(FH_find_pte)
	// Latency cdf
	PROFILE_POINT_TIME(FH_latency_10us)
	PROFILE_POINT_TIME(FH_latency_50us)
	PROFILE_POINT_TIME(FH_latency_100us)
	PROFILE_POINT_TIME(FH_latency_250us)
	PROFILE_POINT_TIME(FH_latency_500us)
	PROFILE_POINT_TIME(FH_latency_1000us)
	PROFILE_POINT_TIME(FH_warn_net_lat)
	PROFILE_POINT_TIME(FH_warn_wait_ack)
	PROFILE_POINT_TIME(FH_succ_net_lat)
	PROFILE_POINT_TIME(FH_shared_net_lat)
	PROFILE_POINT_TIME(FH_modified_net_lat)
	// start total latency tracking
	PROFILE_START(FH_fault_handler_tot_user);
	PROFILE_START(FH_fault_handler_tot_kern);
	PROFILE_START(FH_fault_handler_tot_test_futex_messaging);
	PROFILE_START(FH_fault_handler_tot_test_mem_acc);
	PROFILE_START(FH_fault_handler_tot_test_lock);
	PROFILE_START(FH_fault_handler_abort_user);
	PROFILE_START(FH_fault_handler_abort_kern);
	PROFILE_START(FH_fault_handler_abort_test_futex_messaging);
	PROFILE_START(FH_fault_handler_abort_test_mem_acc);
	PROFILE_START(FH_fault_handler_abort_test_lock);

	if (tsk->is_mind_binary)
		pr_pgfault("begin pfault tgid[%d] pid[%d] ip[%lx] addr[%lx] errcode[%lx] CPU[%d]\n",
				   tsk->tgid, tsk->pid, regs->ip, address, error_code, smp_processor_id());

	if (error_code & X86_PF_USER) {
		pr_pgfault("from user space!\n");
	}
		
	// we need to use kernel's mm for kernel shared memory
	if (is_kshmem_address(address))
	{
		is_kern_shared_mem = 1;
		if (!mm)
		{
			mm = &init_mm;
		}
	}

	/*
	 * When running in the kernel we expect faults to occur only to
	 * addresses in user space.  All other faults represent errors in
	 * the kernel and should generate an OOPS.  Unfortunately, in the
	 * case of an erroneous fault occurring in a code path which already
	 * holds mmap_sem we will deadlock attempting to validate the fault
	 * against the address space.  Luckily the kernel only validly
	 * references user space from well defined areas of code, which are
	 * listed in the exceptions table.
	 *
	 * As the vast majority of faults will be valid we will only perform
	 * the source reference check when there is a possibility of a
	 * deadlock. Attempt to lock the address space, if we cannot we then
	 * validate the source. If this is invalid we can skip the address
	 * space check, thus avoiding the deadlock:
	 */
	if (unlikely(!down_read_trylock(&mm->mmap_sem)))
	{
		if (is_prefetch)
			return; // just fail here

		if (!(error_code & X86_PF_USER) &&
		    !search_exception_tables(regs->ip) && !is_kern_shared_mem) {
			bad_area_nosemaphore(regs, error_code, address, NULL);
			return;
		}
retry:
		down_read(&mm->mmap_sem);
	}
	else
	{
		/*
		* The above down_read_trylock() might have succeeded in
		* which case we'll have missed the might_sleep() from
		* down_read():
		*/
		might_sleep();
	}

	// ======= CUSTOM PAGE FAULT ROUTINE ====== //
	// Initial values
	major = 0;
	reset_req = 0;
	pre_alloc_data = NULL;
	existing_page = 0;
	try_inval_proc = 0;
	wait_node = NULL;
	is_bad_area = 0;
	start_jiff = sched_clock();
	cur_tsk = is_prefetch ? tsk : current;
	hold_vma_rlock = 1;

	PROFILE_START(FH_fault_handler);
	PROFILE_START(FH_nack_and_retry);
	PROFILE_START(FH_pre_process);
	PROFILE_START(FH_latency_10us);
	PROFILE_START(FH_latency_50us);
	PROFILE_START(FH_latency_100us);
	PROFILE_START(FH_latency_250us);
	PROFILE_START(FH_latency_500us);
	PROFILE_START(FH_latency_1000us);

	/*
	 * Try to get vma, filter out bad page fault
	 */

	if (is_kern_shared_mem)
	{
		vma = NULL;
	}else{
		pr_pgfault("find_vma: addr: 0x%lx\n", address);
		if (address == 0)
			BUG();
		vma = find_vma(mm, address); // current or next vma
		pr_pgfault("find_vma: addr: 0x%lx, vma: 0x%lx - 0x%lx\n", address, vma ? vma->vm_start : 0, vma ? vma->vm_end : 0);
	}
	fault = 0;
	if (!(error_code & X86_PF_INSTR))
	{
#ifdef PRINT_PAGE_FAULT
		print_pgfault_error(cur_tsk, error_code, address, vma, is_prefetch);
#endif
		// VMA: the first vma of which the vm_end > address
		if (!vma || (vma && (vma->vm_start > address)) // normal case (e.g., newly mapped mmaps)
			|| (vma_is_anonymous(vma) && !(vma->vm_file) &&
				// WRITE permission
				(((error_code & X86_PF_WRITE) && !(vma->vm_flags & VM_WRITE)) ||
				 // READ permission
				 (!(error_code & X86_PF_WRITE) && !(vma->vm_flags & VM_READ))))
		)
		{
			struct range_lock pgfault_lock;
			pte_t *entry, pte_value;
			int wait_err = -1;
			struct cnthread_page *new_cnpage = NULL;

			range_lock_init(&pgfault_lock, address >> PAGE_SHIFT, address >> PAGE_SHIFT);




			cache_range_lock(is_kern_shared_mem ? DISAGG_KERN_TGID : tsk->tgid, &pgfault_lock);
#if 0
			if (!cache_range_try_lock(is_kern_shared_mem ? DISAGG_KERN_TGID : tsk->tgid, &pgfault_lock))
			{
				return_code = FH_ALREADY_EVICT;
				goto return_and_retry_no_lock;
			}
#endif
			ptl_ptr = NULL;
			PROFILE_START(FH_find_pte);
			entry = ensure_pte(mm, (address & PAGE_MASK), &ptl_ptr);
			if (unlikely(!entry || !ptl_ptr))
			{
				printk(KERN_DEFAULT "CN: fault handler - cannot get pte\n");
				cache_range_unlock(is_kern_shared_mem ? DISAGG_KERN_TGID : tsk->tgid,&pgfault_lock);
				up_read(&mm->mmap_sem);
				BUG();
			}
			PROFILE_LEAVE(FH_find_pte);
			PROFILE_START(FH_pte_lock);
			pr_locks("lockpte addr[%lx] ptl[%llx] CPU[%d] 1 try\n", address, (u64)ptl_ptr, smp_processor_id());
			spin_lock(ptl_ptr);
			pr_locks("lockpte addr[%lx] ptl[%llx] CPU[%d] 1 get\n", address, (u64)ptl_ptr, smp_processor_id());
			PROFILE_LEAVE(FH_pte_lock);

			if (unlikely(entry != find_pte_from_reg(address)))
			{
				printk(KERN_ERR "ERROR: CR3 register and page table are pointing different PTE 0x%lx <-> 0x%lx\n",
					   (unsigned long)entry, (unsigned long)find_pte_from_reg(address));
			}

			// check other thread already solve this
			if ((!(error_code & X86_PF_WRITE) && pte_present(*entry)) // read request
				|| ((error_code & X86_PF_WRITE) && pte_present(*entry) && pte_write(*entry))) // write request // new version have this: !(error_code & X86_PF_PROT) || 
			{
return_and_retry:
				if (likely(ptl_ptr && spin_is_locked(ptl_ptr))) {
					pr_locks("unlockpte addr[%lx] ptl[%llx] CPU[%d] 2\n", address, (u64)ptl_ptr, smp_processor_id());
					spin_unlock(ptl_ptr);
				}
				cache_range_unlock(is_kern_shared_mem ? DISAGG_KERN_TGID : tsk->tgid, &pgfault_lock);

return_and_retry_no_lock:
				pr_pgfault("return and retry addr[%lx] ret_code[%d] vma_rlock[%d] cnt[%lu]\n", address, return_code, hold_vma_rlock, return_cnt++);
				if (hold_vma_rlock)
					up_read(&mm->mmap_sem);

				if (is_prefetch)
					return; // simply fail

				PROFILE_START(FH_nack_and_retry_delay);
				PROFILE_START(FH_nack_for_restore);
				PROFILE_START(FH_nack_for_inval);
				PROFILE_START(FH_nack_for_inval_w);
				// == Help from page fault handler ==
				help_invalidation_and_sleep(back_off_cnt);
				PROFILE_LEAVE(FH_nack_and_retry);
				if(try_inval_proc)
				{


					back_off_cnt ++;
					if (back_off_cnt >= DISAGG_NACK_MAX_BACKOFF)
					{
						back_off_cnt = DISAGG_NACK_MAX_BACKOFF;
					}
					PROFILE_LEAVE(FH_nack_and_retry_delay);
					goto retry;
				}else if(return_code == FH_NACK_FROM_RESTORE){
					PROFILE_LEAVE(FH_nack_for_restore);
				}else if(return_code == FH_ALREADY_EVICT){
					PROFILE_LEAVE(FH_nack_for_inval);
				}else if(return_code == FH_ALREADY_EVICT_WAIT){
					PROFILE_LEAVE(FH_nack_for_inval_w);
				}
				if (is_kern_shared_mem && address >= (unsigned long)get_base_address(DISAGG_KSHMEM_SERV_FUTEX_ID) + offsetof(struct disagg_futex_shmem, buf)
						&& address < (unsigned long)get_base_address(DISAGG_KSHMEM_SERV_FUTEX_ID) + offsetof(struct disagg_futex_shmem, lookup_table)) {
					PROFILE_LEAVE(FH_fault_handler_abort_test_futex_messaging);
				} else if (is_kern_shared_mem || is_kern_ip(regs->ip)) {
					PROFILE_LEAVE(FH_fault_handler_abort_kern);
				} else if (address >= TEST_MEM_ACC_ADDR && address < TEST_LOCK_ADDR) {
					PROFILE_LEAVE(FH_fault_handler_abort_test_mem_acc);
				} else if (address >= TEST_LOCK_ADDR && address < TEST_TLS_ADDR) {
					PROFILE_LEAVE(FH_fault_handler_abort_test_lock);
				} else {
					PROFILE_LEAVE(FH_fault_handler_abort_user);
				}
				return;
			}

			if (!vma || (vma->vm_start > address))
			{
				vma = NULL;
			}
			PROFILE_LEAVE(FH_pre_process);

			/* 
			 * Prepare page if PTE is not presented
			 */
			barrier();
			PROFILE_START(FH_prepare_data);
			pre_alloc_data = prepare_data_page(is_kern_shared_mem ? DISAGG_KERN_TGID : tsk->tgid,
											   mm, entry, address, // error_code,
											   (flags & 0xF) & ~VM_WRITE,
											   &vma, &new_cnpage, &existing_page, &hold_vma_rlock);
			pte_value = *entry;
			if (!new_cnpage)
			{
				// During eviction
				return_code = FH_ALREADY_EVICT;
				goto return_and_retry;
			}
			PROFILE_LEAVE(FH_prepare_data);
			// Now we have pgfault lock

			// Ask to memory node for any changes.
			cpu_id = get_cpu();
			if (!pre_alloc_data)
			{
				// It will dummy access to test permission, data will be copied locally
				// read-only to writable permission
				ret_buf.data = (void *)get_dummy_page_dma_addr(cpu_id);
				pr_pgfault("PFault|Dummy Buf - Addr: 0x%lx, DMA: 0x%lx, ErrCode: 0x%lx",
						   address, (unsigned long)ret_buf.data, (unsigned long)error_code);
			}
			else
			{
				ret_buf.data = (void *)new_cnpage->dma_addr;
				pr_pgfault("PFault|NewReq - Addr: 0x%lx, DMA: 0x%lx, ErrCode: 0x%lx",
						   address, (unsigned long)ret_buf.data, (unsigned long)error_code);
			}

// 	WE DISABLED OWNERSHIP OPTIMIZATION
//	- Thus, the page must be fetched over network, always.
#if 0
			PROFILE_START(FH_ownership_optimization);
			if (new_cnreq->cacheline->ownership)
			{	// exclusively owned cacheline & zerod data in memory
				put_cpu();
				if (!spin_trylock(&new_cnreq->pgfault_lock))
				{
					printk(KERN_WARNING "Pgfault spinlock is already locked: %u, 0x%lx\n",
						   tsk->tgid, address & PAGE_MASK);
					pre_alloc_data = NULL;
					goto return_and_retry;
				}
				if (pre_alloc_data)
					memset(pre_alloc_data, 0, PAGE_SIZE);
				ret_buf.data_size = PAGE_SIZE;
				fault = DISAGG_FAULT_WRITE;
				PROFILE_LEAVE(FH_ownership_optimization);
				goto pagefault_restore_page;
			}
#endif
			PROFILE_START(FH_add_waiting_node);
			wait_node = add_waiting_node(is_kern_shared_mem ? DISAGG_KERN_TGID : tsk->tgid,
										 address & PAGE_MASK, new_cnpage);
			smp_wmb();
			if (unlikely(!wait_node))
			{
				BUG(); // this should never happen due to range lock
			}
			pr_locks("unlockpte addr[%lx] ptl[%llx] CPU[%d] 3\n", address, (u64)ptl_ptr, smp_processor_id());
			spin_unlock(ptl_ptr);
			PROFILE_LEAVE(FH_add_waiting_node);
			net_jiff = jiffies;
			PROFILE_START(FH_warn_net_lat);
			PROFILE_START(FH_succ_net_lat);
			if (error_code & X86_PF_WRITE)
				PROFILE_START(FH_modified_net_lat);
			else
				PROFILE_START(FH_shared_net_lat);
#ifdef PRINT_PAGE_FAULT
			check_switch_cache_directory("*before send_pfault_to_mn", is_kern_shared_mem ? DISAGG_KERN_TGID : tsk->tgid, address & PAGE_MASK);
#endif
			fault = send_pfault_to_mn(tsk, error_code, address, reset_req, &ret_buf);
			put_cpu();
			smp_wmb();
			cnthread_set_page_received(new_cnpage);
			barrier();
			try_inval_proc = 1;
			clear_responsibility = (atomic_read(&new_cnpage->is_used) != IS_PAGE_USED);	// If we are the one who just changed its status
			if (jiffies_to_usecs(jiffies - net_jiff) > 10000)	// more than 1ms
				PROFILE_LEAVE(FH_warn_net_lat);
			{
				if (error_code & X86_PF_WRITE)
					PROFILE_LEAVE(FH_modified_net_lat);
				else
					PROFILE_LEAVE(FH_shared_net_lat);
			}

			wait_jiff = jiffies;
			PROFILE_START_EVAL(FH_ack_waiting_node);
			PROFILE_START(FH_warn_wait_ack);
			pr_pgfault("CN [%d]: fault handler start waiting 0x%lx\n", cpu_id, address);
			wait_node->ack_buf = ret_buf.ack_buf;
			if (fault <= 0)
			{	// NACK
				cancel_waiting_for_nack(wait_node);
			}
			wait_err = wait_ack_from_ctrl(wait_node, NULL, NULL, new_cnpage);
			pr_locks("lockpte addr[%lx] ptl[%llx] CPU[%d] 4 try\n", address, (u64)ptl_ptr, smp_processor_id());
			spin_lock(ptl_ptr);
			pr_locks("lockpte addr[%lx] ptl[%llx] CPU[%d] 4 get\n", address, (u64)ptl_ptr, smp_processor_id());

			// Check exceptional case
			if (!wait_err)
			{
				if (unlikely((entry && (entry->pte != pte_value.pte))))
				{
					BUG();










				}
			}
			if (wait_err)
			{
				// Unlocked & received NACK
				pre_alloc_data = NULL;
				return_code = FH_NACK_NORMAL;
				if (clear_responsibility)
					cnthread_put_page(new_cnpage);
				goto return_and_retry;
			}
			if (jiffies_to_usecs(jiffies - wait_jiff) > 100)
				PROFILE_LEAVE(FH_warn_wait_ack);
			PROFILE_LEAVE_EVAL(FH_ack_waiting_node);
			PROFILE_LEAVE(FH_succ_net_lat);
#ifdef PRINT_PAGE_FAULT
			print_pgfault_error(tsk, error_code, address, vma, is_prefetch);
#endif
			if (fault == DISAGG_FAULT_READ || fault == DISAGG_FAULT_WRITE)
			{
				flags = (flags & 0xF) & ~VM_WRITE;
			}
			switch(fault){
				case DISAGG_FAULT_READ:
				case DISAGG_FAULT_WRITE:
					check_sync_rss_stat(tsk);
					// Retreive data from memory node
					if (ret_buf.data_size >= PAGE_SIZE)
					{
						PROFILE_START(FH_restore_data);
						if (!restore_data_page(cur_tsk, entry, address, error_code,
											   flags, &vma, (unsigned long)ret_buf.data,
											   new_cnpage, existing_page, is_prefetch))
						{
							clear_responsibility = cnthread_rollback_page_received(new_cnpage);
							smp_wmb();
							if (!existing_page || clear_responsibility)	// Was not existing
							{
								pr_pgfault("CN [%d]: put_page (3) 0x%lx\n", cpu_id, address);
								cnthread_put_page(new_cnpage);
							}							
							try_inval_proc = 0;
							return_code = FH_NACK_FROM_RESTORE;
							goto return_and_retry;
						}
						PROFILE_LEAVE(FH_restore_data);
						smp_wmb();
						// release locks
						cache_range_unlock(is_kern_shared_mem ? DISAGG_KERN_TGID : tsk->tgid, &pgfault_lock);
						if (likely(spin_is_locked(ptl_ptr))) {
							pr_locks("unlockpte addr[%lx] ptl[%llx] CPU[%d] 5\n", address, (u64)ptl_ptr, smp_processor_id());
							spin_unlock(ptl_ptr);
						}
						up_read(&mm->mmap_sem);
						PROFILE_LEAVE(FH_fault_handler);
						if (is_kern_shared_mem && address >= (unsigned long)get_base_address(DISAGG_KSHMEM_SERV_FUTEX_ID) + offsetof(struct disagg_futex_shmem, buf)
								&& address < (unsigned long)get_base_address(DISAGG_KSHMEM_SERV_FUTEX_ID) + offsetof(struct disagg_futex_shmem, lookup_table)) {
							PROFILE_LEAVE(FH_fault_handler_tot_test_futex_messaging);
						} else if (is_kern_shared_mem || is_kern_ip(regs->ip)) {
							PROFILE_LEAVE(FH_fault_handler_tot_kern);
						} else if (address >= TEST_MEM_ACC_ADDR && address < TEST_LOCK_ADDR) {
							PROFILE_LEAVE(FH_fault_handler_tot_test_mem_acc);
						} else if (address >= TEST_LOCK_ADDR && address < TEST_TLS_ADDR) {
							PROFILE_LEAVE(FH_fault_handler_tot_test_lock);
						} else {
							PROFILE_LEAVE(FH_fault_handler_tot_user);
						}
						start_jiff = (unsigned long)(sched_clock() - start_jiff) / 1000;
						if (start_jiff <= 10)
							PROFILE_LEAVE(FH_latency_10us);
						else if (start_jiff <= 50)
							PROFILE_LEAVE(FH_latency_50us);
						else if (start_jiff <= 100)
							PROFILE_LEAVE(FH_latency_100us);
						else if (start_jiff <= 250)
							PROFILE_LEAVE(FH_latency_250us);
						else if (start_jiff <= 500)
							PROFILE_LEAVE(FH_latency_500us);
						else
							PROFILE_LEAVE(FH_latency_1000us);
						pr_pgfault("end disagg pfault tgid[%d] pid[%d] ip[%lx] addr[%lx] errcode[%lx] CPU[%d]\n",
								   tsk->tgid, tsk->pid, regs->ip, address, error_code, smp_processor_id());
#ifdef DISAGG_ENABLE_PREFETCH
						pfet_add_struct(tsk, address);
#endif
						return;
					}
					printk(KERN_DEFAULT "ERROR: fault handler - data from MN is not enough\n");
					goto release_write_and_bad_area;

				case DISAGG_FAULT_ERR_NO_VMA:
				// case DISAGG_FAULT_ERR_PERM:
				// case DISAGG_FAULT_ERR_NO_MEM:
				// case DISAGG_FAULT_ERR_NO_ANON:
					printk(KERN_DEFAULT "** Cannot handled by memory node handler err: %d, kern: %d.\n",
							fault, (error_code & X86_PF_USER) ? 0 : 1);
					print_pgfault_error(tsk, error_code, address, vma, is_prefetch);
					BUG();

				default:
					goto release_write_and_bad_area;
			}

release_write_and_bad_area:
			is_bad_area = 1;
			clear_responsibility = cnthread_rollback_page_received(new_cnpage);
			smp_wmb();
			if (likely(spin_is_locked(ptl_ptr))) {
				pr_locks("unlockpte addr[%lx] ptl[%llx] CPU[%d] 6\n", address, (u64)ptl_ptr, smp_processor_id());
				spin_unlock(ptl_ptr);
			}
			if (!existing_page || clear_responsibility)	// Was not existing
			{
				pr_pgfault("CN [%d]: put_page (4) 0x%lx\n", cpu_id, address);
				cnthread_put_page(new_cnpage);
			}
			cache_range_unlock(is_kern_shared_mem ? DISAGG_KERN_TGID : tsk->tgid, &pgfault_lock);
			if (is_bad_area && !is_prefetch)
				goto bad_area;
			else
				return;
		}
		else if (is_prefetch)
		{
			return;	// simply fail
		}
	}

// ====== normal Linux routine ===== //
// normal_linux_routine:
	if (unlikely(!vma)) {
		goto bad_area;
	}
	if (likely(vma->vm_start <= address))
		goto good_area;
	if (unlikely(!(vma->vm_flags & VM_GROWSDOWN))) {
		goto bad_area;
	}
	if (error_code & X86_PF_USER) {






		if (unlikely(address + 65536 + 32 * sizeof(unsigned long) < regs->sp)) {
			goto bad_area;
		}
	}
	if (unlikely(expand_stack(vma, address))) {
		goto bad_area;
	}
	printk(KERN_DEFAULT "CN: stack expanded - addr: 0x%lx, vma: 0x%lx - 0x%lx [0x%lx]\n",
		address, vma->vm_start, vma->vm_end, vma->vm_flags);
	goto good_area;

bad_area:
	if (vma) {
		pr_pgfault("CN: BAD AREA- addr: 0x%lx, vma: 0x%lx - 0x%lx [0x%lx]\n",
			address, vma->vm_start, vma->vm_end, vma->vm_flags);
	}
	print_pgfault_error(tsk, error_code, address, vma, is_prefetch);
	barrier();

	bad_area(regs, error_code, address);
	return;

	/*
	 * Ok, we have a good vm_area for this memory access, so
	 * we can handle it..
	 */
good_area:
	if (unlikely(access_error(error_code, vma))) {
		printk(KERN_DEFAULT "CN: GOOD AREA- addr: 0x%lx, vma: 0x%lx - 0x%lx [0x%lx]\n",
			   address, vma->vm_start, vma->vm_end, vma->vm_flags);
		print_pgfault_error(tsk, error_code, address, vma, is_prefetch);
 		DEBUG_print_vma(mm);
		barrier();

		bad_area_access_error(regs, error_code, address, vma);
		return;
	}
















	pkey = vma_pkey(vma);
	fault = handle_mm_fault(vma, address, flags);
	major |= fault & VM_FAULT_MAJOR;

	/*
	 * If we need to retry the mmap_sem has already been released,
	 * and if there is a fatal signal pending there is no guarantee
	 * that we made any progress. Handle this case first.
	 */
	if (unlikely(fault & VM_FAULT_RETRY)) {
		/* Retry at most once */
		if (flags & FAULT_FLAG_ALLOW_RETRY) {
			flags &= ~FAULT_FLAG_ALLOW_RETRY;
			flags |= FAULT_FLAG_TRIED;
			if (!fatal_signal_pending(tsk))
				goto retry;
		}


		if (flags & FAULT_FLAG_USER)
			return;


		no_context(regs, error_code, address, SIGBUS, BUS_ADRERR);
		return;
	}

	up_read(&mm->mmap_sem);
	if (unlikely(fault & VM_FAULT_ERROR)) {
		mm_fault_error(regs, error_code, address, &pkey, fault);
		return;
	}

}
EXPORT_SYMBOL(get_dummy_page_dma_addr);
EXPORT_SYMBOL(cancel_waiting_for_nack);

noinline
void do_disagg_page_fault(struct task_struct *tsk, struct pt_regs *regs,
						  unsigned long error_code, unsigned long address, unsigned int flags)
{
	_do_disagg_page_fault(tsk, regs, error_code, address, flags, 0);

}
NOKPROBE_SYMBOL(do_disagg_page_fault);

void do_disagg_page_fault_prefetch(struct task_struct *tsk, unsigned long address)
{
	_do_disagg_page_fault(tsk, NULL, X86_PF_WRITE, address, VM_READ | VM_WRITE, 1);
}
