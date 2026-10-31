#include <disagg/cnthread_disagg.h>
#include <disagg/kshmem_disagg.h>
#include <disagg/exec_disagg.h>
#include <disagg/fault_disagg.h>
#include <disagg/profile_points_disagg.h>
#include <disagg/network_disagg.h>
#include <disagg/network_rdma_disagg.h>
#include <disagg/network_fit_disagg.h>
#include <disagg/print_disagg.h>
#include <disagg/range_lock_disagg.h>
#include <disagg/config.h>

#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/uaccess.h>		/* probe_kernel_address()	*/
#include <linux/mempolicy.h>
#include <linux/mmu_notifier.h>
#include <linux/inet.h>
#include <linux/list.h>

#include <asm/tlb.h>
#include <asm/pgtable_types.h>
#include <asm/pgtable.h>
#include <asm/page_types.h>
#include <asm/tlbflush.h>
#include <asm/cacheflush.h>
#include <asm/byteorder.h>

#include <linux/smp.h>

static spinlock_t cnthread_lock;

static spinlock_t free_page_get_lock;   // Get one from the list
static spinlock_t cache_waiting_lock;
static spinlock_t hash_list_lock;
static spinlock_t cnthread_udp_lock;
static spinlock_t cnthread_udp_send_lock;
static spinlock_t cnthread_inval_send_ack_lock[DISAGG_NUM_CPU_CORE_IN_COMPUTING_BLADE];
static spinlock_t cnthread_evict_range_lock;
static spinlock_t cn_fh_per_core_lock[DISAGG_NUM_CPU_CORE_IN_COMPUTING_BLADE];
static spinlock_t cnthread_inval_ack_lock;
static spinlock_t cnthread_inval_buf_write_lock;
static spinlock_t cnthread_pgfault_helper_lock[PG_FAULT_HELPER_CPU];

static LIST_HEAD(cn_handler_lru_list);
static LIST_HEAD(cn_free_page_list);

static struct hlist_head lru_list_hashtable[NUM_HASH_BUCKETS];
static struct hlist_head cn_cache_waiting_list[NUM_HASH_WAIT_LIST];

static atomic_t cnthread_list_counter;
static atomic_t cnthread_evict_counter;
static atomic_t cn_free_page_counter;
static atomic_long_t cn_total_eviction_counter;
static atomic_t cnthread_buf_head_inv_splitter;
static atomic_t cnthread_pgfault_helper_idx;

static int _threshold = (int)(CNTHREAD_CACHED_PERSSURE * (float)CNTHREAD_MAX_CACHE_BLOCK_NUMBER);
static int _high_threshold = (int)(CNTHREAD_HIGH_CACHE_PERSSURE * (float)CNTHREAD_MAX_CACHE_BLOCK_NUMBER);
static int _page_free_threshold = (int)((float)(1.0 - CNTHREAD_CACHED_PERSSURE) * (float)CNTHREAD_MAX_CACHE_BLOCK_NUMBER);
static int _page_high_free_threshold = (int)((float)(1.0 - CNTHREAD_HIGH_CACHE_PERSSURE) * (float)CNTHREAD_MAX_CACHE_BLOCK_NUMBER);

// Priority task queues
static spinlock_t cnthread_task_queue_locks[CNTHREAD_TASK_QUEUE_NUM];




static struct list_head cnthread_task_queues[CNTHREAD_TASK_QUEUE_NUM];
static atomic_t cnthread_inv_req_counter;
static atomic_t cnthread_async_req_counter;

// Buffer for RDMA-based invalidation
static unsigned char* base_inval_buf[DISAGG_QP_NUM_INVAL_BUF] = {NULL};
static unsigned int inval_buf_head = 0;
static unsigned int inval_buf_ack_head[DISAGG_QP_NUM_INVAL_BUF] = {0};
static spinlock_t cnthread_inval_head_buf_lock[DISAGG_QP_NUM_INVAL_BUF];

// dummy value
static struct vm_area_struct dummy_vma;

int cnthread_worker_handler(void *data);
int cnthread_main_handler(void *data);
static int check_inval_req_list_and_try(int from_inv, struct cnthread_inv_msg_ctx *inv_ctx, int flag);
#ifndef MIND_USE_TSO
void _cnthread_send_inval_ack(u16 tgid, unsigned long vaddr, char *inval_buf);
#endif

static int __cn_handler_init_phase;

#ifdef MIND_VERIFY_PAGE_CHKSUM
static void print_page_checksum(char *prefix, void *pte_data, void *dma_data, unsigned long addr, unsigned long pfn, unsigned long dma_addr, int target, struct cnthread_page *cnpage)
{
	unsigned long checksum = 0, dmachecksum = 0, *itr;
	for (itr = pte_data; (char *)itr != ((char *)pte_data + PAGE_SIZE); ++itr)
		checksum += *itr;
    for (itr = dma_data; (char *)itr != ((char *)dma_data + PAGE_SIZE); ++itr)
		dmachecksum += *itr;
	pr_cache("%s addr[%lx] checksum[%lx] dmachecksum[%lx] pfn[%lx] dmaaddr[%lx] cnpage[%llx] target[%d]\n", prefix, addr, checksum, dmachecksum, pfn, dma_addr, (u64)cnpage, target);
}
#endif

// Functions for each page
static __always_inline void _add_cnpage_to_free_list(struct cnthread_page *cnpage)
{
    // caller should have aquared free_page_put_lock
    if (unlikely(cnpage->free_list_node.next || cnpage->free_list_node.prev))
    {
        printk(KERN_ERR "ERROR:: already in the list!! cnpage: 0x%lx\n", (unsigned long)cnpage);
        return;
    }
    atomic_inc(&cn_free_page_counter);
    list_add(&cnpage->free_list_node, &cn_free_page_list);
}

void clean_cnthread_page(struct cnthread_page *cnpage)
{
    if (unlikely(!cnpage || !cnpage->kpage))
    {
        printk(KERN_WARNING "Null struct cnthread_page *\n");
        BUG();
    }
    cnpage->vma = NULL;
    cnpage->pte_ptr = NULL;
    atomic_set(&cnpage->is_used, IS_PAGE_UNUSED);
    atomic_set(&cnpage->kpage->_mapcount, 0); // idle: 0 -> mapped: 1
    atomic_set(&cnpage->kpage->_refcount, 2); // idle: 2 -> mapped: 3
    cnpage->addr = 0;
    cnpage->tgid = 0;
    cnpage->mm = NULL;
    cnpage->ownership = 0;
}

static void cnthread_check_and_del_from_lru_list(struct cnthread_page *cnpage)
{
    // Assumption: we already hold the lock
    lockdep_assert_held(&cnthread_lock);

    if (cnpage->lru_list_node.next && cnpage->lru_list_node.prev)
    {
        int list_cnt;
        list_del(&(cnpage->lru_list_node));    // remove from the LRU list
        cnpage->lru_list_node.next = cnpage->lru_list_node.prev = NULL;
        atomic_dec(&cnthread_list_counter);
        list_cnt = atomic_read(&cnthread_list_counter);
        if (list_cnt % 10000 == 0)
            pr_cache("cnthread_list_counter: %d\n", list_cnt);
    }
}

void cnthread_put_page(struct cnthread_page *cnpage)
{
    // this function removes a page from both hash table and lru_list, then put it back to free_list
    if (unlikely(!cnpage || !cnpage->kpage))
    {
        printk(KERN_WARNING "Null struct cnthread_page *\n");
        BUG();
    }
    // remove from hash table and lru_list
    spin_lock(&cnthread_lock);
    spin_lock(&hash_list_lock);
    cnthread_check_and_del_from_lru_list(cnpage);
    hash_del(&cnpage->hnode);
    spin_unlock(&hash_list_lock);
    spin_unlock(&cnthread_lock);


    spin_lock(&free_page_get_lock);
    clean_cnthread_page(cnpage);
    _add_cnpage_to_free_list(cnpage);
    spin_unlock(&free_page_get_lock);
}


static void cnthread_put_page_no_lock(struct cnthread_page *cnpage) 
{
    // this function assumes that cnthread_lock, hash_list_lock, free_page_put_lock all locked
    if (unlikely(!cnpage || !cnpage->kpage))
    {
        printk(KERN_WARNING "Null struct cnthread_page **\n");
        BUG();
    }
    cnthread_check_and_del_from_lru_list(cnpage);
    hash_del(&cnpage->hnode);
    clean_cnthread_page(cnpage);
    _add_cnpage_to_free_list(cnpage);
}

static void __always_inline init_cnpage(struct cnthread_page *cnpage, struct page *page) {
    cnpage->kpage = page;
    cnpage->free_list_node.next = cnpage->free_list_node.prev = NULL;
    cnpage->lru_list_node.next = cnpage->lru_list_node.prev = NULL;
    clean_cnthread_page(cnpage);
}

static void cnthread_initial_task_queues(void)
{
    int i = 0;
    for (i = 0; i < CNTHREAD_TASK_QUEUE_NUM; i++)
    {
        // manual init
        INIT_LIST_HEAD(&cnthread_task_queues[i]);
        spin_lock_init(&cnthread_task_queue_locks[i]);
    }
}

void cnthread_enqueue_task(unsigned int priority, struct cnthread_task_desc* task_desc)
{
    if (unlikely(priority >= CNTHREAD_TASK_QUEUE_NUM))
    {
        priority = CNTHREAD_TASK_QUEUE_NUM - 1;  // the lowest
    }
    spin_lock(&cnthread_task_queue_locks[priority]);
    list_add(&task_desc->node, &cnthread_task_queues[priority]);
    spin_unlock(&cnthread_task_queue_locks[priority]);
}

static __always_inline void cnthread_enqueue_task_retry(unsigned int priority, struct cnthread_task_desc* task_desc)
{
    if (unlikely(priority >= CNTHREAD_TASK_QUEUE_NUM))
    {
        priority = CNTHREAD_TASK_QUEUE_NUM - 1;  // the lowest
    }
    spin_lock(&cnthread_task_queue_locks[priority]);
    list_add(&task_desc->node, cnthread_task_queues[priority].prev->prev);
    spin_unlock(&cnthread_task_queue_locks[priority]);
}

static __always_inline struct cnthread_task_desc* dequeue_task(unsigned int priority)
{
    struct cnthread_task_desc* task_desc = NULL;
    if (unlikely(priority >= CNTHREAD_TASK_QUEUE_NUM))
    {
        priority = CNTHREAD_TASK_QUEUE_NUM - 1;  // the lowest
    }
    // Check without lock (no need to be accurate, we will retry)
    if (list_empty(&cnthread_task_queues[priority]))
    {
        return NULL;
    }
    // Check with lock
    spin_lock(&cnthread_task_queue_locks[priority]);
    if (list_empty(&cnthread_task_queues[priority]))
    {
        spin_unlock(&cnthread_task_queue_locks[priority]);
        return NULL;
    }
    task_desc = (struct cnthread_task_desc *)container_of(cnthread_task_queues[priority].prev,
                                                          struct cnthread_task_desc, node);
    list_del(&task_desc->node);
    spin_unlock(&cnthread_task_queue_locks[priority]);
    return task_desc;
}

spinlock_t *get_per_core_lock(int cid)
{
    return &cn_fh_per_core_lock[cid];
}
EXPORT_SYMBOL(get_per_core_lock);

static struct cnthread_handler_data pfet_timer_data;
void disagg_cn_thread_init(void)
{

    struct page *page = NULL, *first_page = NULL, *last_page = NULL;
    long i = 0, j = 0;
    struct cnthread_handler_data *data[CNTHREAD_WORKER_NUMBER + 1] = {0};
    spin_lock_init(&cnthread_lock);
    spin_lock_init(&cache_waiting_lock);
    spin_lock_init(&hash_list_lock);
    spin_lock_init(&cnthread_udp_lock);
    spin_lock_init(&cnthread_udp_send_lock);
    spin_lock_init(&cnthread_evict_range_lock);
    spin_lock_init(&cnthread_inval_ack_lock);
    spin_lock_init(&cnthread_inval_buf_write_lock);



    spin_lock_init(&free_page_get_lock);
    cache_range_lock_init();
    __cn_handler_init_phase = 0;
    atomic_set(&cnthread_list_counter, 0);
    atomic_set(&cnthread_evict_counter, 0);
    atomic_set(&cn_free_page_counter, 0);
    atomic_long_set(&cn_total_eviction_counter, 0);
    atomic_set(&cnthread_inv_req_counter, 0);
    atomic_set(&cnthread_async_req_counter, 0);
    atomic_set(&cnthread_buf_head_inv_splitter, 0);
    atomic_set(&cnthread_pgfault_helper_idx, 0);

    hash_init(lru_list_hashtable);
    hash_init(cn_cache_waiting_list);

    cnthread_initial_task_queues();
    init_pgfault_prefetch();
    prealloc_kernel_shared_mem();
    memset(&dummy_vma, 0, sizeof(dummy_vma));

    for (i = 0; i < DISAGG_NUM_CPU_CORE_IN_COMPUTING_BLADE; i++)
    {
        spin_lock_init(&cn_fh_per_core_lock[i]);
        spin_lock_init(&cnthread_inval_send_ack_lock[i]);
    }

    for (i = 0; i < DISAGG_QP_NUM_INVAL_BUF; i++)
    {
        inval_buf_ack_head[i] = 0;  // start from 0
        spin_lock_init(&cnthread_inval_head_buf_lock[i]);
    }

    for (i = 0; i < PG_FAULT_HELPER_CPU; i++)
    {
        spin_lock_init(&cnthread_pgfault_helper_lock[i]);
    }

    for (i = 0; i < CNTHREAD_WORKER_NUMBER + 1; i++)
    {
        data[i] = kzalloc(sizeof(*data[i]), GFP_KERNEL);
        if (!data[i])
        {
            pr_err("Cannot start cnthread handler daemon!\n");
        }
        data[i]->init_stage = &__cn_handler_init_phase;
        data[i]->pin_core_id = LAST_HANDLER_CPU - i;    // starting from the last cpu core
        data[i]->worker_id = i;
    }
    smp_wmb();

    // Initialize cache regions (=cacheline structures)
    for (i = 0; i < CNTHREAD_MAX_CACHE_BLOCK_NUMBER; i++)
    {
        struct cnthread_page *cnpage = kzalloc(sizeof(struct cnthread_page), GFP_KERNEL);
        if (!cnpage)
        {
            pr_err("Disagg_Cached: Cannot initialize cnpage: %ld\n", i);
            BUG();
        }
        page = alloc_page_vma((~__GFP_RECLAIM) & (GFP_USER | __GFP_ZERO), NULL, 0); // It will increase ref count
        if (unlikely(!page))
        {
            pr_err("Cannot allocate new page [%ld, %ld]\n", i, j);
            BUG();
        }
        init_cnpage(cnpage, page);
        _add_cnpage_to_free_list(cnpage);
        if (unlikely(i == 0))
            first_page = page;
        if (unlikely(i == CNTHREAD_MAX_CACHE_BLOCK_NUMBER - 1))
            last_page = page;
    }

    printk(KERN_DEFAULT "Disagg_Cached: Metadata for software cache has been initialized [%ld entries]\n", i);
    printk(KERN_DEFAULT "Disagg_Cached: Assigned page range-kernel VA [0x%lx - 0x%lx]\n",
           (unsigned long)first_page, (unsigned long)last_page);

    init_test_program_thread_cnt();

    // Start kernel handlers
    kthread_run((void *)cnthread_main_handler, (void *)data[0], "disagg_main");
    if (CNTHREAD_WORKER_NUMBER >= (DISAGG_NUM_CPU_CORE_IN_COMPUTING_BLADE / 2))
    {
        pr_warn("Disagg_Cached: not enough cores [%d] | num workers [%d]\n",
                DISAGG_NUM_CPU_CORE_IN_COMPUTING_BLADE, CNTHREAD_WORKER_NUMBER);
    }
    for (i = 1; i < min(CNTHREAD_WORKER_NUMBER, DISAGG_NUM_CPU_CORE_IN_COMPUTING_BLADE / 2) + 1; i++)
        kthread_run((void *)cnthread_worker_handler, (void *)data[i], "disagg_worker_%d", (int)i);

    // Start prefetcher timer handler
    pfet_timer_data.init_stage = &__cn_handler_init_phase;
    pfet_timer_data.pin_core_id = 0; // not used
    pfet_timer_data.worker_id= 0; // not used
    pfet_timer_data.stat = NULL;
    kthread_run((void *)pfet_timer_handler, (void *)&pfet_timer_data, "disagg_pfet_timer");

    // Clean up unused data structure
    for (i = min(CNTHREAD_WORKER_NUMBER, DISAGG_NUM_CPU_CORE_IN_COMPUTING_BLADE / 2) + 1;
         i < CNTHREAD_WORKER_NUMBER + 1; i++)
    {
        if (data[i])
        {
            kfree(data[i]);
            data[i] = NULL;
        }
    }
}

