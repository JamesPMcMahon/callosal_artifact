#include "network_server.h"
#include "network_rdma.h"
#include "memory_management.h"
#include "thpool.h"

struct thpool_worker thpool_worker_map[NR_THPOOL_WORKERS];
static int TW_HEAD __cacheline_aligned;


/*
 * Pre-allocated thpool buffer
 * TB_HEAD points the current available buffer
 */
static int TB_HEAD __cacheline_aligned;
static struct thpool_buffer *thpool_buffer_map __read_mostly;

static inline int thpool_worker_id(struct thpool_worker *worker)
{
	return worker - thpool_worker_map;
}

static inline int thpool_buffer_ix(struct thpool_buffer *buffer)
{
	return buffer - thpool_buffer_map;
}

static inline void
enqueue_tail_thpool_worker(struct thpool_worker *worker, struct thpool_buffer *buffer)
{
	spin_lock(&worker->lock);
	list_add_tail(&buffer->next, &worker->work_head);
	/*
	 * This is not necessary but will do no harm.
	 * Since we are running on x86 TSO.
	 *
	 * We want to make sure the update of above list
	 * fields can be _seen_ by others before the counter
	 * is seen by others. Because the worker thread check
	 * the counter first, then check/dequeue list.
	 */
	smp_wmb();
	inc_queued_thpool_worker(worker);
	update_max_queued_thpool_worker(worker);
	spin_unlock(&worker->lock);
}

static inline struct thpool_buffer *
__dequeue_head_thpool_worker(struct thpool_worker *worker)
{
	struct thpool_buffer *buffer;

	buffer = list_entry(worker->work_head.next, struct thpool_buffer, next);
	list_del(&buffer->next);
	dec_queued_thpool_worker(worker);

	return buffer;
}

static inline struct thpool_buffer *
alloc_thpool_buffer(void)
{
	struct thpool_buffer *tb;
	int idx;

	idx = TB_HEAD % NR_THPOOL_BUFFER;
	tb = thpool_buffer_map + idx;
	TB_HEAD++;

	/*
	 * Buffer allocation is a simple ring.
	 * If the warning is triggered, it basically means:
	 * - buffer is not big enough
	 * - handler are too slow
	 */
	while (ThpoolBufferUsed(tb)) {
		WARN_ON_ONCE(1);
		cpu_relax();
	}

	__SetThpoolBufferUsed(tb);
	return tb;
}

/*
 * Choose a worker based on request types
 */
static inline struct thpool_worker *
select_thpool_worker(struct thpool_buffer *r)
{
	struct thpool_worker *tw;
	int idx;

	idx = TW_HEAD % NR_THPOOL_WORKERS;
	tw = thpool_worker_map + idx;
	TW_HEAD++;
	return tw;
}

/*
 * Main handlers and functions of thread worker
 */
static void send_simple_ack_rdma(struct thpool_buffer *tb, int val)
{
	struct simple_reply sr;
	// char char_buf[DISAGG_NET_SIMPLE_BUFFER_LEN] = {0};	
	void* out_buf = NULL;

	sr.ret = val;



	out_buf = thpool_buffer_tx(tb);


	memcpy(out_buf, &sr, sizeof(sr));
	tb_set_tx_size(tb, sizeof(sr));
	
}

static void handle_bad_request(struct common_header *hdr, struct thpool_buffer *tb)
{


	pr_warn("Unknown: opcode: %u, from node: %u\n",
				hdr->opcode, hdr->src_nid);



	
	send_simple_ack_rdma(tb, ERR_DISAGG_NET_INCORRECT_INT);
}


static void thpool_worker_handler(struct thpool_worker *worker,
				  struct thpool_buffer *buffer)
{
	void *msg;
	void *payload;
	struct common_header *hdr;
	void *tx;
	int ret = -1;

	/*
	 *   | .........| ............. |
	 *   ^          ^
	 *  msg(hdr)  payload
	 */
	tx = thpool_buffer_tx(buffer);
	msg = thpool_buffer_rx(buffer);
	hdr = to_common_header(msg);
	payload = to_payload(msg);








