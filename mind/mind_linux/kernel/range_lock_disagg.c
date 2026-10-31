#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/hashtable.h>
#include <disagg/range_lock_disagg.h>
#include <disagg/print_disagg.h>
#include <disagg/config.h>

static spinlock_t tgid_range_lock_hash_lock;
static struct hlist_head range_lock_hash_list[MAX_PROCESS_BUCKET_SIZE];

void cache_range_lock_init(void) {
    spin_lock_init(&tgid_range_lock_hash_lock);
    hash_init(range_lock_hash_list);
}

static u16 hash_ftn(u16 tgid)
{
    return (tgid & MAX_PROCESS_BUCKET_MASK);
}

// Simply checks if there is a range lock root for a given tgid
static struct per_tgid_range_lock *check_tgid(u16 tgid)
{
    struct per_tgid_range_lock *target = NULL;
    hash_for_each_possible(range_lock_hash_list, target, hnode, hash_ftn(tgid))
    {
        if (target->tgid == tgid)
            return target;
    }
    return NULL;
}

// Checks if there is a range lock root for a given tgid,
// if it does not exist, we will create one for the tgid
static struct per_tgid_range_lock *check_and_get_new_tgid(u16 tgid)
{
    struct per_tgid_range_lock *new_range_lock = check_tgid(tgid);
    if (unlikely(!new_range_lock))    // first check without a lock
    {
        spin_lock(&tgid_range_lock_hash_lock);
        new_range_lock = check_tgid(tgid);  // second check with the lock
        if (!new_range_lock)  // check if others already created one for the tgid
        {
            new_range_lock = kmalloc(sizeof(*new_range_lock), GFP_KERNEL);
            range_lock_tree_init(&new_range_lock->range_lock_root);
            new_range_lock->tgid = tgid;
            hash_add(range_lock_hash_list, &new_range_lock->hnode, hash_ftn(tgid));
            // print msg in a syscall level because it's one-time per tgid
            pr_syscall("New range lock assigned for tgid[%u]\n", tgid);
        }
        spin_unlock(&tgid_range_lock_hash_lock);
        return check_tgid(tgid);
    }
    return new_range_lock;
}

/* returns 1 if successful */
int cache_range_try_lock(u16 tgid, struct range_lock *lock) {
    struct per_tgid_range_lock *range_lock = check_and_get_new_tgid(tgid);
    if (unlikely(!range_lock))      // must not be none
        BUG();

    return range_write_trylock(&range_lock->range_lock_root, lock);
}
EXPORT_SYMBOL(cache_range_try_lock);

void cache_range_lock(u16 tgid, struct range_lock *lock) {
    struct per_tgid_range_lock *range_lock = check_and_get_new_tgid(tgid);
    if (unlikely(!range_lock))      // must not be none
        BUG();

    range_write_lock(&range_lock->range_lock_root, lock);
}

void cache_range_unlock(u16 tgid, struct range_lock *lock){
    struct per_tgid_range_lock *range_lock = check_tgid(tgid);
    if (unlikely(!range_lock))      // must not be none
        BUG();

    range_write_unlock(&range_lock->range_lock_root, lock);
}
EXPORT_SYMBOL(cache_range_unlock);