static int pin_current_thread_to_cpu(int cpu)
{
    struct task_struct *p = current;
    set_cpus_allowed_ptr(p, get_cpu_mask(cpu));
    set_cpu_active(cpu, false);
    return 0;
}

int pin_current_thread(int cpu)
{
    int res;
    res = pin_current_thread_to_cpu(cpu);

    if (!res)
    {
        pr_info("%s - cpu %d(+1) pinned\n", __func__, cpu);
    }
    else
    {
        pr_info("%s - cpu %d(+1) cannot be pinned\n", __func__, cpu);
    }

    return res;
}

static int is_cache_ack_pinned = 0;

void disagg_cnth_end_init_phase()
{
    __cn_handler_init_phase = 1;
    printk(KERN_DEFAULT "Disagg_Cached: Initial phase has been ended\n");
}
EXPORT_SYMBOL(disagg_cnth_end_init_phase);

uint64_t size_index_to_size(uint16_t s_idx)
{
    return ((uint64_t)DYN_MIN_DIR_SIZE) << s_idx;
}

static u16 hash_ftn(u16 tgid, u64 addr)
{
    return ((tgid + addr) >> CACHELINE_MIN_SHIFT) & 0xffff;
}

static u16 hash_ftn_wait_list(u16 tgid, u64 addr)
{
    return ((tgid + addr) >> 12) & 0xff;
}

int is_transient_state(int state)
{
    return state == CACHE_STATE_IS ||
           state == CACHE_STATE_IM ||
           state == CACHE_STATE_SM ||
           state == CACHE_STATE_SI ||
           state == CACHE_STATE_MI ||
           state == CACHE_STATE_II;
}

__always_inline struct cnthread_page *find_cnpage_no_lock(unsigned int tgid, unsigned long addr)
{
    struct cnthread_page *cnpage = NULL;
    addr &= PAGE_MASK;

    hash_for_each_possible(lru_list_hashtable, cnpage, hnode, hash_ftn(tgid, (u64)addr))
    {
        if (cnpage->tgid == tgid && cnpage->addr == addr)
            return cnpage;
    }
    return NULL;
}
EXPORT_SYMBOL(find_cnpage_no_lock);

// returns NULL if empty
static struct cnthread_page *_cnthread_allocate_page(
    unsigned int tgid, unsigned long addr, struct mm_struct *mm)
{



    struct cnthread_page *cnpage;
    spin_lock(&free_page_get_lock);
    if (list_empty(&cn_free_page_list)) {
        spin_unlock(&free_page_get_lock);
        return NULL;
    }





    cnpage = container_of(cn_free_page_list.next, struct cnthread_page, free_list_node);
    list_del(&cnpage->free_list_node);
    cnpage->free_list_node.next = cnpage->free_list_node.prev = NULL;




    atomic_dec(&cn_free_page_counter);
    spin_unlock(&free_page_get_lock);

    // initialize cnpage and add to the hash and lru list
    cnpage->addr = addr;
    cnpage->tgid = tgid;
    cnpage->mm = mm;
    pr_cache("Assigned new page: tgid[%u] addr[0x%lx] mm[0x%lx]\n",
                tgid, addr, (unsigned long)mm);
    // by adding to the hash, now this cnpage is visible from cache eviction handler and page fault handler
    atomic_inc(&cnthread_list_counter);
    hash_add(lru_list_hashtable, &cnpage->hnode, hash_ftn(tgid, (u64)cnpage->addr));
    list_add(&cnpage->lru_list_node, &cn_handler_lru_list);
    return cnpage;
}

// this function returns a cnpage at addr sets was_cnpage_exist 
// when need to create new cnpage, it will retry if out of memory, 
// returns NULL if there is inconsistency (not sure if necessary right now)
// @was_cacheline_exist: return 1 if there is existing cacheline and page, return 0 otherwise.
static struct cnthread_page *_cnthread_get_new_page(unsigned int tgid, unsigned long addr, struct mm_struct *mm,
                                                   int new_page, int *was_cnpage_exist)
{
    struct cnthread_page *cnpage;
    (void)new_page;

    *was_cnpage_exist = 0;

retry_get_free_page:
    // Check there is already populated cacheline
    spin_lock(&cnthread_lock);
    spin_lock(&hash_list_lock);
    cnpage = find_cnpage_no_lock(tgid, addr);
    if (cnpage)
    {
        *was_cnpage_exist = atomic_read(&cnpage->is_used);
        // Check consistency, not sure if this is required right now
        if (unlikely((cnpage->tgid != tgid) || (cnpage->addr != addr) || (cnpage->mm != mm)))
        {
            pr_info_ratelimited("Given: tgid[%u] addr[0x%lx] mm[0x%lx] is_used[%d] <-> found: tgid[%u] addr[0x%lx] mm[0x%lx]\n",
                                tgid, addr, (unsigned long)mm,
                                atomic_read(&cnpage->is_used), cnpage->tgid, cnpage->addr, (unsigned long)cnpage->mm);
            cnpage = NULL;
#if 0
            *was_cacheline_exist = atomic_read(&free_req->is_used);
            if (spin_trylock(&cnline->evict_lock)) // Not being evicted
            {
                if (spin_trylock(&free_req->pgfault_lock)) // Not being evicted
                {
                    // Check consistency
                    if  (free_req->cacheline->tgid != tgid){
                        pr_cache("free_req->cacheline->tgid: %u\n", free_req->cacheline->tgid);
                        pr_cache("tgid: %u\n", tgid);
                        BUG();
                    }

                    if (unlikely((!free_req->cacheline) || (free_req->cacheline->tgid != tgid) ||
                                 (free_req->cacheline->addr != (addr & CNTHREAD_CACHLINE_MASK)) ||
                                 (free_req->cacheline->mm != mm)))
                    {
                        pr_info_ratelimited("Given: tgid[%u] addr[0x%llx] mm[0x%lx] is_used[%d] <-> found: tgid[%u] addr[0x%lx] mm[0x%lx]\n",
                                            tgid, (addr & CNTHREAD_CACHLINE_MASK), (unsigned long)mm,
                                            atomic_read(&free_req->is_used),
                                            free_req->cacheline ? free_req->cacheline->tgid : (unsigned int)-1,
                                            free_req->cacheline ? free_req->cacheline->addr : 0,
                                            free_req->cacheline ? (unsigned long)cnline->mm : 0);
                        spin_unlock(&hash_list_lock);
                        spin_unlock(&cnthread_lock);
                        spin_unlock(&free_req->pgfault_lock);
                        spin_unlock(&cnline->evict_lock);
                        return NULL;
                    }
                    spin_unlock(&cnthread_lock);
                    spin_unlock(&hash_list_lock);
                    spin_unlock(&cnline->evict_lock);
                    return free_req;
                }
                spin_unlock(&cnline->evict_lock);
            }
            spin_unlock(&cnthread_lock);
            spin_unlock(&hash_list_lock);
            return NULL; // Under eviction or parallel access to the same cacheline
#endif
        }
        spin_unlock(&hash_list_lock);
        spin_unlock(&cnthread_lock);
        return cnpage;
    }

    // Due to the retry routine inside fault_disagg.c, the new_page might be set based on outdated value
    // For example, if we get NACK, and the pte IS NOT present because it is invalidated,
    // the new_page can be still 0 because it is set based on the outdated pte value which IS present.

    if (!new_page) {


        pr_info_ratelimited("Err: pte seems to be out-dated | tgid[%u] addr[0x%lx] mm[0x%lx]\n",
                            tgid, addr, (unsigned long)mm);

    }
    // We hold cnthread_lock, hast_list_lock here!!

    // Find new cnpage
    cnpage = _cnthread_allocate_page(tgid, addr, mm); 
    if (!cnpage) {

        pr_info_ratelimited("_cnthread_allocate_page fails. Likely Out of DRAM software cache!! (free page: %d [%d])\n",
                            atomic_read(&cn_free_page_counter), list_empty(&cn_free_page_list));
        spin_unlock(&hash_list_lock);
        spin_unlock(&cnthread_lock);
        msleep(100);
        goto retry_get_free_page;
    } else {
        spin_unlock(&hash_list_lock);
        spin_unlock(&cnthread_lock);
        return cnpage;
    }
}

struct cnthread_page *cnthread_get_new_page(unsigned int tgid, unsigned long addr, struct mm_struct *mm, int *was_cacheline_exist)
{
    return _cnthread_get_new_page(tgid, addr, mm, 1, was_cacheline_exist);
}
EXPORT_SYMBOL(cnthread_get_new_page);

struct cnthread_page *cnthread_get_page(unsigned int tgid, unsigned long addr, struct mm_struct *mm, int *was_cacheline_exist)
{
    return _cnthread_get_new_page(tgid, addr, mm, 0, was_cacheline_exist);
}
EXPORT_SYMBOL(cnthread_get_page);

// currently not compatible with the new range lock
// ===== THIS OPTIMIZATION CURRENTLY DISABLED BY THE SWITCH =====
// This function is called to create local copy of memory only when: 
// - this compute node is the first node request memory allocation to the given virtual address
// - this compute node has exclusive access to the memory
// Switch informs whether the node has 'onwership' or not
void cnthread_create_owner_cacheline(unsigned int tgid, unsigned long addr, struct mm_struct *mm)
{
    BUG();





































}

void cnthread_set_page_received(struct cnthread_page *cnpage)
{
    if (cnpage)
    {
        if (atomic_read(&cnpage->is_used) != IS_PAGE_USED)   // It not already used
        {
            atomic_set(&cnpage->is_used, IS_PAGE_RECEIVED);
        }
    }
}

int cnthread_rollback_page_received(struct cnthread_page *cnpage)
{
    if (cnpage)
    {
        if (atomic_read(&cnpage->is_used) == IS_PAGE_RECEIVED)   // It was unused
        {
            atomic_set(&cnpage->is_used, IS_PAGE_UNUSED);
            return 1;
        }
    }
    return 0;
}














// This function simply update cnreq to have pte (and related page struct reference counters)
inline int cnthread_add_pte_to_list_with_cnpage(
    pte_t * pte, unsigned long address, struct vm_area_struct *vma,
    struct cnthread_page *cnpage, int new_page)
{
    if (unlikely(!cnpage))
    {
        BUG();
    }

    if (new_page && (atomic_read(&cnpage->is_used) != IS_PAGE_USED))
    {
        pr_cache("New page fetched - tgid: %u, addr: 0x%lx [0x%lx - 0x%lx] used[p:%d] free[p:%d]\n",
                 cnpage->tgid, address, cnpage->addr, cnpage->addr + PAGE_SIZE,
                 atomic_read(&cnpage->is_used), atomic_read(&cn_free_page_counter));
        // Set up mapping counter (cnthread itself hold 1 reference mapping--all the time)
        atomic_set(&cnpage->kpage->_mapcount, 1); // idle: 0 -> mapped: 1
        atomic_set(&cnpage->kpage->_refcount, 3); // idle: 1 -> mapped: 2
    }

    cnpage->pte_ptr = pte;
    cnpage->vma = vma ? vma : &dummy_vma;
    smp_wmb();
    atomic_set(&cnpage->is_used, IS_PAGE_USED);
    return 0;
}
EXPORT_SYMBOL(cnthread_add_pte_to_list_with_cnpage);

//=========== Ack and invalidation listener =========//
static struct cnthread_inv_msg_ctx send_ctx;
static struct socket *recv_socket = NULL, *send_socket = NULL;
static char *ack_buf = NULL;
static char *recv_buf = NULL;
static const size_t _recv_buf_size = 2 * DISAGG_NET_MAX_SIZE_ONCE;

DEFINE_PROFILE_POINT(FH_wait_ack)
DEFINE_PROFILE_POINT(FH_wait_ack_switch)
DEFINE_PROFILE_POINT(FH_wait_ack_inval)
DEFINE_PROFILE_POINT(FH_wait_ack_inval_q)
DEFINE_PROFILE_POINT(FH_wait_ack_handler)
DEFINE_PROFILE_POINT(CN_update_roce_recv)
DEFINE_PROFILE_POINT(CN_update_roce_inv_pre)
DEFINE_PROFILE_POINT(CN_update_roce_inv_ack_evt)
DEFINE_PROFILE_POINT(CN_update_roce_inv_ack_inv)
DEFINE_PROFILE_POINT(CN_update_roce_inv_ack_prmpt)
DEFINE_PROFILE_POINT(CN_update_roce_inv_req_evt)
DEFINE_PROFILE_POINT(CN_update_roce_inv_req_inv)
DEFINE_PROFILE_POINT(CN_update_roce_inv_req_prmpt)
DEFINE_PROFILE_POINT(CN_update_roce_inv_queue)
DEFINE_PROFILE_POINT(CN_update_roce_async_inv_queue)
DEFINE_PROFILE_POINT(CN_update_roce_inv_lock)
DEFINE_PROFILE_POINT(CN_update_roce_inv_send)
DEFINE_PROFILE_POINT(CN_update_roce_inv_req)
DEFINE_PROFILE_POINT(CN_inv_target_data)
DEFINE_PROFILE_POINT(CN_inv_dummy_data)
DEFINE_PROFILE_POINT(CN_inv_target_data_prmpt)
DEFINE_PROFILE_POINT(CN_inv_dummy_data_prmpt)
DEFINE_PROFILE_POINT(CN_inv_other_data_prmpt)
DEFINE_PROFILE_POINT(CN_inv_total)
DEFINE_PROFILE_POINT(CN_inv_total_prmpt)
DEFINE_PROFILE_POINT(CN_inv_total_dum)
DEFINE_PROFILE_POINT(CN_latency_10us)
DEFINE_PROFILE_POINT(CN_latency_50us)
DEFINE_PROFILE_POINT(CN_latency_100us)
DEFINE_PROFILE_POINT(CN_latency_250us)
DEFINE_PROFILE_POINT(CN_latency_500us)
DEFINE_PROFILE_POINT(CN_latency_1000us)

static __always_inline struct cache_waiting_node *find_node_nolock(unsigned int tgid, unsigned long addr)
{
    struct cache_waiting_node *w_node = NULL;
    int found = 0;
    hash_for_each_possible(cn_cache_waiting_list, w_node, hnode, hash_ftn_wait_list(tgid, (u64)addr & PAGE_MASK))
    {
        if ((w_node->tgid == tgid) && (w_node->addr == (addr & PAGE_MASK)))
        {
            found = 1;
            break;
        }
    }
    if (found)
        return w_node;
    return NULL;
}

static __always_inline struct cache_waiting_node *find_node(unsigned int tgid, unsigned long addr)
{
    struct cache_waiting_node *w_node = NULL;
    spin_lock(&cache_waiting_lock);
    w_node = find_node_nolock(tgid, addr);
    spin_unlock(&cache_waiting_lock);
    return w_node;
}

__always_inline struct cache_waiting_node *add_waiting_node(unsigned int tgid, unsigned long addr, struct cnthread_page *cnpage)
{
    struct cache_waiting_node *node;
    spin_lock(&cache_waiting_lock);
    node = find_node_nolock(tgid, addr);
    if (unlikely(node)) // this should never happen under range lock
    {
        spin_unlock(&cache_waiting_lock);
        return NULL;
    }
    node = kmalloc(sizeof(*node), GFP_KERNEL);
    if (unlikely(!node))
    {
        spin_unlock(&cache_waiting_lock);
        BUG();
        return NULL;
    }
    node->addr = addr & PAGE_MASK;
    node->tgid = tgid;
    atomic_set(&node->target_counter, DISAGG_MAX_COMPUTE_NODE + 1);
    atomic_set(&node->ack_counter, 0);
    atomic_set(&node->unlock_requested, 0);
    barrier();
    hash_add(cn_cache_waiting_list, &node->hnode, hash_ftn_wait_list(tgid, (u64)addr & PAGE_MASK));
    smp_wmb();
    spin_unlock(&cache_waiting_lock);
    return node;
}
EXPORT_SYMBOL(add_waiting_node);

