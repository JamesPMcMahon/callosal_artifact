/* we may not need this header anymore */

#include "memory_management.h"
#include "rbtree_ftns.h"
#include <linux/vmalloc.h>

atomic64_t last_mm_ctx_id = ATOMIC64_INIT(1);

/*
 * Make sure vm_committed_as in one cacheline and not cacheline shared with
 * other variables. It can be updated by several CPUs frequently.
 */
//linux/cache.h - ____cacheline_aligned_in_smp
struct percpu_counter vm_committed_as ____cacheline_aligned_in_smp;

__cacheline_aligned_in_smp DEFINE_SPINLOCK(mmlist_lock);

int sysctl_max_map_count __read_mostly = DEFAULT_MAX_MAP_COUNT;	// linux/mm.h

#ifndef arch_mmap_check
#define arch_mmap_check(addr, len, flags)	(0)
#endif

/*
 * initialise the percpu counter for VM
 */
void __init mmap_init(void)
{
	int ret = percpu_counter_init(&vm_committed_as, 0, GFP_KERNEL);
	VM_BUG_ON(ret);
}

/* We will not support namespace */








/* We disabled huge tlb page */







/* We disabled cached memory slab for now */
#if 0
/* SLAB cache for signal_struct structures (tsk->signal) */
static struct kmem_cache *signal_cachep;

/* SLAB cache for sighand_struct structures (tsk->sighand) */
struct kmem_cache *sighand_cachep;

/* SLAB cache for files_struct structures (tsk->files) */
struct kmem_cache *files_cachep;

/* SLAB cache for fs_struct structures (tsk->fs) */
struct kmem_cache *fs_cachep;

/* SLAB cache for vm_area_struct structures */
struct kmem_cache *vm_area_cachep;

/* SLAB cache for mm_struct structures (tsk->mm) */
static struct kmem_cache *mm_cachep;

#define allocate_mm()	(kmem_cache_alloc(mm_cachep, GFP_KERNEL))
#define free_mm(mm)	(kmem_cache_free(mm_cachep, (mm)))

