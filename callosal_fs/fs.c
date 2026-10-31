#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <../../mm/internal.h>
#include <../../include/disagg/cnthread_disagg.h>
#include <../../include/disagg/exec_disagg.h>
#include <../../include/disagg/fault_disagg.h>
#include <../../include/disagg/network_rdma_disagg.h>
#include <../../mm/internal.h>
#include <../../roce_modules/roce_for_disagg/roce_disagg.h>
#include <asm/traps.h>
#include <../include/disagg/kshmem_disagg.h>

#include <linux/init.h>

#include "simplefs.h"

uint64_t read_clock_cycles[10] = {0,0,0,0,0,0,0,0,0,0};
uint64_t read_clock_cycles_2[10] = {0,0,0,0,0,0,0,0,0,0};
uint64_t read_clock_counter[10] = {0,0,0,0,0,0,0,0,0,0};
uint64_t write_clock_cycles[10] = {0,0,0,0,0,0,0,0,0,0};
uint64_t write_clock_counter[10] = {0,0,0,0,0,0,0,0,0,0};
uint64_t write_clock_cycles_2[10] = {0,0,0,0,0,0,0,0,0,0};
uint64_t write_clock_counter_2[10] = {0,0,0,0,0,0,0,0,0,0};

uint64_t inval_clock_cycles[10] = {0,0,0,0,0,0,0,0,0,0};
uint64_t inval_clock_counter[10] = {0,0,0,0,0,0,0,0,0,0};
atomic64_t inval_outer_count=ATOMIC64_INIT(0); 
atomic64_t inval_outer_cycle=ATOMIC64_INIT(0); 

atomic64_t inval_hash_count=ATOMIC64_INIT(0); 
atomic64_t inval_hash_cycle=ATOMIC64_INIT(0); 
atomic64_t inval_hash_lock_count=ATOMIC64_INIT(0); 
atomic64_t inval_hash_lock_cycle=ATOMIC64_INIT(0); 


atomic64_t inval_page_lock_count=ATOMIC64_INIT(0); 
atomic64_t inval_page_lock_cycle=ATOMIC64_INIT(0); 


atomic64_t acquire_page_lock_count=ATOMIC64_INIT(0); 
atomic64_t acquire_page_lock_cycle=ATOMIC64_INIT(0); 


atomic64_t lock_loop_iter=ATOMIC64_INIT(0); 
atomic64_t lock_loop_count=ATOMIC64_INIT(0); 
atomic64_t inval_lock_loop_iter=ATOMIC64_INIT(0); 
atomic64_t inval_lock_loop_count=ATOMIC64_INIT(0); 
atomic64_t lock_read_loop_iter=ATOMIC64_INIT(0); 
atomic64_t lock_read_loop_count=ATOMIC64_INIT(0); 





unsigned long shmem_address[FILE_SIZE];
unsigned long inode_address[FILE_COUNT];
unsigned long inode_size_address[FILE_COUNT];

unsigned long size_lock_address;
unsigned long inode_lock_address;
unsigned long new_inode_lock_address[FILE_COUNT];
unsigned long combined_address[1];
unsigned int remote_lock_status[FILE_COUNT]; 
unsigned int inode_size_status[FILE_COUNT];//0 not held, 1 read mode, 2 write mode


struct lock_class_key i_size_key;
struct lock_class_key i_remote_key;

struct rw_semaphore size_test_rwlock[FILE_COUNT];
struct rw_semaphore * size_rwlock[FILE_COUNT]; 

struct rw_semaphore inode_test_rwlock[FILE_COUNT];
struct rw_semaphore * inode_rwlock[FILE_COUNT];

struct rw_semaphore test_metadatalock; 
struct rw_semaphore * metadatalock; 

extern volatile char page_counter[FILE_COUNT][FILE_SIZE];

struct rw_semaphore hash_page_rwsem;

static int readAddress = 0;
//https://lynxbee.com/passing-command-line-arguments-parameters-to-linux-kernel-module/#.ZAUI5oDMKCg
//https://tldp.org/LDP/lkmpg/2.4/html/x354.htm (also used this for printing longs)
module_param(readAddress,int, 0);
module_param_array(combined_address, long, NULL, 0);

