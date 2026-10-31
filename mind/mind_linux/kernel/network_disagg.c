#define __SKIP_OFED_HEADERS__
#include <disagg/network_disagg.h>
#include <disagg/network_rdma_disagg.h>
#include <disagg/exec_disagg.h>
#include <disagg/fault_disagg.h>
#include <disagg/cnthread_disagg.h>
#include <disagg/profile_points_disagg.h>
#include <linux/socket.h>

static unsigned char _destip[5] = {0,0,0,0,'\0'};

static const size_t _recv_buf_size = 2 * DISAGG_NET_MAX_SIZE_ONCE;
static spinlock_t send_msg_lock, recv_msg_lock;
static int _is_connected = 0;
static int _rdma_connected = 0;
static struct socket *_conn_socket = NULL;
static int disagg_computing_node_id = 1;
static void *flush_buf = NULL;
static atomic64_t ongoing_payload;

// From network_disagg.h
int get_local_node_id(void)
{
    return disagg_computing_node_id;
}
EXPORT_SYMBOL(get_local_node_id);

int set_local_node_id(int new_id)
{
    disagg_computing_node_id = new_id;
    return 0;
}
EXPORT_SYMBOL(set_local_node_id);

void __init disagg_network_init(void)
{
    spin_lock_init(&send_msg_lock);
    spin_lock_init(&recv_msg_lock);
    _is_connected = 0;
    _rdma_connected = 0;
    _conn_socket = NULL;
    flush_buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
    atomic64_set(&ongoing_payload, 0);
}

u32 create_address(u8 *ip)
{
    u32 addr = 0;
    int i;

    for(i=0; i<4; i++)
    {
        addr += ip[i];
        if(i==3)
                break;
        addr <<= 8;
    }
    return addr;
}

u32 get_controller_ip(void)
{
    return htonl(create_address(_destip));
}
EXPORT_SYMBOL(get_controller_ip);

static int wait_socket_recv(struct socket *conn_socket)
{
    DECLARE_WAITQUEUE(recv_wait, current);
    int cnt = 0;
    
    if (!conn_socket)
        return -1;


    add_wait_queue(&conn_socket->sk->sk_wq->wait, &recv_wait);  
    while(skb_queue_empty(&conn_socket->sk->sk_receive_queue) 
            && cnt <= _MAX_CHECK_COUNTER)
    {
        __set_current_state(TASK_INTERRUPTIBLE);
        cnt++;
        schedule_timeout(_RECV_CHECK_TIME_IN_JIFFIES);
    }
    __set_current_state(TASK_RUNNING);
    remove_wait_queue(&conn_socket->sk->sk_wq->wait, &recv_wait);

    return 0;
}

DEFINE_PROFILE_POINT(NET_tcp_send_msg)
DEFINE_PROFILE_POINT(NET_tcp_recv_msg)

