#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>

#include <linux/net.h>
#include <net/sock.h>
#include <linux/tcp.h>
#include <linux/in.h>
#include <asm/uaccess.h>
#include <linux/socket.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
//MODULE_AUTHOR("Aby Sam Ross");

#define PORT 0

u32 create_address(u8 *ip);
int tcp_client_send(struct socket *sock, const char *buf, 
                    const size_t length, unsigned long flags);
int tcp_client_connect(struct socket **conn_socket);
int tcp_client_receive(struct socket *sock, char *str, unsigned long flags);
