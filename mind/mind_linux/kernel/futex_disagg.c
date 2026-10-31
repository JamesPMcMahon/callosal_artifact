#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/jhash.h>
#include <linux/init.h>
#include <linux/futex.h>
#include <linux/mount.h>
#include <linux/pagemap.h>
#include <linux/syscalls.h>
#include <linux/signal.h>
#include <linux/export.h>
#include <linux/magic.h>
#include <linux/pid.h>
#include <linux/nsproxy.h>
#include <linux/ptrace.h>
#include <linux/sched/rt.h>
#include <linux/sched/wake_q.h>
#include <linux/sched/mm.h>
#include <linux/hugetlb.h>
#include <linux/freezer.h>
#include <linux/bootmem.h>
#include <linux/fault-inject.h>
#include <linux/delay.h>

#include <asm/futex.h>
#include <linux/kthread.h>

#include "locking/rtmutex_common.h"

#include <disagg/futex_disagg.h>
#include <disagg/print_disagg.h>
#include <disagg/profile_points_disagg.h>
#include <disagg/config.h>
#include <disagg/network_disagg.h>

static struct disagg_futex_shmem *futex_shmem;
DECLARE_HASHTABLE(futex_hlist, DISAGG_FUTEX_HLIST_BIT);
static spinlock_t futex_hlist_lock;

DEFINE_PROFILE_POINT(FTX_wait)
DEFINE_PROFILE_POINT(FTX_wait_abort)
DEFINE_PROFILE_POINT(FTX_awake)
DEFINE_PROFILE_POINT(FTX_wait_awake)
DEFINE_PROFILE_POINT(FTX_wake)
DEFINE_PROFILE_POINT(FTX_get_wq)
DEFINE_PROFILE_POINT(FTX_shmem_alloc_wq)
DEFINE_PROFILE_POINT(FTX_msg_send)
DEFINE_PROFILE_POINT(FTX_msg_recv)
DEFINE_PROFILE_POINT(FTX_msg_recv_timeout)

extern int get_local_node_id(void);

void disagg_futex_init(void) {
#ifndef FUTEX_DUMMY
    int node_id = get_local_node_id();
    u64 alloc_size = sizeof(struct disagg_futex_shmem);
    pr_info("FUTEX_INIT: node id[%d]\n", node_id);
    if (node_id == 1) {
        u64 alloc_addr = (u64)alloc_kshmem(alloc_size, DISAGG_KSHMEM_SERV_FUTEX_ID);
        pr_futex("FUTEX_INIT: allocating kshmem, alloc_size: %llu\n", alloc_size);
        if (!alloc_addr) {
            pr_err("FUTEX_INIT ERR: fail to allocate futex kshmem\n");
            BUG();
            return;
        }
        if (alloc_addr != (u64)get_base_address(DISAGG_KSHMEM_SERV_FUTEX_ID)) {
            pr_err("FUTEX_INIT ERR: alloc_addr[0x%llx] != kshmem_start[0x%llx]\n",
                    alloc_addr, (u64)get_base_address(DISAGG_KSHMEM_SERV_FUTEX_ID));
            BUG();
            return;
        }
    }

    while(1)
    {
        if (!get_base_address(DISAGG_KSHMEM_SERV_FUTEX_ID)) {
            // wait for the first node
            pr_err("FUTEX_INIT: kshmem_start[0x%lx] | wait...\n",
                    (unsigned long)get_base_address(DISAGG_KSHMEM_SERV_FUTEX_ID));
            msleep(DISAGG_KSHMEM_RETRY_TIME_MS);
            continue;
        } else {
            break;
        }
    }
    futex_shmem = (struct disagg_futex_shmem *)get_base_address(DISAGG_KSHMEM_SERV_FUTEX_ID);

    pr_futex("FUTEX_INIT: node [%d] futex kshmem allocated: [0x%lx - 0x%lx]\n",
                node_id, (unsigned long)futex_shmem, (unsigned long)(futex_shmem + alloc_size));
    pr_futex("FUTEX_INIT: done\n");
    return;
#endif
    return;
}
EXPORT_SYMBOL(disagg_futex_init);

