#ifndef MODULE_NAME
#define MODULE_NAME "futex_client"
#endif

#include <linux/module.h> /* Needed by all modules */
#include <linux/kernel.h> /* Needed for KERN_ALERT */
#include "../include/disagg/futex_disagg.h"

MODULE_LICENSE("GPL");

static int __init futex_client_init(void)
{
	pr_info("futex client inserted... Let's initialize futex conn\n");
	disagg_futex_init();
	return 0;
}

static void __exit futex_client_exit(void)
{
	;
}

/* module init and exit */
module_init(futex_client_init)
module_exit(futex_client_exit)
