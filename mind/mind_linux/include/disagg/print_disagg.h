
#ifndef __PRINT_DISAGGREGATION_H__
#define __PRINT_DISAGGREGATION_H__

#ifdef CONFIG_COMPUTE_NODE







#define CONFIG_PROFILING_POINTS

#endif

#ifdef PRINT_CACHE_COHERENCE
#define pr_cache(...) printk(KERN_DEFAULT __VA_ARGS__)
#else
#define pr_cache(...) \
    do                \
    {                 \
    } while (0);
#endif

#ifdef PRINT_RDMA_TRANSMISSION
#define pr_rdma(...) printk(KERN_DEFAULT __VA_ARGS__)
#else
#define pr_rdma(...) \
    do               \
    {                \
    } while (0);
#endif

#ifdef PRINT_PAGE_FAULT
#define pr_pgfault(...) printk(KERN_DEFAULT __VA_ARGS__)
#else
#define pr_pgfault(...) \
    do                  \
    {                   \
    } while (0);
#endif

#ifdef PRINT_LOCKS
#define pr_locks(...) printk(KERN_DEFAULT __VA_ARGS__)
#else
#define pr_locks(...) \
    do                  \
    {                   \
    } while (0);
#endif


#ifdef PRINT_SYSCALLS
#define pr_syscall(...) printk(KERN_DEFAULT __VA_ARGS__)
#else
#define pr_syscall(...) \
    do                  \
    {                   \
    } while (0);
#endif

#ifdef PRINT_FUTEX
#define pr_futex(...) printk(KERN_DEFAULT __VA_ARGS__)
#else
#define pr_futex(...) \
    do                \
    {                 \
    } while (0);
#endif

#ifndef print_remote_syscall_only
#define print_remote_syscall_only(...) {if (current->is_remote) pr_syscall(__VA_ARGS__);}

#endif
#endif
