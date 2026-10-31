#ifndef __RANGE_LOCK_DISAGGREGATION_H__
#define __RANGE_LOCK_DISAGGREGATION_H__

#include <linux/range_lock.h>

struct per_tgid_range_lock
{
    u16 tgid;
    struct range_lock_tree range_lock_root;
    struct hlist_node hnode;
};

void cache_range_lock_init(void);
int cache_range_try_lock(u16 tgid, struct range_lock *lock);
void cache_range_lock(u16 tgid, struct range_lock *lock);
void cache_range_unlock(u16 tgid, struct range_lock *lock);

#endif  /* __RANGE_LOCK_DISAGGREGATION_H__ */
