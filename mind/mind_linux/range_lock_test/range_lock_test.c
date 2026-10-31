#ifndef MODULE_NAME
#define MODULE_NAME "profile_printer"
#endif

#include <linux/module.h> /* Needed by all modules */
#include <linux/kernel.h> /* Needed for KERN_ALERT */
#include <linux/kthread.h>
#include <linux/random.h>
#include <linux/delay.h>
#include "range_lock.h"

MODULE_LICENSE("GPL");


DEFINE_RANGE_LOCK_TREE(mem_range_lock_tree);
spinlock_t hash_table_lock;
#define NUM_INVALID (2)
#define NUM_PGFAULT (8)
#define MAX_POS (10000) // 4GB / 4KB = ~1e6, we use smaller for stress testing
#define TOTAL_THREAD (NUM_INVALID + NUM_PGFAULT)

struct worker_data {
	int worker_id;
} data[TOTAL_THREAD];
struct task_struct *task[TOTAL_THREAD];

void invalidate(spinlock_t *ht_lock, struct range_lock_tree *rl_tree, unsigned long left, unsigned long right) {
	DEFINE_RANGE_LOCK(range_lock, left, right); // TODO change this value
	range_write_lock(rl_tree, &range_lock);
	// simulate find and evict
	spin_lock(ht_lock);
	udelay(1); // simulate acquire page from hash table
	spin_unlock(ht_lock);
	udelay(10); // simulate eviction and the scheduling of async invalidation
	spin_lock(ht_lock);
	udelay(10); // simulate acquire pages from hash table
	spin_unlock(ht_lock);
	udelay(10); // simulate eviction
	range_write_unlock(rl_tree, &range_lock);
}

void invalidation_thread(void *data) {
	struct worker_data *w_data = (struct worker_data *) data;
	int id = w_data->worker_id;
	unsigned long i;
	for (i = 0;; i++) {
		if (i % 2000 == 0) {
			pr_info("invalidation thread %d loop %lu \n", id, i);
		}
		if (kthread_should_stop()) {
			pr_info("invalidation thread %d stops\n", id);
			do_exit(0);
		}
		
		// simulate a random inv req
		// each inv req covers at most 512 pages
		unsigned long left = get_random_u32() % (MAX_POS - 512);
		unsigned long right = left + (2 << (get_random_u32() % 10));
		invalidate(&hash_table_lock, &mem_range_lock_tree, left, right);
	}
}

void pgfault(spinlock_t *ht_lock, struct range_lock_tree *rl_tree, unsigned long pos) {
	DEFINE_RANGE_LOCK(range_lock, pos, pos); 
	int backoff = 50;
	while (1) { // copied from MIND impl
		if(range_write_trylock(rl_tree, &range_lock)) {
			spin_lock(ht_lock);
			udelay(1); // simulate getting entry from HT
			spin_unlock(ht_lock);
			unsigned long page_fault_time = get_random_u32() % 100;
			udelay(page_fault_time); // deal with pgfault
			range_write_unlock(rl_tree, &range_lock);
			break;
		} else {
			// pr_info("pgfault thread %d backoff %d at %lu\n", id, backoff, pos);
			udelay(backoff);
		}
		if (backoff <= 200) {
			backoff += 50;
		}
	}
}

void pgfault_thread(void *data) {
	struct worker_data *w_data = (struct worker_data *) data;
	int id = w_data->worker_id;
	unsigned long i;
	for (i = 0;; i++) {
		if (i % 500 == 0) {
			pr_info("pgfault thread %d loop %lu \n", id, i);
		}
		if (kthread_should_stop()) {
			pr_info("pgfault thread %d stops\n", id);
			do_exit(0);
		}
		
		// simulate pagefault at random page
		unsigned long pos = get_random_u32() % MAX_POS; 
		pgfault(&hash_table_lock, &mem_range_lock_tree, pos);
	}
}

static int __init range_lock_test_init(void)
{
	pr_info("range_lock_test start\n");
	spin_lock_init(&hash_table_lock);
	int i;
	for(i = 0; i < TOTAL_THREAD; i++) {
		data[i].worker_id = i;
	}
	for(i = 0; i < NUM_INVALID; i++) {
		task[i] = kthread_run((void *)invalidation_thread, (void *)(data + i), "invalidation_thread");
	}
	for(i = 0; i < NUM_PGFAULT; i++) {
		task[i + NUM_INVALID] = kthread_run((void *)pgfault_thread, (void *)(data + NUM_INVALID + i), "pgfault_thread");
	}
	return 0;
}

static void __exit range_lock_test_exit(void)
{
	int i;
	for(i = 0; i < TOTAL_THREAD; i++) {
		kthread_stop(task[i]);
	}
	pr_info("range_lock_test exit\n");
}


/* module init and exit */
module_init(range_lock_test_init)
module_exit(range_lock_test_exit)