static int __cnthread_debug_buffer_record[CACHELiNE_ROCE_INVAL_BUF_LENGTH] = {0};
// This function prints out raw values for the given pointer (usually received invalidation request or ACK)
// This function also prints status of the circular invalidation buffer 
// (before and after CNTHREAD_DEBUG_BUFFER_PRINT_RANGE messages)
static void print_raw(char *ptr)
{
    char buf_str[256] = "";
    int i = 0, j = 0;
    pr_info("00: %02x %02x %02x %02x %02x %02x %02x %02x\n",
            ptr[0] & 0xff, ptr[1] & 0xff, ptr[2] & 0xff, ptr[3] & 0xff,
            ptr[4] & 0xff, ptr[5] & 0xff, ptr[6] & 0xff, ptr[7] & 0xff);
    pr_info("08: %02x %02x %02x %02x %02x %02x %02x %02x\n",
            ptr[8] & 0xff, ptr[9] & 0xff, ptr[10] & 0xff, ptr[11] & 0xff,
            ptr[12] & 0xff, ptr[13] & 0xff, ptr[14] & 0xff, ptr[15] & 0xff);

    for (j=0; j<DISAGG_QP_NUM_INVAL_BUF; j++)
    {
        if (inval_buf_ack_head[j] >= CNTHREAD_DEBUG_BUFFER_PRINT_RANGE)
        {
            memset(buf_str, 0, sizeof(char) * 256);
            for (i = 0; i < CNTHREAD_DEBUG_BUFFER_PRINT_RANGE; i++)
            {

                sprintf(buf_str, "%s %02x", buf_str,
                        (base_inval_buf[j][(inval_buf_ack_head[j] - i) * CACHELINE_ROCE_HEADER_LENGTH] & 0xff));
            }
            pr_info("Prev: %s\n", buf_str);
        }
        if (inval_buf_ack_head[j] < CACHELiNE_ROCE_INVAL_BUF_LENGTH - CNTHREAD_DEBUG_BUFFER_PRINT_RANGE){
            memset(buf_str, 0, sizeof(char) * 256);
            for (i = 0; i < CNTHREAD_DEBUG_BUFFER_PRINT_RANGE; i++)
            {

                sprintf(buf_str, "%s %02x", buf_str,
                        (base_inval_buf[j][(inval_buf_ack_head[j] + i) * CACHELINE_ROCE_HEADER_LENGTH] & 0xff));
            }
            pr_info("Next: %s\n", buf_str);
        }
    }
}

const static int RAW_ACK_TO_VADDR_OFFSET = 2;
const static int RAW_ACK_TO_RKEY_OFFSET = 10;
u64 parse_vaddr(char *buf)
{
    u64 fva = *((u64 *)(&(buf[RAW_ACK_TO_VADDR_OFFSET])));
    return __be64_to_cpu(fva); // Network endian to host endian
}
EXPORT_SYMBOL(parse_vaddr);

u32 parse_rkey(char *buf)
{
    u32 ret = *((u32 *)(&(buf[RAW_ACK_TO_RKEY_OFFSET])));
    return __be32_to_cpu(ret);
}
EXPORT_SYMBOL(parse_rkey);

static __always_inline void increase_wait_counter(struct cache_waiting_node *node)
{
    if (unlikely(!node))
    {
        BUG();
    }
    atomic_inc(&node->ack_counter);
}

static __always_inline void set_target_counter(struct cache_waiting_node *node, int val)
{
    if (unlikely(!node))
    {
        BUG();
    }
    atomic_set(&node->target_counter, val);
}

static __always_inline int get_target_counter_from_bitmask(u16 shared_list)
{
    int res = 0;
    while (shared_list > 0)
    {
        if (shared_list & 0x1)
            res++;
        shared_list >>= 1;
    }
    return res;
}

__always_inline int get_id_from_requester(u16 requester)
{
    u16 mask = 0x1;
    int i;
    for (i = 0; i < DISAGG_MAX_COMPUTE_NODE; i++)
    {
        if (requester & mask)
            return i;
        mask <<= 1;
    }
    return -1;
}
EXPORT_SYMBOL(get_id_from_requester);

static void request_unlock_wait_node(struct cache_waiting_node *node)
{
    if (unlikely(!node))
    {
        BUG();
    }
    // Make sure that unlock is requested
    // (actually the check is meaningless, any number >0 would be OK)
    if (atomic_read(&node->unlock_requested) == 0)  // If not requested yet
        atomic_inc(&node->unlock_requested);
}

void cancel_waiting_for_nack(struct cache_waiting_node *w_node)
{
    request_unlock_wait_node(w_node);
    barrier();
    set_target_counter(w_node, 0); // All needed ack received — it's NACK in this case
}

static unsigned int error_cnt = 0;
static int do_handle_ack(struct cache_waiting_node *w_node)
{
    PROFILE_POINT_TIME(FH_wait_ack_handler)
    // initialize and parse data
    char *buf = w_node->ack_buf;
    u64 fva = parse_vaddr(buf);
    u32 rkey = parse_rkey(buf);
    u16 pid = (unsigned int)(fva >> 48);
    u64 vaddr = fva & (~MN_VA_PID_BIT_MASK);            // Pure virtual address (48bits)
    u32 state = rkey & CACHELINE_ROCE_RKEY_STATE_MASK;
    if (pid == DISAGG_KERN_TGID)
    {
        vaddr |= MN_VA_PID_BIT_MASK;  // kernel's virtual address starts from 0xffff
    }

    // start checking ACK
    PROFILE_START(FH_wait_ack_handler);
    if (rkey & CACHELINE_ROCE_RKEY_NACK_MASK) // If NACK is set
    {
        u16 r_state = 0, r_sharer = 0, r_size = 0, r_lock = 0, r_cnt = 0;
        error_cnt++;
#ifndef PRINT_CACHE_COHERENCE
        if (error_cnt > DISAGG_NET_POLLING_SHORT_BREAK)
#endif
        {
            send_cache_dir_full_check(pid, vaddr & PAGE_MASK,
                                    &r_state, &r_sharer, &r_size, &r_lock, &r_cnt, CN_SWITCH_REG_SYNC_NONE);
            printk(KERN_WARNING "ERROR: RDMA-NACK [%u] recieved for PID: %u, VA: 0x%llx, state: 0x%x, sharer: 0x%x, size: %u, lock: %u, cnt: %u\n",
                state, pid, vaddr, r_state, r_sharer, r_size, r_lock, r_cnt);
            error_cnt = 0;
        }
        // Request unlock: page fault handler will retry
        if (likely(w_node))
        {
            cancel_waiting_for_nack(w_node);
        }
        else
        {
            pr_info("NACK: Cannot find waiting node for PID: %u, VA: 0x%llx <-> Cur PID: %u, VA: 0x%lx\n",
                    pid, vaddr, w_node->tgid, w_node->addr);
            BUG();
        }
        return -1;
    }
    else
    {
        // *** Normal ACKs from switch and other nodes *** //
        if (likely(w_node))
        {
            pr_cache("RDMA-ACK [%u] recieved for PID: %u, VA: 0x%llx, state: 0x%x\n",
                     state, pid, vaddr, state);
            if (state == CACHE_STATE_INV_ACK)
            {

                pr_info_ratelimited("ERROR: RDMA-ACK [%u] recieved for PID: %u, VA: 0x%llx, state: 0x%x\n",
                                    state, pid, vaddr, state);
                increase_wait_counter(w_node);
                return -1;
            }
            else
            {   // Ack directly from the switch
                set_target_counter(w_node, 0); // all ack received
                PROFILE_LEAVE(FH_wait_ack_handler);
                return 0;
            }
        }
        else
        {
#ifndef MIND_USE_TSO    // Packets might be reordered in TSO
            pr_info("ACK: Cannot find waiting node for PID: %u, VA: 0x%llx <-> Cur PID: %u, VA: 0x%lx\n",
                    pid, vaddr, w_node->tgid, w_node->addr);
#endif
        }
    }
    return -1;
}

void check_switch_cache_directory(const char *ftn_str, unsigned int tgid, unsigned long vaddr)
{
    u16 r_state = 0, r_sharer = 0, r_size = 0, r_lock = 0, r_cnt = 0;
    {
        send_cache_dir_full_check(tgid, vaddr & PAGE_MASK,
                                &r_state, &r_sharer, &r_size, &r_lock, &r_cnt, CN_SWITCH_REG_SYNC_NONE);
        printk(KERN_INFO "%s || switch $dir for PID: %u, VA: 0x%lx => state: 0x%x, sharer: 0x%x, size: %u, lock: %u, cnt: %u\n",
               ftn_str, tgid, vaddr, r_state, r_sharer, r_size, r_lock, r_cnt);
        error_cnt = 0;
    }
}

inline int wait_until_counter(struct cache_waiting_node *node, spinlock_t *pte_lock, struct rw_semaphore *mmap_sem, struct cnthread_page *cnpage)
{
    int r_val = 0;
    int i = 0, j = 0;
    int ret = 0;
    unsigned long start_time = jiffies;
    int unlocked = 0;

    PROFILE_POINT_TIME(FH_wait_ack)
    PROFILE_POINT_TIME(FH_wait_ack_switch)
    PROFILE_POINT_TIME(FH_wait_ack_inval)
    PROFILE_POINT_TIME(FH_wait_ack_inval_q)

    if (unlikely(!node))
    {
        BUG();
    }
    PROFILE_START(FH_wait_ack);
    PROFILE_START(FH_wait_ack_switch);  // Direct ACK/NACK from switch
    PROFILE_START(FH_wait_ack_inval);   // Invalidation ACK from other nodes
    PROFILE_START(FH_wait_ack_inval_q);

    while (1)
    {
        for (i = 0; i < DISAGG_NET_CTRL_POLLING_SKIP_COUNTER; i++)
        {
            // Check for ACK/NACK from switch
            if (node->ack_buf)
            {
                // If something written from RDMA
                if (node->ack_buf[0])
                {
                    if(!do_handle_ack(node))
                    {
                        PROFILE_LEAVE(FH_wait_ack_switch);
                        goto out_wait;  //ACK
                    }
                }
            }

            r_val = atomic_read(&node->ack_counter);    // This will be set only by ACK/NACK
            if (r_val >= atomic_read(&node->target_counter)) // It only read it
            {
                PROFILE_LEAVE(FH_wait_ack_inval);
                goto out_wait;
            }
#ifdef MIND_USE_TSO
            // If we get message from switch, we are done for cache coherence and let's wait for the actual data
            if (atomic_read(&node->target_counter) <= DISAGG_MAX_COMPUTE_NODE)
            {
                PROFILE_LEAVE(FH_wait_ack_inval_q);
                goto out_wait;
            }
#endif
            for (j = 0; j < DISAGG_QP_NUM_INVAL_BUF; j++)
                try_invalidation_lookahead(PROFILE_CNTHREAD_INV_ACK_SERV_FROM_PRMPT, SERVE_ACK_PER_WAIT);
            r_val = atomic_read(&node->unlock_requested);
            if (r_val == 1)
            {
                // We need to unlock ptl and other locks here
                if (pte_lock)
                {
                    spin_unlock(pte_lock);
                    pr_locks("pte unlocked: tgid: %u, addr: 0x%lx, ack_cnt: %d, tar_cnt: %d\n",
                             node->tgid, node->addr, atomic_read(&node->ack_counter), atomic_read(&node->target_counter));
                }
                if (mmap_sem)
                    up_read(mmap_sem);
                unlocked = 1;
                pr_pgfault("CNTHREAD - pte unlocked: tgid: %u, addr: 0x%lx, ack_cnt: %d, tar_cnt: %d\n",
                         node->tgid, node->addr, atomic_read(&node->ack_counter), atomic_read(&node->target_counter));
                atomic_inc(&node->unlock_requested);
            }
        }
        // Check timer
        if (unlikely(jiffies_to_msecs(jiffies - start_time) >= (unsigned long)(DISAGG_NET_ACK_TIMEOUT_IN_MS)))
        {
            u16 state = 0, sharer = 0;
            // send_cache_dir_check(node->tgid, node->addr & PAGE_MASK,
            //                      &state, &sharer, CN_SWITCH_REG_SYNC_NONE); // pull state before sending something
            u16 dir_size, dir_lock, inv_cnt;
            send_cache_dir_full_always_check(node->tgid, node->addr & PAGE_MASK, &state, &sharer, 
                                             &dir_size, &dir_lock, &inv_cnt, CN_SWITCH_REG_SYNC_NONE);
            printk(KERN_WARNING "ERROR: Cannot receive ACK/NACK - cpu :%d, tgid: %u, addr: 0x%lx, ack_cnt: %d, tar_cnt: %d, timeout (%u ms) / state: 0x%x, sharer: 0x%x\n",
                   smp_processor_id(), node->tgid, node->addr,
                   atomic_read(&node->ack_counter), atomic_read(&node->target_counter),
                   jiffies_to_msecs(jiffies - start_time), state, sharer);
            atomic_inc(&node->unlock_requested);
            print_raw(node->ack_buf);
            if (!base_inval_buf[0])
            {
                printk(KERN_WARNING "ERRPR: cannot get invalidation buffer (head: %u / %u)\n", inval_buf_head, inval_buf_ack_head[0]);
            }
            else
            {
                printk(KERN_WARNING "RDMA-INVAL buf: 0x%lx, head: %u / %u, val: 0x%02x\n",
                       (unsigned long)base_inval_buf[0], inval_buf_head, inval_buf_ack_head[0], 
                       base_inval_buf[0][inval_buf_ack_head[0] * CACHELINE_ROCE_HEADER_LENGTH] & 0xff);
                print_raw(base_inval_buf[0]);
            }
            goto out_wait;
        }
    };
out_wait:
    // Check there was unlock request (NACK)
    barrier();
    r_val = atomic_read(&node->unlock_requested);
    if (r_val > 0)
    {
        ret = -1;
        if (!unlocked)
        {
            if (pte_lock)
            {
                spin_unlock(pte_lock);
                pr_locks("pte unlocked: tgid: %u, addr: 0x%lx, ack_cnt: %d, tar_cnt: %d\n",
                         node->tgid, node->addr, atomic_read(&node->ack_counter), atomic_read(&node->target_counter));
            }
            if (mmap_sem)
                up_read(mmap_sem);

        }
    }
#ifdef PRINT_PAGE_FAULT // this should be part of page fault handler
    check_switch_cache_directory(__func__, node->tgid, node->addr);
#endif

    PROFILE_LEAVE(FH_wait_ack);
    // pr_cache("Wait data - before wait_lock: Node: 0x%lx, Ptr: 0x%lx [cpu :%d]\n",
    //          (unsigned long)node, (unsigned long)node->ack_buf, smp_processor_id());
    spin_lock(&cache_waiting_lock);
    hash_del(&node->hnode);
    smp_wmb();
    spin_unlock(&cache_waiting_lock);
    // pr_cache("Wait data - after wait_lock: Node: 0x%lx, Ptr: 0x%lx [cpu :%d]\n",
    //          (unsigned long)node, (unsigned long)node->ack_buf, smp_processor_id());
    return ret;
}
EXPORT_SYMBOL(wait_until_counter);
























static void __always_inline cnthread_print_cache_status(void)
{
    int cur_size = atomic_read(&cnthread_list_counter);
    pr_info("CNTHREAD - Cache pressure: %d / %d | free [pg:%d] | tot [%lu]\n",
            cur_size, (int)CNTHREAD_MAX_CACHE_BLOCK_NUMBER,
            atomic_read(&cn_free_page_counter),
            (unsigned long)atomic_long_read(&cn_total_eviction_counter));
}

static int __always_inline cnthread_is_pressure(void)
{
    if ((atomic_read(&cnthread_list_counter) >= _threshold) || (atomic_read(&cn_free_page_counter) <= _page_free_threshold))
    {
#ifdef PRINT_CACHE_COHERENCE
        cnthread_print_cache_status();
#endif
        return 1;
    }
    else
        return 0;
}

static int __always_inline cnthread_is_high_pressure(void)
{
    if ((atomic_read(&cnthread_list_counter) >= _high_threshold) || (atomic_read(&cn_free_page_counter) <= _page_high_free_threshold))
    {
#ifdef PRINT_CACHE_COHERENCE
        cnthread_print_cache_status();
#endif
        return 1;
    }
    else
        return 0;
}