int send_msg_to_memory_lock(u32 msg_type, void *payload, u32 len_payload,
                        void *retbuf, u32 max_len_retbuf)
{
    int ret = 0;
    u32 tot_len;
    void *msg = NULL, *payload_msg;
    // void *recv_buf = NULL;
    struct mem_header* hdr;
    int i = 0;
    unsigned long start_ts, end_ts;
    PROFILE_POINT_TIME(NET_tcp_send_msg)



    // printk(KERN_DEFAULT "Send msg - type: %u, size: %u\n", 
    //         msg_type, len_payload);

    if (!retbuf)
        return -ERR_DISAGG_NET_INCORRECT_BUF;








    for (i = 0; i < DISAGG_NET_POLLING_SKIP_COUNTER; i++)
    {
        spin_lock(&send_msg_lock);
        if ((unsigned long)atomic64_read(&ongoing_payload))
        {
            spin_unlock(&send_msg_lock);
            usleep_range(10, 50);
            // goto retry_lock;
        }else{
            goto have_lock;
        }
    }


    printk(KERN_ERR "Msg timeout Initiate new connection\n");
    return -ERR_DISAGG_NET_TIMEOUT;

have_lock:

#ifndef USER_SPACE_TCP
    if (!_conn_socket)
    {
        ret = tcp_initialize_conn(&_conn_socket, create_address(_destip), _destport);
        if ((ret < 0) || !_conn_socket)
        {
            spin_unlock(&send_msg_lock);
            return ret;
        }
        _is_connected = 1;










    }
#endif
    atomic64_set(&ongoing_payload, (unsigned long)payload);   // I am the owner now
    spin_unlock(&send_msg_lock);




    // make header and attach payload
    tot_len = len_payload + sizeof(*hdr);

    msg = kmalloc(tot_len, GFP_KERNEL);


    if (!msg) {
		ret = -ENOMEM;
        goto out_sendmsg_err;
    }

    hdr = get_header_ptr(msg);
	hdr->opcode = msg_type;
    hdr->sender_id = get_local_node_id();
    hdr->size = tot_len;

    payload_msg = get_payload_ptr(msg);
	memcpy(payload_msg, payload, len_payload);


    // send request
    PROFILE_START(NET_tcp_send_msg);
#ifdef USER_SPACE_TCP
    ret = register_send_recv_buffer((char *)msg, tot_len, (char *) retbuf, max_len_retbuf);
#else
    ret = tcp_send(_conn_socket, msg, tot_len, MSG_DONTWAIT);
#endif
    if (ret < tot_len){
        ret = -ERR_DISAGG_NET_FAILED_TX;
        goto out_sendmsg_err;
    }
    PROFILE_LEAVE(NET_tcp_send_msg);



    memset(retbuf, 0, max_len_retbuf);

    start_ts = jiffies;
    while (1)
    {
        for (i = 0; i < DISAGG_NET_CTRL_POLLING_SKIP_COUNTER; i++)
        {


            {
#ifdef USER_SPACE_TCP
                ret = read_from_buffer((char *) retbuf, max_len_retbuf);
#else
                ret = tcp_receive(_conn_socket, retbuf, max_len_retbuf, MSG_DONTWAIT);
#endif
                if (ret > 0)
                    goto out_sendmsg;

            }
        }
        end_ts = jiffies;
        if ((end_ts > start_ts) && jiffies_to_msecs(end_ts - start_ts) > DISAGG_NET_TCP_TIMEOUT_IN_MS)
        {
            break;
        }

    }
    ret = -ERR_DISAGG_NET_TIMEOUT;
    printk(KERN_ERR "Msg timeout\n");

out_sendmsg:


out_sendmsg_err:






    if ((unsigned long)atomic64_read(&ongoing_payload) != (unsigned long)payload)
    {
        // something went wrong
        printk(KERN_ERR "ongoing_payload does not match\n");
        ret = -ERR_DISAGG_NET_TIMEOUT;
    }else{
        printk("ongoing_payload: Atomically finish the use\n");
        atomic64_set(&ongoing_payload, 0);    // atomically finish the use
    }

    if (msg)
        kfree(msg);


    return ret;
}

int send_msg_to_memory(u32 msg_type, void *payload, u32 len_payload,
                        void *retbuf, u32 max_len_retbuf)
{
    return send_msg_to_memory_lock(msg_type, payload, len_payload, retbuf, max_len_retbuf);
}
EXPORT_SYMBOL(send_msg_to_memory);

int tcp_send(struct socket *sock, const char *buf, 
                    const size_t length, unsigned long flags)
{
        struct msghdr msg;

        struct kvec vec;
        int len, written = 0, left = length;
        // mm_segment_t oldmm;



        msg.msg_name    = 0;
        msg.msg_namelen = 0;
        msg.msg_control = NULL;
        msg.msg_controllen = 0;
        msg.msg_flags   = flags;


repeat_send:
        vec.iov_len = left;
        vec.iov_base = (char *)buf + written;

        len = kernel_sendmsg(sock, &msg, &vec, 1, left);
        if((len == -ERESTARTSYS) || 
            (!(flags & MSG_DONTWAIT) && (len == -EAGAIN)))
                goto repeat_send;










        return len;
}