static struct disagg_futex_hnode *find_wq_by_pid_addr(u64 tgid, u16 pid, u64 uaddr) {
    struct disagg_futex_table *table;
    spinlock_t *table_lock;
    int i, first_empty;
    struct disagg_futex_hnode *res = NULL, *cur;
    u32 key32 = (((u64)pid) << 16) + (uaddr % (1L << 32));
    u64 key64 = (((u64)pid) << 48) + uaddr;
    u64 key64tgid = (((u64)tgid) << 48) + uaddr;
    PROFILE_POINT_TIME(FTX_shmem_alloc_wq)

    
    spin_lock(&futex_hlist_lock);
    hash_for_each_possible(futex_hlist, cur, node, key32) {

        if (cur->key64 == key64) {
            res = cur;
            break;
        }
    }
    if (!res) {
        res = (struct disagg_futex_hnode *)kzalloc(sizeof(struct disagg_futex_hnode), GFP_KERNEL);
        if (!res) {
            pr_err("FUTEX_BUG: can not allocate futex queue pid[%d], uaddr[0x%llx]\n", pid, uaddr);
            goto ret;
        } else {
            pr_futex("FUTEX_ALLOC: local futex hnode pid[%d], uaddr[0x%llx] allocated[%p]\n", pid, uaddr, res);
        }
        // set key
        res->key64 = key64;

        // set wq
        PROFILE_START(FTX_shmem_alloc_wq);
        table = &futex_shmem->lookup_table;
        table_lock = &table->table_lock;
        spin_lock(table_lock);
        first_empty = atomic_read_acquire(&(table->next_empty));
        for (i = 0; i < first_empty; ++i) { // look up existing futex
            if (table->entries[i].key == key64tgid) { // futex exist
                break;
            }
        }
        if (i == first_empty) { // futex does not exist
            pr_futex("FUTEX_ALLOC: global futex pid[%d], uaddr[0x%llx] slot[%d] wq[0x%llx]\n", pid, uaddr, i, (u64)(&(futex_shmem->wqs[i])));
            table->entries[i].key = key64tgid;
            atomic_inc_return_release(&(table->next_empty));
        }
        spin_unlock(table_lock);

        if (i == DISAGG_FUTEX_MAX_NR) {
            pr_err("FUTEX_BUG: max nr futex[%d] has been reached\n", DISAGG_FUTEX_MAX_NR);
            kfree(res);
            res = NULL;
            goto ret;
        } else {
            res->wq = &(futex_shmem->wqs[i]);
        }
        
        // set wait_loc
        res->wait_loc = atomic_inc_return_release(&(res->wq->nr_waiters)) - 1; // assign wait loc
        if (res->wait_loc > DISAGG_FUTEX_MAX_WAITER) {
            pr_err("FUTEX_BUG: max nr waiter[%d] has been reached\n", DISAGG_FUTEX_MAX_WAITER);
        }
        PROFILE_LEAVE(FTX_shmem_alloc_wq);

        // add to hlist
        smp_mb();
        hash_add(futex_hlist, &(res->node), key32);
    } else {

    }
ret:
    spin_unlock(&futex_hlist_lock);
    return res;
}