	switch (hdr->opcode) {
	case DISSAGG_FORK:
		if (hdr->length >= sizeof(*hdr) + sizeof(struct fork_msg_struct)){
			pr_info("FORK: received [tgid: %u]\n", ((struct fork_msg_struct*)payload)->tgid);

			ret = handle_fork_rdma(hdr, payload, buffer);
		}else{
			ret = -1;
			send_simple_ack_rdma(buffer, ret);
		}


		break;
	
	case DISSAGG_EXEC:
		if (hdr->length >= sizeof(*hdr) + sizeof(struct exec_msg_struct)){
			pr_info("EXEC: received [tgid: %u]\n", ((struct exec_msg_struct*)payload)->tgid);

			ret = handle_exec_rdma(hdr, payload);
		}else{
			ret = -1;
		}
		send_simple_ack_rdma(buffer, ret);
		break;

	// Exit message
	case DISSAGG_EXIT:
		if (hdr->length >= sizeof(*hdr) + sizeof(struct exit_msg_struct)){
			pr_info("EXIT: received [tgid: %u]\n", ((struct exit_msg_struct*)payload)->tgid);

			ret = handle_exit_rdma(hdr, payload, buffer);
		}else{
			ret = -1;
			send_simple_ack_rdma(buffer, ret);
		}
		break;

	// ALLOCATION - mmap
	case DISSAGG_MMAP:
		if (hdr->length >= sizeof(*hdr) + sizeof(struct mmap_msg_struct)){
			pr_info("MMAP: received [tgid: %u]\n", ((struct mmap_msg_struct*)payload)->tgid);

			ret = handle_mmap_rdma(hdr, payload, buffer);
		}else{
			ret = -1;
			send_simple_ack_rdma(buffer, 0);
		}


		break;

	// ALLOCATION - brk
	case DISSAGG_BRK:
		if (hdr->length >= sizeof(*hdr) + sizeof(struct brk_msg_struct)){
			pr_info("BRK: received [tgid: %u]\n", ((struct brk_msg_struct*)payload)->tgid);

			ret = handle_brk_rdma(hdr, payload, buffer);
		}else{
			ret = -1;
			send_simple_ack_rdma(buffer, 0);
		}
		break;

	// ALLOCATION - munmap
	case DISSAGG_MUNMAP:
		if (hdr->length >= sizeof(*hdr) + sizeof(struct munmap_msg_struct)){


			ret = handle_munmap_rdma(hdr, payload, buffer);
		}else{
			ret = -1;
			send_simple_ack_rdma(buffer, 0);
		}
		break;

	// ALLOCATION - mremap
	case DISSAGG_MREMAP:
		if (hdr->length >= sizeof(*hdr) + sizeof(struct mremap_msg_struct)){


			ret = handle_mremap_rdma(hdr, payload, buffer);
		}else{
			ret = -1;
			send_simple_ack_rdma(buffer, 0);
		}
		break;

	// Page fault
	case DISSAGG_PFAULT:
		if (hdr->length >= sizeof(*hdr) + sizeof(struct fault_msg_struct)){

			ret = handle_pfault_rdma(hdr, payload, buffer);
			if (ret){
				send_simple_ack_rdma(buffer, ret);
			}
		}else{
			ret = -1;
			send_simple_ack_rdma(buffer, ret);
		}
		break;

	// Mem data transmission
	case DISSAGG_DATA_PUSH:
		if (hdr->length >= sizeof(*hdr) + sizeof(struct fault_data_struct)){


			ret = handle_data_rdma(hdr, payload);
		}else{
			ret = -1;
		}
		send_simple_ack_rdma(buffer, ret);
		break;


	case DISSAGG_COPY_VMA:
		if (hdr->length >= sizeof(*hdr) + sizeof(struct exec_msg_struct)){


			ret = handle_exec_rdma(hdr, payload);
		}else{
			ret = -1;
		}
		send_simple_ack_rdma(buffer, ret);
		break;
	
	case DISSAGG_CHECK_VMA:
		if (hdr->length >= sizeof(*hdr) + sizeof(struct exec_msg_struct)){


			ret = handle_check_rdma(hdr, payload);
		}else{
			ret = -1;
		}
		send_simple_ack_rdma(buffer, ret);
		break;

	default:

		pr_err("RDMA: Cannot recognize opcode: %u\n", hdr->opcode);
		handle_bad_request(hdr, buffer);
	}
}

