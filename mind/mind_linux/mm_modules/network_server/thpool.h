#ifndef __MN_NETWORK_THREAD_POOL_H__
#define __MN_NETWORK_THREAD_POOL_H__

/*
 * == Copyright from LegoOS ==
 * 
 * Copyright (c) 2016-2020 Wuklab, Purdue University. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "../../include/disagg/network_disagg.h"
#include "../../include/disagg/network_rdma_disagg.h"
#include "../../include/disagg/network_fit_disagg.h"
#include "../../include/disagg/profile_points_disagg.h"

#include <linux/list.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/compiler.h>
#include <linux/kthread.h>
#include <linux/sched/signal.h>


/*
 * This is the maximum reply buffer size.
 * Retrict your reply size to below the limit.
 */
#define THPOOL_TX_SIZE		(PAGE_SIZE * 32)	// was (PAGE_SIZE * 1024)
#define NR_THPOOL_BUFFER	(16)	// was 256


#define NR_THPOOL_WORKERS	1

struct thpool_buffer;

struct tw_padding {
	char x[0];
} ____cacheline_aligned;
#define TW_PADDING(name)	struct tw_padding name

#define QUEUING_STAT_STRIDE_US	(5)
#define QUEUING_STAT_STRIDE_NS	(QUEUING_STAT_STRIDE_US*1000)
#define QUEUING_STAT_ENTRIES	(40)

/* This structure describes a worker thread */
struct thpool_worker {
	/*
	 * This counter is updated while the list
	 * is updated. And they are updated under @lock.
	 * Thus a simple int will do.
	 *
	 * Besides, the top three fields will always be
	 * updated together, so aggregate them into one
	 * standalone cache line.
	 */
	int			cpu;
	int			nr_queued;
	spinlock_t		lock;
	struct list_head	work_head;
	struct task_struct	*task;
	TW_PADDING(_pad1);


	unsigned long		nr_handled;
	unsigned long		total_queuing_delay_ns;
	unsigned long		max_queuing_delay_ns;
	unsigned long		min_queuing_delay_ns;

	/* us: [0, 5), [5, 10) ... [195, 200) */
	unsigned long		queuing_stats[QUEUING_STAT_ENTRIES];
	int			max_nr_queued;
	unsigned long		flags;
	struct thpool_buffer	*wip_buffer;
} ____cacheline_aligned;

static inline void set_cpu_thpool_worker(struct thpool_worker *tw, int cpu)
{
	tw->cpu = cpu;
}

static inline int cpu_thpool_worker(struct thpool_worker *tw)
{
	return tw->cpu;
}

static inline int nr_queued_thpool_worker(struct thpool_worker *tw)
{
	return tw->nr_queued;
}

static inline void inc_queued_thpool_worker(struct thpool_worker *tw)
{
	tw->nr_queued++;
}

static inline void dec_queued_thpool_worker(struct thpool_worker *tw)
{
	tw->nr_queued--;
}

struct tb_padding {
	char x[0];
} __aligned(PAGE_SIZE);
#define THPOOL_PADDING(name)	struct tb_padding name

struct thpool_buffer {
	unsigned long		flags;
	unsigned long		time_enqueue_ns;
	unsigned long		time_dequeue_ns;
	struct list_head	next;

	void			*fit_rx;
	void			*fit_ctx;
	void			*fit_imm;
	int			fit_node_id;
	int			fit_offset;

	/*
	 * Handler supplied tx buffer
	 * Only valid if privateTX flag is set
	 */
	void			*private_tx;
	int			tx_size;

	THPOOL_PADDING(_pad1);
	char			tx[THPOOL_TX_SIZE];
};

enum thpool_buffer_flags {
	THPOOL_BUFFER_used,
	THPOOL_BUFFER_noreply,
	THPOOL_BUFFER_privateTX,

	NR_THPOOL_BUFFER_FLAGS,
};

