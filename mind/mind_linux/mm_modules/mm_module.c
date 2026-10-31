#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/kthread.h>

#include <linux/errno.h>
#include <linux/types.h>

#include <linux/netdevice.h>
#include <linux/ip.h>
#include <linux/in.h>

#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/un.h>
#include <linux/unistd.h>
#include <linux/wait.h>
#include <linux/ctype.h>
#include <asm/unistd.h>
#include <net/sock.h>
#include <net/tcp.h>
#include <net/inet_connection_sock.h>
#include <net/request_sock.h>

#define DEFAULT_PORT 0
#define CONNECT_PORT 0
#define MODULE_NAME "remote_memory"
#define INADDR_SEND INADDR_LOOPBACK

static atomic_t revc_count;
static atomic_t send_count;

struct rmem_service
{
	int running;
	struct socket *listen_socket;
	struct task_struct *thread;
	struct task_struct *accept_worker;
};

struct rmem_service *rmem_svc;

int rmem_recv(struct socket *sock,unsigned char *buf,int len) 
{
	if(sock==NULL) {
		printk("recv the cscok is NULL\n");
		return -1;
	}
	else {
		printk("recv.the csock is:%d,%d\n",(int)sock,rmem_svc->listen_socket);
	}

	printk("Test the cscok:%d \n",sock->sk->sk_rmem_alloc);
	printk(KERN_INFO "rmem_recv");
	struct msghdr msg;
	struct iovec iov;
	mm_segment_t oldfs;
	int size=0;

	{
		if(sock->sk==NULL) return 0;

		iov.iov_base=buf;
		iov.iov_len=len;

		msg.msg_control=NULL;
		msg.msg_controllen=0;
		msg.msg_flags=0;
		msg.msg_name=0;
		msg.msg_namelen=0;
		msg.msg_iov=&iov;
		msg.msg_iovlen=1;
	}
	oldfs=get_fs();
	set_fs(KERNEL_DS);
	printk(KERN_INFO "rmem_recv.sock_recvmsg");
	size=sock_recvmsg(sock,&msg,len,msg.msg_flags);
	printk(KERN_INFO "rmem_recved");
	set_fs(oldfs);
	printk("the message is : %s\n",buf);
	atomic_inc(&revc_count);

	return size;
}
  
int rmem_send(struct socket *sock,char *buf,int len) 
{
	printk(KERN_INFO "rmem_send");
	if(sock==NULL)
	{
		printk("ksend the cscok is NULL\n");
		return -1;
	}
	struct msghdr msg;
	struct iovec iov;
	int size;
	mm_segment_t oldfs;

	iov.iov_base=buf;
	iov.iov_len=len;

	msg.msg_control=NULL;
	msg.msg_controllen=0;
	msg.msg_flags=0;
	msg.msg_iov=&iov;
	msg.msg_iovlen=1;
	msg.msg_name=0;
	msg.msg_namelen=0;

	oldfs=get_fs();
	set_fs(KERNEL_DS);
	printk(KERN_INFO "rmem_send.sock_sendmsg");
	size=sock_sendmsg(sock,&msg,len);
	printk(KERN_INFO "message sent!");
	set_fs(oldfs);

	atomic_inc(&send_count);

	return size;
}

