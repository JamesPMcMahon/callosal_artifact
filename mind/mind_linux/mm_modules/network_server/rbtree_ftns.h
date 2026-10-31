#ifndef __MN_RBTREE_FTNS_MODULE_H__
#define __MN_RBTREE_FTNS_MODULE_H__

//rb_tree
#include <linux/rbtree.h>
#include <linux/rbtree_augmented.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/atomic.h>
#include <linux/rwsem.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/mm_types.h>

void __vma_link_rb(struct mm_struct *mm, struct vm_area_struct *vma,
		struct rb_node **rb_link, struct rb_node *rb_parent);

unsigned long count_vma_pages_range(struct mm_struct *mm,
		unsigned long addr, unsigned long end);

void vma_link(struct mm_struct *mm, struct vm_area_struct *vma,
		struct vm_area_struct *prev, struct rb_node **rb_link,
		struct rb_node *rb_parent);

struct vm_area_struct *mn_find_vma(struct mm_struct *mm, unsigned long addr);

int mn_find_vma_links(struct mm_struct *mm, unsigned long addr,
		unsigned long end, struct vm_area_struct **pprev,
		struct rb_node ***rb_link, struct rb_node **rb_parent);

struct vm_area_struct *mn_vma_merge(struct mm_struct *mm,
			struct vm_area_struct *prev, unsigned long addr,
			unsigned long end, unsigned long vm_flags,
			struct anon_vma *anon_vma, struct file *file,
			pgoff_t pgoff, struct mempolicy *policy);

int __vma_adjust(struct vm_area_struct *vma, unsigned long start,
	unsigned long end, pgoff_t pgoff, struct vm_area_struct *insert,
	struct vm_area_struct *expand);

int __mn_split_vma(struct mm_struct *mm, struct vm_area_struct *vma,
		unsigned long addr, int new_below);

void detach_vmas_to_be_unmapped(struct mm_struct *mm, struct vm_area_struct *vma,
		struct vm_area_struct *prev, unsigned long end);



void remove_vma_list(struct mm_struct *mm, struct vm_area_struct *vma);

#endif  /* __MN_RBTREE_FTNS_MODULE_H__ */