#define TEST_THPOOL_BUFFER_FLAGS(uname, lname)				\
static inline int ThpoolBuffer##uname(const struct thpool_buffer *p)	\
{									\
	return test_bit(THPOOL_BUFFER_##lname, &p->flags);		\
}

#define SET_THPOOL_BUFFER_FLAGS(uname, lname)				\
static inline void SetThpoolBuffer##uname(struct thpool_buffer *p)	\
{									\
	set_bit(THPOOL_BUFFER_##lname, &p->flags);			\
}

#define __SET_THPOOL_BUFFER_FLAGS(uname, lname)				\
static inline void __SetThpoolBuffer##uname(struct thpool_buffer *p)	\
{									\
	__set_bit(THPOOL_BUFFER_##lname, &p->flags);			\
}

#define CLEAR_THPOOL_BUFFER_FLAGS(uname, lname)				\
static inline void ClearThpoolBuffer##uname(struct thpool_buffer *p)	\
{									\
	clear_bit(THPOOL_BUFFER_##lname, &p->flags);			\
}

#define __CLEAR_THPOOL_BUFFER_FLAGS(uname, lname)			\
static inline void __ClearThpoolBuffer##uname(struct thpool_buffer *p)	\
{									\
	__clear_bit(THPOOL_BUFFER_##lname, &p->flags);			\
}

#define THPOOL_BUFFER_FLAGS(uname, lname)				\
	TEST_THPOOL_BUFFER_FLAGS(uname, lname)				\
	SET_THPOOL_BUFFER_FLAGS(uname, lname)				\
	CLEAR_THPOOL_BUFFER_FLAGS(uname, lname)				\
	__SET_THPOOL_BUFFER_FLAGS(uname, lname)				\
	__CLEAR_THPOOL_BUFFER_FLAGS(uname, lname)

THPOOL_BUFFER_FLAGS(Used, used)
THPOOL_BUFFER_FLAGS(Noreply, noreply)
THPOOL_BUFFER_FLAGS(PrivateTX, privateTX)

static inline void tb_set_tx_size(struct thpool_buffer *tb, int size)
{
	if (unlikely(size >= THPOOL_TX_SIZE))
		panic("Size: %d\n", size);
	tb->tx_size = size;
}

static inline void tb_reset_tx_size(struct thpool_buffer *tb)
{
	tb->tx_size = 0;
}

/*
 * Both set/clear are using the thread-local thpool buffer
 * thus non-atomic bitops are fine here.
 */
static inline void tb_set_private_tx(struct thpool_buffer *tb, void *private_tx)
{
	tb->private_tx = private_tx;
	__SetThpoolBufferPrivateTX(tb);
}

static inline void tb_reset_private_tx(struct thpool_buffer *tb)
{
	tb->private_tx = NULL;
	__ClearThpoolBufferPrivateTX(tb);
}

static inline void *thpool_buffer_rx(struct thpool_buffer *tb)
{
	return tb->fit_rx;
}

static inline void *thpool_buffer_tx(struct thpool_buffer *tb)
{
	return tb->tx;
}




static inline int thpool_worker_in_handler(struct thpool_worker *tw) { return 0; }
static inline void set_in_handler_thpool_worker(struct thpool_worker *tw) { }
static inline void clear_in_handler_thpool_worker(struct thpool_worker *tw) { }
static inline int max_queued_thpool_worker(struct thpool_worker *tw) { return 0; }
static inline void update_max_queued_thpool_worker(struct thpool_worker *tw) { }
static inline void
set_wip_buffer_thpool_worker(struct thpool_worker *tw, struct thpool_buffer *tb) { }
static inline void clear_wip_buffer_thpool_worker(struct thpool_worker *tw) { }
static inline struct thpool_buffer *
wip_buffer_thpool_worker(struct thpool_worker *tw) { return NULL; }

/* Queuing delay */
static inline unsigned long thpool_buffer_queuing_delay(struct thpool_buffer *tb) { return 0; }
static inline void thpool_buffer_dequeue_time(struct thpool_buffer *tb) { }
static inline void thpool_buffer_enqueue_time(struct thpool_buffer *tb) { }
static inline void add_thpool_worker_total_queuing(struct thpool_worker *tw, unsigned long diff_ns) { }

static inline void inc_thpool_worker_nr_handled(struct thpool_worker *tw) { }


void fit_ack_reply_callback(struct thpool_buffer *b);
void thpool_callback(void *fit_ctx, void *fit_imm,
		     void *rx, int rx_size, int node_id, int fit_offset);
#endif /* __MN_NETWORK_THREAD_POOL_H__ */
