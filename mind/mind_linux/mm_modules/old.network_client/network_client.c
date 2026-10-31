#include "network_client.h"

static struct socket *conn_socket = NULL;

#define MAX_RES_LENGTH 49

static int __init network_client_init(void)
{
        pr_info(" *** mtp | network client init | network_client_init *** \n");
        tcp_client_connect(&conn_socket);
	printk(KERN_DEFAULT "Conn socket: 0x%lx\n", (unsigned long)conn_socket);
        return 0;
}

static void __exit network_client_exit(void)
{
        char response[MAX_RES_LENGTH+1];
        char reply[MAX_RES_LENGTH+1];


        DECLARE_WAIT_QUEUE_HEAD(exit_wait);

        memset(&reply, 0, MAX_RES_LENGTH+1);
        strcat(reply, "ADIOS"); 

        tcp_client_send(conn_socket, reply, strlen(reply), MSG_DONTWAIT);







         wait_event_timeout(exit_wait,\
                         !skb_queue_empty(&conn_socket->sk->sk_receive_queue),\
                                                                        5*HZ);
        if(!skb_queue_empty(&conn_socket->sk->sk_receive_queue))
        {
                memset(&response, 0, MAX_RES_LENGTH+1);
                tcp_client_receive(conn_socket, response, MSG_DONTWAIT);

        }

        //}

        if(conn_socket != NULL)
        {
                sock_release(conn_socket);
        }
        pr_info(" *** mtp | network client exiting | network_client_exit *** \n");
}

module_init(network_client_init)
module_exit(network_client_exit)