// Set of profiling points regarding invalidation messaging
DEFINE_PROFILE_POINT(cnthread_evict_one)
DEFINE_PROFILE_POINT(cnthread_evict_one_lock)
DEFINE_PROFILE_POINT(cnthread_evict_one_rdma)
DEFINE_PROFILE_POINT(cnthread_evict_finish_proc)
DEFINE_PROFILE_POINT(cnthread_evict_flush_page)
DEFINE_PROFILE_POINT(cnthread_send_fin_ack_inv)
DEFINE_PROFILE_POINT(cnthread_send_fin_ack_evt)
DEFINE_PROFILE_POINT(cnthread_evict_flush_tlb)
DEFINE_PROFILE_POINT(cnthread_evict_range)
DEFINE_PROFILE_POINT(cnthread_evict_range_flush)
DEFINE_PROFILE_POINT(cnthread_evict_range_remove)
DEFINE_PROFILE_POINT(cnthread_evict_range_page)
// check victim, pte, pte_lock, vma and make pte read-only if it is presented and writable
static //__always_inline
struct cnthread_inval_pte 
__cnthread_check_and_make_read_only(struct cnthread_page *victim, unsigned long addr)
{
    pte_t *pte = NULL;  //, pte_val;
    spinlock_t *pte_lock = NULL;
    pte_t entry;
    struct cnthread_inval_pte res = {0, 0, 0, 0};

    if (victim->vma && (victim->vma->vm_file || !vma_is_anonymous(victim->vma)))
    {
        return res;   // skip to unmap file or non-anonynous mappings and read only pages
    }

    // While we are doing this task, some memory accesses can occur
    // So, let's make it as a read-only page first
    if (likely(victim && victim->mm))
    {
        pte = find_pte_target_lock(victim->mm, addr, &pte_lock);
    }

    if (unlikely(!pte || !pte_lock || !pte_present(*pte)))
    {
        unsigned long dummy = 0;
        if (unlikely(probe_kernel_address((unsigned long *)victim->pte_ptr, dummy)))
        {
            pr_cache("CNTHREAD - no pte: tgid: %u, addr: 0x%lx, pte: 0x%lx [val: 0x%lx], pte_victim: 0x%lx [bad: %d, 0x%lx]\n",
                     victim->tgid, 
                     addr, (unsigned long)pte, pte ? (unsigned long)pte->pte : 0,
                     (unsigned long)victim->pte_ptr, 1, dummy);
        }
        // It was already freed by others
        res.skip = 1;
        return res;
    }

    if (unlikely(pte != victim->pte_ptr))
    {
        pr_cache("CNTHREAD - pte != given victim pte: pte: 0x%lx, pte_victim: 0x%lx\n",
                 (unsigned long)pte, (unsigned long)victim->pte_ptr);
    }

    if (likely(victim->vma || is_kshmem_address(addr)))
    {
        // Let's rely on pagefault lock which is per page
        pr_locks("lockpte addr[%lx] ptl[%llx] CPU[%d] c try\n", addr, (u64)pte_lock, smp_processor_id());
        spin_lock(pte_lock);
        pr_locks("lockpte addr[%lx] ptl[%llx] CPU[%d] c get\n", addr, (u64)pte_lock, smp_processor_id());
        if (!pte || !pte_present(*pte))
        {
            // If already evicted
            res.skip = 1;
            pr_locks("unlockpte addr[%lx] ptl[%llx] CPU[%d] d\n", addr, (u64)pte_lock, smp_processor_id());
            spin_unlock(pte_lock);
            return res;
        }

        // Flush data from cache
        entry = *pte; // current value
        if (pte_write(*pte))
        {

            entry = pte_wrprotect(entry);
            res.need_push_back = 1;
            smp_wmb();
            set_pte_at_notify(victim->mm, addr, pte, entry);
        }
        spin_unlock(pte_lock);
        pr_locks("unlockpte addr[%lx] ptl[%llx] CPU[%d] e\n", addr, (u64)pte_lock, smp_processor_id());
    }else{
        BUG();
    }
    res.pte = pte;
    res.ptl = pte_lock;
    return res;
}

static //__always_inline
int __cnthread_send_other_data_and_zap_pte(struct cnthread_inval_pte *ctx, struct cnthread_page *victim,
                                           unsigned long addr, int remove_data, struct mmu_gather *tlb)
{
    struct vm_area_struct *vma = victim->vma;
    PROFILE_POINT_TIME(cnthread_evict_range_page)
#ifdef MIND_VERIFY_PAGE_CHKSUM
    // print checksum
    struct page *ptepage = NULL;
    void *ptedata = NULL;
	if (ctx->pte && !pte_none(*ctx->pte)) {
	    ptepage = pte_page(*ctx->pte);
        ptedata = kmap_atomic(ptepage);
	} else {
        pr_err("invalid pte for checksum: pte_ptr: %p, pte_val: 0x%llx", ctx->pte, ctx->pte ? (u64)ctx->pte->pte : 0);
    }
    if (!(victim && victim->kmap)) {
        pr_err("invalid dma for checksum: victim: %p, kmap: 0x%llx", victim, victim ? (u64)victim->kmap : 0);
    }
#endif

    if (ctx->need_push_back)    // was writable
    {
        PROFILE_START(cnthread_evict_range_page);

#ifdef MIND_VERIFY_PAGE_CHKSUM
        //print checksum
        print_page_checksum("sendotherdata-wrback", ptedata, victim->kmap, addr, pte_pfn(*(ctx->pte)), victim->dma_addr, CN_OTHER_PAGE, victim);
        kunmap_atomic(ptedata);
#endif
        if (unlikely(cn_copy_page_data_to_mn(victim->tgid, victim->mm, addr,
                                             ctx->pte, CN_OTHER_PAGE, 0, (void *)victim->dma_addr)))
        {
            pr_warn("CNTHREAD - Cannot send data to MN...\n");
            goto rollback;  // It will simply return error -1
        }
        PROFILE_LEAVE(cnthread_evict_range_page);
    }else{
        pr_cache("Read only data: 0x%lx\n", addr);
#ifdef MIND_VERIFY_PAGE_CHKSUM
        // print checksum
        print_page_checksum("sendotherdata-rdonly", ptedata, victim->kmap, addr, pte_pfn(*(ctx->pte)), victim->dma_addr, 0, victim);
        kunmap_atomic(ptedata);
#endif
    }

    if (remove_data)
    {
        pr_locks("lockpte addr[%lx] ptl[%llx] CPU[%d] f try\n", addr, (u64)(ctx->ptl), smp_processor_id());
        spin_lock(ctx->ptl);
        pr_locks("lockpte addr[%lx] ptl[%llx] CPU[%d] f get\n", addr, (u64)(ctx->ptl), smp_processor_id());
        if (pte_present(*ctx->pte))
        {


            update_hiwater_rss(victim->mm);
            zap_one_pte_without_lock(tlb, vma, addr, addr + PAGE_SIZE,
                                     ctx->pte, NULL);
            wmb();

        }
        pr_locks("unlockpte addr[%lx] ptl[%llx] CPU[%d] g\n", addr, (u64)(ctx->ptl), smp_processor_id());
        spin_unlock(ctx->ptl);
    }
    return 0;

rollback:
    return 0;
}

static int __cnthread_evict_range(struct cnthread_page **victims, unsigned long num_pages, unsigned long start_addr,
                                  unsigned long tar_addr, int remove_data, int is_invalid)
{
    PROFILE_POINT_TIME(cnthread_evict_range)
    PROFILE_POINT_TIME(cnthread_evict_range_flush)
    PROFILE_POINT_TIME(cnthread_evict_range_remove)
    int i;
    unsigned long tmp_addr = 0;
    struct mmu_gather tlb;
    int flush_cnt = 0;
    struct cnthread_inval_pte *on_going_pte_list = NULL;
    struct cnthread_page *chosen_victim = NULL;
    PROFILE_START(cnthread_evict_range);
    on_going_pte_list = kzalloc(sizeof(struct cnthread_inval_pte) * num_pages, GFP_KERNEL);
    if (unlikely(!on_going_pte_list))
    {
        BUG();
    }
    tmp_addr = start_addr;
    PROFILE_START(cnthread_evict_range_flush);
    for (i = 0; i < num_pages; i++)
    {
        if (tmp_addr != tar_addr && victims[i]) {
            if (atomic_read(&victims[i]->is_used) == IS_PAGE_USED) 
            {
                on_going_pte_list[i] = __cnthread_check_and_make_read_only(victims[i], tmp_addr);
                if (on_going_pte_list[i].need_push_back)
                    flush_cnt ++;
            }
            chosen_victim = victims[i];
        }
        tmp_addr += PAGE_SIZE;
    }
    if (!chosen_victim) {
        pr_cache("evict_range quit without a chosen victim\n");
        if (likely(on_going_pte_list))
            kfree(on_going_pte_list);
        return 0; // if nothing is in range, we have nothing to do
    }
    pr_cache("evict_range: chose victim with addr 0x%lx\n", chosen_victim->addr);
    if (flush_cnt)
    {
        pr_cache("evict_range: flush count is %d\n", flush_cnt);
        flush_tlb_mm(chosen_victim->mm);
        PROFILE_LEAVE(cnthread_evict_range_flush);
    }
    // If we need to remove data (if another compute blade wants to have exclusive permission to the data)
    if (remove_data || flush_cnt)
    {
        PROFILE_START(cnthread_evict_range_remove);
        lru_add_drain();
        tlb_gather_mmu(&tlb, chosen_victim->mm, start_addr, start_addr + num_pages * PAGE_SIZE);
        tmp_addr = start_addr;
        for (i = 0; i < num_pages; i++)
        {
            if (on_going_pte_list[i].pte && !on_going_pte_list[i].skip && (tmp_addr != tar_addr))
            {
                __cnthread_send_other_data_and_zap_pte(&on_going_pte_list[i], victims[i], tmp_addr, remove_data, &tlb);
            }
            tmp_addr += PAGE_SIZE;
        }
        tlb_finish_mmu(&tlb, start_addr, start_addr + num_pages * PAGE_SIZE);
        // Finalize
        if (remove_data)
        {
            tmp_addr = start_addr;
            for (i = 0; i < num_pages; i++)
            {
                if (victims[i])
                {
                    if ((on_going_pte_list[i].skip || on_going_pte_list[i].pte) && (tmp_addr != tar_addr))
                    {
                        if (likely(atomic_read(&victims[i]->is_used) == IS_PAGE_USED))
                        {
                            cnthread_put_page(victims[i]);
                            atomic_long_inc(&cn_total_eviction_counter);
                        }
                    }
                }
                tmp_addr += PAGE_SIZE;
            }
        }
        PROFILE_LEAVE(cnthread_evict_range_remove);
    }
    PROFILE_LEAVE(cnthread_evict_range);
    kfree(on_going_pte_list);
    return 0;
}

// prototype here
static void cnthread_send_dummy_data(unsigned int tgid, unsigned long addr,
                                     struct cnthread_inv_msg_ctx *ctx);

/* 
 *  We assume that the caller of this function already hold mm->mmap_sem
 */
static int ___cnthread_evict_one(struct cnthread_page *victim, unsigned long addr, int cur_size, int high_threshold,
                                 struct cnthread_inv_msg_ctx *inv_ctx, char* inval_buf, u32 req_qp)
{
    
    pte_t *pte = NULL;
    spinlock_t *pte_lock = NULL;
    struct mmu_gather tlb;
    pte_t entry;
    int need_push_back = 0;
    int skip_eviction = 0;
    struct vm_area_struct *vma = NULL;
   
    PROFILE_POINT_TIME(cnthread_evict_one)
    PROFILE_POINT_TIME(cnthread_evict_one_lock)
    PROFILE_POINT_TIME(cnthread_evict_one_rdma)
    PROFILE_POINT_TIME(cnthread_evict_finish_proc)
    PROFILE_POINT_TIME(cnthread_evict_flush_page)
    PROFILE_POINT_TIME(cnthread_evict_flush_tlb)

    PROFILE_START(cnthread_evict_one);

    if (victim->vma && (victim->vma->vm_file || !vma_is_anonymous(victim->vma)))
    {
        return 0;
    }

    // While we are doing this task, some memory accesses can occur
    // So, let it be a read only first
    if (likely(victim->mm))
    {
        pte = find_pte_target_lock(victim->mm, addr, &pte_lock);
    }

    if (unlikely(!pte || !pte_lock || !pte_present(*pte)))
    {
        unsigned long dummy = 0;
        if (unlikely(probe_kernel_address((unsigned long *)victim->pte_ptr, dummy)))
        {
            pr_cache("CNTHREAD - no pte: tgid: %u, addr: 0x%lx, pte: 0x%lx [val: 0x%lx], pte_victim: 0x%lx [bad: %d, 0x%lx]\n",
                     victim->tgid, addr,
                     (unsigned long)pte,
                     pte ? (unsigned long)pte->pte : 0,
                     (unsigned long)victim->pte_ptr,
                     1, dummy);
        }
        // It was already freed by others
        goto clear_victim_from_evict_list;
    }

    if (unlikely(pte != victim->pte_ptr))
    {
        pr_cache("CNTHREAD - pte != given victim pte: pte: 0x%lx, pte_victim: 0x%lx\n",
                 (unsigned long)pte, (unsigned long)victim->pte_ptr);
    }

    vma = victim->vma;
    if (likely(vma && (atomic_read(&victim->is_used) == IS_PAGE_USED)))
    {
        // Let's rely on pagefault lock which is per page
        PROFILE_START(cnthread_evict_one_lock);
        // We are safe to modify victim thanks to pgfault_lock,
        // but to prevent other accesses to the pte
        pr_locks("lockpte addr[%lx] ptl[%llx] CPU[%d] 7 try\n", addr, (u64)pte_lock, smp_processor_id());
        spin_lock(pte_lock);
        pr_locks("lockpte addr[%lx] ptl[%llx] CPU[%d] 7 get\n", addr, (u64)pte_lock, smp_processor_id());
        PROFILE_LEAVE(cnthread_evict_one_lock);

        if (!pte || !pte_present(*pte))
        {
            // if already evicted
            pr_locks("unlockpte addr[%lx] ptl[%llx] CPU[%d] 8\n", addr, (u64)pte_lock, smp_processor_id());
            spin_unlock(pte_lock);
            goto clear_victim_from_evict_list;
        }

        entry = *pte; // Current value
        if (pte_write(*pte))
        {



            entry = pte_wrprotect(entry);
            need_push_back = 1;

            PROFILE_START(cnthread_evict_flush_tlb);
            smp_wmb();
            set_pte_at_notify(victim->mm, addr, pte, entry);
            // Please check cpu list for better understading of the latency: mm_cpumask(mm)
            flush_tlb_mm_range(victim->mm, addr, addr + PAGE_SIZE, VM_NONE);
            PROFILE_LEAVE(cnthread_evict_flush_tlb);
        }
        pr_locks("unlockpte addr[%lx] ptl[%llx] CPU[%d] 9\n", addr, (u64)pte_lock, smp_processor_id());
        spin_unlock(pte_lock);

        // Now we have read-only pte and data in memory
        // We assume that communication/re-tx was already handled by the RDMA library.
        PROFILE_START(cnthread_evict_one_rdma);

#ifdef MIND_VERIFY_PAGE_CHKSUM
        // print checksum
        struct page *ptepage = NULL;
        void *ptedata = NULL;
        if (pte && !pte_none(*pte)) {
            ptepage = pte_page(*pte);
            ptedata = kmap_atomic(ptepage);
        } else {
            pr_err("invalid pte for checksum: pte_ptr: %p, pte_val: 0x%llx", pte, pte ? (u64)pte->pte : 0);
        }
        if (!(victim && victim->kmap)) {
            pr_err("invalid dma for checksum: victim: %p, kmap: 0x%llx", victim, victim ? (u64)victim->kmap : 0);
        }
#endif
        if (need_push_back || inv_ctx->is_target_data)
        {








#ifdef MIND_VERIFY_PAGE_CHKSUM
            // print checksum
            print_page_checksum("evictone-wrback", ptedata, victim->kmap, addr, pte_pfn(*pte), victim->dma_addr, inv_ctx->is_target_data ? CN_TARGET_PAGE : CN_OTHER_PAGE, victim);
            kunmap_atomic(ptedata);
#endif
	    if (unlikely(cn_copy_page_data_to_mn(victim->tgid, victim->mm, addr,
                                                 pte, inv_ctx->is_target_data ? CN_TARGET_PAGE : CN_OTHER_PAGE, 
                                                 req_qp, (void *)victim->dma_addr)))

            {
                pr_warn("CNTHREAD - Cannot send data to MN...\n");
                skip_eviction = -1;
                put_cpu();
                goto rollback;  // will simply return error -1
            }
            put_cpu();
            PROFILE_LEAVE(cnthread_evict_one_rdma);
        }
        else
        {
            pr_cache("Read only data: 0x%lx\n", addr);
#ifdef MIND_VERIFY_PAGE_CHKSUM
            // print checksum
            print_page_checksum("evictone-rdonly", ptedata, victim->kmap, addr, pte_pfn(*pte), victim->dma_addr, 0, victim);
            kunmap_atomic(ptedata);
#endif
        }

        if (inv_ctx->remove_data)
        {
            PROFILE_START(cnthread_evict_finish_proc);
            pr_locks("lockpte addr[%lx] ptl[%llx] CPU[%d] a try\n", addr, (u64)pte_lock, smp_processor_id());
            spin_lock(pte_lock);
            pr_locks("lockpte addr[%lx] ptl[%llx] CPU[%d] a try\n", addr, (u64)pte_lock, smp_processor_id());
            tlb_gather_mmu(&tlb, victim->mm, addr, addr + PAGE_SIZE);
            update_hiwater_rss(victim->mm);


            zap_one_pte_without_lock(&tlb, vma, addr, addr + PAGE_SIZE,
                                     pte, NULL); // flush now for this page
            wmb();

            pr_locks("unlockpte addr[%lx] ptl[%llx] CPU[%d] b\n", addr, (u64)pte_lock, smp_processor_id());
            spin_unlock(pte_lock);
            tlb_finish_mmu(&tlb, addr, addr + PAGE_SIZE);
            PROFILE_LEAVE(cnthread_evict_finish_proc);
        }
    }else{
        // Unlikely case
        printk(KERN_ERR "ERROR: suspicious used_page: %d\n", atomic_read(&victim->is_used));
    }

clear_victim_from_evict_list:
    if (victim && inv_ctx->remove_data && likely(atomic_read(&victim->is_used) == IS_PAGE_USED))
    {
        cnthread_put_page(victim);
        atomic_long_inc(&cn_total_eviction_counter);
    }

#ifdef PRINT_CACHE_COHERENCE