DEFINE_PROFILE_POINT(thpool_worker_handler)
DEFINE_PROFILE_POINT(thpool_worker_fit_ack_reply)

static int thpool_worker_func(void *_worker)
{
	struct thpool_worker *w = _worker;
	struct thpool_buffer *b;
	unsigned long queuing_delay;
	PROFILE_POINT_TIME(thpool_worker_handler)
	PROFILE_POINT_TIME(thpool_worker_fit_ack_reply)
	int pskip_counter = 0;


	pr_info("thpool: CPU%2d %s worker_id: %d UP\n",
		smp_processor_id(), current->comm, thpool_worker_id(w));


	set_cpu_thpool_worker(w, smp_processor_id());
















	while (1) {
		/* Check comments on enqueue */
		pskip_counter = 0;
		while (!nr_queued_thpool_worker(w)){
			cpu_relax();

			pskip_counter ++;
			if (pskip_counter > DISAGG_RDMA_POLLING_SKIP_COUNTER)
			{
				pskip_counter = 0;
				msleep(1);
			}
		}

		spin_lock(&w->lock);
		while (!list_empty(&w->work_head)) {
			b = __dequeue_head_thpool_worker(w);
			spin_unlock(&w->lock);








			thpool_buffer_dequeue_time(b);
			queuing_delay = thpool_buffer_queuing_delay(b);
			add_thpool_worker_total_queuing(w, queuing_delay);

			set_in_handler_thpool_worker(w);
			set_wip_buffer_thpool_worker(w, b);

			PROFILE_START(thpool_worker_handler);

			/* Invoke the real handler */
			tb_reset_tx_size(b);
			tb_reset_private_tx(b);
			thpool_worker_handler(w, b);





			BUG_ON(!b->tx_size);
			PROFILE_LEAVE(thpool_worker_handler);

			/*
			 * Callback to FIT layer to perform the
			 * last two steps: ACK, and REPLY.
			 */
			PROFILE_START(thpool_worker_fit_ack_reply);
			fit_ack_reply_callback(b);
			PROFILE_LEAVE(thpool_worker_fit_ack_reply);

			clear_wip_buffer_thpool_worker(w);
			clear_in_handler_thpool_worker(w);

			/* Return buffer to free pool */
			__ClearThpoolBufferNoreply(b);
			__ClearThpoolBufferUsed(b);

			inc_thpool_worker_nr_handled(w);
			spin_lock(&w->lock);
		}
		spin_unlock(&w->lock);
	}






	BUG();
	return 0;
}

unsigned long nr_thpool_reqs;

void thpool_callback(void *fit_ctx, void *fit_imm,
		     void *rx, int rx_size, int node_id, int fit_offset)
{
	struct thpool_buffer *b;
	struct thpool_worker *w;

	b = alloc_thpool_buffer();
	b->fit_rx = rx;
	b->fit_ctx = fit_ctx;
	b->fit_imm = fit_imm;
	b->fit_offset = fit_offset;
	b->fit_node_id = node_id;

	/*
	 * Select a worker thread and pass the buffer
	 * to it. The worker should do ACK and REPLY.
	 */
	thpool_buffer_enqueue_time(b);
	w = select_thpool_worker(b);
	enqueue_tail_thpool_worker(w, b);
	nr_thpool_reqs++;
}

/*
 * Callback for thread pool
 */
