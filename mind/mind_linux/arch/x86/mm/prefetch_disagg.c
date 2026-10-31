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


#include <disagg/fault_disagg.h>

#include <disagg/cnthread_disagg.h>
#include <disagg/print_disagg.h>
#include <disagg/profile_points_disagg.h>

static spinlock_t cnthread_pgfault_stat_lock;
static atomic_t cnthread_pgfault_stat_counter;
static LIST_HEAD(cnthread_pgfault_stat_list);

void init_pgfault_prefetch(void)
{
    spin_lock_init(&cnthread_pgfault_stat_lock);
	atomic_set(&cnthread_pgfault_stat_counter, 0);
}

struct fault_stat_struct *pfet_get_struct(void)
{
	if (atomic_read(&cnthread_pgfault_stat_counter) > 0 && !list_empty(&cnthread_pgfault_stat_list))
	{
		struct fault_stat_struct *pfet_stat = NULL;
		if (spin_trylock(&cnthread_pgfault_stat_lock))
		{
			if (!list_empty(&cnthread_pgfault_stat_list))
			{
				// get the oldest entry
				pfet_stat = container_of(cnthread_pgfault_stat_list.prev, struct fault_stat_struct, node);
				list_del(&pfet_stat->node);
				atomic_dec(&cnthread_pgfault_stat_counter);
			}
			spin_unlock(&cnthread_pgfault_stat_lock);
			return pfet_stat;
		}
	}
	return NULL;
}

int pfet_add_struct(struct task_struct *tsk, u64 fva)
{
	if (atomic_read(&cnthread_pgfault_stat_counter) < PREFETCH_PGFAULT_MAX_STAT)
	{
		struct fault_stat_struct *pfet_stat = kzalloc(sizeof(struct fault_stat_struct), GFP_KERNEL);
		pfet_stat->tsk = tsk;
		pfet_stat->vaddr= fva;
		spin_lock(&cnthread_pgfault_stat_lock);
		list_add(&pfet_stat->node, &cnthread_pgfault_stat_list);
		atomic_inc(&cnthread_pgfault_stat_counter);
		spin_unlock(&cnthread_pgfault_stat_lock);
		return 0;
	}
	return -1;
}

static int pfet_prefetch_task(void *pfet_argv);

/* Main routine of the prefetch to generate a prefetch request */
void pfet_prefetch_entry(void)
{
	struct fault_stat_struct *pfet_stat = NULL;








	// EXAMPLE BELOW
	{
		// Simply consume all the stats
		struct fault_stat_struct *pfet_tmp_stat = pfet_get_struct();
		while (pfet_tmp_stat)
		{
			pfet_stat = pfet_tmp_stat;
			pfet_tmp_stat = pfet_get_struct();
		}

		if (pfet_stat)
		{
			// HOW TO ENQUEUE REQUEST
			struct cnthread_task_desc *task_desc = kzalloc(sizeof(*task_desc), GFP_KERNEL);
			pfet_stat->vaddr += PAGE_SIZE;	// simply try to fetch the next page
			task_desc->argv = pfet_stat;
			task_desc->init_ftn = NULL;
			task_desc->main_ftn = pfet_prefetch_task;
			task_desc->clean_ftn = NULL;	// task_desc will be removed inside main_ftn()
			cnthread_enqueue_task(CNTHREAD_EVT_PRIO, task_desc);	// low priority
		}
	}
}

static int pfet_prefetch_task(void *pfet_argv)
{
	if (pfet_argv)
	{
		struct fault_stat_struct *pfet_stat = (struct fault_stat_struct *)pfet_argv;
		if (pfet_stat->tsk && pfet_stat->vaddr)
		{
			pr_info("tsk: 0x%lx, addr: 0x%lx\n", (unsigned long)pfet_stat->tsk, (unsigned long)pfet_stat->vaddr);

			do_disagg_page_fault_prefetch(pfet_stat->tsk, pfet_stat->vaddr);
		}
		kfree(pfet_stat);
		return 0;
	}
	return -1;
}


// == PREFETCH TIMER ROUTINE == //
int pfet_timer_handler(void *data)
{
    struct cnthread_handler_data *h_data = (struct cnthread_handler_data *)data;
    int *after_init_stage = h_data->init_stage;

    allow_signal(SIGKILL | SIGSTOP);
    // Wait until roce kernel module is initialized (and connected to the switch)
    while (!(*after_init_stage))
    {
        usleep_range(10, 10);
    }
    pr_info("pgfault timer has been started\n");

    while (1)
    {
        if (kthread_should_stop())
        {
            goto release;
        }

        if (signal_pending(current))
        {
            __set_current_state(TASK_RUNNING);
            goto release;
        }
		pfet_prefetch_entry();
		usleep_range(PFET_TIMER_INTERVAL_IN_US, PFET_TIMER_INTERVAL_IN_US);
    }

release: // Please release memory here
    if (data)
    {
        kfree(data);
        data = NULL; // meaningless
    }
    return 0;
}