    {
        if (likely(victim))
        {
            pr_cache("Evicted cache - tgid: %d, addr: 0x%lx, Ddel: %d\n",
                    victim ? (int)victim->tgid : -1, addr, inv_ctx->remove_data);
        }
    }
#endif
    PROFILE_LEAVE(cnthread_evict_one);
    return skip_eviction;

rollback:
    // Please unlock locks if there is any (no for now)
    return skip_eviction;
}

static void cnthread_send_dummy_data(unsigned int tgid, unsigned long addr,
                                     struct cnthread_inv_msg_ctx *ctx)
{
    u8 opcode, dummy_flag = 0x40;
    u32 dummy_ack_req_and_psn = 0x80000000 | ctx->rdma_ctx.psn;
    u32 ret, dma_len = PAGE_SIZE;
    u16 ret_16;
    struct sockaddr_in addr_in;
    PROFILE_POINT_TIME(CN_inv_dummy_data)
    PROFILE_START(CN_inv_dummy_data);
    spin_lock(&cnthread_udp_lock);
    memcpy(&addr_in, &ctx->addr_in, sizeof(addr_in));
    // Generate RoCE Req here
    // - switch need: src ip, dest ip, and qp
    // - switch will swap src and dest, and set up opcode as RoCE read request
    opcode = ROCE_WRITE_REQ_OPCODE;
    memcpy(&(ctx->dummy_buf[CACHELINE_ROCE_OFFSET_TO_OPCODE]), &opcode, sizeof(opcode));
    memcpy(&(ctx->dummy_buf[CACHELINE_ROCE_OFFSET_TO_FLAGS]), &dummy_flag, sizeof(dummy_flag));
    // set up psn
    dummy_ack_req_and_psn = __cpu_to_be32(dummy_ack_req_and_psn);
    memcpy(&(ctx->dummy_buf[CACHELINE_ROCE_OFFSET_TO_ACKREQ]), &dummy_ack_req_and_psn, sizeof(dummy_ack_req_and_psn));
    // set up qp
    ret = ctx->original_qp;
    ret = __cpu_to_be32(ret);
    memcpy(&(ctx->dummy_buf[CACHELINE_ROCE_OFFSET_TO_QP]), &ret, sizeof(ret));
    // set up pkey
    ret_16 = 0xffff; //65535
    ret_16 = __cpu_to_be16(ret_16);
    memcpy(&(ctx->dummy_buf[CACHELINE_ROCE_OFFSET_TO_PKEY]), &ret_16, sizeof(ret_16));
    // set up rkey to mark this message as dummy data requets
    ret = *((u32 *)(&(ctx->dummy_buf[CACHELINE_ROCE_OFFSET_TO_RKEY])));
    ret = __be32_to_cpu(ret);
    ret &= ~((u32)MN_RKEY_PERMISSION_MASK); // erase given permission
    ret |= (VM_READ << MN_RKEY_PERMISSION_SHIFT);
    ret = __cpu_to_be32(ret);
    memcpy(&(ctx->dummy_buf[CACHELINE_ROCE_OFFSET_TO_RKEY]), &ret, sizeof(ret));
    // DMA length in the RoCE header
    dma_len = __cpu_to_be32(dma_len);
    memcpy(&(ctx->dummy_buf[CACHELINE_ROCE_OFFSET_TO_DMALEN]), &dma_len, sizeof(dma_len));
    // set up destination addr as requster's data
    addr_in.sin_addr.s_addr = ctx->addr_in.sin_addr.s_addr;
    addr_in.sin_port = htons(DEFAULT_ROCE_PORT); // send to the roce
    udp_send(ctx->sk, ctx->dummy_buf, CACHELINE_ROCE_HEADER_LENGTH, MSG_DONTWAIT, &addr_in); // send back to controller
    spin_unlock(&cnthread_udp_lock);
    PROFILE_LEAVE(CN_inv_dummy_data);
}

static int cnthread_send_finish_ack_roce(u16 tgid, unsigned long addr, int is_invalid)
{
    struct fault_data_struct payload;
    struct fault_reply_struct reply; //dummy buffer for ack
    int ret;
    u32 msg_type = DISSAGG_ROCE_FIN_ACK;
    int cpu_id = get_cpu();
    // WE MAY NOT NEED LOCK HERE (no data buffer to send)
    spin_lock(&cnthread_inval_send_ack_lock[cpu_id]);
    payload.pid = tgid; // dummy, will not be used
    payload.tgid = tgid;
    payload.address = addr;
    payload.data_size = CACHELINE_ROCE_HEADER_LENGTH;
    // dummy pointer, no meanful data
    payload.data = (void*)get_dummy_page_dma_addr(DISAGG_NUM_CPU_CORE_IN_COMPUTING_BLADE + cpu_id);
    barrier();
    if (!is_invalid)
        msg_type = DISSAGG_ROCE_EVICT_ACK;
    ret = send_msg_to_memory_rdma(msg_type, &payload, CACHELINE_ROCE_HEADER_LENGTH,
                                  &reply, sizeof(reply));
    spin_unlock(&cnthread_inval_send_ack_lock[cpu_id]);
    put_cpu();
    return ret;
}

void cnthread_send_finish_ack(unsigned int tgid, unsigned long addr,
                                     struct cnthread_inv_msg_ctx *ctx, int is_invalid)
{
    u8 opcode;
    u32 ret;
    u64 fva;
    int res, i;
    struct sockaddr_in addr_in;
    PROFILE_POINT_TIME(cnthread_send_fin_ack_inv)
    PROFILE_POINT_TIME(cnthread_send_fin_ack_evt)
    if (unlikely(!ctx || !ack_buf))
    {
        BUG();
    }
    PROFILE_START(cnthread_send_fin_ack_inv);
    PROFILE_START(cnthread_send_fin_ack_evt);
#ifndef ROCE_FIN_ACK
    spin_lock(&cnthread_udp_send_lock);
    // clean up
    memset(ack_buf, 0, _recv_buf_size);
    // Opcode @ CACHELINE_ROCE_OFFSET_TO_OPCODE
    opcode = ROCE_WRITE_REQ_OPCODE;
    memcpy(&(ack_buf[CACHELINE_ROCE_OFFSET_TO_OPCODE]), &opcode, sizeof(opcode));
    // virtual address
    fva = generate_full_addr(tgid, addr);
    fva = __cpu_to_be64(fva); // network endian to host endian (e.g., big to little in usual X86)
    memcpy(&(ack_buf[CACHELINE_ROCE_OFFSET_TO_VADDR]), &fva, sizeof(fva));
    // Switch need to differentiate eviction and invalidation:
    //       - S^D and M^D requires invalidation (with ACK and/or DATA) not just eviction
    // Invalidation finished ack: send with permission 0x3 = (VM_READ | VM_WRITE)
    ret = *((u32 *)(&(ack_buf[CACHELINE_ROCE_OFFSET_TO_RKEY])));
    ret = __be32_to_cpu(ret);
    ret &= ~((u32)MN_RKEY_PERMISSION_MASK); // erase given permission
    if (is_invalid)
        ret |= ((VM_READ | VM_WRITE) << MN_RKEY_PERMISSION_SHIFT);
    else
        ret |= (MN_RKEY_VM_EVICTION << MN_RKEY_PERMISSION_SHIFT);
    ret = __cpu_to_be32(ret);
    memcpy(&(ack_buf[CACHELINE_ROCE_OFFSET_TO_RKEY]), &ret, sizeof(ret));
    // dummy qp
    ret = DISAGG_QP_INV_ACK_OFFSET_START;   // we do not send data via this QP
    ret = __cpu_to_be32(ret);
    memcpy(&(ack_buf[CACHELINE_ROCE_OFFSET_TO_QP]), &ret, sizeof(ret));
    barrier();

    // Set address
    addr_in.sin_family = AF_INET;
    addr_in.sin_addr.s_addr = get_controller_ip();
    addr_in.sin_port = htons(DEFAULT_ROCE_PORT); // send to the roce

    res = udp_send(ctx->sk, ack_buf, ROCE_REQ_SIZE, MSG_DONTWAIT, &addr_in); // send back to controller
    if (unlikely(res < ROCE_REQ_SIZE))
    {
        printk(KERN_ERR "ERROR: sent finish ACK [%d]\n", res);
        for (i = 0; i < ROCE_REQ_SIZE; i += 4)
        {
            pr_cache("0x%x ", *((unsigned int *)&ack_buf[i]));
        }
    }
    spin_unlock(&cnthread_udp_send_lock);
#else
    (void)opcode;
    (void)ret;
    (void)fva;
    (void)res;
    (void)i;
    (void)addr_in;
    cnthread_send_finish_ack_roce(tgid, addr, is_invalid);
#endif
    if (is_invalid == TRY_INVALIDATION)
        PROFILE_LEAVE(cnthread_send_fin_ack_inv);
    else
        PROFILE_LEAVE(cnthread_send_fin_ack_evt);
}
EXPORT_SYMBOL(cnthread_send_finish_ack);

inline u64 generate_full_addr(u16 tgid, unsigned long addr)
{
    u64 full_addr = (u64)tgid << MN_VA_PID_SHIFT; // first 16 bits
    full_addr += addr & (~MN_VA_PID_BIT_MASK);    // last 48 bits
    return full_addr;
}
EXPORT_SYMBOL(generate_full_addr);










static char *dummy_page_buf[DISAGG_NUM_CPU_CORE_IN_COMPUTING_BLADE * 3] = {NULL};   // 3x: | PageFault | Fin ack | Inval ack
static char *dummy_inv_page_buf = NULL;
static unsigned long dummy_page_dma_addr[DISAGG_NUM_CPU_CORE_IN_COMPUTING_BLADE * 3] = {0};
static unsigned long dummy_inv_page_dma_addr = 0;
#ifndef MIND_USE_TSO
static int cn_send_inval_ack_roce(u16 tgid, unsigned long addr, void* inval_buf)
{
    struct fault_data_struct payload;
    struct fault_reply_struct reply; //buffer for ack
    int ret, cpu_id = get_cpu();
    spin_lock(&cnthread_inval_send_ack_lock[cpu_id]);
    payload.pid = tgid;
    payload.tgid = tgid;
    payload.address = addr;
    payload.data_size = CACHELINE_ROCE_HEADER_LENGTH;
    // FIX: use dummy_page_buf
    payload.data = (void *)get_dummy_page_dma_addr((2 * DISAGG_NUM_CPU_CORE_IN_COMPUTING_BLADE) + cpu_id);
    memcpy((void *)get_dummy_page_buf_addr((2 * DISAGG_NUM_CPU_CORE_IN_COMPUTING_BLADE) + cpu_id),
           inval_buf, CACHELINE_ROCE_HEADER_LENGTH);


    barrier();
    ret = send_msg_to_memory_rdma(DISSAGG_ROCE_INVAL_ACK, &payload, CACHELINE_ROCE_HEADER_LENGTH,
                                  &reply, sizeof(reply));
    put_cpu();
    spin_unlock(&cnthread_inval_send_ack_lock[cpu_id]);
    return ret;
}

void _cnthread_send_inval_ack(u16 tgid, unsigned long vaddr, char *inval_buf)
{
    int err;
    PROFILE_POINT_TIME(CN_update_roce_inv_send)
    PROFILE_START(CN_update_roce_inv_send);
    err = cn_send_inval_ack_roce(tgid, vaddr, inval_buf);
    PROFILE_LEAVE(CN_update_roce_inv_send);
    pr_cache("Inv ACK sent [err: %d] - tgid: %u, addr: 0x%lx\n", err, tgid, vaddr);
}
EXPORT_SYMBOL(_cnthread_send_inval_ack);

#endif

static int cnthread_find_and_evict(unsigned int tgid, unsigned long addr, unsigned long dir_size,
                            int is_invalid, int data_required,
                            int remove_data, char* inv_ack_buf,
                            struct cnthread_inv_msg_ctx *inv_ctx, int from_preampt)
{
    unsigned long start_addr = (addr / dir_size) * dir_size;
    unsigned long end_addr = start_addr + dir_size;
    unsigned long tmp_addr;
    struct cnthread_page *victim = NULL;
    int removed = 0;
    int cnt = 0, wcnt = 0, ret = 0;
    struct cache_waiting_node *w_node = NULL;
    unsigned long start, start_lock, end_lock;
    u16 state = 0, sharer = 0;
    PROFILE_POINT_TIME(CN_inv_target_data)
    PROFILE_POINT_TIME(CN_inv_total)
    PROFILE_POINT_TIME(CN_inv_total_dum)
    PROFILE_POINT_TIME(CN_inv_total_prmpt)
    PROFILE_POINT_TIME(CN_inv_target_data_prmpt)
    PROFILE_POINT_TIME(CN_inv_dummy_data_prmpt)
    // make compiler happy
    (void)wcnt;
    addr &= PAGE_MASK; // may not be needed
    // send_cache_dir_check(tgid, addr & CNTHREAD_CACHLINE_MASK,
    //                      &state, &sharer, CN_SWITCH_REG_SYNC_NONE); // pull state from the switch (**high overhead**)
    pr_cache("Eviction triggered-tgid: %u, addr: 0x%lx [0x%lx - 0x%lx], inval: %d, Dreq: %d, Ddel:%d / state: 0x%x, sharer: 0x%x\n",
           tgid, addr, start_addr, end_addr, is_invalid, data_required, remove_data, state, sharer);
    PROFILE_START(CN_inv_total);
    PROFILE_START(CN_inv_total_dum);
    PROFILE_START(CN_inv_total_prmpt);
    // 1) Find target node in hash table
    start = sched_clock();
    cnt = 0;
    spin_lock(&cnthread_lock);
    spin_lock(&hash_list_lock);
    victim = find_cnpage_no_lock(tgid, addr);

    // 2) If no victim found, only send ack / dummy data for invalidation
    if (!victim) 
    {
        spin_unlock(&hash_list_lock);
        spin_unlock(&cnthread_lock);
        if (is_invalid != TRY_INVALIDATION) // no ack for eviction if there was no victim
        {
            return CNTHREAD_EVICTION_DATAPUSH;
        }else{
            goto send_target_dummy_data;
        }
    }else{
        pr_cache("Cacheline::Remove from list-tgid: %u, addr: 0x%lx [0x%lx - 0x%lx], retry: %d\n",
                    tgid, addr, start_addr, end_addr, ret);
        cnthread_check_and_del_from_lru_list(victim);

        spin_unlock(&hash_list_lock);
        spin_unlock(&cnthread_lock);
    }
    pr_cache("cnpage found-tgid: %u, addr: 0x%lx - 0x%lx\n", tgid, start_addr, end_addr);

    // 3) Invalidate the target page / data
    // There shouldn't be any waiting node with our range lock impl
#if 0
    barrier();
    if (is_invalid)
    {
        // send cancel message to all the on-going data fetch
        for (tmp_addr = start_addr; tmp_addr < end_addr; tmp_addr += PAGE_SIZE)
        {
            w_node = find_node(tgid, tmp_addr);
            if (w_node)
            {
                BUG();


            }
        }
    }
#endif
    smp_wmb();
    pr_cache("Canceled waiting node [found: %d] - tgid: %u, addr: 0x%lx - 0x%lx, inval: %d, Dreq: %d, Ddel:%d, wCnt: %d, CPU: %d\n",
                cnt, tgid, start_addr, end_addr, is_invalid, data_required, remove_data, wcnt, smp_processor_id());
    start_lock = jiffies;
    w_node = NULL;