__always_inline int tcp_receive(struct socket *sock, char *buf, size_t bufsize, 
                        unsigned long flags)
{
        // mm_segment_t oldmm;
        struct msghdr msg;

        struct kvec vec;
        int len;
        PROFILE_POINT_TIME(NET_tcp_recv_msg)


        msg.msg_name    = 0;
        msg.msg_namelen = 0;




        msg.msg_control = NULL;
        msg.msg_controllen = 0;
        msg.msg_flags   = flags;




        vec.iov_len = bufsize;
        vec.iov_base = buf;


        PROFILE_START(NET_tcp_recv_msg);


        len = kernel_recvmsg(sock, &msg, &vec, 1, bufsize, flags);








        if (len > 0)
            PROFILE_LEAVE(NET_tcp_recv_msg);


        return len;
}



int tcp_initialize_conn(struct socket **conn_socket, 
                                u32 destip_32, u16 destport)
{
    int ret = -1;
    struct sockaddr_in saddr;
    int retry = 0;  //, yes = 1;

    ret = sock_create(PF_INET, SOCK_STREAM, IPPROTO_TCP, conn_socket);

    if(ret < 0 || !(*conn_socket))
    {
        /* NULL POINTER ERROR */
        return -ERR_DISAGG_NET_CREATE_SOCKET;
    }
    pr_info("Socket created for addr 0x%0x\n", destip_32);








    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(destport);
    saddr.sin_addr.s_addr = htonl(destip_32);

    while (1)
    {
        ret = (*conn_socket)->ops->connect(*conn_socket, (struct sockaddr *)&saddr,
                                        sizeof(saddr), O_RDWR);
        pr_info("Try to connect...%d (%d/%d)\n", ret, retry, DISAGG_NET_TCP_MAX_RETRY);

        if(!ret || (ret == -EINPROGRESS))
        {
            break;  // success
        }
        retry ++;

        if (retry >= DISAGG_NET_TCP_MAX_RETRY)
        {
            pr_info(" *** mtp | Error: %d while connecting using conn "
                    "socket. | setup_connection *** \n", ret);
            return -ERR_DISAGG_NET_CONN_SOCKET;
        }

        
    }
    return 0;
}

int tcp_finish_conn(struct socket *conn_socket)
{
    struct mem_header* hdr;
    void *msg = kmalloc(sizeof(*hdr), GFP_KERNEL);
    
	if (unlikely(!msg)) {
		tcp_release_conn(conn_socket);
        return -ENOMEM;
	}

    hdr = get_header_ptr(msg);
	hdr->opcode = DISSAGG_FIN_CONN;
    hdr->sender_id = get_local_node_id();
    hdr->size = sizeof(*hdr);

    // ignore error here, because we will relase the connection anyway
    tcp_send(conn_socket, msg, sizeof(*hdr), MSG_DONTWAIT);
    kfree(msg);
    return tcp_release_conn(conn_socket);
}

int tcp_release_conn(struct socket *conn_socket)
{
    if(conn_socket != NULL)
    {
        sock_release(conn_socket);
    }
    return 0;
}

int tcp_try_next_data_no_lock(void *retbuf, u32 max_len_retbuf)
{
    int ret = -1;


    wait_socket_recv(_conn_socket);

    if(!skb_queue_empty(&_conn_socket->sk->sk_receive_queue))
    {
        memset(retbuf, 0, max_len_retbuf);
        ret = tcp_receive(_conn_socket, retbuf, max_len_retbuf, MSG_DONTWAIT);
        printk(KERN_DEFAULT "Get next msg (%d)\n", ret);
    }


    return ret;
}