void fit_ack_reply_callback(struct thpool_buffer *b)
{
	int last_ack, ack_flag = 0;
	int reply_size, node_id, offset;
	int reply_connection_id;
	void *reply_data;
	ppc *ctx;
	struct imm_message_metadata *request_metadata;

	ctx = b->fit_ctx;
	request_metadata = b->fit_imm;
	node_id = b->fit_node_id;
	offset = b->fit_offset;





	reply_data = b->tx;
	reply_size = b->tx_size;

	/*
	 * Step II
	 * FIT internal ACK
	 */
	spin_lock(&ctx->local_last_ack_index_lock[node_id]);
	last_ack = ctx->local_last_ack_index[node_id];
	if ((offset >= last_ack && offset - last_ack >= IMM_ACK_FREQ) ||
	    (offset < last_ack && offset + IMM_PORT_CACHE_SIZE - last_ack >= IMM_ACK_FREQ)) {
		ack_flag = 1;
		ctx->local_last_ack_index[node_id] = offset;
	}
	spin_unlock(&ctx->local_last_ack_index_lock[node_id]);

	if (ack_flag) {
		struct send_and_reply_format *pass;

		pass = kmalloc(sizeof(*pass), GFP_KERNEL);
		if (!pass) {
			WARN_ON_ONCE(1);
			return;
		}

		pass->msg = (void *)(long)node_id;
		pass->length = offset;
		pass->type = MSG_DO_ACK_INTERNAL;

		enqueue_wq(pass);
	}

	/* Comes from ibapi_send() */
	if (ThpoolBufferNoreply(b))
		return;

	/*
	 * Step III
	 * Reply message
	 */
    reply_connection_id = fit_get_connection_by_atomic_number(ctx, node_id, LOW_PRIORITY);

	/* Send it out. It is really a mess. */
	fit_send_message_with_rdma_write_with_imm_request(ctx, reply_connection_id,
			request_metadata->reply_rkey,
            request_metadata->reply_addr,
			reply_data, reply_size, 0,
			request_metadata->reply_indicator_index | IMM_SEND_REPLY_RECV,
                        FIT_SEND_MESSAGE_IMM_ONLY, NULL, 1);
}
/*
 * Allocate the thread pool buffer array
 */
static void memory_manager_early_init(void)
{
	u64 size;
	int i;

	size = NR_THPOOL_BUFFER * sizeof(struct thpool_buffer);
	thpool_buffer_map = kzalloc(size, GFP_KERNEL);
	if (!thpool_buffer_map)
		panic("Unable to allocate thpool buffer array!");

	TB_HEAD = 0;

	for (i = 0; i < NR_THPOOL_BUFFER; i++) {
		struct thpool_buffer *tb;

		tb = thpool_buffer_map + i;
		INIT_LIST_HEAD(&tb->next);
	}

	fit_debug("Memory: thpool_buffer [0x%lx - 0x%lx] %Lx bytes nr:%d size:%zu\n",
		(unsigned long)thpool_buffer_map, (unsigned long)(thpool_buffer_map) + (unsigned long)size, size,
		NR_THPOOL_BUFFER, sizeof(struct thpool_buffer));
}

/* Create worker and polling threads */
void thpool_init(void)
{
	int i;
	struct task_struct *p;
	struct thpool_worker *worker;

	memory_manager_early_init();

	TW_HEAD = 0;
	for (i = 0; i < NR_THPOOL_WORKERS; i++) {
		worker = &thpool_worker_map[i];

		worker->nr_queued = 0;
		worker->max_nr_queued = 0;
		worker->flags = 0;
		worker->nr_handled = 0;
		worker->total_queuing_delay_ns = 0;
		worker->max_queuing_delay_ns = 0;
		worker->min_queuing_delay_ns = ULONG_MAX;
		INIT_LIST_HEAD(&worker->work_head);
		spin_lock_init(&worker->lock);
		memset(worker->queuing_stats, 0, sizeof(worker->queuing_stats));

		/* 
		 * We do not need completion because we will not pin thread for now 
		 * We also have Linux's own scheduling mechanism
		 */


		p = kthread_run(thpool_worker_func, worker, "thpool-worker%d", i);
		if (IS_ERR(p))
			panic("fail to create thpool-workder%d", i);


		worker->task = p;
	}
}
