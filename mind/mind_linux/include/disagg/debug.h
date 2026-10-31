#ifndef __DEBUG_DISAGGREGATION_H__
#define __DEBUG_DISAGGREGATION_H__












#ifdef DISAGG_DEBUG_TGID_PGFAULT_PROFILE
struct pf_profile_node {
    unsigned int tgid;
    unsigned long addr;
    struct hlist_node hnode;
    struct list_head node;
    atomic_long_t cnt;
    unsigned int latest_cpu_id;
};

void add_pf_profile_counter(unsigned int tgid, unsigned long addr, int cpu_id);
void increase_pf_profile_counter(unsigned int tgid, unsigned long addr, int cpu_id);
void print_pf_profile(void);
void print_pf_profile_entry(unsigned int tgid, unsigned long addr, int cur_cpu);
#endif

#define _mind_debug_str_size 384
#define _mind_debug_stack_str_size 8192

struct err_msg_struct {
	unsigned int pid;
	unsigned int tgid;
	char debug_str[_mind_debug_str_size];
	unsigned long reserved[8];
} __packed;

struct err_stack_msg_struct {
	unsigned int pid;
	unsigned int tgid;
	char debug_str[_mind_debug_stack_str_size];
	unsigned long reserved[8];
} __packed;

struct err_reply_struct {
	int			ret;		// response
} __packed;

#endif  // __DEBUG_DISAGGREGATION_H__