    end_lock = jiffies;
    if (unlikely((end_lock > start_lock) && (jiffies_to_usecs(end_lock - start_lock) >= DISAGG_SLOW_LOCK_REPORT_IN_USEC)))    // report when suspicious
    {
        // send_cache_dir_full_check(tgid, start_addr, //CNTHREAD_CACHLINE_MASK,
        //                         &state, &sharer, &r_size, &r_lock, &r_cnt, CN_SWITCH_REG_SYNC_NONE);
        printk(KERN_DEFAULT "ERROR: suspcious locking time (tar) - tgid: %u, addr: 0x%lx, w_node: 0x%lx, inval: %d, Dreq: %d, Ddel:%d, time[%u ms]\n",
                tgid, addr, (unsigned long)w_node, is_invalid, data_required, remove_data, jiffies_to_msecs(end_lock - start_lock));
    }
    // since we hold pgfault_lock: IS_PAGE_UNUSED or IS_PAGE_USED (not IS_PAGE_RECEIVED)
    if (victim->vma && (atomic_read(&victim->is_used) != IS_PAGE_UNUSED))
    {
        u32 req_qp = 0;
        int dummy_ctx = 0;
        if ((is_invalid == TRY_INVALIDATION) && inv_ctx)
            req_qp = (get_id_from_requester(inv_ctx->rdma_ctx.requester) * DISAGG_QP_PER_COMPUTE) + inv_ctx->original_qp;
        // now we have lock, so there must be no intermediate state such as IS_PAGE_RECEIVED
        if (unlikely(atomic_read(&victim->is_used) == IS_PAGE_RECEIVED))
        {
            printk(KERN_WARNING "ERROR: Suspicious (tar) is_used [%d] - tgid: %u, addr: 0x%lx, inval: %d, Dreq: %d, Ddel:%d\n",
                atomic_read(&victim->is_used), tgid, addr, is_invalid, data_required, remove_data);
        }
        PROFILE_START(CN_inv_target_data);
        PROFILE_START(CN_inv_target_data_prmpt);
        // Send data only when data push was required
        if (!inv_ctx)
        {
            inv_ctx = kzalloc(sizeof(*inv_ctx), GFP_KERNEL);
            dummy_ctx = 1;
        }
        inv_ctx->is_target_data = data_required;
        inv_ctx->remove_data = remove_data;
        inv_ctx->is_invalid = is_invalid;
	
pr_cache("start ___cnthread_evict_one for addr: %lx CPU: %d\n", addr, smp_processor_id());
        // this function puts the cnpage back to the free list
	
        ret = ___cnthread_evict_one(victim, addr, _threshold, _high_threshold,
                                    inv_ctx, inv_ack_buf, req_qp);
pr_cache("done ___cnthread_evict_one for addr: %lx CPU: %d\n", addr, smp_processor_id());
        if (dummy_ctx)
        {
            kfree(inv_ctx);
            inv_ctx = NULL;
        }
        
        if (is_invalid == TRY_INVALIDATION) // Only for invalidations
        {
            if (from_preampt)
                PROFILE_LEAVE(CN_inv_target_data_prmpt);
            else
                PROFILE_LEAVE(CN_inv_target_data);
        }
        if (unlikely(ret < 0))
            BUG();
        if (ret > 0)    // evition skipped
            ;
        else if (remove_data) // page removed
            removed++;
    }else{
send_target_dummy_data:
        if (is_invalid && data_required)    // ACK w/ data is only required for invalidation
        {   
            if (unlikely(!inv_ctx))
                BUG();
            PROFILE_START(CN_inv_dummy_data_prmpt);
            cnthread_send_dummy_data(tgid, addr, inv_ctx);
            if (from_preampt)
                PROFILE_LEAVE(CN_inv_dummy_data_prmpt);
            pr_cache("Dummy data sent-tgid(w/ cacheline): %u, addr: 0x%lx, inval: %d, Dreq: %d, Ddel:%d\n",
                        tgid, addr, is_invalid, data_required, remove_data);
            udelay(10); // To avoid reordered data - ACK (it must not be in this order: ACK - data)
        }else{
            pr_cache("No data and no dummy needed: %u, addr: 0x%lx, inval: %d, Dreq: %d, Ddel:%d\n",
                     tgid, addr, is_invalid, data_required, remove_data);
        }
    }
    barrier();
    // Now target page is cleared
    if (is_invalid && inv_ack_buf)
    {
        if (is_invalid == TRY_INVALIDATION)
        {
#ifndef MIND_USE_TSO
	    _cnthread_send_inval_ack(tgid, addr, inv_ack_buf);
            barrier();
#endif
        }
    }
    pr_cache("Target/first page invalidated - tgid: %u, addr: 0x%lx, inval: %d, Dreq: %d, Ddel:%d, CPU:%d\n",
             tgid, addr, is_invalid, data_required, remove_data, smp_processor_id());

    if (is_invalid)
    {
        if (from_preampt)
            PROFILE_LEAVE(CN_inv_total_prmpt);
        else
            PROFILE_LEAVE(CN_inv_total);
    }
    pr_cache("[Middle] of evcit-tgid(w/ cacheline): %u, addr: 0x%lx, inval: %d, Dreq: %d, Ddel: %d, Removed: %d",
                tgid, addr, is_invalid, data_required, remove_data, removed);
    // Page was indeed invalid and return success
    if (is_invalid == EVICT_BUT_RETRYING)
        return CNTHREAD_EVICTION_DATAPUSH;
    else
        return CNTHREAD_INVALIDATION_SUCCESS;
}

static int cnthread_find_and_evict_remaining(unsigned int tgid, unsigned long addr, unsigned long dir_size,
                                             int is_invalid, int data_required,
                                             int remove_data, char *inv_ack_buf,
                                             struct cnthread_inv_msg_ctx *inv_ctx, int from_preampt)
{
    unsigned long start_addr = (addr / dir_size) * dir_size;
    unsigned long end_addr = start_addr + dir_size;
    unsigned long tmp_addr;
    unsigned long num_pages = dir_size / PAGE_SIZE;
    int wcnt = 0, rpage_idx = 0, victim_cnt = 0;
    unsigned long start, start_ack, end, proc_time;
    u16 state = 0, sharer = 0, r_size = 0, r_lock = 0, r_cnt = 0;
    struct cnthread_page **victims = NULL;
    PROFILE_POINT_TIME(CN_inv_other_data_prmpt)

    if (is_invalid != TRY_INVALIDATION) {
        BUG(); // this function should only be used for async invalidation, eviction should use find_and_evict
    }

    victims = kzalloc(sizeof(struct cnthread_page *) * num_pages, GFP_KERNEL);
    if (!victims) {
        BUG();
    }

    // make compiler happy
    (void)wcnt;
    addr &= PAGE_MASK; // may not be needed
    // send_cache_dir_check(tgid, addr & CNTHREAD_CACHLINE_MASK,
    //                      &state, &sharer, CN_SWITCH_REG_SYNC_NONE); // pull state from the switch (**high overhead**)
    pr_cache("Eviction triggered(async)-tgid: %u, addr: 0x%lx [0x%lx - 0x%lx], inval: %d, Dreq: %d, Ddel:%d / state: 0x%x, sharer: 0x%x\n",
           tgid, addr, start_addr, end_addr, is_invalid, data_required, remove_data, state, sharer);
    start = sched_clock();

    // Find all victims to evict
    rpage_idx = -1; 
    spin_lock(&cnthread_lock);
    spin_lock(&hash_list_lock);
    for (tmp_addr = start_addr; tmp_addr < end_addr; tmp_addr += PAGE_SIZE)
    {
        rpage_idx ++;
        if (tmp_addr == addr)   // Skip the target (already flushed in find_and_evict)
            continue;
        victims[rpage_idx] = find_cnpage_no_lock(tgid, tmp_addr);
        if (victims[rpage_idx])
        {
            victim_cnt++;
            // pr_cache("Cacheline (range)::Remove from list-tgid: %u, addr: 0x%lx [0x%lx - 0x%lx]\n",
            //          tgid, tmp_addr, start_addr, end_addr);
            cnthread_check_and_del_from_lru_list(victims[rpage_idx]);
        }
    }
    pr_cache("evict_range: found %d victims\n", victim_cnt);
    spin_unlock(&hash_list_lock);
    spin_unlock(&cnthread_lock);
    // For any remaining pages
    PROFILE_START(CN_inv_other_data_prmpt);
    __cnthread_evict_range(victims, num_pages, start_addr, addr, remove_data, is_invalid);
    if (from_preampt)
        PROFILE_LEAVE(CN_inv_other_data_prmpt);

#ifdef PRINT_CACHE_COHERENCE
    if (is_invalid && data_required)
    {
        // send_cache_dir_full_check(tgid, start_addr,
        //                         &state, &sharer, &r_size, &r_lock, &r_cnt, CN_SWITCH_REG_SYNC_NONE);
        pr_cache("After Data sent-tgid(w/ cacheline): %u, addr: 0x%lx, inval: %d, Dreq: %d, Ddel: %d / state: 0x%x, sharer: 0x%x, CPU: %d\n",
            tgid, addr, is_invalid, data_required, remove_data,
            state, sharer, smp_processor_id());
    }
#endif
    // Send final data ack - we invalidated or evicted whole region
    start_ack = jiffies;
    // @is_invalid = 0: eviction, TRY_INVALIDATION: actual invalidation, EVICT_BUT_RETRYING: eviction in invalidate mode
    cnthread_send_finish_ack(tgid, addr & PAGE_MASK, &send_ctx, 1);
    end = jiffies;
    proc_time = (end > start) ? jiffies_to_usecs(end - start) : 0;
#ifndef PRINT_CACHE_COHERENCE
    if (unlikely(proc_time >= DISAGG_SLOW_LOCK_REPORT_IN_USEC))    // report when suspicious
#endif
    {
        // send_cache_dir_check(tgid, start_addr,    // CNTHREAD_CACHLINE_MASK,
        //                      &state, &sharer, CN_SWITCH_REG_SYNC_NONE); // pull state from the switch (**high overhead**)
        // send_cache_dir_full_check(tgid, start_addr,
        //                         &state, &sharer, &r_size, &r_lock, &r_cnt, CN_SWITCH_REG_SYNC_NONE);
        printk(KERN_WARNING "Fin2 Ack sent-tgid(w/ cacheline): %u, addr: 0x%lx (0x%lx), inval: %d, Dreq: %d, Ddel: %d, Time: %lu us (ack: %u us), CPU: %d\n",
                tgid, addr, start_addr, is_invalid, data_required, remove_data,
                proc_time, (end >= start_ack) ? jiffies_to_usecs(end - start_ack) : 0, smp_processor_id());
        printk(KERN_WARNING "Fin2 Reg: state: 0x%x, sharer: 0x%x, size: %u, lock: %u, cnt: %u, CPU: %d\n", state, sharer, r_size, r_lock, r_cnt, smp_processor_id());
    }

    pr_cache("End evcit-tgid(w/ cacheline): %u, addr: 0x%lx, inval: %d, Dreq: %d, Ddel: %d\n",
             tgid, addr, is_invalid, data_required, remove_data);


    kfree(victims);
#if 0

    if (((is_invalid == EVICT_BUT_RETRYING) || (is_invalid == EVICT_FORCED)) && is_invalid && remove_data && (atomic_read(&cnline->used_page) <= 0))
    {
        spin_lock(&hash_list_lock);
        hash_del(&cnline->hnode);
        smp_wmb();
        if (is_invalid && spin_is_locked(&cnline->evict_lock))
            spin_unlock(&cnline->evict_lock);
        spin_unlock(&cnline->on_going_lock);
        add_cacheline_to_free_list(cnline); // clean and add to the free list
        spin_unlock(&hash_list_lock);

    }
    else
    {
out_evict:
        // Put the cnline back to the list and we will try again next time
        // (only possible for eviction)
        spin_lock(&cnthread_lock);
        list_add(&cnline->node, &cn_handler_lru_list);
        atomic_inc(&cnthread_list_counter);
        spin_unlock(&cnthread_lock);
        if (is_invalid && spin_is_locked(&cnline->evict_lock))
            spin_unlock(&cnline->evict_lock);
        spin_unlock(&cnline->on_going_lock);
    }
#endif
    return CNTHREAD_INVALIDATION_SUCCESS;
}
















int cnthread_evict_one(int threshold, int high_threshold)
{
    int cur_size, cur_cnt;
    struct cnthread_page *cnpage = NULL;
    int ret = 0;
    unsigned long addr = 0;
    unsigned int tgid = 0;
    struct range_lock lock;
    unsigned long page_index = 0;

    // Get one from the list
    if (cnthread_is_pressure())
    {
        spin_lock(&cnthread_lock);
        // Now we need to evict one: from the last (oldest)
        // Follow FIFO model in LegoOS (instead of LRU or LFU)
        if (cn_handler_lru_list.prev && (cn_handler_lru_list.prev != &cn_handler_lru_list))
        {   

            cnpage = container_of(cn_handler_lru_list.prev, struct cnthread_page, lru_list_node);
            tgid = cnpage->tgid;
            addr = cnpage->addr;
        }
        spin_unlock(&cnthread_lock);
    }


    if (!cnpage)
    {
        return -1;
    }

    // Main eviction function
    page_index = addr >> PAGE_SHIFT;
    range_lock_init(&lock, page_index, page_index);
    if(cache_range_try_lock(tgid, &lock)) {
        ret = cnthread_find_and_evict(tgid, addr, PAGE_SIZE, 0, 0, 1, NULL, NULL, 0);
        cache_range_unlock(tgid, &lock);
        if (ret == CNTHREAD_INVALIDATION_SUCCESS)
        {
            // Here we print number before this eviction
            cur_cnt = (int)atomic_read(&cnthread_evict_counter);
            cur_size = atomic_read(&cnthread_list_counter);
            if (unlikely(cur_cnt % 1000 == 0))
            {

                if ((cur_size > high_threshold) || (cur_cnt % 10000 == 0))
                {
                    atomic_set(&cnthread_evict_counter, 0);
                    cnthread_print_cache_status();
                }
            }             
            atomic_inc(&cnthread_evict_counter);
        } else {
            BUG();
        }
    }   

    return ret;
}

static atomic_t DEBUG_trigger_eviction;
static unsigned int DEBUG_eviction_tgid = 0;
static unsigned long DEBUG_eviction_addr = 0;

void DEBUG_trigger_evict(unsigned int tgid, unsigned int addr)
{
    int tval = 0;
    DEBUG_eviction_tgid = tgid;
    DEBUG_eviction_addr = addr;
    barrier();
    atomic_set(&DEBUG_trigger_eviction, 1);
    while(1)
    {
        msleep(100);
        tval = atomic_read(&DEBUG_trigger_eviction);
        if(!tval)
            break;
    }
}
EXPORT_SYMBOL(DEBUG_trigger_evict);

unsigned long get_dummy_page_dma_addr(int cpu_id)
{
    return dummy_page_dma_addr[cpu_id];
}

void *get_dummy_page_buf_addr(int cpu_id)
{
    return dummy_page_buf[cpu_id];
}
EXPORT_SYMBOL(get_dummy_page_buf_addr);

unsigned long get_dummy_inv_page_dma_addr(void)
{
    return dummy_inv_page_dma_addr;
}

static void cnthread_initial_kmap(void)
{
    struct cnthread_page *cnpage = NULL;
    int i = 0;
    spin_lock(&free_page_get_lock);
    list_for_each_entry(cnpage, &cn_free_page_list, free_list_node)
    {
        cnpage->kmap = kmap_atomic(cnpage->kpage);
        cnpage->dma_addr = mapping_dma_region_page(cnpage->kmap);
    }
    spin_unlock(&free_page_get_lock);
    for (i = 0; i < DISAGG_NUM_CPU_CORE_IN_COMPUTING_BLADE * 3; i++)
    {
        dummy_page_buf[i] = kmalloc(PAGE_SIZE, GFP_KERNEL);
        memset(dummy_page_buf[i], 0, PAGE_SIZE);
        dummy_page_dma_addr[i] = mapping_dma_region_page(dummy_page_buf[i]);
    }
    dummy_inv_page_buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
    memset(dummy_inv_page_buf, 0, PAGE_SIZE);
    dummy_inv_page_dma_addr = mapping_dma_region_page(dummy_inv_page_buf);
    pr_info("* Kmapping and DMA addresses are initialied\n");
}

// Currently, it just delete from the list
// since we assume the actual page was unmapped by using vm_mummap
// (so should NOT be used *inside* invalidation routine)
int _cnthread_delete_from_list(u16 tgid, unsigned long address, int already_locked)
{
    struct cnthread_page *cnpage;
    struct range_lock page_pos_lock;
    address &= PAGE_MASK;
    range_lock_init(&page_pos_lock, address >> PAGE_SHIFT, address >> PAGE_SHIFT);

    if (!already_locked) {
        spin_lock(&cnthread_lock);
        spin_lock(&hash_list_lock);
    }
    cnpage = find_cnpage_no_lock(tgid, address);
    
    if (cnpage && (atomic_read(&cnpage->is_used) == IS_PAGE_USED))
    {
        spin_lock(&free_page_get_lock);
        cnthread_put_page_no_lock(cnpage);
        spin_unlock(&free_page_get_lock);
    } else {
        if (!already_locked)
        {
            spin_unlock(&hash_list_lock);
            spin_unlock(&cnthread_lock);
        }
        return -1;
    }
    if (!already_locked) {
        spin_unlock(&hash_list_lock);
        spin_unlock(&cnthread_lock);
    }
    return 0;
}

int cnthread_delete_from_list(u16 tgid, unsigned long address)
{
    return _cnthread_delete_from_list(tgid, address, 0);
}

int cnthread_delete_from_list_no_lock(u16 tgid, unsigned long address)
{
    return _cnthread_delete_from_list(tgid, address, 1);
}