void __init proc_caches_init(void)
{
	sighand_cachep = kmem_cache_create("sighand_cache",
			sizeof(struct sighand_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_TYPESAFE_BY_RCU|
			SLAB_ACCOUNT, sighand_ctor);
	signal_cachep = kmem_cache_create("signal_cache",
			sizeof(struct signal_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_ACCOUNT,
			NULL);
	files_cachep = kmem_cache_create("files_cache",
			sizeof(struct files_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_ACCOUNT,
			NULL);
	fs_cachep = kmem_cache_create("fs_cache",
			sizeof(struct fs_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_ACCOUNT,
			NULL);







	mm_cachep = kmem_cache_create("mm_struct",
			sizeof(struct mm_struct), ARCH_MIN_MMSTRUCT_ALIGN,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_ACCOUNT,
			NULL);
	vm_area_cachep = KMEM_CACHE(vm_area_struct, SLAB_PANIC|SLAB_ACCOUNT);
	mmap_init();


}
#endif



static inline int dup_userfaultfd(struct vm_area_struct *vma,
				  struct list_head *l)
{
	return 0;
}
/* rmap.c */

static void mn_mpol_put(struct mempolicy *p)
{
	if (!atomic_dec_and_test(&p->refcnt))
		return;
}

#define COPY_MM_VALUES(MM, EXR, F)	(MM->F = EXR->F)

#define mn_set_up_layout(D_MM, S_MM)	{\
	COPY_MM_VALUES(D_MM, S_MM, hiwater_rss);\
	COPY_MM_VALUES(D_MM, S_MM, hiwater_vm);\
	COPY_MM_VALUES(D_MM, S_MM, total_vm);\
	COPY_MM_VALUES(D_MM, S_MM, locked_vm);\
	COPY_MM_VALUES(D_MM, S_MM, pinned_vm);\
	COPY_MM_VALUES(D_MM, S_MM, data_vm);\
	COPY_MM_VALUES(D_MM, S_MM, exec_vm);\
	COPY_MM_VALUES(D_MM, S_MM, stack_vm);\
	COPY_MM_VALUES(D_MM, S_MM, def_flags);\
	COPY_MM_VALUES(D_MM, S_MM, start_code);\
	COPY_MM_VALUES(D_MM, S_MM, end_code);\
	COPY_MM_VALUES(D_MM, S_MM, start_data);\
	COPY_MM_VALUES(D_MM, S_MM, end_data);\
	COPY_MM_VALUES(D_MM, S_MM, start_brk);\
	COPY_MM_VALUES(D_MM, S_MM, brk);\
	COPY_MM_VALUES(D_MM, S_MM, start_stack);\
	COPY_MM_VALUES(D_MM, S_MM, arg_start);\
	COPY_MM_VALUES(D_MM, S_MM, arg_end);\
	COPY_MM_VALUES(D_MM, S_MM, env_start);\
	COPY_MM_VALUES(D_MM, S_MM, env_end);\
	COPY_MM_VALUES(D_MM, S_MM, mmap_base);\
	COPY_MM_VALUES(D_MM, S_MM, mmap_legacy_base);\
}

void DEBUG_print_one_vma(struct vm_area_struct *cur, int i)
{
	struct vm_area_struct *ln, *rn;
	ln = cur->vm_rb.rb_left ? rb_entry(cur->vm_rb.rb_left, struct vm_area_struct, vm_rb) : 0;
	rn = cur->vm_rb.rb_right ? rb_entry(cur->vm_rb.rb_right, struct vm_area_struct, vm_rb) : 0;
	pr_info("  *[%d, %p]: addr: 0x%lx - 0x%lx [0x%lx], alloc: %d, l: %p, r: %p\n",
			i, cur, cur->vm_start, cur->vm_end, cur->vm_flags,
			cur->vm_private_data ? 1 : 0,	// if allocated, then 1
			ln, rn);
}

void DEBUG_print_vma(struct mm_struct *mm)
{
	int i = 0;
	struct vm_area_struct *cur = mm->mmap;
	for(;cur;cur = cur->vm_next)
	{
		DEBUG_print_one_vma(cur, i);
		i++;
	}
}

void DEBUG_print_exec_vma( struct exec_msg_struct* exec_req)
{
	int i = 0;
	struct exec_vmainfo *exec;
	for (i = 0; i < (int)exec_req->num_vma; i++)
	{
		exec = &((&exec_req->vmainfos)[i]);
		pr_info("  *[%d, %p]: addr: 0x%lx - 0x%lx [%lx] (f:%d)\n",
			i, exec, exec->vm_start, exec->vm_end, exec->vm_flags,
			exec->file_id ? 1 : 0);
	}
}

void DEBUG_print_vma_diff(struct mm_struct *mm, struct exec_msg_struct* exec_req)
{
	int i = 0;
	struct vm_area_struct *prev, *vma = mm->mmap;
	struct exec_vmainfo *exec;
	unsigned long prev_end;
	unsigned long mn_only = 0, cn_only = 0;
	
	if (down_read_killable(&mm->mmap_sem)) {
		return ;
	}

	prev = NULL;
	pr_info("  # of VMAs (tgid: %d): CN[%d]\n", 
		(int)exec_req->tgid, (int)exec_req->num_vma);

	for (i = 0; i < (int)exec_req->num_vma; i++)
	{
		exec = &((&exec_req->vmainfos)[i]);
		prev_end = ((i == 0) ? 0 : (&exec_req->vmainfos)[i-1].vm_end);
		while (vma && (vma->vm_end <= exec->vm_start))
		{
			// all vma in between (prev_end) to (exec->vm_start) is mn only
			if (prev_end < vma->vm_end)
			{
				// pr_info("  *[%d]: addr: 0x%lx - 0x%lx (mn only)\n",
				// 		i, max(prev_end, vma->vm_start), vma->vm_end);
				mn_only += vma->vm_end - max(prev_end, vma->vm_start);
			}
			prev = vma;
			vma = vma->vm_next;
		}
		
		// now vma->vm_end >= exec->vm_start
		// any remaining part of vma < exec->vm_start
		if (vma && vma->vm_start < exec->vm_start)
		{
			if (prev_end < exec->vm_start)
			{
				// pr_info("  *[%d]: addr: 0x%lx - 0x%lx (mn only)\n",
				// 		i, max(prev_end, vma->vm_start), exec->vm_start);
				mn_only += exec->vm_start - max(prev_end, vma->vm_start);
			}
			if (vma->vm_end <= exec->vm_end)
			{
				prev = vma;
				vma = vma->vm_next;
			}
		}

		// exec regions in between (vma->vm_prev->vm_end) to (vma->vm_start)
		while (vma && (vma->vm_start >= exec->vm_start && vma->vm_start < exec->vm_end))
		{
			if (vma->vm_start > exec->vm_start)
			{
				if (!prev || (vma->vm_start > prev->vm_end))
				{
					if (!exec->file_id)
					{
						pr_info("  *[%d]: addr: 0x%lx - 0x%lx f:%d (cn only)\n",
							i, max(prev ? prev->vm_end : 0, exec->vm_start), vma->vm_start,
							(exec->file_id ? 1 : 0));
					}
					cn_only += vma->vm_start - max(prev ? prev->vm_end : 0, exec->vm_start);
				}
			}
			if (vma->vm_end <= exec->vm_end)
			{
				prev = vma;
				vma = vma->vm_next;
			}else
				break;
		}

		// now vma->vm_start >= exec->vm_end OR vma == NULL 
		//   OR (vma->vm_start < exec->vm_start && vma->vm_end >= exec->vm_end)
		// any remaining part of vma->prev->vm_end < exec->vm_end
		if ((prev ? prev->vm_end : 0) < exec->vm_end)
		{
			if (!vma || (vma->vm_start > exec->vm_start))
			{
				if (!exec->file_id)
				{
					pr_info("  *[%d]: addr: 0x%lx - 0x%lx f:%d (cn only)\n",
							i, max(prev ? prev->vm_end : 0, exec->vm_start), exec->vm_end,
							(exec->file_id ? 1 : 0));
				}
				cn_only += exec->vm_end - max(prev ? prev->vm_end : 0, exec->vm_start);
			}
		}
	}

	prev_end = (i > 0) ? ((&exec_req->vmainfos)[i-1]).vm_end : 0;
	
	// remaining vmas
	while (vma)
	{
		if (vma->vm_end > prev_end)
		{
			// pr_info("  *[%d]: addr: 0x%lx - 0x%lx (mn only)\n",
			// 		i, max(vma->vm_start, prev_end), vma->vm_end);
			mn_only += vma->vm_end - max(vma->vm_start, prev_end);
		}
		prev = vma;
		vma = vma->vm_next;
	}

	up_read(&mm->mmap_sem);
	// print summary
	pr_info("  *cn only: %lu <-> mn only: %lu\n", cn_only, mn_only);
}

/* create mmap from msg */
int mn_build_mmap_from_exec(struct mm_struct *mm, struct exec_msg_struct* exec_req)
{
	int i;
	struct vm_area_struct *prev, *mgnt, **pprev;
	struct rb_node **rb_link, *rb_parent;
	int error = 0;
	
	if (down_write_killable(&mm->mmap_sem)) {
		return -EINTR;
	}

	// copy field from exec_req
	mn_set_up_layout(mm, exec_req);

	rb_link = &mm->mm_rb.rb_node;
	rb_parent = NULL;
	prev = NULL;
	pprev = &mm->mmap;



	for (i = 0; i < (int)exec_req->num_vma; i++)
	{














		mgnt = kzalloc(sizeof(*mgnt), GFP_KERNEL);
		if (!mgnt){
			error = -ENOMEM;	// cannot allocate memory
			goto mn_build_mmap_error;
		}
		// copy values
		mgnt->vm_start = (&exec_req->vmainfos)[i].vm_start;
		mgnt->vm_end = (&exec_req->vmainfos)[i].vm_end;
		mgnt->vm_flags = (&exec_req->vmainfos)[i].vm_flags;
		mgnt->vm_pgoff = (&exec_req->vmainfos)[i].vm_pgoff;







		mgnt->vm_private_data = NULL;








		// flags: we added last two referring LegoOS
		mgnt->vm_flags &= ~(VM_LOCKED|VM_LOCKONFAULT|VM_UFFD_MISSING|VM_UFFD_WP);
		
		// use file pointer as an identifier (no actual allocation)
		mgnt->vm_file = (struct file*)((&exec_req->vmainfos)[i].file_id);

		mgnt->rb_subtree_gap = (&exec_req->vmainfos)[i].rb_substree_gap;
		mgnt->vm_page_prot.pgprot = (&exec_req->vmainfos)[i].vm_page_prot;

		mgnt->vm_next = mgnt->vm_prev = NULL;
		mgnt->vm_mm = mm;

		// set up linked list
		*pprev = mgnt;
		pprev = &mgnt->vm_next;
		mgnt->vm_prev = prev;
		prev = mgnt;

		// we need to copy rb_tree
		__vma_link_rb(mm, mgnt, rb_link, rb_parent);
		rb_link = &mgnt->vm_rb.rb_right;
		rb_parent = &mgnt->vm_rb;

		mm->map_count++;



		// COPIED from EXEC REQ

	}


	up_write(&mm->mmap_sem);
	return 0;

mn_build_mmap_error:
	if (mm->mmap)
		mn_remove_vmas(mm);

	up_write(&mm->mmap_sem);
	return error;
}


static __latent_entropy int dup_mmap(struct mm_struct *mm,
					struct mm_struct *oldmm)
{
	struct vm_area_struct *prev, *mgnt, **pprev, *tmp;
	struct rb_node **rb_link, *rb_parent;
	int retval = 0;


	if (!mm || !oldmm){
		return -EINTR;
	}
	
	if (down_write_killable(&oldmm->mmap_sem)) {
		return -EINTR;
	}
	down_write(&mm->mmap_sem);




	mm->total_vm = oldmm->total_vm;
	mm->data_vm = oldmm->data_vm;
	mm->exec_vm = oldmm->exec_vm;
	mm->stack_vm = oldmm->stack_vm;

	rb_link = &mm->mm_rb.rb_node;
	rb_parent = NULL;
	prev = NULL;
	pprev = &mm->mmap;





	
#if 0
	struct vm_area_struct *mpnt, *tmp, *prev, **pprev;
	struct rb_node **rb_link, *rb_parent;
	int retval;
	unsigned long charge;
	LIST_HEAD(uf);

	uprobe_start_dup_mmap();	//semaphore..
	if (down_write_killable(&oldmm->mmap_sem)) {
		retval = -EINTR;
		goto fail_uprobe_end;
	}
	flush_cache_dup_mm(oldmm);	//nothing for x86
	uprobe_dup_mmap(oldmm, mm);	//test and set uprobe bit (flag)
	/*
	 * Not linked in yet - no deadlock potential:
	 */
	down_write_nested(&mm->mmap_sem, SINGLE_DEPTH_NESTING);

	/* No ordering required: file already has been exposed. */
	RCU_INIT_POINTER(mm->exe_file, get_mm_exe_file(oldmm));	//initialize RCU protected pointer
	//get_mm_exe_file gets RCU dereferenced file pointer

	mm->total_vm = oldmm->total_vm;
	mm->data_vm = oldmm->data_vm;
	mm->exec_vm = oldmm->exec_vm;
	mm->stack_vm = oldmm->stack_vm;

	rb_link = &mm->mm_rb.rb_node;
	rb_parent = NULL;
	pprev = &mm->mmap;
	/* We disabled ksm and huge page */






	prev = NULL;
#endif

	// pr_info("dup_mmap: 0x%lx -> 0x%lx\n", 
	// 		(unsigned long)oldmm, (unsigned long)mm);

	for (mgnt = oldmm->mmap; mgnt; mgnt = mgnt->vm_next) {


		if (mgnt->vm_flags & VM_DONTCOPY) {
			//decrease stat counter, because it will not be copied
			vm_stat_account(mm, mgnt->vm_flags, -vma_pages(mgnt));
			continue;
		}








		tmp = kzalloc(sizeof(*tmp), GFP_KERNEL);
		if (!tmp){
			retval = -ENOMEM;	// cannot allocate memory
			goto mn_fork_mmap_error;
		}
		*tmp = *mgnt;
		INIT_LIST_HEAD(&tmp->anon_vma_chain);	// initialze anonymous vma chain
		





		tmp->vm_mm = mm;












		tmp->vm_flags &= ~(VM_LOCKED | VM_LOCKONFAULT);
		tmp->vm_next = tmp->vm_prev = NULL;



















		if (mgnt->vm_private_data){
			unsigned long len = tmp->vm_end - tmp->vm_start;
			tmp->vm_private_data = vmalloc(len);
			if (!tmp->vm_private_data)
			{
				retval = -ENOMEM;	// cannot allocate memory
				kfree(tmp);	//not linked yet, just free it
				goto mn_fork_mmap_error;
			}
			// pr_info("dup_mmap - memcpy: 0x%lx - 0x%lx, len: %lu, data_ptr: 0x%lx\n", 
			// 	tmp->vm_start, tmp->vm_end, len, tmp->vm_private_data);



			memcpy(tmp->vm_private_data, mgnt->vm_private_data, len);
		}else{
			tmp->vm_private_data = NULL;
		}

		/*
		 * Clear hugetlb-related page reserves for children. This only
		 * affects MAP_PRIVATE mappings. Faults generated by the child
		 * are not guaranteed to succeed, even if read-only
		 */




		/*
		 * Link in the new vma and copy the page table entries.
		 */
		*pprev = tmp;
		pprev = &tmp->vm_next;
		tmp->vm_prev = prev;
		prev = tmp;

		__vma_link_rb(mm, tmp, rb_link, rb_parent);	// update rb_tree gaps, insert new node
		rb_link = &tmp->vm_rb.rb_right;
		rb_parent = &tmp->vm_rb;

		mm->map_count++;








	}
	/* a new mm has just been created */


	up_write(&mm->mmap_sem);

	up_write(&oldmm->mmap_sem);



	return retval;








mn_fork_mmap_error:
	pr_info("mn_fork_mmap_error: mm: 0x%lx, map: 0x%lx\n", 
			(unsigned long)mm, (unsigned long)mm->mmap);

	if (mm->mmap)
		mn_remove_vmas(mm);
	
	up_write(&mm->mmap_sem);
	up_write(&oldmm->mmap_sem);
	return retval;
}


/* Initialization of mm_init */









static inline int mm_alloc_pgd(struct mm_struct *mm)
{
	mm->pgd = pgd_alloc(mm);
	if (unlikely(!mm->pgd))
		return -ENOMEM;
	return 0;
}

static inline void mm_free_pgd(struct mm_struct *mm)
{
	pgd_free(mm, mm->pgd);
}



static void mm_init_aio(struct mm_struct *mm)
{

	spin_lock_init(&mm->ioctx_lock);
	mm->ioctx_table = NULL;

}



static void mm_init_owner(struct mm_struct *mm, struct task_struct *p)
{

	mm->owner = p;

}



static void mm_init_uprobes_state(struct mm_struct *mm)
{

	mm->uprobes_state.xol_area = NULL;

}

/*
 * uprobe_clear_state - Free the area allocated for slots.
 */
void uprobe_clear_state(struct mm_struct *mm)
{
	struct xol_area *area = mm->uprobes_state.xol_area;

	if (!area)
		return;




}

struct mm_struct *mm_init(struct mm_struct *mm, struct task_struct *p)

{
    // set initial values for mm_struct
	mm->mmap = NULL;
	mm->mm_rb = RB_ROOT;
	mm->vmacache_seqnum = 0;
	atomic_set(&mm->mm_users, 1);
	atomic_set(&mm->mm_count, 1);
	init_rwsem(&mm->mmap_sem);
	INIT_LIST_HEAD(&mm->mmlist);
	mm->core_state = NULL;

	//INTEAD OF mm_pgtables_bytes_init(mm); we used the following code
	atomic_long_set(&mm->pgtables_bytes, 0);
	
	mm->map_count = 0;
	mm->locked_vm = 0;
	mm->pinned_vm = 0;
	memset(&mm->rss_stat, 0, sizeof(mm->rss_stat));
	spin_lock_init(&mm->page_table_lock);
	mm_init_cpumask(mm);
	


	
	mm_init_owner(mm, p);
	RCU_INIT_POINTER(mm->exe_file, NULL);

	


	
	init_tlb_flush_pending(mm);
/* we disabled huge page */



	mm_init_uprobes_state(mm);






	mm->flags = 0;	// Let's ignore this for now like LegoOS
	mm->def_flags = 0;
	// }











	return mm;






}

static void check_mm(struct mm_struct *mm)
{
	int i;

	for (i = 0; i < NR_MM_COUNTERS; i++) {
		long x = atomic_long_read(&mm->rss_stat.count[i]);

		if (unlikely(x))
			printk(KERN_ALERT "BUG: Bad rss-counter state "
					  "mm:%p idx:%d val:%ld\n", mm, i, x);
	}

	if (mm_pgtables_bytes(mm))
		pr_alert("BUG: non-zero pgtables_bytes on freeing mm: %ld\n",
				mm_pgtables_bytes(mm));
}

static inline void munlock_vma_pages_all(struct vm_area_struct *vma)
{


	;
}

/* Release all mmaps. */
static void mn_exit_mmap(struct mm_struct *mm)
{
	/* DO NOT consdier tlb in remote memory for now */

	struct vm_area_struct *vma;


	/* mm's last user has gone, and its about to be pulled down */


	if (mm->locked_vm) {
		vma = mm->mmap;
		while (vma) {
			if (vma->vm_flags & VM_LOCKED)
				munlock_vma_pages_all(vma);
			vma = vma->vm_next;
		}
	}




	vma = mm->mmap;
	if (!vma)	/* Can happen if dup_mmap() received an OOM */
		return;

#if 0
	lru_add_drain();
	flush_cache_mm(mm);
	tlb_gather_mmu(&tlb, mm, 0, -1);

	/* Use -1 here to ensure all VMAs in the mm are unmapped */
	unmap_vmas(&tlb, vma, 0, -1);

	if (unlikely(mm_is_oom_victim(mm))) {
		/*
		 * Wait for oom_reap_task() to stop working on this
		 * mm. Because MMF_OOM_SKIP is already set before
		 * calling down_read(), oom_reap_task() will not run
		 * on this "mm" post up_write().
		 *
		 * mm_is_oom_victim() cannot be set from under us
		 * either because victim->mm is already set to NULL
		 * under task_lock before calling mmput and oom_mm is
		 * set not NULL by the OOM killer only if victim->mm
		 * is found not NULL while holding the task_lock.
		 */
		set_bit(MMF_OOM_SKIP, &mm->flags);
		down_write(&mm->mmap_sem);
		up_write(&mm->mmap_sem);
	}
	free_pgtables(&tlb, vma, FIRST_USER_ADDRESS, USER_PGTABLES_CEILING);
	tlb_finish_mmu(&tlb, 0, -1);

	/*
	 * Walk the list again, actually closing and freeing it,
	 * with preemption enabled, without holding any MM locks.
	 */
	while (vma) {
		if (vma->vm_flags & VM_ACCOUNT)
			nr_accounted += vma_pages(vma);
		vma = remove_vma(vma);
	}
	vm_unacct_memory(nr_accounted);
#endif
	mn_remove_vmas(mm);

	return;
}

// ASSUME we do not use mmu_notifier
/* this is called after the last mmu_notifier_unregister() returned */








/*
 * Called when the last reference to the mm
 * is dropped: either by a lazy thread or by
 * mmput. Free the page directory and the mm.
 */
void __mn_mmdrop(struct mm_struct *mm)
{





	check_mm(mm);

	

	kfree(mm);
}

static inline void mn_mmdrop(struct mm_struct *mm)
{
	if (unlikely(atomic_dec_and_test(&mm->mm_count)))
		__mn_mmdrop(mm);
}

static inline void __mn_mmput(struct mm_struct *mm)
{
	VM_BUG_ON(atomic_read(&mm->mm_users));

	uprobe_clear_state(mm);




	/* NO KERNEL SAME PAGE MERGING */


	/* NO HUGEPAG */





	mn_exit_mmap(mm);



	if (!list_empty(&mm->mmlist)) {
		spin_lock(&mmlist_lock);
		list_del(&mm->mmlist);
		spin_unlock(&mmlist_lock);
	}


	mn_mmdrop(mm);
}

void mn_mmput(struct mm_struct *mm)
{
	might_sleep();

	if (atomic_dec_and_test(&mm->mm_users))
		__mn_mmput(mm);
}

/*
 * Allocate a new mm structure and copy contents from the
 * mm structure of the passed in task structure.
 */
struct mm_struct *mn_dup_mm(struct task_struct *new_tsk,
                            struct task_struct *old_tsk)
{
	struct mm_struct *mm = NULL, *oldmm = NULL;
	int err;
	// We assuem that we already hold spinlock for old_tsk

	if (old_tsk)
		 oldmm = old_tsk->mm;

    // 1) allocation
	mm = kzalloc(sizeof(*mm), GFP_KERNEL);
	if (!mm)
		return NULL;

	if (oldmm){
		pr_info("Mn_dup_mm: copy mm meta-data: 0x%lx -> 0x%lx\n", 
			(unsigned long)oldmm, (unsigned long)mm);
		memcpy(mm, oldmm, sizeof(*mm));
	}

    // 2) initialization
	if (!mm_init(mm, new_tsk))	//mm->user_ns, old_tsk))
		goto fail_nomem;
	// other method
	// a) return mm_init(mm, current, current_user_ns());
	// b) LegoOS does not use namesapce

    // 3) actual copy
	if (oldmm)
	{
		err = dup_mmap(mm, oldmm);
		if (err){
			//error
			pr_info("Mn_dup_mm: Cannot dup_mmap (err: %d)\n", err);
			goto free_pt;
		}

		mm->hiwater_rss = get_mm_rss(mm);	// get counter fo rss stat
		mm->hiwater_vm = mm->total_vm;		// 
	}else{
		// if old_tsk == NULL, we just initialize mm
		mm->hiwater_rss = 0;				// no rss stat
		mm->hiwater_vm = mm->total_vm = 0;	// no vma 
	}
	







	return mm;

free_pt:
	/* don't put binfmt in mmput, we haven't got module yet */

	mn_mmput(mm);

fail_nomem:
	return NULL;
}

/*
 * mm/internal.h
 * Data area - private, writable, not stack
 */
static inline bool is_data_mapping(vm_flags_t flags)
{
	return (flags & (VM_WRITE | VM_SHARED | VM_STACK)) == VM_WRITE;
}

/*
 * Return true if the calling process may expand its vm space by the passed
 * number of pages
 */
static bool ignore_rlimit_data = false;	// do not ignore for now
bool may_expand_vm(struct mm_struct *mm, vm_flags_t flags, unsigned long npages)
{
	if (mm->total_vm + npages > rlimit(RLIMIT_AS) >> PAGE_SHIFT)
		return false;

	if (is_data_mapping(flags) &&	// sched/signal.h
	    mm->data_vm + npages > rlimit(RLIMIT_DATA) >> PAGE_SHIFT) {
		/* Workaround for Valgrind */
		if (rlimit(RLIMIT_DATA) == 0 &&
		    mm->data_vm + npages <= rlimit_max(RLIMIT_DATA) >> PAGE_SHIFT)
			return true;
		if (!ignore_rlimit_data) {
			pr_warn_once("VmData %lu exceed data ulimit %lu. Update limits or use boot option ignore_rlimit_data.\n",
				    //  current->comm, current->pid,
				     (mm->data_vm + npages) << PAGE_SHIFT,
				     rlimit(RLIMIT_DATA));
			return false;
		}
	}

	return true;
}

/*
 * Get rid of page table information in the indicated region.
 *
 * Called with the mm semaphore held.
 */
static void unmap_region(struct mm_struct *mm,
		struct vm_area_struct *vma, struct vm_area_struct *prev,
		unsigned long start, unsigned long end)
{











	return ;
}


/* Munmap is split into 2 main parts -- this part which finds
 * what needs doing, and the areas themselves, which do the
 * work.  This now handles partial unmappings.
 * 00000 <00000>
 */
int mn_munmap(struct mm_struct *mm, unsigned long start, size_t len)
{
	unsigned long end;
	struct vm_area_struct *vma, *prev, *last;

	if ((offset_in_page(start)) || start > TASK_SIZE || len > TASK_SIZE-start)	//linux/mm.h
		return -EINVAL;

	len = PAGE_ALIGN(len);	//linux/mm.h
	if (len == 0)
		return -EINVAL;

	/* Find the first overlapping VMA */
	vma = mn_find_vma(mm, start);
	if (!vma)
		return 0;
	prev = vma->vm_prev;
	/* we have  start < vma->vm_end  */

	/* if it doesn't overlap, we have nothing.. */
	end = start + len;
	if (vma->vm_start >= end)
		return 0;








	if (start > vma->vm_start) {
		int error;

		/*
		 * Make sure that map_count on return from munmap() will
		 * not exceed its limit; but let map_count go just above
		 * its limit temporarily, to help free resources as expected.
		 */
		if (end < vma->vm_end && mm->map_count >= sysctl_max_map_count)
			return -ENOMEM;

		error = __mn_split_vma(mm, vma, start, 0);
		if (error)
			return error;
		prev = vma;
	}


	last = mn_find_vma(mm, end);
	if (last && end > last->vm_start) {
		int error = __mn_split_vma(mm, last, end, 1);
		if (error)
			return error;
	}
	vma = prev ? prev->vm_next : mm->mmap;








	/*
	 * unlock any mlock()ed ranges before detaching vmas
	 */











	/*
	 * Remove the vma's, and unmap the actual pages
	 */
	detach_vmas_to_be_unmapped(mm, vma, prev, end);




	/* Fix up all other VM information */
	remove_vma_list(mm, vma);




	return 0;
}

/*
 * Functions for mremap
 */
/*
 * Copy the vma structure to a new location in the same mm,
 * prior to moving page table entries, to effect an mremap move.
 */
static struct vm_area_struct *mn_copy_vma(struct vm_area_struct **vmap,
	unsigned long addr, unsigned long len, pgoff_t pgoff,
	bool *need_rmap_locks)
{
	struct vm_area_struct *vma = *vmap;
	unsigned long vma_start = vma->vm_start;
	struct mm_struct *mm = vma->vm_mm;
	struct vm_area_struct *new_vma, *prev;
	struct rb_node **rb_link, *rb_parent;
	bool faulted_in_anon_vma = true;

	/*
	 * If anonymous vma has not yet been faulted, update new pgoff
	 * to match new location, to increase its chance of merging.
	 */
	if (unlikely(vma_is_anonymous(vma))) {	//mm.h
		pgoff = addr >> PAGE_SHIFT;
		faulted_in_anon_vma = false;
	}

	if (mn_find_vma_links(mm, addr, addr + len, &prev, &rb_link, &rb_parent))
		return NULL;	/* should never get here */
	new_vma = mn_vma_merge(mm, prev, addr, addr + len, vma->vm_flags,
			    vma->anon_vma, vma->vm_file, pgoff, NULL);
	if (new_vma) {
		/*
		 * Source vma may have been merged into new_vma
		 */
		if (unlikely(vma_start >= new_vma->vm_start &&
			     vma_start < new_vma->vm_end)) {
			/*
			 * The only way we can get a vma_merge with
			 * self during an mremap is if the vma hasn't
			 * been faulted in yet and we were allowed to
			 * reset the dst vma->vm_pgoff to the
			 * destination address of the mremap to allow
			 * the merge to happen. mremap must change the
			 * vm_pgoff linearity between src and dst vmas
			 * (in turn preventing a vma_merge) to be
			 * safe. It is only safe to keep the vm_pgoff
			 * linear if there are no pages mapped yet.
			 */
			VM_BUG_ON_VMA(faulted_in_anon_vma, new_vma);
			*vmap = vma = new_vma;
		}
		*need_rmap_locks = (new_vma->vm_pgoff <= vma->vm_pgoff);
	} else {
		new_vma = kmalloc(sizeof(*new_vma), GFP_KERNEL);
		if (!new_vma)
			goto out;
		*new_vma = *vma;
		new_vma->vm_start = addr;
		new_vma->vm_end = addr + len;
		new_vma->vm_pgoff = pgoff;

		if(vma->vm_private_data)
		{
			new_vma->vm_private_data = vzalloc(len);
			if (!(new_vma->vm_private_data))
			{
				goto out_free_vma;
			}
			memcpy(new_vma->vm_private_data, vma->vm_private_data,
					min(len, vma->vm_end - vma->vm_start));
		}else
			new_vma->vm_private_data = NULL;













		vma_link(mm, new_vma, prev, rb_link, rb_parent);
		*need_rmap_locks = false;
	}
	return new_vma;



out_free_vma:

	kfree(new_vma);
out:
	return NULL;
}


static unsigned long move_vma(struct vm_area_struct *vma,
		unsigned long old_addr, unsigned long old_len,
		unsigned long new_len, unsigned long new_addr,
		bool *locked)
{
	struct mm_struct *mm = vma->vm_mm;
	struct vm_area_struct *new_vma;
	unsigned long vm_flags = vma->vm_flags;
	unsigned long new_pgoff;

	unsigned long excess = 0;
	unsigned long hiwater_vm;
	int split = 0;

	bool need_rmap_locks;

	/*
	 * We'd prefer to avoid failure later on in do_munmap:
	 * which may split one vma into three before unmapping.
	 */
	if (mm->map_count >= sysctl_max_map_count - 3)
		return -ENOMEM;

	/*
	 * Advise KSM to break any KSM pages in the area to be moved:
	 * it would be confusing if they were to turn up at the new
	 * location, where they happen to coincide with different KSM
	 * pages recently unmapped.  But leave vma->vm_flags as it was,
	 * so KSM can come around to merge on vma and new_vma afterwards.
	 */





	new_pgoff = vma->vm_pgoff + ((old_addr - vma->vm_start) >> PAGE_SHIFT);
	new_vma = mn_copy_vma(&vma, new_addr, new_len, new_pgoff,
			   &need_rmap_locks);
	if (!new_vma)
		return -ENOMEM;

	/*
	 *  ASSUME we do not consider page-table related update for now
	 */

























	/* Conceal VM_ACCOUNT so old reservation is not undone */
	if (vm_flags & VM_ACCOUNT) {
		vma->vm_flags &= ~VM_ACCOUNT;
		excess = vma->vm_end - vma->vm_start - old_len;
		if (old_addr > vma->vm_start &&
		    old_addr + old_len < vma->vm_end)
			split = 1;
	}

	/*
	 * If we failed to move page tables we still do total_vm increment
	 * since do_munmap() will decrement it by old_len == new_len.
	 *
	 * Since total_vm is about to be raised artificially high for a
	 * moment, we need to restore high watermark afterwards: if stats
	 * are taken meanwhile, total_vm and hiwater_vm appear too high.
	 * If this were a serious issue, we'd add a flag to do_munmap().
	 */
	hiwater_vm = mm->hiwater_vm;
	vm_stat_account(mm, vma->vm_flags, new_len >> PAGE_SHIFT);

	/* Tell pfnmap has moved from this vma */



	if (mn_munmap(mm, old_addr, old_len) < 0) {
		/* OOM: unable to split vma, just get accounts right */

		excess = 0;
	}
	mm->hiwater_vm = hiwater_vm;

	/* Restore VM_ACCOUNT if one or two pieces of vma left */
	if (excess) {
		vma->vm_flags |= VM_ACCOUNT;
		if (split)
			vma->vm_next->vm_flags |= VM_ACCOUNT;
	}

	if (vm_flags & VM_LOCKED) {
		mm->locked_vm += new_len >> PAGE_SHIFT;
		*locked = true;
	}

	return new_addr;
}

static struct vm_area_struct *vma_to_resize(struct task_struct *tsk,
	unsigned long addr, unsigned long old_len, unsigned long new_len, 
	unsigned long *p)
{
	struct mm_struct *mm = tsk->mm;
	struct vm_area_struct *vma = mn_find_vma(mm, addr);
	unsigned long pgoff;

	if (!vma || vma->vm_start > addr)
		return ERR_PTR(-EFAULT);

	/*
	 * !old_len is a special case where an attempt is made to 'duplicate'
	 * a mapping.  This makes no sense for private mappings as it will
	 * instead create a fresh/new mapping unrelated to the original.  This
	 * is contrary to the basic idea of mremap which creates new mappings
	 * based on the original.  There are no known use cases for this
	 * behavior.  As a result, fail such attempts.
	 */
	if (!old_len && !(vma->vm_flags & (VM_SHARED | VM_MAYSHARE))) {
		pr_warn_once("%s (%d): attempted to duplicate a private mapping with mremap.  This is not supported.\n", tsk->comm, tsk->pid);
		return ERR_PTR(-EINVAL);
	}

	if (is_vm_hugetlb_page(vma))
		return ERR_PTR(-EINVAL);

	/* We can't remap across vm area boundaries */
	if (old_len > vma->vm_end - addr)
		return ERR_PTR(-EFAULT);

	if (new_len == old_len)
		return vma;

	/* Need to be careful about a growing mapping */
	pgoff = (addr - vma->vm_start) >> PAGE_SHIFT;
	pgoff += vma->vm_pgoff;
	if (pgoff + (new_len >> PAGE_SHIFT) < pgoff)
		return ERR_PTR(-EINVAL);

	if (vma->vm_flags & (VM_DONTEXPAND | VM_PFNMAP))
		return ERR_PTR(-EFAULT);

	if (vma->vm_flags & VM_LOCKED) {
		unsigned long locked, lock_limit;
		locked = mm->locked_vm << PAGE_SHIFT;
		lock_limit = rlimit(RLIMIT_MEMLOCK);
		locked += new_len - old_len;
		if (locked > lock_limit && !capable(CAP_IPC_LOCK))
			return ERR_PTR(-EAGAIN);
	}

	if (!may_expand_vm(mm, vma->vm_flags,
				(new_len - old_len) >> PAGE_SHIFT))
		return ERR_PTR(-ENOMEM);

	if (vma->vm_flags & VM_ACCOUNT) {
		unsigned long charged = (new_len - old_len) >> PAGE_SHIFT;


		*p = charged;
	}

	return vma;
}

static unsigned long mremap_to(struct task_struct *tsk,
		unsigned long addr, unsigned long old_len,
		unsigned long new_addr, unsigned long new_len, bool *locked)



{
	struct mm_struct *mm = tsk->mm;
	struct vm_area_struct *vma;
	unsigned long ret = -EINVAL;
	unsigned long charged = 0;
	unsigned long map_flags;

	if (offset_in_page(new_addr))
		goto out;

	if (new_len > TASK_SIZE || new_addr > TASK_SIZE - new_len)
		goto out;

	/* Ensure the old/new locations do not overlap */
	if (addr + old_len > new_addr && new_addr + new_len > addr)
		goto out;

	ret = mn_munmap(mm, new_addr, new_len);
	if (ret)
		goto out;

	if (old_len >= new_len) {
		ret = mn_munmap(mm, addr+new_len, old_len - new_len);
		if (ret && old_len != new_len)
			goto out;
		old_len = new_len;
	}

	vma = vma_to_resize(tsk, addr, old_len, new_len, &charged);
	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		goto out;
	}

	map_flags = MAP_FIXED;
	if (vma->vm_flags & VM_MAYSHARE)
		map_flags |= MAP_SHARED;

	ret = mn_get_unmapped_area(tsk, new_addr, new_len, vma->vm_pgoff +
				((addr - vma->vm_start) >> PAGE_SHIFT),
				map_flags, vma->vm_file);
	if (offset_in_page(ret))
		goto out1;

	ret = move_vma(vma, addr, old_len, new_len, new_addr, locked);
	// , uf, uf_unmap);
	if (!(offset_in_page(ret)))
		goto out;
out1:


out:
	return ret;
}

static int vma_expandable(struct task_struct *tsk,
						  struct vm_area_struct *vma, unsigned long delta)
{
	unsigned long end = vma->vm_end + delta;
	if (end < vma->vm_end) /* overflow */
		return 0;
	if (vma->vm_next && vma->vm_next->vm_start < end) /* intersection */
		return 0;
	if (mn_get_unmapped_area(tsk, vma->vm_start, end - vma->vm_start,
			      0, MAP_FIXED, NULL) & ~PAGE_MASK)
		return 0;
	return 1;
}

unsigned long mn_mremap(struct task_struct *tsk, unsigned long addr, unsigned long old_len,
			unsigned long new_len, unsigned long flags,	unsigned long new_addr)
{
	struct mm_struct *mm = tsk->mm;
	struct vm_area_struct *vma;
	unsigned long ret = 0;
	unsigned long charged = 0;
	bool locked = false;



	
	if (flags & ~(MREMAP_FIXED | MREMAP_MAYMOVE))
		return ret;

	if (flags & MREMAP_FIXED && !(flags & MREMAP_MAYMOVE))
		return ret;

	if (offset_in_page(addr))
		return ret;

	old_len = PAGE_ALIGN(old_len);
	new_len = PAGE_ALIGN(new_len);

	/*
	 * We allow a zero old-len as a special case
	 * for DOS-emu "duplicate shm area" thing. But
	 * a zero new-len is nonsensical.
	 */
	if (!new_len)
		return ret;

	if (flags & MREMAP_FIXED) {
		ret = mremap_to(tsk, addr, old_len, new_addr, new_len, &locked);
		//, &uf, &uf_unmap_early, &uf_unmap);
		goto out;
	}

	/*
	 * Always allow a shrinking remap: that just unmaps
	 * the unnecessary pages..
	 * do_munmap does all the needed commit accounting
	 */
	if (old_len >= new_len) {
		ret = mn_munmap(mm, addr+new_len, old_len - new_len);
		if (ret && old_len != new_len)
			goto out;
		ret = addr;
		goto out;
	}

	/*
	 * Ok, we need to grow..
	 */
	vma = vma_to_resize(tsk, addr, old_len, new_len, &charged);
	if (IS_ERR(vma)) {
		ret = 0;
		goto out;
	}

	/* old_len exactly to the end of the area..
	 */
	if (old_len == vma->vm_end - addr) {

		if (vma_expandable(tsk, vma, new_len - old_len)) {
			int pages = (new_len - old_len) >> PAGE_SHIFT;

			if (__vma_adjust(vma, vma->vm_start, addr + new_len,
				       vma->vm_pgoff, NULL, NULL)) {
				ret = 0;
				goto out;
			}

			vm_stat_account(mm, vma->vm_flags, pages);
			if (vma->vm_flags & VM_LOCKED) {
				mm->locked_vm += pages;
				locked = true;
				new_addr = addr;
			}
			ret = addr;
			goto out;
		}
	}

	/*
	 * We weren't able to just expand or shrink the area,
	 * we need to create a new one and move it..
	 */
	ret = 0;
	if (flags & MREMAP_MAYMOVE) {
		unsigned long map_flags = 0;
		if (vma->vm_flags & VM_MAYSHARE)
			map_flags |= MAP_SHARED;

		new_addr = mn_get_unmapped_area(tsk, 0, new_len,
					vma->vm_pgoff + ((addr - vma->vm_start) >> PAGE_SHIFT),
					map_flags, vma->vm_file);
		if (offset_in_page(new_addr)) {
			ret = new_addr;
			goto out;
		}

		ret = move_vma(vma, addr, old_len, new_len, new_addr, &locked);
		// , &uf, &uf_unmap);
	}
out:
	if (offset_in_page(ret)) {

		locked = 0;
	}







	return ret;
}

/*
 * We account for memory if it's a private writeable mapping,
 * not hugepages and VM_NORESERVE wasn't set.
 */
static inline int accountable_mapping(struct file *file, vm_flags_t vm_flags)
{
	return (vm_flags & (VM_NORESERVE | VM_SHARED | VM_WRITE)) == VM_WRITE;
}

/*
 * Executable code area - executable, not writable, not stack
 */
static inline bool is_exec_mapping(vm_flags_t flags)
{
	return (flags & (VM_EXEC | VM_WRITE | VM_STACK)) == VM_EXEC;
}

/*
 * Stack area - atomatically grows in one direction
 *
 * VM_GROWSUP / VM_GROWSDOWN VMAs are always private anonymous:
 * do_mmap() forbids all other combinations.
 */
static inline bool is_stack_mapping(vm_flags_t flags)
{
	return (flags & VM_STACK) == VM_STACK;
}

// DATA is already declared
/*
 * Data area - private, writable, not stack
 */





void vm_stat_account(struct mm_struct *mm, vm_flags_t flags, long npages)
{
	mm->total_vm += npages;

	if (is_exec_mapping(flags))
		mm->exec_vm += npages;
	else if (is_stack_mapping(flags))
		mm->stack_vm += npages;
	else if (is_data_mapping(flags))
		mm->data_vm += npages;
	


}

//vm_get_page_prot is exposed from mm/mmap.c
static pgprot_t vm_pgprot_modify(pgprot_t oldprot, unsigned long vm_flags)
{
	return pgprot_modify(oldprot, vm_get_page_prot(vm_flags));
}

/* Update vma->vm_page_prot to reflect vma->vm_flags. */
void vma_set_page_prot(struct vm_area_struct *vma)
{
	unsigned long vm_flags = vma->vm_flags;
	pgprot_t vm_page_prot;

	vm_page_prot = vm_pgprot_modify(vma->vm_page_prot, vm_flags);





	/* remove_protection_ptes reads vma->vm_page_prot without mmap_sem */
	WRITE_ONCE(vma->vm_page_prot, vm_page_prot);
}

static struct vm_area_struct *mn_allocate_vma(struct mm_struct *mm, unsigned long addr,
		unsigned long len, vm_flags_t vm_flags, unsigned long pgoff, int is_file)
{
	struct vm_area_struct *vma;
	vma = kzalloc(sizeof(*vma), GFP_KERNEL);
	if (!vma) {
		return NULL;
	}

	vma->vm_mm = mm;
	vma->vm_start = addr;
	vma->vm_end = addr + len;
	vma->vm_flags = vm_flags;
	vma->vm_page_prot = vm_get_page_prot(vm_flags);
	vma->vm_pgoff = pgoff;
	vma->vm_private_data = NULL;
	INIT_LIST_HEAD(&vma->anon_vma_chain);




	if (!is_file)
	{
		vma->vm_private_data = vzalloc(len);
		if (!(vma->vm_private_data)){
			kfree(vma);
			return NULL;		
		}
	}
	return vma;
}

unsigned long mn_mmap_region(struct task_struct* tsk, unsigned long addr,
		unsigned long len, vm_flags_t vm_flags, unsigned long pgoff, struct file *file)
{
	struct mm_struct *mm = tsk->mm;
	struct vm_area_struct *vma, *prev;

	struct rb_node **rb_link, *rb_parent;
	unsigned long charged = 0;

	/* Check against address space limit. */
	if (!may_expand_vm(mm, vm_flags, len >> PAGE_SHIFT)) {
		unsigned long nr_pages;

		/*
		 * MAP_FIXED may remove pages of mappings that intersects with
		 * requested mapping. Account for the pages it would unmap.
		 */
		nr_pages = count_vma_pages_range(mm, addr, addr + len);

		if (!may_expand_vm(mm, vm_flags,
					(len >> PAGE_SHIFT) - nr_pages))
			return -ENOMEM;
	}

	/* Clear old maps */
	while (mn_find_vma_links(mm, addr, addr + len, &prev, &rb_link,
			      &rb_parent)) {
		if (mn_munmap(mm, addr, len))
			return -ENOMEM;
	}

	/*
	 * Private writable mapping: check memory availability
	 */
	if (accountable_mapping(file, vm_flags)) {
		charged = len >> PAGE_SHIFT;




		vm_flags |= VM_ACCOUNT;
	}




	vma = mn_vma_merge(mm, prev, addr, addr + len, vm_flags,
					NULL, file, pgoff, NULL);
	if (vma)
		goto out;

	/*
	 * Determine the object being mapped and call the appropriate
	 * specific mapper. the address has already been validated, but
	 * not unmapped, but the maps are removed from the list.
	 */

	vma = mn_allocate_vma(mm, addr, len, vm_flags, pgoff, (int)file);
	if(!vma)
		goto unacct_error;

#if 0
	if (file) {
		if (vm_flags & VM_DENYWRITE) {
			error = deny_write_access(file);
			if (error)
				goto free_vma;
		}
		if (vm_flags & VM_SHARED) {
			error = mapping_map_writable(file->f_mapping);
			if (error)
				goto allow_write_and_free_vma;
		}

		/* ->mmap() can change vma->vm_file, but must guarantee that
		 * vma_link() below can deny write-access if VM_DENYWRITE is set
		 * and map writably if VM_SHARED is set. This usually means the
		 * new file must not have been exposed to user-space, yet.
		 */
		vma->vm_file = get_file(file);	// increase counter
		error = call_mmap(file, vma);	//f->f_op->mmap
		if (error)
			goto unmap_and_free_vma;








		WARN_ON_ONCE(addr != vma->vm_start);

		addr = vma->vm_start;
		vm_flags = vma->vm_flags;
	} else if (vm_flags & VM_SHARED) {
		error = shmem_zero_setup(vma);
		if (error)
			goto free_vma;
	}
#else
	//dummy file
	if (file) {
		vma->vm_file = file;




	}
#endif

	vma_link(mm, vma, prev, rb_link, rb_parent);








out:


	vm_stat_account(mm, vm_flags, len >> PAGE_SHIFT);	//increase total vm
	if (vm_flags & VM_LOCKED) {
		mm->locked_vm += (len >> PAGE_SHIFT);
	}











	/*
	 * New (or expanded) vma always get soft dirty status.
	 * Otherwise user-space soft-dirty page tracker won't
	 * be able to distinguish situation when vma area unmapped,
	 * then new mapped in-place (which must be aimed as
	 * a completely new data area).
	 */
	// ASSUME no user space tracking
	// vma->vm_flags |= VM_SOFTDIRTY;

	vma_set_page_prot(vma);

	return addr;

// unmap_and_free_vma:
	vma->vm_file = NULL;


	/* Undo any partial mapping done by a device driver. */








unacct_error:



	return -EINTR;	//no address = NULL
}

/*
 * If a hint addr is less than mmap_min_addr change hint to be as
 * low as possible but still greater than mmap_min_addr
 */
#define mmap_min_addr		0UL
inline unsigned long mn_round_hint_to_min(unsigned long hint)
{
	hint &= PAGE_MASK;
	if (((void *)hint != NULL) &&
	    (hint < mmap_min_addr))
		return PAGE_ALIGN(mmap_min_addr);
	return hint;
}

inline int mn_mlock_future_check(struct mm_struct *mm,
				     unsigned long flags,
				     unsigned long len)
{
	unsigned long locked, lock_limit;


	if (flags & VM_LOCKED) {
		locked = len >> PAGE_SHIFT;
		locked += mm->locked_vm;
		lock_limit = rlimit(RLIMIT_MEMLOCK);
		lock_limit >>= PAGE_SHIFT;
		if (locked > lock_limit && !capable(CAP_IPC_LOCK))
			return -EAGAIN;
	}
	return 0;
}

unsigned long
mn_get_unmapped_area(struct task_struct *tsk, unsigned long addr, unsigned long len,
		    unsigned long pgoff, unsigned long flags, struct file *file)
{




	unsigned long error = arch_mmap_check(addr, len, flags);

	// pr_info("mn_get_unmapped_area: err %lu, addr: 0x%lx, len: 0x%lx, flags: 0x%lx\n",
	// 		error, addr, len, flags);

	if (error)
		return error;

	/* Careful about overflows.. */
	if (len > TASK_SIZE)
		return -ENOMEM;


#if 0
	if (file) {
		if (file->f_op->get_unmapped_area)
			get_area = file->f_op->get_unmapped_area;
	} else if (flags & MAP_SHARED) {
		/*
		 * mmap_region() will call shmem_zero_setup() to create a file,
		 * so use shmem's get_unmapped_area in case it can be huge.
		 * do_mmap_pgoff() will clear pgoff, so match alignment.
		 */
		pgoff = 0;
		get_area = shmem_get_unmapped_area;
	}
#endif
	addr = mn_arch_get_unmapped_area_topdown(tsk, file, addr, len, pgoff, flags);
	if (IS_ERR_VALUE(addr))
		return addr;

	if (addr > TASK_SIZE - len)
		return -ENOMEM;
	if (offset_in_page(addr))
		return -EINVAL;



	return addr;
}

inline unsigned long
mn_vm_unmapped_area(struct task_struct *tsk, struct vm_unmapped_area_info *info)
{
	if (info->flags & VM_UNMAPPED_AREA_TOPDOWN)
		return mn_unmapped_area_topdown(tsk, info);
	else
		return mn_unmapped_area(tsk, info);
}

// for vm_unmapped_area from linux/mm.h
unsigned long mn_unmapped_area(struct task_struct *tsk, 
		struct vm_unmapped_area_info *info)
{
	/*
	 * We implement the search by looking for an rbtree node that
	 * immediately follows a suitable gap. That is,
	 * - gap_start = vma->vm_prev->vm_end <= info->high_limit - length;
	 * - gap_end   = vma->vm_start        >= info->low_limit  + length;
	 * - gap_end - gap_start >= length
	 */

	struct mm_struct *mm = tsk->mm;
	struct vm_area_struct *vma;
	unsigned long length, low_limit, high_limit, gap_start, gap_end;

	/* Adjust search length to account for worst case alignment overhead */
	length = info->length + info->align_mask;
	if (length < info->length)
		return -ENOMEM;

	/* Adjust search limits by the desired length */
	if (info->high_limit < length)
		return -ENOMEM;
	high_limit = info->high_limit - length;

	if (info->low_limit > high_limit)
		return -ENOMEM;
	low_limit = info->low_limit + length;

	/* Check if rbtree root looks promising */
	if (RB_EMPTY_ROOT(&mm->mm_rb))
		goto check_highest;
	vma = rb_entry(mm->mm_rb.rb_node, struct vm_area_struct, vm_rb);
	if (vma->rb_subtree_gap < length)
		goto check_highest;

	while (true) {
		/* Visit left subtree if it looks promising */
		gap_end = vm_start_gap(vma);
		if (gap_end >= low_limit && vma->vm_rb.rb_left) {
			struct vm_area_struct *left =
				rb_entry(vma->vm_rb.rb_left,
					 struct vm_area_struct, vm_rb);
			if (left->rb_subtree_gap >= length) {
				vma = left;
				continue;
			}
		}

		gap_start = vma->vm_prev ? vm_end_gap(vma->vm_prev) : 0;
check_current:
		/* Check if current node has a suitable gap */
		if (gap_start > high_limit)
			return -ENOMEM;
		if (gap_end >= low_limit &&
		    gap_end > gap_start && gap_end - gap_start >= length)
			goto found;

		/* Visit right subtree if it looks promising */
		if (vma->vm_rb.rb_right) {
			struct vm_area_struct *right =
				rb_entry(vma->vm_rb.rb_right,
					 struct vm_area_struct, vm_rb);
			if (right->rb_subtree_gap >= length) {
				vma = right;
				continue;
			}
		}

		/* Go back up the rbtree to find next candidate node */
		while (true) {
			struct rb_node *prev = &vma->vm_rb;
			if (!rb_parent(prev))
				goto check_highest;
			vma = rb_entry(rb_parent(prev),
				       struct vm_area_struct, vm_rb);
			if (prev == vma->vm_rb.rb_left) {
				gap_start = vm_end_gap(vma->vm_prev);
				gap_end = vm_start_gap(vma);
				goto check_current;
			}
		}
	}

check_highest:
	/* Check highest gap, which does not precede any rbtree node */
	gap_start = mm->highest_vm_end;
	gap_end = ULONG_MAX;  /* Only for VM_BUG_ON below */
	if (gap_start > high_limit)
		return -ENOMEM;

found:
	/* We found a suitable gap. Clip it with the original low_limit. */
	if (gap_start < info->low_limit)
		gap_start = info->low_limit;

	/* Adjust gap address to the desired alignment */
	gap_start += (info->align_offset - gap_start) & info->align_mask;

	VM_BUG_ON(gap_start + info->length > info->high_limit);
	VM_BUG_ON(gap_start + info->length > gap_end);
	return gap_start;
}

unsigned long mn_unmapped_area_topdown(struct task_struct *tsk, 
		struct vm_unmapped_area_info *info)
{
	struct mm_struct *mm = tsk->mm;
	struct vm_area_struct *vma;
	unsigned long length, low_limit, high_limit, gap_start, gap_end;

	// pr_info("mn_unmapped_area_topdown: gap_start: 0x%lx, gap_end: 0x%lx, len: 0x%lx\n",
	// 		mm->highest_vm_end, info->high_limit, info->length);

	/* Adjust search length to account for worst case alignment overhead */
	length = info->length + info->align_mask;
	if (length < info->length)
		return -ENOMEM;

	/*
	 * Adjust search limits by the desired length.
	 * See implementation comment at top of unmapped_area().
	 */
	gap_end = info->high_limit;
	if (gap_end < length)
		return -ENOMEM;
	high_limit = gap_end - length;

	if (info->low_limit > high_limit)
		return -ENOMEM;
	low_limit = info->low_limit + length;

	/* Check highest gap, which does not precede any rbtree node */
	gap_start = mm->highest_vm_end;
	if (gap_start <= high_limit)
		goto found_highest;

	/* Check if rbtree root looks promising */
	if (RB_EMPTY_ROOT(&mm->mm_rb))
		return -ENOMEM;
	vma = rb_entry(mm->mm_rb.rb_node, struct vm_area_struct, vm_rb);
	if (vma->rb_subtree_gap < length)
		return -ENOMEM;

	while (true) {
		/* Visit right subtree if it looks promising */
		gap_start = vma->vm_prev ? vm_end_gap(vma->vm_prev) : 0;
		if (gap_start <= high_limit && vma->vm_rb.rb_right) {
			struct vm_area_struct *right =
				rb_entry(vma->vm_rb.rb_right,
					 struct vm_area_struct, vm_rb);
			if (right->rb_subtree_gap >= length) {
				vma = right;
				continue;
			}
		}

check_current:
		/* Check if current node has a suitable gap */
		gap_end = vm_start_gap(vma);
		if (gap_end < low_limit)
			return -ENOMEM;
		
		// pr_info("mn_unmapped_area_topdown: gap_start: 0x%lx, gap_end: 0x%lx, len: 0x%lx\n",
		// 	gap_start, gap_end, length);
		if (gap_start <= high_limit &&
		    gap_end > gap_start && gap_end - gap_start >= length)
			goto found;

		/* Visit left subtree if it looks promising */
		if (vma->vm_rb.rb_left) {
			struct vm_area_struct *left =
				rb_entry(vma->vm_rb.rb_left,
					 struct vm_area_struct, vm_rb);
			if (left->rb_subtree_gap >= length) {
				vma = left;
				continue;
			}
		}

		/* Go back up the rbtree to find next candidate node */
		while (true) {
			struct rb_node *prev = &vma->vm_rb;
			if (!rb_parent(prev))
				return -ENOMEM;
			vma = rb_entry(rb_parent(prev),
				       struct vm_area_struct, vm_rb);
			if (prev == vma->vm_rb.rb_right) {
				gap_start = vma->vm_prev ?
					vm_end_gap(vma->vm_prev) : 0;
				goto check_current;
			}
		}
	}

found:
	/* We found a suitable gap. Clip it with the original high_limit. */
	if (gap_end > info->high_limit)
		gap_end = info->high_limit;

found_highest:
	/* Compute highest gap address at the desired alignment */
	gap_end -= info->length;
	gap_end -= (gap_end - info->align_offset) & info->align_mask;

	VM_BUG_ON(gap_end < info->low_limit);
	VM_BUG_ON(gap_end < gap_start);
	return gap_end;
}

/*
 *  this is really a simplified "do_mmap".  it only handles
 *  anonymous maps.  eventually we may be able to do some
 *  brk-specific accounting here.
 */
int mn_do_brk_flags(struct task_struct *tsk, unsigned long addr, 
					unsigned long request, unsigned long flags)
{
	struct mm_struct *mm = tsk->mm;
	struct vm_area_struct *vma, *prev;
	unsigned long len;
	struct rb_node **rb_link, *rb_parent;
	pgoff_t pgoff = addr >> PAGE_SHIFT;
	int error;

	len = PAGE_ALIGN(request);
	if (len < request)
		return -ENOMEM;
	if (!len)
		return 0;

	/* Until we need other flags, refuse anything except VM_EXEC. */
	if ((flags & (~VM_EXEC)) != 0)
		return -EINVAL;
	flags |= VM_DATA_DEFAULT_FLAGS | VM_ACCOUNT | mm->def_flags;

	error = mn_get_unmapped_area(tsk, addr, len, 0, MAP_FIXED, NULL);
	if (offset_in_page(error))
		return error;





	/*
	 * mm->mmap_sem is required to protect against another thread
	 * changing the mappings in case we sleep.
	 */


	/*
	 * Clear old maps.  this also does some error checking for us
	 */
	while (mn_find_vma_links(mm, addr, addr + len, &prev, &rb_link,
			      &rb_parent)) {
		if (mn_munmap(mm, addr, len))
			return -ENOMEM;
	}

	/* Check against address space limits *after* clearing old maps... */
	if (!may_expand_vm(mm, flags, len >> PAGE_SHIFT))
		return -ENOMEM;

	if (mm->map_count > sysctl_max_map_count)
		return -ENOMEM;





	vma = mn_vma_merge(mm, prev, addr, addr + len, flags,
			NULL, NULL, pgoff, NULL);
	if (vma)
		goto out;

	/*
	 * create a vma struct for an anonymous mapping
	 */
	vma = mn_allocate_vma(mm, addr, len, flags, pgoff, 0);
	if (!vma) {
		return -ENOMEM;
	}
	vma_link(mm, vma, prev, rb_link, rb_parent);
out:

	vm_stat_account(mm, flags, len >> PAGE_SHIFT);
	if (vma->vm_flags & VM_LOCKED) {
		mm->locked_vm += (len >> PAGE_SHIFT);
	}


	vma->vm_flags |= VM_SOFTDIRTY;


	pr_info("BRK: tgid: %d, addr: 0x%lx, len: %lu, flag: 0x%lx\n", 
		(int)tsk->tgid, addr, len, flags);

	return 0;
}

unsigned long
mn_push_data_to_vmas(struct task_struct *tsk, char* data, unsigned long addr, unsigned long len)
{
	//find vmas and copy data
	struct vm_area_struct *cur = mn_find_vma(tsk->mm, addr);
	int copied = 0;
	unsigned long copy_len = 0, offset = 0, data_offset = 0;

	if(cur && (cur->vm_start <= addr))
	{
		do {
			offset = max(addr, cur->vm_start) - cur->vm_start;
			data_offset =  max(addr, cur->vm_start) - addr;
			copy_len = min(cur->vm_end - addr, len);

			//if private data is NULL, we also need to initialize it
			if (!cur->vm_private_data)
			{
				cur->vm_private_data = vmalloc(cur->vm_end - cur->vm_start);
				if (!cur->vm_private_data)
					return -ENOMEM;
			}
			pr_info("DATA: tgid: %d, addr: 0x%lx, len: %lu\n", (int)tsk->tgid, addr, len);
			DEBUG_print_one_vma(cur, -1);

			memcpy(cur->vm_private_data + offset, data + data_offset, copy_len);
			copied += copy_len;

			// if this vma is the last one
			if (cur->vm_end >= addr + len)
				break;

			cur = cur->vm_next;
		}while (cur);
	}
	barrier();

	return copied;
}