long do_disagg_futex(u32 __user *uaddr, int op, u32 val, ktime_t *timeout, u32 __user *uaddr2, u32 val2, u32 val3) {
    struct disagg_futex_wait_queue *wq;
    spinlock_t *wq_lock;
    int res = 0;
    int i, j, k;
	int cmd = op & FUTEX_CMD_MASK;
    int nr_waiters;
    int wait_loc;
    atomic_t *waiters;
    u32 ticket, smallest, tmp;
    u64 sleep_cnt = 0;
    int user_val;
    atomic_t *msg_loc;
    int lock_released = 0;

    PROFILE_POINT_TIME(FTX_wait)
    PROFILE_POINT_TIME(FTX_wait_abort)
    PROFILE_POINT_TIME(FTX_awake)
    PROFILE_POINT_TIME(FTX_wait_awake)
    PROFILE_POINT_TIME(FTX_wake)
    PROFILE_POINT_TIME(FTX_get_wq)
    PROFILE_POINT_TIME(FTX_msg_send)
    PROFILE_POINT_TIME(FTX_msg_recv)
    PROFILE_POINT_TIME(FTX_msg_recv_timeout)
#ifdef FUTEX_DUMMY
    if (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) {
        PROFILE_START(FTX_wait);
        pr_futex("FUTEX_WAIT tgid[%d] pid[%d] uaddr[0x%llx] timeout[%lluns] val[%d]\n", tgid, current->pid, (u64)uaddr, timeout ? *timeout : 0, val);
        usleep_range(300, 300);
        PROFILE_LEAVE(FTX_wait);
    } else if (cmd == FUTEX_WAKE || cmd == FUTEX_WAKE_BITSET) {
        PROFILE_START(FTX_wake);
        pr_futex("FUTEX_WAKEUP_NOTIFY tgid[%d] pid[%d] uaddr[0x%llx] n[%d]\n", tgid, current->pid, (u64)uaddr, val);
        PROFILE_LEAVE(FTX_wake);
    } else {
        pr_err("FUTEX_BUG tgid[%d] pid[%d] uaddr[0x%llx] cmd[%d]\n", tgid, current->pid, (u64)uaddr, cmd);
        res = -1;
    }
    return res;
#endif
    PROFILE_START(FTX_get_wq);
    struct disagg_futex_hnode *wq_hnode = find_wq_by_pid_addr(current->tgid, current->pid, (u64)uaddr);
    if (!wq_hnode) {
        pr_err("FUTEX_BUG: can not find futex queue tgid[%d], pid[%d], uaddr[0x%llx]\n", current->tgid, current->pid, (u64)uaddr);
        res = -1;
        return res;
    }

    wq = wq_hnode->wq;

    wait_loc = (get_local_node_id() - 1) * DISAGG_FUTEX_MAX_WAITER_PER_BLADE + smp_processor_id();
    wq_lock = &(wq->wq_lock);
    waiters = wq->waiters;
    PROFILE_LEAVE(FTX_get_wq);

    if (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) {
        PROFILE_START(FTX_wait);
        PROFILE_START(FTX_wait_abort);
        PROFILE_START(FTX_wait_awake);
        pr_futex("FUTEX_WAIT: tgid[%d] pid[%d] uaddr[0x%llx] timeout[%lluns] val[%d] wq[0x%llx]\n",
            current->tgid, current->pid, (u64)uaddr, timeout ? *timeout : 0, val, (u64)wq);
        spin_lock(wq_lock);
        // check user space value
        if (get_user(user_val, uaddr)) {
            pr_err("FUTEX_BUG: can not get user on addr %p", uaddr);
        } else {
            if (user_val != val) {
                spin_unlock(wq_lock);
                PROFILE_LEAVE(FTX_wait_abort);
                return -EAGAIN;
            }
        }
        // get new ticket and start waiting, linearization point is writing the ticket
        ticket = atomic_inc_return_acquire(&(wq->ticket));
        atomic_set_release(&waiters[wait_loc], ticket);
        spin_unlock(wq_lock);
        PROFILE_LEAVE(FTX_wait);

        PROFILE_START(FTX_msg_recv);
        PROFILE_START(FTX_msg_recv_timeout);
        msg_loc = (atomic_t *)(futex_shmem->buf[wait_loc]);
        while (1) {

            PROFILE_START(FTX_awake);


            if (atomic_cmpxchg_acquire(msg_loc, DISAGG_FUTEX_WAKE, DISAGG_FUTEX_WAIT) == DISAGG_FUTEX_WAKE) {
                PROFILE_LEAVE(FTX_awake);
                PROFILE_LEAVE(FTX_msg_recv);
                break;
            }
            usleep_range(10, 20);
            ++sleep_cnt;
        }
        if (sleep_cnt == DISAGG_FUTEX_MAX_SLEEP_CNT) {
            pr_err("FUTEX_BUG: tgid[%d] pid[%d] uaddr[0x%llx] wait_loc[%d] CPU[%d] timeout!!!\n",
                current->tgid, current->pid, (u64)uaddr, wait_loc, smp_processor_id());
            PROFILE_LEAVE(FTX_msg_recv_timeout);
        }








        pr_futex("FUTEX_AWAKE: tgid[%d] uaddr[0x%llx]\n", current->tgid, (u64)uaddr);
        PROFILE_LEAVE(FTX_wait_awake);

    } else if (cmd == FUTEX_WAKE || cmd == FUTEX_WAKE_BITSET) {
        PROFILE_START(FTX_wake);
        pr_futex("FUTEX_WAKEUP_NOTIFY: tgid[%d] pid[%d] uaddr[0x%llx] n[%d] wq[0x%llx]\n",
            current->tgid, current->pid, (u64)uaddr, val, (u64)wq);
        spin_lock(wq_lock);

        nr_waiters = DISAGG_FUTEX_MAX_WAITER;
        if (val < DISAGG_FUTEX_MAX_WAITER) {
            // wake up 'val' waiters in FIFO order, typically 'val' = 1, so the firt loop is dummy
            for (i = 0; i < val; ++i) {
                smallest = (u32)-1;
                k = -1;
                for (j = 0; j < nr_waiters; ++j) {
                    tmp = atomic_read_acquire(&waiters[j]);
                    if (tmp != DISAGG_FUTEX_TICKET_NONE && tmp < smallest) {
                        smallest = tmp;
                        k = j;
                    }
                }
                if (k != -1) {

                    atomic_set_release(&waiters[k], DISAGG_FUTEX_TICKET_NONE);
                    // dumy early lock release optimization for val = 1
                    if (val == 1) {
                        lock_released = 1;
                        spin_unlock(wq_lock);
                    }
                    // send msg
                    PROFILE_START(FTX_msg_send);
                    pr_futex("FUTEX_MSG CPU[%d] wake up CPU[%d]\n", smp_processor_id(), k);
                    msg_loc = (atomic_t *)(futex_shmem->buf[k]);

                    if (atomic_xchg_release(msg_loc, DISAGG_FUTEX_WAKE) != DISAGG_FUTEX_WAIT) {
                        BUG();
                    }
                    PROFILE_LEAVE(FTX_msg_send);
                }
            }
            if (lock_released == 0)
                spin_unlock(wq_lock);
        } else {
            // wake up all, typically 'val' = intmax
            char sent_map[DISAGG_FUTEX_MAX_WAITER];
            memset(sent_map, 0, sizeof(sent_map));

            pr_futex("FUTEX_WAKEUP_ALL: tgid[%d] pid[%d] uaddr[0x%llx] n[%d] wq[0x%llx]\n",
                current->tgid, current->pid, (u64)uaddr, val, (u64)wq);

            for (i = 0; i < nr_waiters; ++i) {

                if (atomic_read_acquire(&waiters[i]) != DISAGG_FUTEX_TICKET_NONE) {
                    atomic_set_release(&waiters[i], DISAGG_FUTEX_TICKET_NONE);
                    sent_map[i] = 1;
                }
            }
            spin_unlock(wq_lock);
            for (i = 0; i < nr_waiters; ++i) {
                if (sent_map[i] != 0) {
                    // send msg
                    PROFILE_START(FTX_msg_send);
                    pr_futex("FUTEX_MSG CPU[%d] wake up CPU[%d]\n", smp_processor_id(), i);
                    msg_loc = (atomic_t *)(futex_shmem->buf[i]);
                    if (atomic_xchg_release(msg_loc, DISAGG_FUTEX_WAKE) != DISAGG_FUTEX_WAIT) {
                        BUG();
                    }
                    PROFILE_LEAVE(FTX_msg_send);
                }
            }
        }

        pr_futex("FUTEX_WAKEUP_NOTIFY_DONE: tgid[%d] uaddr[0x%llx]\n", current->tgid, (u64)uaddr);
        PROFILE_LEAVE(FTX_wake);

    } else {
        pr_err("FUTEX_BUG: unsupported futex: tgid[%d] pid[%d] uaddr[0x%llx] cmd[%d]\n",
            current->tgid, current->pid, (u64)uaddr, cmd);
        res = -1;
    }
    return res;
}