DEFINE_PROFILE_POINT(cnthread_delete_one_cnreq) 
static int _cnthread_delete_all_request(u16 tgid, int flush_data)
{
    // this function removes all cnpage with tgid
    PROFILE_POINT_TIME(cnthread_delete_one_cnreq)
    struct cnthread_page *cnpage;
    struct list_head *next = NULL;
    int deleted = 0;
    unsigned long addr;
    printk(KERN_DEFAULT "Del all req.: tgid: %u\n", (unsigned int)tgid);

restart_search:
    spin_lock(&cnthread_lock);
    spin_lock(&hash_list_lock);
    if (!list_empty(&cn_handler_lru_list))
    {
        cnpage = container_of(cn_handler_lru_list.next, struct cnthread_page, lru_list_node);
        while(1)
        {
            next = cnpage->lru_list_node.next;
            if (cnpage && cnpage->tgid == tgid)
            {
                if (flush_data)
                {
                    int sync_ret = 0;
                    addr = cnpage->addr;
                    spin_unlock(&hash_list_lock);
                    spin_unlock(&cnthread_lock);
                    sync_ret = cnthread_find_and_evict(tgid, addr, PAGE_SIZE, EVICT_FORCED, 0, 1, NULL, NULL, 0);
                    goto restart_search;
                }
                else
                {
                    if (!cnthread_delete_from_list_no_lock(tgid, cnpage->addr)) // cnthread_lock and hash_list_lock was already hold
                        deleted ++;
                }
            }

            if (!next || next == &cn_handler_lru_list)
                break;

            cnpage = container_of(next, struct cnthread_page, lru_list_node);
        }
    }
    spin_unlock(&hash_list_lock);
    spin_unlock(&cnthread_lock);
    if (deleted > 0)
    {
        cnthread_delete_all_request(tgid);  // Check again (to remove pending ones)
    }
    return 0;
}

int cnthread_delete_all_request(u16 tgid)
{
    return _cnthread_delete_all_request(tgid, 0);
}

int cnthread_flush_all_request(u16 tgid)
{
    return _cnthread_delete_all_request(tgid, 1);
}

int cnthread_clean_up_non_existing_entry(u16 tgid, struct mm_struct *mm)
{
    // remove all pages with same tgid
    struct cnthread_page *cnpage;
    struct list_head *next = NULL;
    struct vm_area_struct *vma;

    spin_lock(&cnthread_lock);
    spin_lock(&hash_list_lock);
    if (!list_empty(&cn_handler_lru_list))
    {
        cnpage = container_of(cn_handler_lru_list.next, struct cnthread_page, lru_list_node);
        while (1)
        {
            next = cnpage->lru_list_node.next;
            if (cnpage && cnpage->tgid == tgid)
            {
                vma = find_vma(mm, cnpage->addr); // current or next vma
                if (!vma || !(vma->vm_start > cnpage->addr))    // no vma
                {
                    cnthread_delete_from_list_no_lock(tgid, cnpage->addr); // cnthread_lock and hash_list_lock was already hold
                }
                // Update mm (it was EXECed!)
                if (cnpage->mm != mm)
                {
                    cnpage->mm = mm;
                }
            }
            if (!next || next == &cn_handler_lru_list)
                break;

            cnpage = container_of(next, struct cnthread_page, lru_list_node);
        }
    }
    spin_unlock(&hash_list_lock);
    spin_unlock(&cnthread_lock);
    return 0;
}

int DEBUG_try_evict(u32 tgid, u64 address)
{
    struct fault_data_struct payload;
    int res = -1;
    u32 tot_size = sizeof(struct fault_reply_struct);
    struct fault_reply_struct reply;
    void *buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
    if (unlikely(!buf))
    {
        BUG();
    }
    payload.tgid = tgid;
    payload.address = address;
    payload.data = buf;
    payload.data_size = PAGE_SIZE;
    printk(KERN_DEFAULT "Evict to 0x%llx...\n", address);
    // Check the size of the received data: it should have at least default struct size
    res = send_msg_to_memory_rdma(DISSAGG_DATA_PUSH, &payload, PAGE_SIZE,
                                  &reply, tot_size);
    kfree(buf);
    return res;
}

inline void create_invalidation_rdma_ack(char *inval_buf, u64 fva, u32 rkey, u32 qp)
{
    u16 pkey = ROCE_DEFAULT_PKEY;
    // Opcode @ CACHELINE_ROCE_OFFSET_TO_OPCODE
    u16 preamble = CACHELINE_ROCE_ACK_PREAMBLE;
    memcpy(&inval_buf[CACHELINE_ROCE_OFFSET_TO_OPCODE], &preamble, sizeof(preamble));
    // Pkey
    pkey = __cpu_to_be16(pkey);
    memcpy(&(inval_buf[CACHELINE_ROCE_OFFSET_TO_PKEY]), &pkey, sizeof(pkey));
    // QP
    qp = CACHELINE_BYPASS_MULTICAST_MASK;   // last 16 bits, to make it always pass multicasting mask
    qp = __cpu_to_be32(qp);
    memcpy(&(inval_buf[CACHELINE_ROCE_OFFSET_TO_QP]), &qp, sizeof(qp));
    // virtual address
    fva = __cpu_to_be64(fva); // network endian to host endian (e.g., big to little in usual X86)
    memcpy(&(inval_buf[CACHELINE_ROCE_OFFSET_TO_VADDR]), &fva, sizeof(fva));
    // rkey
    rkey &= ~((u32)CACHELINE_ROCE_RKEY_INVALIDATION_MASK);          // clear invalidation mask
    rkey = __cpu_to_be32((rkey & ~((u32)0xf)) | CACHE_STATE_INV_ACK); // set state as CACHE_STATE_INV_ACK
    memcpy(&(inval_buf[CACHELINE_ROCE_OFFSET_TO_RKEY]), &rkey, sizeof(rkey));


}
EXPORT_SYMBOL(create_invalidation_rdma_ack);

static __always_inline void inc_inval_rdma_ack(int head_idx)
{
    // for DISAGG_QP_NUM_INVAL_BUF
    inval_buf_ack_head[head_idx] = (inval_buf_ack_head[head_idx] + 1) % CACHELiNE_ROCE_INVAL_BUF_LENGTH;
}

static inline void *check_inval_rdma_ack(int *msg_is_empty, int head_idx)
{
    unsigned int cur_ack_pos;
    if (unlikely(!base_inval_buf[head_idx]))
        return NULL;
    // Check current head
    cur_ack_pos = inval_buf_ack_head[head_idx] * CACHELINE_ROCE_HEADER_LENGTH;
    if (base_inval_buf[head_idx][cur_ack_pos])
        *msg_is_empty = 0;
    else
        *msg_is_empty = 1;

    return ((base_inval_buf[head_idx][cur_ack_pos] & 0xff) == (CACHELINE_ROCE_ACK_PREAMBLE_8U)) ? &(base_inval_buf[head_idx][cur_ack_pos]) : NULL;
}

static inline int check_inval_rdma_type(unsigned char* rdma_buf)
{
    u32 ret = *((u32 *)(&(rdma_buf[CACHELINE_ROCE_OFFSET_TO_RKEY])));
    ret = __be32_to_cpu(ret);
    if (ret & CACHELINE_ROCE_RKEY_INVALIDATION_MASK)
    {
        u32 qp_val = *((u32 *)(&(rdma_buf[CACHELINE_ROCE_OFFSET_TO_QP])));
        qp_val = __be32_to_cpu(qp_val);
        //
        if (qp_val & CACHELINE_ROCE_QP_INV_REQUESTER)
            return MSG_OVER_RDMA_SELF_INV;
        else
            return MSG_OVER_RDMA_INV_REQ;
    }else{
        return MSG_OVER_RDMA_ACK;
    }
}

static void inval_rdma_retrieve_base(struct cnthread_rdma_msg_ctx *ctx, unsigned char* rdma_buf)
{
    if (unlikely(!ctx))
        return;

    ctx->ip_val = *((u32*)(&(rdma_buf[CACHELINE_ROCE_VOFFSET_TO_IP])));
    // Find vaddr
    ctx->fva = *((u64 *)(&(rdma_buf[CACHELINE_ROCE_OFFSET_TO_VADDR])));
    ctx->fva = __be64_to_cpu(ctx->fva); // Network endian to host endian (e.g., big to little in usual X86)
    ctx->ret = *((u32 *)(&(rdma_buf[CACHELINE_ROCE_OFFSET_TO_RKEY])));
    ctx->ret = __be32_to_cpu(ctx->ret);
    ctx->pid = (unsigned int)(ctx->fva >> 48);
    ctx->vaddr = ctx->fva & (~MN_VA_PID_BIT_MASK); // Pure virtual address (48bits)
    if (ctx->pid == DISAGG_KERN_TGID)
    {
        ctx->vaddr |= MN_VA_PID_BIT_MASK;  // kernel's virtual address starts from 0xffff
    }
    ctx->state = ctx->ret & CACHELINE_ROCE_RKEY_STATE_MASK;
    pr_cache("[cRDMA] Buf[0x%lx] || FVA: 0x%llx => PID: %u, VA: 0x%llx || RKEY: 0x%x, IP: 0x%lx\n",
                (unsigned long)rdma_buf, ctx->fva, ctx->pid, ctx->vaddr, ctx->ret, (unsigned long)ctx->ip_val);
}

static void inval_rdma_retrieve_ext(struct cnthread_rdma_msg_ctx *ctx, unsigned char* rdma_buf)
{
    if (unlikely(!ctx))
        return;
    ctx->requester = *((u16 *)(&(rdma_buf[CACHELINE_ROCE_OFFSET_TO_PKEY])));
    ctx->requester = __be16_to_cpu(ctx->requester);
    ctx->qp_val = *((u32 *)(&(rdma_buf[CACHELINE_ROCE_OFFSET_TO_QP])));
    ctx->qp_val = __be32_to_cpu(ctx->qp_val);
    ctx->sharer = ctx->qp_val & CACHELINE_ROCE_QP_SHARER_MASK;
    ctx->psn = *((u32 *)(&(rdma_buf[CACHELINE_ROCE_OFFSET_TO_ACKREQ])));
    ctx->psn = (__be32_to_cpu(ctx->psn) & CACHELINE_ROCE_PSN_MASK);  // the last 24 bits
}

static __always_inline void serve_inval_rdma_ack(unsigned char* rdma_buf, int msg_type)
{
    struct cnthread_rdma_msg_ctx rdma_ctx;
    struct cache_waiting_node *w_node = NULL;
    PROFILE_POINT_TIME(CN_update_roce_recv)
    PROFILE_START(CN_update_roce_recv);
    // Mark that we picked this message
    rdma_buf[0] = 0x0;
    inval_rdma_retrieve_base(&rdma_ctx, rdma_buf);
    w_node = find_node(rdma_ctx.pid, (unsigned long)rdma_ctx.vaddr);
    inval_rdma_retrieve_ext(&rdma_ctx, rdma_buf);   // to get sharer

    // Main message processing routine
    if (msg_type == MSG_OVER_RDMA_SELF_INV)
    {
        int val;
        val = get_target_counter_from_bitmask((u16)rdma_ctx.sharer); // number of sharers in the shared list
        if (w_node)
        {
            pr_cache("[cRDMA] Pre-Ack for Node: 0x%lx, PID: %u, VA: 0x%llx, State: %u, Sharer: 0x%x\n",
                    (unsigned long)w_node, rdma_ctx.pid, rdma_ctx.vaddr, 
                    (unsigned int)rdma_ctx.state, (unsigned int)rdma_ctx.sharer);
            set_target_counter(w_node, val - 1);
        }
        else
        {
#ifndef MIND_USE_TSO
            pr_info("ERROR: Cannot find waiting node for PID: %u, VA: 0x%llx\n",
                    rdma_ctx.pid, rdma_ctx.vaddr);
#endif
        }
    }
    else if(msg_type == MSG_OVER_RDMA_ACK)
    {
        if (w_node)
        {
            pr_cache("[cRDMA] Ack for Node: 0x%lx, PID: %u, VA: 0x%llx, State: %u, Sharer: 0x%x\n",
                    (unsigned long)w_node, rdma_ctx.pid, rdma_ctx.vaddr, 
                    (unsigned int)rdma_ctx.state,  (unsigned int)rdma_ctx.sharer);
            if (rdma_ctx.state == CACHE_STATE_INV_ACK)
            { // ACK from other node
                // pr_info("[ACK_NODE] Ack for Node: 0x%lx, PID: %u, VA: 0x%llx, State: %u, Sharer: 0x%x\n",
                //         (unsigned long)w_node, rdma_ctx.pid, rdma_ctx.vaddr,
                //         (unsigned int)rdma_ctx.state, (unsigned int)rdma_ctx.sharer);
                increase_wait_counter(w_node);
            }
            else
            { // ACK directly from switch
                set_target_counter(w_node, 0); // all ack received
            }
            PROFILE_LEAVE(CN_update_roce_recv);
        }
        else
        {
#ifndef MIND_USE_TSO
            pr_info("[cRDMA] Cannot find waiting node for PID: %u, VA: 0x%llx, State: %u\n",
                    rdma_ctx.pid, rdma_ctx.vaddr, (unsigned int)rdma_ctx.state);
#endif
        }
    }else{
        BUG();  // ERROR: Unknown message type
    }
}











































static __always_inline int check_inval_req_list_and_try(int from_inv, struct cnthread_inv_msg_ctx *inv_ctx, int is_async)
{
    struct cnthread_rdma_msg_ctx *rdma_ctx = NULL;
    PROFILE_POINT_TIME(CN_update_roce_inv_req_evt)
    PROFILE_POINT_TIME(CN_update_roce_inv_req_inv)
    PROFILE_POINT_TIME(CN_update_roce_inv_req_prmpt)
    PROFILE_POINT_TIME(CN_update_roce_inv_queue)
    PROFILE_POINT_TIME(CN_update_roce_async_inv_queue)
    PROFILE_POINT_TIME(CN_latency_10us)
    PROFILE_POINT_TIME(CN_latency_50us)
    PROFILE_POINT_TIME(CN_latency_100us)
    PROFILE_POINT_TIME(CN_latency_250us)
    PROFILE_POINT_TIME(CN_latency_500us)
    PROFILE_POINT_TIME(CN_latency_1000us)










    // Check lock
    if (inv_ctx)
    {
        int ret = CNTHREAD_RET_NORMAL;
        unsigned int dir_size;
        rdma_ctx = &inv_ctx->rdma_ctx;
        dir_size = (unsigned int)((rdma_ctx->ret & CACHELINE_ROCE_RKEY_SIZE_MASK) >> CACHELINE_ROCE_RKEY_SIZE_SHIFT);
        if (!is_async) { // sync inval: initiate range lock and try lock range
            u64 dir_size_bytes = size_index_to_size(dir_size);
            u64 start_addr = rdma_ctx->vaddr / dir_size_bytes * dir_size_bytes;
            u64 end_addr = start_addr + dir_size_bytes - 1;
            range_lock_init(&inv_ctx->inv_lock, start_addr >> PAGE_SHIFT, end_addr >> PAGE_SHIFT);




            // Use blocking lock API
            cache_range_lock(rdma_ctx->pid, &inv_ctx->inv_lock);
            inv_ctx->is_locked = 1;


        }

        if (inv_ctx->is_locked)
        {
            int data_required, inv_ret = 0;
            u32 inval_mod;
            int shared_inval, self_request;

            int queue_length = 0;
            // Counters for invalidation queue
            if (is_async == 0)
            {
                atomic_dec(&cnthread_inv_req_counter); // do not need to be accurate and synchronized (in timing)
                PROFILE_ADD_MEASUREMENT(CN_update_roce_inv_queue, (unsigned long)(sched_clock() - inv_ctx->last_update));
            }
            if (is_async == 1)
            {
                atomic_dec(&cnthread_async_req_counter);
                PROFILE_ADD_MEASUREMENT(CN_update_roce_async_inv_queue, (unsigned long)(sched_clock() - inv_ctx->last_update));
            }
            inv_ctx->last_update = sched_clock();
            PROFILE_START(CN_update_roce_inv_req_evt);
            PROFILE_START(CN_update_roce_inv_req_inv);
            PROFILE_START(CN_update_roce_inv_req_prmpt);

            // Original QP used by the requester: right 8 bits, we assume that left 16 bits = 0
            inv_ctx->original_qp = (rdma_ctx->ret & CACHELINE_ROCE_RKEY_QP_MASK) >> CACHELINE_ROCE_RKEY_QP_SHIFT;
            create_invalidation_rdma_ack(inv_ctx->inval_buf, rdma_ctx->fva, rdma_ctx->ret, rdma_ctx->qp_val);
            *((u32 *)(&(inv_ctx->inval_buf[CACHELINE_ROCE_VOFFSET_TO_IP]))) = rdma_ctx->ip_val;
            // Invalidation context
            data_required = ((rdma_ctx->ret & CACHELINE_INVALIDATION_DATA_REQ) > 0) ? 1 : 0;
            inval_mod = rdma_ctx->ret & CACHELINE_ROCE_RKEY_INVALIDATION_MASK; // if 0, then not invalidation
            shared_inval = ((inval_mod & CACHELINE_INVALIDATION_SHARED) == CACHELINE_INVALIDATION_SHARED) ? 1 : 0;
            self_request = (rdma_ctx->qp_val & CACHELINE_ROCE_QP_INV_REQUESTER) ? 1 : 0;
            pr_cache("[cRDMA] Inval (State: %u, Sharer: 0x%04x, Size: %u, ReqId: %d, Type: %c, IamReq: %d, QP: %u) for PID: %u, VA: 0x%llx, IP:0x%x\n",
                     (unsigned int)rdma_ctx->state, (unsigned int)rdma_ctx->sharer, dir_size, get_id_from_requester(rdma_ctx->requester),
                     shared_inval ? 'S' : 'M', self_request, inv_ctx->original_qp, rdma_ctx->pid, rdma_ctx->vaddr, rdma_ctx->ip_val);

            queue_length = is_async ? atomic_read(&cnthread_async_req_counter) : atomic_read(&cnthread_inv_req_counter);







            barrier();
            if (is_async == 0)
            {
                inv_ret = cnthread_find_and_evict(rdma_ctx->pid, rdma_ctx->vaddr, size_index_to_size(dir_size), TRY_INVALIDATION,
                                        data_required, !shared_inval, inv_ctx->inval_buf, inv_ctx, from_inv == PROFILE_CNTHREAD_INV_ACK_SERV_FROM_PRMPT);
                barrier();
                if (inv_ret == CNTHREAD_INVALIDATION_SUCCESS) {
                    ret = CNTHREAD_RET_NEXT_TASK;
                } else {
                    BUG();
                }
            }
            else
            {
                inv_ret = cnthread_find_and_evict_remaining(rdma_ctx->pid, rdma_ctx->vaddr, size_index_to_size(dir_size), TRY_INVALIDATION,
                                                data_required, !shared_inval, inv_ctx->inval_buf, inv_ctx, from_inv == PROFILE_CNTHREAD_INV_ACK_SERV_FROM_PRMPT);
                barrier();
                cache_range_unlock(rdma_ctx->pid, &inv_ctx->inv_lock);
                ret = CNTHREAD_RET_RELEASE;
            }














            if (from_inv == PROFILE_CNTHREAD_INV_ACK_SERV_FROM_EVICT)
                PROFILE_LEAVE(CN_update_roce_inv_req_evt);
            else if (from_inv == PROFILE_CNTHREAD_INV_ACK_SERV_FROM_INVAL)
                PROFILE_LEAVE(CN_update_roce_inv_req_inv);
            else if (from_inv == PROFILE_CNTHREAD_INV_ACK_SERV_FROM_PRMPT)
                PROFILE_LEAVE(CN_update_roce_inv_req_prmpt);
            return ret;
        }
        else
        {
            // Put it back to the list
            if (is_async == 0)
            {
                ret = CNTHREAD_RET_RETRY;
            }
            else
            {
                BUG();  // must not happen, it must be locked during sync invalidation
            }
            return ret;
        }
    }
    return CNTHREAD_RET_ERROR;
}