/* Mount a simplefs partition */
struct dentry *simplefs_mount(struct file_system_type *fs_type,
                              int flags,
                              const char *dev_name,
                              void *data)
{
    struct dentry *dentry =
        mount_bdev(fs_type, flags, dev_name, data, simplefs_fill_super);
    if (IS_ERR(dentry))
        pr_err("'%s' mount failure\n", dev_name);
    else
        pr_info("'%s' mount success\n", dev_name);

    return dentry;
}

/* Unmount a simplefs partition */
void simplefs_kill_sb(struct super_block *sb)
{
    kill_block_super(sb);

    pr_info("unmounted disk\n");
}

static struct file_system_type simplefs_file_system_type = {
    .owner = THIS_MODULE,
    .name = "simplefs",
    .mount = simplefs_mount,
    .kill_sb = simplefs_kill_sb,
    .fs_flags = FS_REQUIRES_DEV,
    .next = NULL,
};

static int __init simplefs_init(void)
{
	int i;
	int j;
	int ret;
	uintptr_t start_address; 
	uintptr_t current_address; 

	pr_info("test invalidate page callback %d", testing_invalidate_page_callback);
	pr_info("test shmem_address check %d", shmem_address_check);
	set_invalidate_page_callback(testing_invalidate_page_callback);
	set_shmem_address_check(shmem_address_check);
	//this sets up the addresses for the invalidation callback functions
	//so that the kernel can call back into them	


	pr_info("loading simplefs\n");
	pr_info("value of readAddress %d", readAddress);
	//module parameter print out

	//lock and status init
	init_rwsem(&hash_page_rwsem);
	init_rwsem(&test_metadatalock);
	metadatalock = &test_metadatalock; 

	for(i = 0; i < FILE_COUNT; i++){
		for(j = 0; j < FILE_SIZE; j++){
			page_counter[i][j] = 0;
		}
	}

	for(i = 0; i < FILE_COUNT; i++){

		//included locks include the size locks, and inode locks
		//since they are directly synced.

		init_rwsem(&(size_test_rwlock[i]));
		size_rwlock[i] = &(size_test_rwlock[i]);
		init_rwsem(&(inode_test_rwlock[i]));
		inode_rwlock[i] = &(inode_test_rwlock[i]);
		remote_lock_status[i] = 0; 
		inode_size_status[i] = 0;

	}
	//end of lock and status init

	//if nobody else has allocated the memory that is synced allocate it here
	if(!readAddress){
		pr_info("test allocation page size %d", PAGE_SIZE);
		start_address = (uintptr_t)alloc_kshmem(8192 * PAGE_SIZE, DISAGG_KSHMEM_SERV_FS_ID + 1);
		current_address = start_address; 
		pr_info("finished test allocation %ld", current_address);
		uintptr_t test_address = (uintptr_t)alloc_kshmem(7 * 1000 * PAGE_SIZE, DISAGG_KSHMEM_SERV_FS_ID + 1);
		pr_info("finished test allocation test address is %ld", test_address);
		pr_info("1 is %lu", start_address);
		pr_info("2 is %lu", test_address);
		pr_info("diff is %lu", test_address - start_address);
		pr_info("size is %lu", 8192 * PAGE_SIZE);
		if(8192 * PAGE_SIZE != test_address - start_address){
			BUG_ON(1);
		}

		pr_info("addresses:");
		for(i = 0; i < FILE_COUNT; i++){
			shmem_address[i] = current_address; 
			current_address += FILE_SIZE * PAGE_SIZE;
			pr_info("%ld, ", shmem_address[i]);
		}

		pr_info("single print addresses %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld", shmem_address[0],
				shmem_address[1],
				shmem_address[2],
				shmem_address[3],
				shmem_address[4],
				shmem_address[5],
				shmem_address[6],
				shmem_address[7],
				shmem_address[8],
				shmem_address[9],
				shmem_address[10],
				shmem_address[11],
				shmem_address[12],
				shmem_address[13],
				shmem_address[14],
				shmem_address[15],
				shmem_address[16],
				shmem_address[17],
				shmem_address[18],
				shmem_address[19]
		       );

		pr_info("\n");

		pr_info("inode addresses:");
		for(i = 0; i < FILE_COUNT; i++){
			inode_address[i] = current_address;
			current_address += PAGE_SIZE;
			pr_info("%ld, ", inode_address[i]);
		}


		pr_info("single print inode addresses %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld", inode_address[0],
				inode_address[1],
				inode_address[2],
				inode_address[3],
				inode_address[4],
				inode_address[5],
				inode_address[6],
				inode_address[7],
				inode_address[8],
				inode_address[9],
				inode_address[10],
				inode_address[11],
				inode_address[12],
				inode_address[13],
				inode_address[14],
				inode_address[15],
				inode_address[16],
				inode_address[17],
				inode_address[18],
				inode_address[19]

					);


				size_lock_address = current_address;
				current_address += PAGE_SIZE;
				inode_lock_address = current_address;
				current_address += PAGE_SIZE;
				pr_info("size lock and inode lock %ld %ld", size_lock_address, inode_lock_address);


				pr_info("inode size addresses:");
				for(i = 0; i < FILE_COUNT; i++){
					inode_size_address[i] = current_address;
					current_address += PAGE_SIZE;
					pr_info("%ld, ", inode_size_address[i]);
				}




				pr_info("single print inode size addresses %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld", inode_size_address[0],
						inode_size_address[1],
						inode_size_address[2],
						inode_size_address[3],
						inode_size_address[4],
						inode_size_address[5],
						inode_size_address[6],
						inode_size_address[7],
						inode_size_address[8],
						inode_size_address[9],
						inode_size_address[10],
						inode_size_address[11],
						inode_size_address[12],
						inode_size_address[13],
						inode_size_address[14],
						inode_size_address[15],
						inode_size_address[16],
						inode_size_address[17],
						inode_size_address[18],
						inode_size_address[19]
				       );

				pr_info("new inode lock addresses:");
				for(i = 0; i < FILE_COUNT; i++){
					new_inode_lock_address[i] = inode_address[i];
					pr_info("%ld, ", new_inode_lock_address[i]);
				}

				pr_info("single print new inode size addresses %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld", 
						new_inode_lock_address[0],
						new_inode_lock_address[1],
						new_inode_lock_address[2],
						new_inode_lock_address[3],
						new_inode_lock_address[4],
						new_inode_lock_address[5],
						new_inode_lock_address[6],
						new_inode_lock_address[7],
						new_inode_lock_address[8],
						new_inode_lock_address[9],
						new_inode_lock_address[10],
						new_inode_lock_address[11],
						new_inode_lock_address[12],
						new_inode_lock_address[13],
						new_inode_lock_address[14],
						new_inode_lock_address[15],
						new_inode_lock_address[16],
						new_inode_lock_address[17],
						new_inode_lock_address[18],
						new_inode_lock_address[19]


							);





				pr_info("\n");
	}else{
		//if someone already allocated the memory, read the 
		//addresses from the module arguments
		uintptr_t start_address = combined_address[0]; 
		uintptr_t current_address = start_address; 
		for(i = 0; i < FILE_COUNT; i++){
			shmem_address[i] = current_address;
			current_address += FILE_SIZE * PAGE_SIZE;	
		}

		for(i = 0; i < FILE_COUNT; i++){
			inode_address[i] = current_address;
			current_address += PAGE_SIZE;	

		}
		size_lock_address = current_address;
		current_address += PAGE_SIZE;	
		inode_lock_address = current_address;
		current_address += PAGE_SIZE;	


		for(i = 0; i < FILE_COUNT; i++){
			inode_size_address[i] = current_address; 
			current_address += PAGE_SIZE;	

		}
		for(i = 0; i < FILE_COUNT; i++){
			new_inode_lock_address[i] = inode_address[i];
		}


		pr_info("read addresses:");
		for(i = 0; i < FILE_SIZE; i++){
			pr_info("%ld, ", shmem_address[i]);
		}
		pr_info("\n");

		pr_info("inode addresses:");
		for(i = 0; i < FILE_COUNT; i++){
			pr_info("%ld, ", inode_address[i]);
		}
		pr_info("\n");


		pr_info("size lock address %ld, inode lock address %ld", size_lock_address, inode_lock_address);

		pr_info("inode size addresses:");
		for(i = 0; i < FILE_COUNT; i++){
			pr_info("%ld, ", inode_size_address[i]);
		}

		pr_info("new inode lock addresses:");
		for(i = 0; i < FILE_COUNT; i++){
			pr_info("%ld, ", new_inode_lock_address[i]);
		}
		pr_info("\n");



	}



	//normal simplefs stuff
	ret = simplefs_init_inode_cache();
	if (ret) {
		pr_err("inode cache creation failed\n");
		goto end;
	}

	ret = register_filesystem(&simplefs_file_system_type);
	if (ret) {
		pr_err("register_filesystem() failed\n");
		goto end;
	}

	pr_info("module loaded\n");
end:
	return ret;
}