int rmem_accept_worker()
{
	printk("accept_worker fired!\n");
	int error,ret;
	struct socket *socket;
	struct socket *cscok;
	int len=10;
	unsigned char buf[len+1];

	printk("declare the wait queue in the accept_worker\n");
	DECLARE_WAITQUEUE(wait,current);

	spin_lock_irqsave();
	{
		rmem_svc->running = 1;
		current->flags |= PF_NOFREEZE;
		/* daemonize (take care with signals, after daemonize() they are disabled) */
		daemonize("accept worker");
		allow_signal(SIGKILL|SIGSTOP);
	}
	spin_unlock_irqrestore();

	socket = rmem_svc->listen_socket;
	printk("Create the client accept socket\n");
	cscok=(struct socket*)kmalloc(sizeof(struct socket),GFP_KERNEL);
	/*error = sock_create(PF_INET,SOCK_STREAM,IPPROTO_TCP,&cscok);*/
	error = sock_create_lite(PF_INET,SOCK_STREAM,IPPROTO_TCP,&cscok);

	if(error<0) {
		printk(KERN_ERR "CREATE CSOCKET ERROR");
		return error;
	}

	printk("accept_worker.the cscok is :%d,%d\n",cscok,rmem_svc->listen_socket);

	/*check the accept queue*/

	struct inet_connection_sock *isock = inet_csk(socket->sk);
	while (rmem_svc->running == 1) {
		/*if(socket->sk->tp_pinfo.af_tcp.accept_queue==NULL) {*/
		/*if(skb_queue_empty(&socket->sk->sk_receive_queue)){*/
		if(reqsk_queue_empty(&isock->icsk_accept_queue)){
			/*printk("%s\n","the receive queue is NULL,so sleep");*/
			add_wait_queue(&socket->sk->sk_wq->wait, &wait);
			__set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ);
			printk("icsk queue empty?: %d\n",reqsk_queue_empty(&isock->icsk_accept_queue));
			printk("recv queue empty?: %d\n",skb_queue_empty(&socket->sk->sk_receive_queue));

			/*printk("icsk queue lenth: %d\n",reqsk_queue_len(&isock->icsk_accept_queue));*/
			__set_current_state(TASK_RUNNING);
			remove_wait_queue(&socket->sk->sk_wq->wait, &wait);
			continue;
		}

		printk("do accept\n");
		ret = socket->ops->accept(socket,cscok,O_NONBLOCK);
		if(ret<0){
			printk("accept error,release the socket\n");
			sock_release(cscok);
			return ret;
		}

		/*receive*/
		memset(&buf,0,len+1);
		printk("do receive the package\n");
		while(rmem_recv(cscok,buf,len))
		{
			/*rmem_send*/
			memset(&buf, 0, len+1);
			strcat(buf, "testing...");
			printk("do send the package\n");
			rmem_send(cscok,buf,strlen(buf));
		}
	}

	return ret;
	}

	int rmem_start_listen()
	{
		int error;
		struct socket *socket;
		struct sockaddr_in sin,sin_send;

		DECLARE_WAIT_QUEUE_HEAD(wq);

		spin_lock_irqsave();
		{
			rmem_svc->running = 1;
			current->flags |= PF_NOFREEZE;
			daemonize(MODULE_NAME);
			allow_signal(SIGKILL|SIGSTOP);
		}
		spin_unlock_irqrestore();

		error = sock_create(PF_INET,SOCK_STREAM,IPPROTO_TCP,&rmem_svc->listen_socket);

		if(error<0) {
			printk(KERN_ERR "CREATE SOCKET ERROR");
			return -1;
		}

		socket = rmem_svc->listen_socket;
		rmem_svc->listen_socket->sk->sk_reuse=1;









		sin.sin_addr.s_addr=htonl(INADDR_ANY);
		sin.sin_family=AF_INET;
		sin.sin_port=htons(DEFAULT_PORT);

		error = socket->ops->bind(socket,(struct sockaddr*)&sin,sizeof(sin));
		if(error<0) {
			printk(KERN_ERR "BIND ADDRESS");
			return -1;
		}

		error = socket->ops->listen(socket,5);
		if(error<0) {
			printk(KERN_ERR "LISTEN ERROR");
			return -1;
		}

		rmem_svc->accept_worker=kthread_run((void *)rmem_accept_worker,NULL,MODULE_NAME);

		while (1) {
			wait_event_timeout(wq,0,3*HZ);

			if(signal_pending(current)) 
				break;
		}

		return 1;
	}

	int rmem_start()
	{
		rmem_svc->running = 1;

		/* kernel thread initialization */
		rmem_svc->thread = kthread_run((void *)rmem_start_listen, NULL, MODULE_NAME);

		return 1;
	}

	int init_module()
	{
		printk("ktcp module init\n");
		rmem_svc=kmalloc(sizeof(struct rmem_service),GFP_KERNEL);
		rmem_start();
		return 1;
	}

	void cleanup_module()
	{
		int err;

		printk("module cleanup\n");
		if(rmem_svc->thread==NULL)
			printk(KERN_INFO MODULE_NAME": no kernel thread to kill\n");
		else{
			spin_lock_irqsave();
			{
				printk("stop the thead\n");
				err=kthread_stop(rmem_svc->thread);
				printk("stop the accept_worker\n");
				err=kthread_stop(rmem_svc->accept_worker);
			}
			spin_unlock_irqrestore();

			/* free allocated resources before exit */
			if (rmem_svc->listen_socket!= NULL) 
			{
				printk("release the listen_socket\n");
				sock_release(rmem_svc->listen_socket);
				rmem_svc->listen_socket= NULL;
			}

			kfree(rmem_svc);
			rmem_svc = NULL;

			printk(KERN_INFO MODULE_NAME": module unloaded\n");
		}
	}
