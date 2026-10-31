#ifndef __FUTEX_DISAGGREGATION_H__
#define __FUTEX_DISAGGREGATION_H__

#include <linux/hashtable.h>
#include <linux/futex.h>

#include <disagg/kshmem_disagg.h>
#include <disagg/network_disagg.h>
#include <disagg/network_fit_disagg.h>
#include <disagg/cnthread_disagg.h>
#include <disagg/config.h>

#ifndef TEST_MAX_FUTEX
#define DISAGG_FUTEX_MAX_NR 2048
#else
#define DISAGG_FUTEX_MAX_NR TEST_MAX_FUTEX
#endif
#define DISAGG_FUTEX_WQ_SIZE PAGE_SIZE
#define DISAGG_FUTEX_WAIT 0
#define DISAGG_FUTEX_WAKE 1
#define DISAGG_FUTEX_TICKET_NONE 0
#define DISAGG_FUTEX_MAX_WAITER_PER_BLADE DISAGG_NUM_CPU_CORE_IN_COMPUTING_BLADE
#define DISAGG_FUTEX_MAX_WAITER (DISAGG_FUTEX_MAX_WAITER_PER_BLADE * DISAGG_MAX_COMPUTE_NODE)
#define DISAGG_FUTEX_ENTRY_KEY_NONE 0

#define DISAGG_FUTEX_HLIST_BIT 10
#define DISAGG_FUTEX_MAX_SLEEP_CNT 50000 // around 2s

#define DISAGG_FUTEX_WQ_LOCK_PAD (PAGE_SIZE - sizeof(spinlock_t))
#define DIAGG_FUTEX_WQ_LEN ((DISAGG_FUTEX_WQ_SIZE - (sizeof(atomic_t) * 2)) / sizeof(atomic_t))
/*
 * global structures on kernel shared memory
*/
struct disagg_futex_wait_queue {
	spinlock_t wq_lock;
	char pad[DISAGG_FUTEX_WQ_LOCK_PAD];
	atomic_t nr_waiters;
	atomic_t ticket;
	atomic_t waiters[DIAGG_FUTEX_WQ_LEN];
}__aligned(PAGE_SIZE);

struct disagg_futex_table_entry {
	u64 key;

};

struct disagg_futex_table {
	spinlock_t table_lock;
	atomic_t next_empty;
	struct disagg_futex_table_entry entries[DISAGG_FUTEX_MAX_NR];
}__aligned(CACHELINE_MAX_SIZE);

struct disagg_futex_shmem {
	struct disagg_futex_wait_queue wqs[DISAGG_FUTEX_MAX_NR];
	char buf[DISAGG_FUTEX_MAX_WAITER][PAGE_SIZE];
	struct disagg_futex_table lookup_table;
}__aligned(PAGE_SIZE);

/*
 * local structures
*/
struct disagg_futex_hnode {
	u64 key64; // = pid << 16 + uaddr (not tgid)
	u64 wait_loc; // our waiting position in wq
	struct disagg_futex_wait_queue *wq;
	struct hlist_node node;
};

long do_disagg_futex(u32 __user *uaddr, int op, u32 val, ktime_t *timeout,
		u32 __user *uaddr2, u32 val2, u32 val3);
void disagg_futex_init(void);

#endif