int tcp_reset_conn(void)
{
#if 0
    int ret = 0;
    char *retbuf = kmalloc(_recv_buf_size, GFP_KERNEL);

    if (!retbuf)
        return -ENOMEM;
    
    spin_lock(&send_msg_lock);
    if (_conn_socket)
    {
        // flush msg
        wait_socket_recv(_conn_socket);    

        while(!skb_queue_empty(&_conn_socket->sk->sk_receive_queue))
        {
            memset(retbuf, 0, _recv_buf_size);
            ret = tcp_receive(_conn_socket, retbuf, _recv_buf_size, MSG_DONTWAIT);
            printk(KERN_DEFAULT "Msg flushed (%d)\n", ret);

            if(ret <= 0)
            {
                break;
            }
        }

        // finish connection
        tcp_finish_conn(_conn_socket);
        _conn_socket = NULL;
        _is_connected = 0;
    }
    
    // re-initialize connection
    if (!_conn_socket)
    {
        ret = tcp_initialize_conn(&_conn_socket, create_address(_destip), _destport);
        if ((ret < 0) | !_conn_socket)
        {
            goto reset_conn_out;
        }
        _is_connected = 1;
    }

reset_conn_out:
    if (retbuf)
        kfree(retbuf);

    spin_unlock(&send_msg_lock);
    return ret;
#else
    return 0;
#endif
}

// ========== UDP listener ========== //
__always_inline int udp_receive(struct socket *sock, char *buf, size_t bufsize,
                                unsigned long flags, struct sockaddr_in *tmp_addr)
{
    // mm_segment_t oldmm;
    struct msghdr msg;

    struct kvec vec;
    int len;



    msg.msg_name = tmp_addr;
    msg.msg_namelen = sizeof(*tmp_addr);




    msg.msg_control = NULL;
    msg.msg_controllen = 0;
    msg.msg_flags = flags;




    vec.iov_len = bufsize;
    vec.iov_base = buf;
    


    len = kernel_recvmsg(sock, &msg, &vec, 1, bufsize, flags);










    return len;
}

__always_inline int udp_send(struct socket *sock, char *buf, size_t bufsize,
                             unsigned long flags, struct sockaddr_in *tmp_addr)
{
    // mm_segment_t oldmm;
    struct msghdr msg;

    struct kvec vec;
    int len;



    msg.msg_name = tmp_addr;
    msg.msg_namelen = sizeof(*tmp_addr);




    msg.msg_control = NULL;
    msg.msg_controllen = 0;
    msg.msg_flags = flags;




    vec.iov_len = bufsize;
    vec.iov_base = buf;

retry:
    len = kernel_sendmsg(sock, &msg, &vec, 1, bufsize);
    if (((len > 0) && (len < bufsize)) || len == -EAGAIN)
        goto retry;

    return len;
}



int udp_initialize(struct socket **conn_socket, u16 destport, int bind)
{
    int ret = -1;
    struct sockaddr_in saddr;
    int retry = 0; //, yes = 1;

    ret = sock_create(PF_INET, SOCK_DGRAM, IPPROTO_UDP, conn_socket);
    if (ret < 0 || !(*conn_socket))
    {
        /* NULL POINTER ERROR */
        return -ERR_DISAGG_NET_CREATE_SOCKET;
    }







    if (bind) {
        memset(&saddr, 0, sizeof(saddr));
        saddr.sin_family = AF_INET;
        saddr.sin_port = htons(destport);
        saddr.sin_addr.s_addr = INADDR_ANY;

        while (1)
        {

            ret = (*conn_socket)->ops->bind((*conn_socket), (struct sockaddr *)&saddr, sizeof(struct sockaddr));
            if (!ret || (ret == -EINPROGRESS))
            {
                break; // success
            }
            retry++;

            if (retry >= DISAGG_NET_TCP_MAX_RETRY)
            {
                // pr_info(" *** mtp | Error: %d while connecting using conn "
                //         "socket. | setup_connection *** \n", ret);
                return -ERR_DISAGG_NET_CONN_SOCKET;
            }
            msleep(DISAGG_NET_SLEEP_RESET_IN_MS);
        }
    }
    printk(KERN_DEFAULT "UDP listener initialized\n");
    return 0;
}