static int cnthread_sync_invalidation(void *inv_argv);
static int cnthread_async_invalidation(void *inv_argv);
static void cnthread_clean_inv_arg(int main_ret, void *inv_argv);
static int cnthread_sync_invalidation(void *inv_argv)
{
    int ret = check_inval_req_list_and_try(PROFILE_CNTHREAD_INV_ACK_SERV_FROM_INVAL,
                                           &((struct cnthread_inv_argv *)inv_argv)->inv_ctx, 0);
    if (ret == CNTHREAD_RET_NEXT_TASK)
    {


        struct cnthread_task_desc *task_desc = kzalloc(sizeof(*task_desc), GFP_KERNEL);
        task_desc->argv = inv_argv;
        task_desc->init_ftn = NULL;
        task_desc->main_ftn = cnthread_async_invalidation;
        task_desc->clean_ftn = cnthread_clean_inv_arg;
        ((struct cnthread_inv_argv *)inv_argv)->inv_ctx.last_update = sched_clock();
        cnthread_enqueue_task(CNTHREAD_ASYNC_INV_PRIO, task_desc);
    }

    return ret;
}


static int disagg_fs_sync_invalidation(void *inv_argv_pointer)
{
    struct cnthread_rdma_msg_ctx *rdma_ctx;
    struct cnthread_inv_msg_ctx *inv_ctx;
    struct cnthread_inv_argv *inv_argv = inv_argv_pointer; 
    struct cnthread_inval_pte *ctx = ctx;
    if (!inv_argv)
        BUG();

    inv_ctx = &(inv_argv->inv_ctx);
    rdma_ctx = &(inv_ctx->rdma_ctx);




    call_invalidate_page_callback(rdma_ctx->vaddr, &((struct cnthread_inv_argv *)inv_argv_pointer)->inv_ctx);





    
    return 0;
}


static int cnthread_async_invalidation(void *inv_argv)
{
    return check_inval_req_list_and_try(PROFILE_CNTHREAD_INV_ACK_SERV_FROM_INVAL,
                                        &((struct cnthread_inv_argv *)inv_argv)->inv_ctx, 1);
}

static void cnthread_clean_inv_arg(int main_ret, void *inv_argv)
{
    if (main_ret == CNTHREAD_RET_RELEASE)
    {
        if (inv_argv)
        {
            kfree(((struct cnthread_inv_argv *)inv_argv));
        }
    }
}

static __always_inline void serve_inval_rdma_req(unsigned char* rdma_buf)
{
    PROFILE_POINT_TIME(CN_update_roce_inv_req)
    struct cnthread_task_desc *task_desc = kzalloc(sizeof(struct cnthread_task_desc), GFP_KERNEL);
    struct cnthread_inv_argv *inv_argv = kzalloc(sizeof(struct cnthread_inv_argv), GFP_KERNEL);
    struct cnthread_inv_msg_ctx *inv_ctx;
    struct cnthread_rdma_msg_ctx *rdma_ctx;
    if (!task_desc || !inv_argv)
        BUG();

    inv_ctx = &(inv_argv->inv_ctx);
    rdma_ctx = &(inv_ctx->rdma_ctx);
    // Mark that we picked this message
    rdma_buf[0] = 0x0;
    inval_rdma_retrieve_base(rdma_ctx, rdma_buf);
    inval_rdma_retrieve_ext(rdma_ctx, rdma_buf);   // to get sharer, qp_val

    // Main routine
    // Prepare inv_ctx and put invalidation request to the list
    inv_ctx->sk = recv_socket;
    inv_ctx->rdma_buf = rdma_buf;  // should be fine until we put 0xff into rdma_buf[0] again.
    PROFILE_START(CN_update_roce_inv_req);
    memcpy(inv_ctx->dummy_buf, rdma_buf, CACHELINE_ROCE_HEADER_LENGTH); // this buffer will be used for sending dummy data
    inv_ctx->addr_in.sin_family = AF_INET;
    inv_ctx->addr_in.sin_addr.s_addr = rdma_ctx->ip_val;   // same network endian
    inv_ctx->is_locked = 0;

    inv_ctx->last_update = sched_clock();
    task_desc->argv = (void*)inv_argv;



    task_desc->init_ftn = NULL;
    if(call_shmem_address_check(rdma_ctx->vaddr,0)==0){
	    task_desc->main_ftn = cnthread_sync_invalidation;
	    task_desc->clean_ftn = cnthread_clean_inv_arg;
    }
    else{

	task_desc->main_ftn = disagg_fs_sync_invalidation;

	task_desc->clean_ftn = cnthread_clean_inv_arg;
    }
    cnthread_enqueue_task(CNTHREAD_SYNC_INV_PRIO, task_desc);
    PROFILE_LEAVE(CN_update_roce_inv_req);
}

static __always_inline int get_next_buf_head_idx(void)
{
    return ((unsigned int)atomic_inc_return(&cnthread_buf_head_inv_splitter)) % DISAGG_QP_NUM_INVAL_BUF;
}

static __always_inline int one_step_inval_rdma_ack(int from_inv, int head_idx)
{
    PROFILE_POINT_TIME(CN_update_roce_inv_ack_inv)
    PROFILE_POINT_TIME(CN_update_roce_inv_ack_evt)
    PROFILE_POINT_TIME(CN_update_roce_inv_ack_prmpt)
    int msg_is_empty = 1, msg_type = 0;
    char *rdma_buf = NULL;
    rdma_buf = check_inval_rdma_ack(&msg_is_empty, head_idx);
    if (!rdma_buf)
    {
        if (msg_is_empty)
        {
            return -1;  // no more ack
        }
        else
        {   // there is other than preamble
            BUG();
        }
    }
    PROFILE_START(CN_update_roce_inv_ack_inv);
    PROFILE_START(CN_update_roce_inv_ack_evt);
    PROFILE_START(CN_update_roce_inv_ack_prmpt);
    msg_type = check_inval_rdma_type(rdma_buf);
    pr_cache("Message type: %d [ack head: %u]", msg_type, inval_buf_ack_head[head_idx]);
    if (msg_type != MSG_OVER_RDMA_INV_REQ)
    {
        // Serve ack here
        serve_inval_rdma_ack(rdma_buf, msg_type);
        if (from_inv == PROFILE_CNTHREAD_INV_ACK_SERV_FROM_INVAL)
            PROFILE_LEAVE(CN_update_roce_inv_ack_inv);
        else if (from_inv == PROFILE_CNTHREAD_INV_ACK_SERV_FROM_PRMPT)
            PROFILE_LEAVE(CN_update_roce_inv_ack_prmpt);
        else
            PROFILE_LEAVE(CN_update_roce_inv_ack_evt);
    }else{
        // Parse and put the requests into the list
        serve_inval_rdma_req(rdma_buf);
        atomic_inc(&cnthread_inv_req_counter);  // do not need to be accurate and synchronized (in timing)
    }
    __cnthread_debug_buffer_record[inval_buf_ack_head[head_idx]] = msg_type;  // record the last message type
    inc_inval_rdma_ack(head_idx);
    return 0;
}

void try_invalidation_lookahead(int from_inv, int lookahead)
{
    int rcv_i = 0;
    int head_idx = 0;
    spin_lock(&cnthread_inval_ack_lock);
    head_idx = get_next_buf_head_idx();
    spin_unlock(&cnthread_inval_ack_lock);
    if (spin_trylock(&cnthread_inval_head_buf_lock[head_idx]))
    {
        // Foresee ACKs
        for (rcv_i = 0; rcv_i < lookahead; rcv_i++)
        {
            if (one_step_inval_rdma_ack(from_inv, head_idx))
            {
                break;
            }
        }
        mb();
        spin_unlock(&cnthread_inval_head_buf_lock[head_idx]);
    }
}
EXPORT_SYMBOL(try_invalidation_lookahead);

static __always_inline int receive_next_request(int from_inv)
{
    int i = 0, locked_req = 0;
    struct cnthread_task_desc *task_desc = NULL;
    get_cpu();
    for (i = 0; i < DISAGG_QP_NUM_INVAL_BUF; i++)
        try_invalidation_lookahead(from_inv, SERVE_ACK_PER_INV);

    //Check priority queue
    for (i = 0; i < CNTHREAD_TASK_QUEUE_NUM; i++)
    {
        task_desc = dequeue_task(i);
        if (task_desc)
        {


            int res = CNTHREAD_RET_ERROR;

            if (task_desc->init_ftn)
            {
                task_desc->init_ftn(task_desc->argv);
            }

            if (task_desc->main_ftn)
            {
                res = task_desc->main_ftn(task_desc->argv);
            }

            if (task_desc->clean_ftn)
            {
                task_desc->clean_ftn(res, task_desc->argv);
            }

            if (res == CNTHREAD_RET_RETRY)
            {
                cnthread_enqueue_task_retry(i, task_desc);   // retry later
                locked_req ++;
                if (locked_req <= 1)
                    i --;   // retry the current priority again
                continue;
            }else{
                kfree(task_desc);   // we finish to use this task descriptor
            }
            put_cpu();
            return 0;
        }
    }














    // No invalidation, return with -1
    put_cpu();
    return -1;
}

// == EVICTION HANDLER ROUTINE == //
int cnthread_worker_handler(void *data)
{
    struct cnthread_handler_data *h_data = (struct cnthread_handler_data *)data;
    int *after_init_stage = h_data->init_stage;
    int loop_i = 0;

    allow_signal(SIGKILL | SIGSTOP);
    // Wait until roce kernel module is initialized (and connected to the switch)
    while (!(*after_init_stage))
    {
        usleep_range(10, 10);
    }
    pin_current_thread(h_data->pin_core_id);
    pr_info("CN_worker_thread [%d] has been started\n", h_data->worker_id);

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

        if (receive_next_request(PROFILE_CNTHREAD_INV_ACK_SERV_FROM_EVICT) < 0)
        {
            if (unlikely(loop_i >= DISAGG_NET_CTRL_POLLING_SKIP_COUNTER))
            {
                loop_i = 0;
                usleep_range(10, 10);
            }
            loop_i++;
        }
    }

release: // Please release memory here
    if (data)
    {
        kfree(data);
        data = NULL; // meaningless
    }
    return 0;
}

// == INVALIDATION HANDLER ROUTINE == //
// Main body of the invalidation handler
static unsigned long inval_timer_start = 0;
int cnthread_main_handler(void *data)
{
    struct cnthread_handler_data *h_data = (struct cnthread_handler_data *)data;
    int *after_init_stage = h_data->init_stage;
    int i = 0, loop_i = 0;
    unsigned long inval_timer_end;

    allow_signal(SIGKILL | SIGSTOP);
    atomic_set(&DEBUG_trigger_eviction, 0);
    cnthread_initial_task_queues();

    // Initialize socket
    while (!recv_buf)
    {
        recv_buf = kmalloc(_recv_buf_size, GFP_KERNEL);
        msleep(1);
    }

    while (!ack_buf)
    {
        ack_buf = kmalloc(_recv_buf_size, GFP_KERNEL);
        msleep(1);
    }
    memset(ack_buf, 0, _recv_buf_size);
    pr_info("CN_ack_handler has been started\n");

    // Wait until roce module is initialized (and connected to the switch)
    while (!(*after_init_stage))
    {
        usleep_range(10, 10);
    }
    // Initialize cache pages (and corresponding DMA mappings)
    cnthread_initial_kmap();

    if (!is_cache_ack_pinned)
        pin_current_thread(h_data->pin_core_id);
    is_cache_ack_pinned = 1;

    udp_initialize(&recv_socket, DEFAULT_UDP_PORT, 1);
    udp_initialize(&send_socket, DEFAULT_UDP_SEND_PORT, 1);
    send_ctx.sk = send_socket;

    // Set up invalidation rdma receiver
    for (i = 0; i < DISAGG_QP_NUM_INVAL_BUF; i++)
    {
        base_inval_buf[i] = get_inval_buf_ptr(i);
        pr_info("CN_thread_handler [inval buf[%d]: 0x%lx]\n",
                i, (unsigned long)base_inval_buf[i]);
    }

    // initialize kernel shared memory, as we initialized cache invaidation metadata
    init_kernel_shared_mem();

    // Send initial test message
    inval_timer_start = jiffies;    // intial timer
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

        // Try evition
	    cnthread_evict_one(_threshold, _high_threshold);

        if (receive_next_request(PROFILE_CNTHREAD_INV_ACK_SERV_FROM_INVAL) < 0)
        {
            if (unlikely(loop_i >= DISAGG_NET_CTRL_POLLING_SKIP_COUNTER))
            {
                loop_i = 0;
                usleep_range(10, 10);
            }
            loop_i++;
        }







        inval_timer_end = jiffies;
        if (unlikely((inval_timer_end > inval_timer_start) 
                     && (jiffies_to_msecs(inval_timer_end - inval_timer_start) > (unsigned long)CNTHREAD_HEARTBEAT_IN_MS)))
        {
            inval_timer_start = inval_timer_end;
            printk(KERN_DEFAULT "CNTHREAD: HeartBeat from Invalidation handler: CPU[%d] (head: %u / ack_head: %u, %u) Free(p: %d)\n",
                   (int)smp_processor_id(), inval_buf_head, inval_buf_ack_head[0], inval_buf_ack_head[1],
                   atomic_read(&cn_free_page_counter));
        }
    }

release: // Please release memory here
    // We are shutting down the system...
    if (data)
    {
        kfree(data);
        data = NULL; // meaningless
    }


    for (i = 0; i < 3 * DISAGG_NUM_CPU_CORE_IN_COMPUTING_BLADE; i++)
    {
        if (dummy_page_buf[i])
        {
            kfree(dummy_page_buf[i]);
            dummy_page_buf[i] = NULL;
        }
    }

    if (dummy_inv_page_buf)
    {
        kfree(dummy_inv_page_buf);
        dummy_inv_page_buf = NULL;
    }

    if (recv_buf)
    {
        kfree(recv_buf);
        recv_buf = NULL;
    }

    if (ack_buf)
    {
        kfree(ack_buf);
        ack_buf = NULL;
    }

    return 0;
}