static void __exit simplefs_exit(void)
{
    int ret = unregister_filesystem(&simplefs_file_system_type);
    if (ret)
        pr_err("unregister_filesystem() failed\n");

    simplefs_destroy_inode_cache();

    pr_info("module unloaded\n");
    int i;
    for(i = 0; i < 10; i++){
	if(read_clock_counter[i] != 0){
		pr_info("cpu %d cycles %lu read executions %lu average %lu", i, read_clock_cycles[i], read_clock_counter[i], read_clock_cycles[i] / read_clock_counter[i]);
		pr_info("cpu %d cycles %lu read copy %lu average %lu", i, read_clock_cycles_2[i], read_clock_counter[i], read_clock_cycles_2[i] / read_clock_counter[i]);

	}else{
		pr_info("cpu %d no read cycles", i);
	}
	if(write_clock_counter[i] != 0){
		pr_info("cpu %d cycles %lu write executions %lu average %lu", i, write_clock_cycles[i], write_clock_counter[i], write_clock_cycles[i] / write_clock_counter[i]);
		if(write_clock_counter_2[i] != 0){	
			pr_info("cpu %d cycles %lu write copy %lu average %lu", i, write_clock_cycles_2[i], write_clock_counter_2[i], write_clock_cycles_2[i] / write_clock_counter_2[i]);
		}

	}else{
		pr_info("cpu %d no write cycles", i);
	}
	if(inval_clock_counter[i] != 0){
		pr_info("cpu %d cycles %lu inval executions %lu average %lu", i, inval_clock_cycles[i], inval_clock_counter[i], inval_clock_cycles[i] / inval_clock_counter[i]);
	}else{
		pr_info("cpu %d no inval cycles", i);
	}


    }
if(inval_outer_count.counter != 0){
	pr_info("inval outer count %lu cycles %lu average %lu", inval_outer_count.counter, inval_outer_cycle.counter, inval_outer_cycle.counter / inval_outer_count.counter);
}else{
	pr_info("outer inval didn't record anything");
}


if(inval_hash_count.counter != 0){
	pr_info("inval hash count %lu cycles %lu average %lu", inval_hash_count.counter, inval_hash_cycle.counter, inval_hash_cycle.counter / inval_hash_count.counter);
}else{
	pr_info("hash inval didn't record anything");
}
if(inval_hash_lock_count.counter != 0){
	pr_info("inval hash lock count %lu cycles %lu average %lu", inval_hash_lock_count.counter, inval_hash_lock_cycle.counter, inval_hash_lock_cycle.counter / inval_hash_lock_count.counter);
}else{
	pr_info("hash lock inval didn't record anything");
}

if(inval_page_lock_count.counter != 0){
	pr_info("inval page lock count %lu cycles %lu average %lu", inval_page_lock_count.counter, inval_page_lock_cycle.counter, inval_page_lock_cycle.counter / inval_page_lock_count.counter);
}else{
	pr_info("page lock inval didn't record anything");
}




if(acquire_page_lock_count.counter != 0){
	pr_info("acquire page lock count %lu cycles %lu average %lu", acquire_page_lock_count.counter, acquire_page_lock_cycle.counter, 
			acquire_page_lock_cycle.counter / acquire_page_lock_count.counter);
}else{
	pr_info("outer inval didn't record anything");
}



if(lock_loop_count.counter != 0){
	pr_info("lock loop count %lu iterations %lu average %lu", lock_loop_count.counter, lock_loop_iter.counter, 
			lock_loop_iter.counter / lock_loop_count.counter);
}else{
	pr_info("lock loopdidn't record anything");
}


if(inval_lock_loop_count.counter != 0){
	pr_info("inval lock loop count %lu iterations %lu average %lu", inval_lock_loop_count.counter, inval_lock_loop_iter.counter, 
			inval_lock_loop_iter.counter / inval_lock_loop_count.counter);
}else{
	pr_info("inval lock loopdidn't record anything");
}

if(lock_read_loop_count.counter != 0){
	pr_info("read lock loop count %lu iterations %lu average %lu", lock_read_loop_count.counter, lock_read_loop_iter.counter, 
			lock_read_loop_iter.counter / lock_read_loop_count.counter);
}else{
	pr_info("read lock loopdidn't record anything");
}


}


module_init(simplefs_init);
module_exit(simplefs_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("a simple file system");