// ========== RDMA ========== //
// profiles that can be used in module
// DEFINE_PROFILE_POINT(NET_send_rdma)
// DEFINE_PROFILE_POINT(NET_send_rdma_lock)
static rdma_msg_callback send_msg_to_memory_rdma_body = NULL;
void set_rdma_msg_callback(rdma_msg_callback callbk)
{
    send_msg_to_memory_rdma_body = callbk;
}
EXPORT_SYMBOL(set_rdma_msg_callback);

static get_inval_buf_callback get_inval_buf_ptr_body = NULL;
void set_inval_buf_callback(get_inval_buf_callback callbk)
{
    get_inval_buf_ptr_body = callbk;
}
EXPORT_SYMBOL(set_inval_buf_callback);

inline void *get_inval_buf_ptr(int buf_id)
{
    if (likely(get_inval_buf_ptr_body != NULL))
    {
        return get_inval_buf_ptr_body(buf_id);
    }
    return NULL;
}

// DEFINE_PROFILE_POINT(FH_fetch_remote_data)
__always_inline int send_msg_to_memory_rdma(u32 msg_type, void *payload, u32 len_payload,
                                   void *retbuf, u32 max_len_retbuf)
{
    unsigned long start = jiffies, end;
    int res;

    if (likely(send_msg_to_memory_rdma_body != NULL))
    {

        res = send_msg_to_memory_rdma_body(msg_type, payload, len_payload, retbuf, max_len_retbuf);

        end = jiffies;
        if (unlikely((jiffies_to_nsecs(end - start) > DISAGG_NET_RDMA_TIMEOUT_REPORT) && (end > start) && payload))
        {
            u16 state = 0, sharer = 0;
            struct fault_msg_struct *tmp = payload;
            send_cache_dir_check(tmp->tgid, tmp->address & PAGE_MASK,   //CNTHREAD_CACHLINE_MASK,
                                 &state, &sharer, CN_SWITCH_REG_SYNC_NONE); // pull state before sending something
            printk(KERN_WARNING "ERROR (potential) suspicious network latency [%u ms] for 0x%lx [%u]: cpu[%d] state[0x%x] sharer[0x%x]\n",
                   jiffies_to_msecs(end - start), tmp->address, (unsigned int)msg_type, (int)smp_processor_id(), state, sharer);
        }
        return res;
    }

    // No callback, return error
    return -EINTR;
}

static page_init_callback dma_init_body = NULL;
void set_rdma_rmap_callback(page_init_callback callbk)
{
    dma_init_body = callbk;
}
EXPORT_SYMBOL(set_rdma_rmap_callback);

inline unsigned long mapping_dma_region_page(void *addr)
{
    if (likely(dma_init_body != NULL))
    {
        return dma_init_body(addr, PAGE_SIZE);
    }
    return 0;
}

static page_init_callback_new invalidate_page_callback = NULL;
void set_invalidate_page_callback(page_init_callback_new callbk_new)
{
    pr_info("calling invalidate_page_callback");
    invalidate_page_callback = callbk_new;
    pr_info("callback address %d", invalidate_page_callback);
}
EXPORT_SYMBOL(set_invalidate_page_callback);

inline unsigned long call_invalidate_page_callback(void *addr, void *inv_argv)
{
    if (likely(invalidate_page_callback != NULL))
    {
        return invalidate_page_callback(addr, inv_argv);
    }
    else {
      pr_info("call_invalidate_page_callback null");
    }
    return 0;
}
EXPORT_SYMBOL(call_invalidate_page_callback);

static page_init_callback shmem_address_check = NULL;
void set_shmem_address_check(page_init_callback callbk)
{
    pr_info("calling shmem_address_check");
    shmem_address_check = callbk;
    pr_info("shmem address %d", shmem_address_check);
}
EXPORT_SYMBOL(set_shmem_address_check);

inline unsigned long call_shmem_address_check(void *addr, unsigned long size)
{
    if (likely(shmem_address_check != NULL))
    {
        return shmem_address_check(addr, size);
    }
    else {
      pr_info("call_shmem_address_check null");
    }
    return 0;
}
EXPORT_SYMBOL(call_shmem_address_check);

