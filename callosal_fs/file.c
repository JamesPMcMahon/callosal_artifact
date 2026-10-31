#define pr_fmt(fmt) "simplefs: " fmt

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mpage.h>
#include <linux/uio.h>
#include <linux/hashtable.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/rwsem.h>
#include "bitmap.h"
#include <../../include/disagg/cnthread_disagg.h>
#include <../../include/disagg/exec_disagg.h>
#include <../../include/disagg/fault_disagg.h>
#include <../../mm/internal.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/init.h>
#include <linux/threads.h>
#include <linux/mm.h>		/* for struct page */
#include <linux/pagemap.h>
#include <asm/paravirt.h>

//for inode_to_bdi
#include<linux/backing-dev.h>

#include <../../roce_modules/roce_for_disagg/roce_disagg.h>
#include <asm/traps.h>
#include <../include/disagg/kshmem_disagg.h>
#include <linux/swap.h> /* for the mark_page_accessed function*/

#include "simplefs.h"
#include <linux/time.h>

// "MSI" coherence states tracked by this FS's coherence scheme.

// Ensures no two threads attempt to use the same dummy buffer at the same time.
// thread migrations from causing races to the buffers.

#define DFS_READ 1
#define DFS_WRITE 2

//locking and sync variables/structs/functions
spinlock_t cnthread_inval_send_ack_lock[DISAGG_NUM_CPU_CORE_IN_COMPUTING_BLADE];
extern struct rw_semaphore hash_page_rwsem;
extern void lock_loop(int ino, bool write);
extern struct rw_semaphore * inode_rwlock[FILE_COUNT];
extern unsigned long shmem_address[FILE_SIZE];
static int invalidate_page_write(struct page * testp, struct file *file, struct inode * inode, int page, bool readpage);

extern struct rw_semaphore * metadatalock; 


volatile char page_counter[FILE_COUNT][FILE_SIZE];

//hashtable used for keeping track of pages
//pages can occasionally change the address that they are 
//allocated to so instead of syncing using the pages themselves
//we ensure synchronization using a separate hashtable 
DEFINE_HASHTABLE(shmem_states, 8); // 8 = 256 buckets



//shared memory coherence state struct
//this is what is stored in hashmaps that 
//are used to keep track of pages
struct shmem_coherence_state {
    	unsigned long shmem_addr;
	unsigned long i_ino;
	struct inode * inode;
	int pagenum;
	int state;
	int page_locked_state;
	int table_locked_state;
	struct address_space *mapping;
	struct hlist_node link;
	struct rw_semaphore rwsem;
};



static __inline __attribute__((always_inline)) uint64_t test_rdtsc(int loops)
{

	udelay(750 * loops);	
	return 10;
}


extern atomic64_t lock_loop_iter;
extern atomic64_t lock_loop_count;
extern atomic64_t acquire_page_lock_count;
extern atomic64_t acquire_page_lock_cycle;

/*
 * Adds a shared memory page to the hashtable and marks the inode number and the state of the page 
 *
 */
static void hash_shmem(unsigned long shmem_addr, int inodenum, int pagenum, struct address_space *mapping, int state) {

	struct shmem_coherence_state *shmem_state;
	//refer more to Documentation/kernel-hacking/hacking.rst
	shmem_state = kmalloc(sizeof(struct shmem_coherence_state), GFP_KERNEL);
	if(shmem_state == NULL){
		pr_err("couldn't malloc shmem_state");
		BUG_ON(1);
	}
	shmem_state->i_ino = inodenum;
	shmem_state->pagenum = pagenum;
	shmem_state->mapping = mapping;
	shmem_state->state = state;
	shmem_state->shmem_addr = shmem_addr;
	shmem_state->page_locked_state = 0;
	shmem_state->table_locked_state = 0;

	//init the page lock
	init_rwsem(&(shmem_state->rwsem));

	//acquire the page in writemode 
	down_write(&(shmem_state->rwsem));

	hash_add(shmem_states, &(shmem_state->link), shmem_addr);
}

//https://kernelnewbies.org/FAQ/Hashtables
//returns page_coherence_state if the page is in the hashmap
//checks the index for the inode number, and then iterates
//through the list of all the inode/page combos that end up 
//in the same bucket.
inline static struct shmem_coherence_state * shmem_in_hashmap(unsigned long shmem_addr) {
	struct shmem_coherence_state *tempshmem;

	hash_for_each_possible(shmem_states, tempshmem, link, shmem_addr) {
		if(tempshmem->shmem_addr == shmem_addr){ 
			return tempshmem; 
		}
	}	
	return NULL; 

}



//struct for keeping track of how a page lock was acquired.
//state is the pointer that we need to get to the lock
struct page_lock_status {
	struct shmem_coherence_state * state;
	int page_lock;
	int table_lock;
	int old_state;
};

volatile int test_counter = 0;
volatile long test_long = 0;
struct page_lock_status acquire_page_lock(struct file * file, struct inode * inode, int currentpage, uintptr_t inode_pages_address, struct address_space * mapping, int mode){





	//acquire read lock for the hashtable
	int table_write_locked = 0;
	int page_write_locked = 0;
	int old_state = -1;
	struct shmem_coherence_state * coherence_state;
	struct page_lock_status temp;


	//acquire hashtable in read mode since we might not need to modify it
	down_read(&hash_page_rwsem);

	//surrounded by hashmap readlock
	//read the inode_page_address from the hashtable to find
	//the page we are trying to lock
	coherence_state = shmem_in_hashmap(inode_pages_address);

	if(coherence_state == NULL){
		//we don't have the page in the hashtable, so this
		//was the first time we tried to access it.
		//Add it to the hashtable
		up_read(&hash_page_rwsem);

		//acquire hashtable in write mode to add the new page
		down_write(&hash_page_rwsem);
		table_write_locked = 2;

		//surrounded by hashmap writelock
		coherence_state = shmem_in_hashmap(inode_pages_address);

		if(coherence_state == NULL){


			//page *still* not in hashmap add it (and acquire write lock)
			hash_shmem(inode_pages_address, mapping->host->i_ino, currentpage, mapping, mode);
			page_write_locked = 1;

			coherence_state = shmem_in_hashmap(inode_pages_address);
			if(coherence_state == NULL){
				pr_info("FAILED TO ADD PAGE TO HASHMAP");
				BUG_ON(1);
			}
			
		}else{
			//someone already added it before we could
			//acquire locks for updating mode
			down_write(&(coherence_state->rwsem));


			page_write_locked = 1;
			old_state = coherence_state->state;	
			if(coherence_state->state < mode){
				coherence_state->state = mode;

			}
		}
		//release hashtable locks
		up_write(&hash_page_rwsem);
	}else{
		//page was already represented in the hashtable
		//lock the page 
		if(mode == -1){
			//special case for read path for page_ok part
			down_read(&(coherence_state->rwsem));



			up_read(&hash_page_rwsem);

		}else{


			down_write(&(coherence_state->rwsem));



			page_write_locked = 1;
			up_read(&hash_page_rwsem);
			old_state = coherence_state->state;

			//check if we need to upgrade the mode
			if(coherence_state->state < mode){
				coherence_state->state = mode;
			}else{
			}
		}
	}

	temp.state = coherence_state;
	temp.page_lock = page_write_locked;
	temp.table_lock = table_write_locked;
	temp.old_state = old_state;

	//only measure in the read case or when upgrading 
	if(mode == 0 || mode == -1 || mode > old_state){
	}


	return temp; 
}


/**
 * Fetch a page from MIND's shared memory starting at shmem_address and
 * putting it into the buffer at page_dma_address. Populates the value pointed
 * to by data_size with the bytes copied from shared memory on success.
 * Always returns 0; otherwise it trips a BUG_ON instead.
 * 
 * Requirements:
 * Caller must ensure that page_dma_address is the DMA address of a page-sized
 * buffer that this function can use without outside concurrent access.
 */
static int mind_fetch_page(
	uintptr_t shmem_address, void *page_dma_address, size_t *data_size)
{

	struct fault_reply_struct ret_buf;
	struct cache_waiting_node *wait_node = NULL;
	int r;
	ret_buf.data_size = PAGE_SIZE;
	ret_buf.data = page_dma_address;

	wait_node = add_waiting_node(DISAGG_KERN_TGID, shmem_address, NULL);
	BUG_ON(!wait_node);


	BUG_ON(!is_kshmem_address(shmem_address));

	// NULL struct task_struct* is okay here because
	// if is_kshmem_address(shmem_address) then task_struct is never
	// derefenced.
	
	
	//sends the page fault request	
	r = send_pfault_to_mn(NULL, 0, shmem_address, 0, &ret_buf);

	wait_node->ack_buf = ret_buf.ack_buf;

	pr_pgfault("CN [%d]: start waiting 0x%lx\n", get_cpu(), shmem_address);

	if(r <= 0)
	{
		cancel_waiting_for_nack(wait_node);
		return -1;

	}

	r = wait_ack_from_ctrl(wait_node, NULL, NULL, NULL);

	if(r)
	{
		cancel_waiting_for_nack(wait_node);
		return -1;
	}

	
	*data_size = ret_buf.data_size;
	return 0;
}


//mind_fetch_page, but instead fetch page with write permissions
static int mind_fetch_page_write(
        uintptr_t shmem_address, void *page_dma_address, size_t *data_size)
{
        struct fault_reply_struct ret_buf;
        struct cache_waiting_node *wait_node = NULL;
        int r;
        ret_buf.data_size = PAGE_SIZE;
        ret_buf.data = page_dma_address;

        wait_node = add_waiting_node(DISAGG_KERN_TGID, shmem_address, NULL);
        BUG_ON(!wait_node);
	

        BUG_ON(!is_kshmem_address(shmem_address));

        // NULL struct task_struct* is okay here because
        // if is_kshmem_address(shmem_address) then task_struct is never
        // derefenced.
	r = send_pfault_to_mn(NULL, X86_PF_WRITE, shmem_address, 0, &ret_buf);
        wait_node->ack_buf = ret_buf.ack_buf;

        pr_pgfault("CN [%d]: start waiting 0x%lx\n", get_cpu(), shmem_address);
        if(r <= 0){
                cancel_waiting_for_nack(wait_node);
		pr_info("writer fail send pfault");
		BUG_ON(1);
	}

        r = wait_ack_from_ctrl(wait_node, NULL, NULL, NULL);

	if(r){
		cancel_waiting_for_nack(wait_node);

		return -1;
	}


        *data_size = ret_buf.data_size;

        return 0;
}


//this performs a read on the given page, read into the given buffer
//this bypasses the normal read operations and does the minimal
//amount of setup needed in order to call copy_page_to_iter
//
//at its core this is copying the data out of the page and into the buffer
//this is used when writting data to the buffers used for coherence 
//
//this takes code from various places on the read path in order to copy the data
//without following all of the sync steps (since we already followed them)
ssize_t simplefs_kernel_page_read(struct page * testpage, void * buf, size_t count, loff_t *pos)
{
	mm_segment_t old_fs;
	unsigned int index, offset;
	int result;
	struct iov_iter iter;
	struct iovec iov = {.iov_base = buf, .iov_len = count};

        old_fs = get_fs();
        set_fs(get_ds());
        /* The cast to a user pointer is valid due to the set_fs() */

	index = *pos >> PAGE_SHIFT;
	offset = *pos & ~PAGE_MASK;

	//create the iov_iter
	iov_iter_init(&iter, DFS_READ, &iov, 1, count);

	result = copy_page_to_iter(testpage, 0, count, &iter);
	set_fs(old_fs);
	if(likely(result == count)){
		return 0;
		
	}

	pr_err("failed to copy page to iter");
	BUG_ON(1);

        return 0;
}


//this performs a write on the given page, using the given buffer
//this bypasses the normal write operations and does the minimal
//amount of setup needed in order to call copy_page_from_iter
//
//This is the opposite of simplefs_kernel_page_read in that it is copying data
//into the page. 
//This is used on the remote read path since we have to "write" data from the sync buffer
//into the page that is used in the local page cache.
ssize_t simplefs_kernel_page_write(struct page * testpage, void * buf, size_t count, loff_t pos)
{
	mm_segment_t old_fs;
        ssize_t result;
	unsigned int index, offset;
	struct iov_iter iter;
	struct iovec iov;

	//from kernel_read in fs/read_write.c
        old_fs = get_fs();
        set_fs(get_ds());
        /* The cast to a user pointer is valid due to the set_fs() */

	index = pos >> PAGE_SHIFT;
	offset = pos & ~PAGE_MASK;

	//create the iov_iter (from new_sync_read)
        iov.iov_base = buf;
	iov.iov_len = count; //from new_sync_read
	iov_iter_init(&iter, DFS_READ, &iov, 1, count); //also from new_sync_read

	//actually copy the data to the page
        result = copy_page_from_iter(testpage, pos, count, &iter);
	set_fs(old_fs);
	if(likely(result == count)){
		return result;
	}
	pr_err("failed to copy page to iter");
	BUG_ON(1);
	

        return result;
}

/*
 * Map the buffer_head passed in argument with the iblock-th block of the file
 * represented by inode. If the requested block is not allocated and create is
 * true,  allocate a new block on disk and map it.
 *
 * Original simplefs stuff 
 */
static int simplefs_file_get_block(struct inode *inode,
                                   sector_t iblock,
                                   struct buffer_head *bh_result,
                                   int create)
{
    struct super_block *sb = inode->i_sb;
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    struct simplefs_inode_info *ci = SIMPLEFS_INODE(inode);
    struct simplefs_file_ei_block *index;
    struct buffer_head *bh_index;
    bool alloc = false;
    int ret = 0, bno;
    uint32_t extent;

    /* If block number exceeds filesize, fail */
    if (iblock >= SIMPLEFS_MAX_BLOCKS_PER_EXTENT * SIMPLEFS_MAX_EXTENTS)
        return -EFBIG;

    /* Read directory block from disk */
    bh_index = sb_bread(sb, ci->ei_block);

    if (!bh_index)
        return -EIO;	

    index = (struct simplefs_file_ei_block *) bh_index->b_data;

    extent = simplefs_ext_search(index, iblock);

    if (extent == -1) {
        ret = -EFBIG;
        goto brelse_index;
    }

    /*
     * Check if iblock is already allocated. If not and create is true,
     * allocate it. Else, get the physical block number.
     */
    if (index->extents[extent].ee_start == 0) {
        if (!create)
            return 0;
        bno = get_free_blocks(sbi, 8);
        if (!bno) {
            ret = -ENOSPC;
            goto brelse_index;
        }
        index->extents[extent].ee_start = bno;
        index->extents[extent].ee_len = 8;
        index->extents[extent].ee_block =
            extent ? index->extents[extent - 1].ee_block +
                         index->extents[extent - 1].ee_len
                   : 0;
        alloc = true;
    } else {
        bno = index->extents[extent].ee_start + iblock -
              index->extents[extent].ee_block;
    }

    /* Map the physical block to to the given buffer_head */
    map_bh(bh_result, sb, bno);

brelse_index:
    brelse(bh_index);

    return ret;
}


/*
 * TAKEN FROM MPAGE.C
 * support function for mpage_readpages.  The fs supplied get_block might
 * return an up to date buffer.  This is used to map that buffer into
 * the page, which allows readpage to avoid triggering a duplicate call
 * to get_block.
 *
 * The idea is to avoid adding buffers to pages that don't already have
 * them.  So when the buffer is up to date and the page size == block size,
 * this marks the page up to date instead of adding new buffers.
 */
static void 
map_buffer_to_page(struct page *page, struct buffer_head *bh, int page_block) 
{
	struct inode *inode = page->mapping->host;
	struct buffer_head *page_bh, *head;
	int block = 0;

	if (!page_has_buffers(page)) {
		/*
		 * don't make any buffers if there is only one buffer on
		 * the page and the page just needs to be set up to date
		 */
		if (inode->i_blkbits == PAGE_SHIFT &&
		    buffer_uptodate(bh)) {
			SetPageUptodate(page);    
			return;
		}
		create_empty_buffers(page, i_blocksize(inode), 0);
	}
	head = page_buffers(page);
	page_bh = head;
do {
	if (block == page_block) {
			page_bh->b_state = bh->b_state;
			page_bh->b_bdev = bh->b_bdev;
			page_bh->b_blocknr = bh->b_blocknr;
			break;
		}
		page_bh = page_bh->b_this_page;
		block++;
	} while (page_bh != head);
}




/*
 * Copies one block from MIND kernel shared memory into a buffer in the CN's
 * local page cache. This function also updates metadata needed to track the
 * fact that the CN is caching this block so that MIND invalidations can
 * be handled to ensure coherence.
 * Called by the page cache to read a page from the "physical disk" (in our case the shared memory) and map it in
 * memory.
 *   file: the file descriptor being read
 *   page: a descriptor given by the page cache for it to manage this page
 *   return: 0 on success; all other paths fail a BUG_ON.
 */
static int simplefs_readpage(struct file *file, struct page *page)
{
	struct buffer_head bh;
	uintptr_t inode_pages_address;
	int r;
	int cpu;
	size_t data_size;
	void *buf;
	int page_counter_start;
	int page_counter_end;

	const struct address_space *mapping = file->f_mapping;
	inode_pages_address = shmem_address[mapping->host->i_ino] +
				(PAGE_SIZE * (page->index));

	//page is locked here in either read or write mode

	//page is locked for write now should always be locked for write
	//because we should always be updating the state when we get here


	bh.b_state = 0;
	bh.b_size = 1;
	bh.b_page = page;


	//acquire mind CPU locks	
	cpu = get_cpu();
	
	while(!spin_trylock(&cnthread_inval_send_ack_lock[cpu]));

	buf = (void*)get_dummy_page_dma_addr(cpu);

	//fetch page into the local sync buffer
	r = mind_fetch_page(inode_pages_address, buf, &data_size);
	if(r == -1){
		spin_unlock(&cnthread_inval_send_ack_lock[cpu]);
		unlock_page(page);
		return -1337;
	}


	page_counter_start = page_counter[mapping->host->i_ino][page->index] + 1;
	page_counter_end = page_counter_start + 1;

	WRITE_ONCE(page_counter[mapping->host->i_ino][page->index], page_counter_start); 
	smp_wmb();

	// If this page doesn't have buffers yet, 
	// 0 below is the index of this block in the page; always 0 here
	// since this file system always has block size == page size.
	map_buffer_to_page(page, &bh, 0);

	//write synced data from sync buffer into the cached page	
	simplefs_kernel_page_write(page, get_dummy_page_buf_addr(cpu), PAGE_SIZE, 0);
	set_buffer_mapped(&bh);
	set_buffer_uptodate(&bh);

	smp_wmb();
	WRITE_ONCE(page_counter[mapping->host->i_ino][page->index], page_counter_end); 

	SetPageUptodate(page);

	BUG_ON(!PageUptodate(page));

	//it worked, mark the page as remote valid	
	SetPageRemoteValid(page);

	spin_unlock(&cnthread_inval_send_ack_lock[cpu]);
	unlock_page(page);


	return 0;
}

/*
 * Called by the page cache to write a dirty page to the physical disk (when
 * sync is called or when memory is needed).
 */
static int simplefs_writepage(struct page *page, struct writeback_control *wbc)
{
	//TODO data isn't written back to disk since that requires some hacks
	//and isn't super interesting for our tests
	unlock_page(page);
	return 0; 
}


//function used for requesting the page from the remote memory in write mode
//unlike the readpath, which uses mind_fetch_page
//this one uses mind_fetch_page_write and requests in write mode
static int invalidate_page_write(struct page * testp, struct file *file, struct inode * inode, int page, bool readpage){
	while(1){

		uintptr_t inode_pages_address;
		int r;
		struct mm_struct *mm;
		spinlock_t *ptl_ptr = NULL;
		pte_t *temppte;
		void *ptrdummy;
		const struct address_space *mapping;
		int cpu_id;
		size_t data_size;
		void * buf;
		mm = get_init_mm();

		mapping = file->f_mapping;

		inode_pages_address = shmem_address[mapping->host->i_ino] + (PAGE_SIZE * (page));

		cpu_id = get_cpu();

		while(!spin_trylock(&cnthread_inval_send_ack_lock[cpu_id]));


		buf = (void *)get_dummy_page_dma_addr(cpu_id);
		r = mind_fetch_page_write(inode_pages_address, buf, &data_size);
		if(r == -1){
			spin_unlock(&cnthread_inval_send_ack_lock[cpu_id]);
			return -1;
		}

		temppte = ensure_pte(mm, (uintptr_t)get_dummy_page_buf_addr(cpu_id), &ptl_ptr);

		ptrdummy = get_dummy_page_buf_addr(cpu_id);
		//read the data from the page if we don't already have it
		uint64_t start_cycle_2 = 0;

		if(readpage){	

			simplefs_kernel_page_write(testp, get_dummy_page_buf_addr(cpu_id), PAGE_SIZE, 0);
		}

		spin_unlock(&cnthread_inval_send_ack_lock[cpu_id]);

		return 1;
	}
}

//Taken from kernel for debugging
struct page *test_grab_cache_page_write_begin(struct address_space *mapping,
					pgoff_t index, unsigned flags)
{
	struct page *page;
	int fgp_flags = FGP_LOCK|FGP_WRITE|FGP_CREAT;

	if (flags & AOP_FLAG_NOFS)
		fgp_flags |= FGP_NOFS;

	page = pagecache_get_page(mapping, index, fgp_flags,
			mapping_gfp_mask(mapping));


	if (page)
		wait_for_stable_page(page);

	return page;
}

//Taken from kernel for debugging
int test_block_write_begin(struct address_space *mapping, loff_t pos, unsigned len,
		unsigned flags, struct page **pagep, get_block_t *get_block)
{
	pgoff_t index = pos >> PAGE_SHIFT;
	struct page *page;
	int status;
	page = test_grab_cache_page_write_begin(mapping, index, flags);
	if (!page)
		return -ENOMEM;

	status = __block_write_begin(page, pos, len, get_block);
	if (unlikely(status)) {
		unlock_page(page);
		put_page(page);
		page = NULL;
	}

	*pagep = page;
	return status;
}

/*
 * Called by the VFS when a write() syscall occurs on file before writing the
 * data in the page cache. This functions checks if the write will be able to
 * complete and allocates the necessary blocks through block_write_begin().
 *
 */


static int simplefs_write_begin(struct file *file,
                                struct address_space *mapping,
                                loff_t pos,
                                unsigned int len,
                                unsigned int flags,
                                struct page **pagep,
                                void **fsdata)
{


    //need to do the currentpage thing and not pass in the 
    //actual page since it causes null dereference stuff
   
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(file->f_inode->i_sb);

    int err;
    uint32_t nr_allocs = 0;

    if (pos + len > SIMPLEFS_MAX_FILESIZE)
        return -ENOSPC;

    //shouldn't have to worry about size changing here
    
    //changed to use i_size_read in case of incoming invalidation 
    nr_allocs = max(pos + len, i_size_read(file->f_inode)) / SIMPLEFS_BLOCK_SIZE;

    if (nr_allocs > file->f_inode->i_blocks - 1)
        nr_allocs -= file->f_inode->i_blocks - 1;
    else
        nr_allocs = 0;
    if (nr_allocs > sbi->nr_free_blocks)
        return -ENOSPC;


    /* prepare the write */
    err = test_block_write_begin(mapping, pos, len, flags, pagep,
                            simplefs_file_get_block);
    /* if this failed, reclaim newly allocated blocks */
    if (err < 0)
        pr_err("newly allocated blocks reclaim not implemented yet\n");
    return err;
}


int simple_generic_write_end(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned len, unsigned copied,
			struct page *page, void *fsdata)
{
	struct inode *inode = mapping->host;
	loff_t old_size = i_size_read(inode); //changed
	int i_size_changed = 0;

	copied = block_write_end(file, mapping, pos, len, copied, page, fsdata);

	/*
	 * No need to use i_size_read() here, the i_size
	 * cannot change under us because we hold i_mutex.
	 *
	 * But it's important to update i_size while still holding page lock:
	 * page writeout could otherwise come in and zero beyond i_size.
	 */
	if (pos+copied > old_size) { //changed because mutex is different for remote
		i_size_write(inode, pos+copied);
		i_size_changed = 1;
	}

	unlock_page(page);
	put_page(page);

	if (old_size < pos)
		pagecache_isize_extended(inode, old_size, pos);
	/*
	 * Don't mark the inode dirty under page lock. First, it unnecessarily
	 * makes the holding time of page lock longer. Second, it forces lock
	 * ordering of page lock and transaction start for journaling
	 * filesystems.
	 */
	if (i_size_changed)
		mark_inode_dirty(inode);

	return copied;
}
/*
 * Called by the VFS after writing data from a write() syscall to the page
 * cache. This functions updates inode metadata and truncates the file if
 * necessary.
 */
static int simplefs_write_end(struct file *file,
                              struct address_space *mapping,
                              loff_t pos,
                              unsigned int len,
                              unsigned int copied,
                              struct page *page,
                              void *fsdata)
{
    struct inode *inode = file->f_inode;
    struct simplefs_inode_info *ci = SIMPLEFS_INODE(inode);
    struct super_block *sb = inode->i_sb;
    uint32_t nr_blocks_old;


    /* Complete the write() */

    //calls the version that no longer does a read of the size without i_size_read
    int ret = simple_generic_write_end(file, mapping, pos, len, copied, page, fsdata);


    if (ret < len) {

	return ret;
    }

    nr_blocks_old = inode->i_blocks;

    /* Update inode metadata */
    inode->i_blocks = i_size_read(inode) / SIMPLEFS_BLOCK_SIZE + 2;

    inode->i_mtime = inode->i_ctime = current_time(inode);
    mark_inode_dirty(inode);

    /* If file is smaller than before, free unused blocks */
  
     if (nr_blocks_old > inode->i_blocks && 1 < 0) {
	    int i;
	    struct buffer_head *bh_index;
	    struct simplefs_file_ei_block *index;
	    uint32_t first_ext;

	    // Free unused blocks from page cache */
	    truncate_pagecache(inode, inode->i_size);

	    // Read ei_block to remove unused blocks */
	    bh_index = sb_bread(sb, ci->ei_block);
	    if (!bh_index) {
		    pr_err("failed truncating '%s'. we just lost %lu blocks\n",
				    file->f_path.dentry->d_name.name,
				    nr_blocks_old - inode->i_blocks);
		    goto end;
	    }
	    index = (struct simplefs_file_ei_block *) bh_index->b_data;

	    first_ext = simplefs_ext_search(index, inode->i_blocks - 1);
	    // Reserve unused block in last extent */
	    if (inode->i_blocks - 1 != index->extents[first_ext].ee_block)
		    first_ext++;

	    for (i = first_ext; i < SIMPLEFS_MAX_EXTENTS; i++) {
		    if (!index->extents[i].ee_start)
			    break;
		    put_blocks(SIMPLEFS_SB(sb), index->extents[i].ee_start,
				    index->extents[i].ee_len);
		    memset(&index->extents[i], 0, sizeof(struct simplefs_extent));
	    }
	    mark_buffer_dirty(bh_index);
	    brelse(bh_index);
    }
end:


    return ret;

}



//function used on the read path for releasing the remote
//sync lock for pages
inline void release_temp_lock(struct page_lock_status * temp_status){
	if(temp_status->page_lock){
		up_write(&((temp_status->state)->rwsem));
	}else{
		up_read(&((temp_status->state)->rwsem));
	}
}


static int test_copyout(void __user *to, const void *from, size_t n)
{
	if (access_ok(VERIFY_WRITE, to, n)) {
		kasan_check_read(from, n);
		n = raw_copy_to_user(to, from, n);
	}
	return n;
}


static size_t retry_able_copy_page_to_iter_iovec(struct page *page, size_t offset, size_t bytes,
			 struct iov_iter *i, int inode_index, int page_index, int starting_counter)
{

	size_t skip, copy, left, wanted;
	const struct iovec *iov;
	char __user *buf;
	void *kaddr, *from;
	int ending_counter; 


	if (unlikely(bytes > i->count))
		bytes = i->count;

	if (unlikely(!bytes))
		return 0;

	might_fault();
	wanted = bytes;
	iov = i->iov;
	skip = i->iov_offset;
	buf = iov->iov_base + skip;
	copy = min(bytes, iov->iov_len - skip);
	if (IS_ENABLED(CONFIG_HIGHMEM) && !fault_in_pages_writeable(buf, copy)) {
		pr_info("HIGHMEM WORKED");
		kaddr = kmap_atomic(page);
		from = kaddr + offset;

		/* first chunk, usually the only one */
		left = test_copyout(buf, from, copy);
		copy -= left;
		skip += copy;
		from += copy;
		bytes -= copy;

		while (unlikely(!left && bytes)) {
			iov++;
			buf = iov->iov_base;
			copy = min(bytes, iov->iov_len);
			left = test_copyout(buf, from, copy);
			copy -= left;
			skip = copy;
			from += copy;
			bytes -= copy;
		}
		if (likely(!bytes)) {
			kunmap_atomic(kaddr);
			goto done;
		}
		offset = from - kaddr;
		buf += copy;
		kunmap_atomic(kaddr);
		copy = min(bytes, iov->iov_len - skip);
	}

	/* Too bad - revert to non-atomic kmap */

	kaddr = kmap(page);
	from = kaddr + offset;
	left = test_copyout(buf, from, copy);
	copy -= left;
	skip += copy;
	from += copy;
	bytes -= copy;
	while (unlikely(!left && bytes)) {
		iov++;
		buf = iov->iov_base;
		copy = min(bytes, iov->iov_len);
		left = test_copyout(buf, from, copy);
		copy -= left;
		skip = copy;
		from += copy;
		bytes -= copy;
	}
	kunmap(page);

done:
	if (skip == iov->iov_len) {
		iov++;
		skip = 0;
	}

	smp_rmb();
	ending_counter = READ_ONCE(page_counter[inode_index][page_index]);
	if(starting_counter != ending_counter){
		pr_info("failed counter issue %d", ending_counter);

		return -1337;
	}

	i->count -= wanted - bytes;
	i->nr_segs -= iov - i->iov;
	i->iov = iov;
	i->iov_offset = skip;
	return wanted - bytes;
}



/**
 * generic_file_buffered_read - generic file read routine
 * @iocb:       the iocb to read
 * @iter:       data destination
 * @written:    already copied
 *
 * This is a generic file read routine, and uses the
 * mapping->a_ops->readpage() function for the actual low-level stuff.
 *
 * This is really ugly. But the goto's actually try to clarify some
 * of the logic when it comes to error handling etc.
 *
 * DFS NOTE: This function is copied from the kernel and is 
 * the primary function that determines how pages are read
 * whether from local or going through the remote calls
 *
 */
static ssize_t simplefs_generic_file_buffered_read(struct kiocb *iocb,
                struct iov_iter *iter, ssize_t written)
{
	struct file *filp = iocb->ki_filp;
	struct address_space *mapping = filp->f_mapping;
	struct inode *inode = mapping->host;
	struct file_ra_state *ra = &filp->f_ra;
	loff_t *ppos = &iocb->ki_pos;
	pgoff_t index;
	pgoff_t last_index;
	pgoff_t prev_index;
	int new_isize;
	int forced_retry;
	unsigned long offset;      /* offset into pagecache page */
	unsigned int prev_offset;
	int current_value;
	uintptr_t inode_pages_address;
	int error = 0;
	int retry_counter = 0;
	struct page_lock_status temp_status;
	if (unlikely(*ppos >= inode->i_sb->s_maxbytes))
		return 0;
	iov_iter_truncate(iter, inode->i_sb->s_maxbytes);

	index = *ppos >> PAGE_SHIFT;
	prev_index = ra->prev_pos >> PAGE_SHIFT;
	prev_offset = ra->prev_pos & (PAGE_SIZE-1);
	last_index = (*ppos + iter->count + PAGE_SIZE-1) >> PAGE_SHIFT;
	offset = *ppos & ~PAGE_MASK;

	//explainations of code:
	//this function largely retains its behavior from the normal
	//version that is included in the kernel aside from a couple key points
	/* iage locks are usually wrapped with "acquire_page_lock"
	 * 	this function syncs writes, reads and invalidations
	 * 	and is the primary sync lock used for syncing page data
	 *
	 * page locks are released using "release_temp_lock"
	 *
	 * remote validitiy is checked using "PageRemoteValid"
	 * 	this functions in a very similar way to "PageUptodate"
	 *	"PageUptodate" is marked as false when a concurrent write 
	 *	is happening. "PageRemoteValid" is marked as false
	 *	when we no longer have read access to a page.
	 *
	 */


	for (;;) {
		struct page *page;
		pgoff_t end_index;
		loff_t isize;
		unsigned long nr, ret;

		cond_resched();
		inode_pages_address = shmem_address[mapping->host->i_ino] +
			(PAGE_SIZE * (index));

find_page:
		forced_retry = 0;
		if (fatal_signal_pending(current)) {
			error = -EINTR;
			goto out;
		}

		page = find_get_page(mapping, index);
		if (!page) {
			if (iocb->ki_flags & IOCB_NOWAIT)
				goto would_block;

			//readahead isn't properly synced so we remove it
			page = find_get_page(mapping, index);
			if (unlikely(page == NULL))
				goto no_cached_page;
		}

		//again, page readahead isn't synced so remove it

		//we found the page in the page cache, 
		
		//check to see if we actually have read access to it
		if(!PageRemoteValid(page)){
			//jump to page_not_up_to_date
			//where we will acquire the page lock and sync
			//with the remote system	
			goto page_not_up_to_date;
			//follow steps here similar to PageUptodate
		}

		if (!PageUptodate(page)) {
			if (iocb->ki_flags & IOCB_NOWAIT) {
				put_page(page);
				goto would_block;
			}

			/*
			 * See comment in do_read_cache_page on why
			 * wait_on_page_locked is used to avoid unnecessarily
			 * serialisations and why it's safe.
			 */
			error = wait_on_page_locked_killable(page);
			if (unlikely(error))
				goto readpage_error;
			
			//invalidations mark page as not up to date so 
			//we fall back into the paths that it would
			//take for that case	
			if (PageUptodate(page))
				goto page_ok;

			if (inode->i_blkbits == PAGE_SHIFT ||
					!mapping->a_ops->is_partially_uptodate)

				goto page_not_up_to_date;

			/* pipes can't handle partially uptodate pages */
			if (unlikely(iter->type & ITER_PIPE))

				goto page_not_up_to_date;

			//wrap the potential page lock acquire with the remote sync lock 
			//updated state is 0 since we aren't trying to upgrade it yet, we do 
			//that at the end of the readpage sectoin
			temp_status = acquire_page_lock(filp, mapping->host, index, inode_pages_address, mapping, 0);
			if (!trylock_page(page)){
			
				//lock acquire failed, release remote sync lock to avoid deadlocking	
				release_temp_lock(&temp_status);
				goto page_not_up_to_date;
			}
			if (!page->mapping)
				goto page_not_up_to_date_locked;
			if (!mapping->a_ops->is_partially_uptodate(page,
							offset, iter->count))
				goto page_not_up_to_date_locked;
			
			//release page locks	
			unlock_page(page);
			release_temp_lock(&temp_status);
		}
page_ok:

		/*
		 * i_size must be checked after we know the page is Uptodate.
		 *
		 * Checking i_size after the check allows us to calculate
		 * the correct value for "nr", which means the zero-filled
		 * part of the page is not copied back to userspace (unless
		 * another truncate extends the file - this is desired though).
		 */
		
retry_page_ok:
		current_value = READ_ONCE(page_counter[inode->i_ino][index]);
		smp_rmb();
		int set_lock = 0;
		if(page_counter[inode->i_ino][index] & 1 || forced_retry){
			set_lock = 1;
			temp_status = acquire_page_lock(filp, inode, index, inode_pages_address, mapping, -1);
			current_value = READ_ONCE(page_counter[inode->i_ino][index]);
			smp_rmb();

			forced_retry = 1;
		}	
		//sync with remote size
		isize = i_size_read(inode);
		//remote writes shouldn't be able to interrupt because
		//it should block incoming invalidations so it would
		//block writers being able to write to this page so any
		//size changes specific to this page will probably not change
		//however, it could truncate the file and we should read zeroes
		//but we should catch that from the page remote valid check 
		//file size getting larger wouldn't matter
		//to properly handle truncations the page would have to be invalidated
		//first so the next check would fail. It is possible though that
		//we end up reading zeros though (if properly truncated)

		//make sure we didn't get invalidated
		if(!PageRemoteValid(page)){
			if(set_lock){
			release_temp_lock(&temp_status);
			}
			forced_retry = 0;
			goto find_page;
		}	



		//double check to make sure the size didn't change
		//this would further avoid us having a truncation issue 
		new_isize = i_size_read(inode);
		if(new_isize != isize){
			if(set_lock){
			release_temp_lock(&temp_status);
			}
			forced_retry = 0;
			goto find_page;
		}

		end_index = (isize - 1) >> PAGE_SHIFT;
		if (unlikely(!isize || index > end_index)) {
			put_page(page);
			if(set_lock){
			release_temp_lock(&temp_status);
			}

			forced_retry = 0;	
			goto out;
		}

		/* nr is the maximum number of bytes to copy from this page */
		nr = PAGE_SIZE;
		if (index == end_index) {
			nr = ((isize - 1) & ~PAGE_MASK) + 1;
			if (nr <= offset) {
			
				put_page(page);
				if(set_lock){
					release_temp_lock(&temp_status);
				}

				goto out;

			}
		}
		nr = nr - offset;

		/* If users can be writing to this page using arbitrary
		 * virtual addresses, take care about potential aliasing
		 * before reading the page on the kernel side.
		 */
		if (mapping_writably_mapped(mapping))
			flush_dcache_page(page);

		/*
		 * When a sequential read accesses a page several times,
		 * only mark it as accessed the first time.
		 */
		if (prev_index != index || offset != prev_offset)
			mark_page_accessed(page);
		prev_index = index;

		/*
		 * Ok, we have the page, and it's up-to-date, so
		 * now we can copy it to user space...
		 */
			
		ret = retry_able_copy_page_to_iter_iovec(page, offset, nr, iter, inode->i_ino, index, current_value);
		if(set_lock){
			release_temp_lock(&temp_status);
		}
		if(ret == -1337){
			forced_retry = 1;
			retry_counter++;
			uint64_t start_cycle = test_rdtsc(retry_counter);


			goto retry_page_ok;
		}

		offset += ret;
		index += offset >> PAGE_SHIFT;
		offset &= ~PAGE_MASK;
		prev_offset = offset;

		put_page(page);
		written += ret;

		if (!iov_iter_count(iter))
			goto out;
		if (ret < nr) {
			pr_info("EFAULT ret was %ld nr was %ld", ret, nr);
			error = -EFAULT;
			goto out;
		}
		continue;

page_not_up_to_date:
		
		/* Get exclusive access to the page ... */
		//both local and remote sync set up 
		temp_status = acquire_page_lock(filp, mapping->host, index, inode_pages_address, mapping, 0);
		error = lock_page_killable(page);
		if (unlikely(error))
			goto readpage_error;

page_not_up_to_date_locked:

		if (!page->mapping) {
			unlock_page(page);
			put_page(page);
			release_temp_lock(&temp_status);
			continue;
		}

		//check to see if the page needs to be read from remote
		if(!PageRemoteValid(page)){
			goto readpage;
		}

		if (PageUptodate(page)) {

			unlock_page(page);
			release_temp_lock(&temp_status);
			goto page_ok;
		}

readpage:

		//check to make sure we don't acquire a duplicat copy
		if(PageRemoteValid(page)){
			pr_info("page was found; preventing undefined duplicate access ");
			release_temp_lock(&temp_status);
			unlock_page(page);
			goto page_ok;
		}
		/*
		 * A previous I/O error may have been due to temporary
		 * failures, eg. multipath errors.
		 * PG_error will be set again if readpage fails.
		 */
		ClearPageError(page);

		/* Start the actual read. The read will unlock the page. */
		//additionally this read syncs with remote instead of 
		//reading from the disk.
		error = mapping->a_ops->readpage(filp, page);
	
		//allow for failure to get page from remote
		//need to retry request	
		if(error == -1337){

			//on failure release the remote sync lock
			release_temp_lock(&temp_status);
			uint64_t start_cycle = test_rdtsc(1);

			goto find_page;
		}


		if (unlikely(error)) {
			pr_err("readpage errored");
			BUG_ON(1);
			if (error == AOP_TRUNCATED_PAGE) {
				put_page(page);
				error = 0;
				release_temp_lock(&temp_status);
				goto find_page;
			}
			release_temp_lock(&temp_status);
			goto readpage_error;
		}
		//if we succeed we should be able to update the coherence state that the writer checks
		temp_status.state->state = DFS_READ;



		//if somehow things broke, downgrade the remote state
		//for the write path
		if(!PageRemoteValid(page)){
			//also if there is failure we can sync here
			temp_status.state->state = 0;

			release_temp_lock(&temp_status);
			goto find_page;
			//treat this as failing to gain remote access
			//go to the findpage section and try again
		}

		//syncing with local writers (which acquire page lock should block)
		if (!PageUptodate(page)) {
			error = lock_page_killable(page);
			if (unlikely(error)){
				release_temp_lock(&temp_status);
				goto readpage_error;
			}
			if (!PageUptodate(page)) {
				if (page->mapping == NULL) {
					/*
					 * invalidate_mapping_pages got it
					 */
					unlock_page(page);
					put_page(page);
					release_temp_lock(&temp_status);
					goto find_page;
				}
				unlock_page(page);
				error = -EIO;
				release_temp_lock(&temp_status);
				goto readpage_error;
			}
			unlock_page(page);
		}

		//finally release the lock and copy the data to userspace
		release_temp_lock(&temp_status);
		goto page_ok;

readpage_error:
		/* UHHUH! A synchronous read error occurred. Report it */
		put_page(page);
		goto out;

no_cached_page:

		temp_status = acquire_page_lock(filp, mapping->host, index, inode_pages_address, mapping, 0);

		/*
		 * Ok, it wasn't cached, so we need to create a new
		 * page..
		 */
		page = page_cache_alloc(mapping);
		if (!page) {
			error = -ENOMEM;
			goto out;
		}
		error = add_to_page_cache_lru(page, mapping, index,
				mapping_gfp_constraint(mapping, GFP_KERNEL));
		if (error) {
			put_page(page);
			if (error == -EEXIST) {
				error = 0;
				release_temp_lock(&temp_status);
				goto find_page;
			}
			release_temp_lock(&temp_status);
			pr_info("failed to add page to page cache");
			BUG_ON(1);
			goto out;
		}
		goto readpage;
	}

would_block:
	error = -EAGAIN;
out:
			
	ra->prev_pos = prev_index;
	ra->prev_pos <<= PAGE_SHIFT;
	ra->prev_pos |= prev_offset;

	*ppos = ((loff_t)index << PAGE_SHIFT) + offset;
	file_accessed(filp);
	return written ? written : error;
}


//taken from kernel so that we can edit the generic_file_buffered_read function

	inline ssize_t
simplefs_generic_file_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{

        size_t count = iov_iter_count(iter);
        ssize_t retval = 0;
	struct file *file;
	struct address_space *mapping;
        struct inode *inode; 
	loff_t size;
        if (!count)
                goto out; /* skip atime */

        if (iocb->ki_flags & IOCB_DIRECT) {
		pr_err("direct reads not supported");
		BUG_ON(1);
                file = iocb->ki_filp;
                mapping = file->f_mapping;
                inode = mapping->host;
                

                size = i_size_read(inode);
                if (iocb->ki_flags & IOCB_NOWAIT) {
                        if (filemap_range_has_page(mapping, iocb->ki_pos,
                                                   iocb->ki_pos + count - 1))
                                return -EAGAIN;
                } else {
                        retval = filemap_write_and_wait_range(mapping,
                                                iocb->ki_pos,
                                                iocb->ki_pos + count - 1);
                        if (retval < 0)
                                goto out;
                }

                file_accessed(file);

                retval = mapping->a_ops->direct_IO(iocb, iter);
                if (retval >= 0) {
                        iocb->ki_pos += retval;
                        count -= retval;
                }
                iov_iter_revert(iter, count - iov_iter_count(iter));

                /*
                 * Btrfs can have a short DIO read if we encounter
                 * compressed extents, so if there was an error, or if
                 * we've already read everything we wanted to, or if
                 * there was a short read because we hit EOF, go ahead
                 * and return.  Otherwise fallthrough to buffered io for
                 * the rest of the read.  Buffered reads will not work for
                 * DAX files, so don't bother trying.
                 */
                if (retval < 0 || !count || iocb->ki_pos >= size ||
                    IS_DAX(inode))
                        goto out;
        }

	//call into our modified buffered_read function
        retval = simplefs_generic_file_buffered_read(iocb, iter, retval);
out:
        return retval;
}

	


//used in inode.c to check if address is for a page 
u64 page_shmem_address_check(void *addr, unsigned long size)
{


    struct  shmem_coherence_state * coherence_state;
    down_read(&hash_page_rwsem);
    coherence_state = shmem_in_hashmap((unsigned long)addr);
    up_read(&hash_page_rwsem);
    if(coherence_state != NULL){
	    return 1;
    }else{
    }
	
    return 0;
}



//called on write path to get write access to remote page, potentially
//copying that remote page into the local page buffer.
static bool shmem_invalidate_page_write(struct address_space * mapping, struct page * pagep, int page_index, void *inv_argv){

        struct page * testp = pagep;
        uintptr_t inode_pages_address;
        struct mm_struct *mm;
	void * buf;
        spinlock_t *ptl_ptr = NULL;
        pte_t *temppte;
        void *ptrdummy;
        loff_t test = 20; 
	int cpu_id;
	u32 req_qp;
	struct cnthread_rdma_msg_ctx *rdma_ctx; 
        struct cnthread_inv_msg_ctx *inv_ctx; 
	

        mm = get_init_mm();
        inode_pages_address = shmem_address[mapping->host->i_ino] + (PAGE_SIZE * (page_index));
	
	cpu_id = get_cpu();
	

        spin_lock(&cnthread_inval_send_ack_lock[cpu_id]);

        buf = (void*)get_dummy_page_dma_addr(cpu_id);

        temppte = ensure_pte(mm, (uintptr_t)get_dummy_page_buf_addr(cpu_id), &ptl_ptr);

        ptrdummy = get_dummy_page_buf_addr(cpu_id);

        //writes data to that page
        //copy data into dummy buffer, and send to switch
	if(testp == NULL){	
	}else{
        	simplefs_kernel_page_read(testp, (void*)get_dummy_page_buf_addr(cpu_id), PAGE_SIZE, &test);
	}

	rdma_ctx = NULL;
        inv_ctx = &((struct cnthread_inv_argv *)inv_argv)->inv_ctx;
	
	rdma_ctx = &inv_ctx->rdma_ctx;
	inv_ctx->original_qp = (rdma_ctx->ret & CACHELINE_ROCE_RKEY_QP_MASK) >> CACHELINE_ROCE_RKEY_QP_SHIFT;
        create_invalidation_rdma_ack(inv_ctx->inval_buf, rdma_ctx->fva, rdma_ctx->ret, rdma_ctx->qp_val);
        *((u32 *)(&(inv_ctx->inval_buf[CACHELINE_ROCE_VOFFSET_TO_IP]))) = rdma_ctx->ip_val;

	
	req_qp = (get_id_from_requester(inv_ctx->rdma_ctx.requester) * DISAGG_QP_PER_COMPUTE) + inv_ctx->original_qp;
	
	cn_copy_page_data_to_mn(DISAGG_KERN_TGID, mm, inode_pages_address,
        temppte, CN_TARGET_PAGE, req_qp, buf);



        _cnthread_send_inval_ack(DISAGG_KERN_TGID, inode_pages_address, inv_ctx->inval_buf);

        cnthread_send_finish_ack(DISAGG_KERN_TGID, inode_pages_address, inv_ctx, 1);



	spin_unlock(&cnthread_inval_send_ack_lock[cpu_id]);
	return true;
}


//invalidation path for pages, locks the page, copies back to remote system
//and then marks page as invalid
static bool shmem_invalidate(struct shmem_coherence_state * coherence_state, void *inv_argv){


	void *pagep;
	struct page * testp; 

	struct address_space *mapping = coherence_state->mapping;

	//this is taken from find_get_entry in filemap.c
	//spin locks taken from fs/nilfs2/page.c
	pagep = find_get_page(mapping, coherence_state->pagenum);
		
	if(pagep){
		lock_page(pagep);

		if(!PageUptodate(pagep)){
			pr_info("INODE %ld PAGE NOT UP TO DATE?", mapping->host->i_ino);
		}
		if(!PageRemoteValid(pagep)){

		}
		testp = pagep;

		//mark page as invalid from remote system
		//cleared the flags up here so that any future reads are blocked
		//by the MIND system instead of potentially reading stale data.
		ClearPageRemoteValid(testp);
		coherence_state->state = 0;
		
		


		//perform page invalidation stuff here
		shmem_invalidate_page_write(coherence_state->mapping, testp, coherence_state->pagenum, inv_argv);


		//mark page as invalid from remote system
	
				//doesn't matter where this is set since the lock is acquired

		unlock_page(pagep);
	}else{

		struct page * testp = NULL;
		//this can also be reached if something has been truncated
		shmem_invalidate_page_write(coherence_state->mapping, testp, coherence_state->pagenum, inv_argv);
		pr_info("page missing");
		if(coherence_state->mapping->host->i_size != 0){
			pr_info("file length wasn't zero");
			if(coherence_state->mapping->host->i_size < coherence_state->pagenum * 4096){
				pr_info("file has shrunk smaller than this page location");
			}else{
			}
		}
		
		coherence_state->state = 0;

	}

	return true;

}

//called from inode.c for the invalidation path for pages
u64 page_testing_invalidate_page_callback(void *addr, void *inv_argv)
{


    struct shmem_coherence_state * coherence_state;
    while(!down_read_trylock(&hash_page_rwsem));
    coherence_state = shmem_in_hashmap((unsigned long)addr);

    if(coherence_state != NULL){
	    while(!down_write_trylock(&(coherence_state->rwsem))){ //lock the page
	    } 

	    up_read(&hash_page_rwsem); //unlock the hashtable now

	    shmem_invalidate(coherence_state, inv_argv);
	    up_write(&(coherence_state->rwsem)); //lock the page

    }else{
	    up_read(&hash_page_rwsem);
	    pr_err("PAGE NOT IN HASHMAP");
	    return 0;
    }
    return 1;
}



ssize_t hole_punch_fix(struct file *file,
				struct iov_iter *i, loff_t pos, int holepunchbytes, int holepunchpos, int holepunchend, int holepunchstartpage, int holepunchendpage)
{
	struct address_space *mapping = file->f_mapping;
	const struct address_space_operations *a_ops = mapping->a_ops;
	long status = 0;
	ssize_t written = 0;
	unsigned int flags = 0;
	int result = 0;
	int j; //for zero out part
	unsigned int currentpage;
	struct inode * inode; 
	uintptr_t inode_pages_address;
	struct page_lock_status temp; 
	char * zero_buf;
	pr_info("hole punch fix");
	zero_buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if(zero_buf == NULL){
		pr_err("failed to malloc");
		BUG_ON(1);
	}
	for(j = 0; j < PAGE_SIZE; j++){
		zero_buf[j] = 0;	
	}
	pos = holepunchpos;
	do {
		struct page *page;
		unsigned long offset;	/* Offset into pagecache page */
		unsigned long bytes;	/* Bytes to write to page */
		size_t copied;		/* Bytes copied from user */
		void *fsdata;

		offset = (pos & (PAGE_SIZE - 1));

		bytes =  PAGE_SIZE - offset;
					


		if (fatal_signal_pending(current)) {
			status = -EINTR;
			break;
		}

		//this sets up the addresses that we need for remote page sync
		currentpage = pos / PAGE_SIZE;
		inode = mapping->host;


		inode_pages_address = shmem_address[inode->i_ino] +
			(PAGE_SIZE * (currentpage));

retry:
		//acquire page lock acquires the local lock that is used to sync local reads, writes and 
		//invalidations	
		temp = acquire_page_lock(file, inode, currentpage, inode_pages_address, mapping, DFS_WRITE);


		status = a_ops->write_begin(file, mapping, pos, bytes, flags,
						&page, &fsdata);

		if (unlikely(status < 0)){
			pr_err("STATUS WAS ZERO");
			BUG_ON(1);
			break;
		}

		if (mapping_writably_mapped(mapping)){
			flush_dcache_page(page);

		}


		//acquire page lock tells us the status that it used to have before updating it
		//if that status is *lower* than what we need (we need it in write mode) then 
		//we have to go through the process of upgrading it with the remote system.	
		if (temp.old_state < DFS_WRITE){
			if(temp.old_state < DFS_READ || !PageRemoteValid(page)){

								//request access to page and then read the page 
				//if we don't have in read mode then we are missing data
				//that means that we need to copy the data from the received copy
				//into the page before performing the write
				int page_counter_start = page_counter[inode->i_ino][page->index] + 1;
				int page_counter_end = page_counter_start + 1;

				WRITE_ONCE(page_counter[inode->i_ino][page->index], page_counter_start);
				smp_wmb();


				result = invalidate_page_write(page, file, inode, currentpage, true);
				smp_wmb();
				WRITE_ONCE(page_counter[inode->i_ino][page->index], page_counter_end);


			}else{
				result = invalidate_page_write(page, file, inode, currentpage, false);
			}
		}

		//if we fail...
		if(result == -1){
			//downgrade the state (only used for sync on write path)
			temp.state->state = temp.old_state;

			//release the lock from acquire_page_lock
			release_temp_lock(&temp);

			//page hasn't been marked as remote valid yet
			//on failure data in the page hasn't been changed yet
		
			//unlock the page locally 	
			unlock_page(page);

			goto retry;

		}

		//if we didn't fail, mark the page as remote valid (for read path to use)
		//now the local readers just have to wait for PageUptodate
		SetPageRemoteValid(page);

		//this is a hole punched write, then zero out the data in the gap
		simplefs_kernel_page_write(page, zero_buf, bytes, offset);
		kfree(zero_buf);
		copied = bytes;
		//perform the write

		flush_dcache_page(page);

		status = a_ops->write_end(file, mapping, pos, bytes, copied,
						page, fsdata);


		//release the lock from acquire page lock
		release_temp_lock(&temp);


		if (unlikely(status < 0))
			break;
		copied = status;

		cond_resched();
		
		pos += copied;
		written += copied;

		balance_dirty_pages_ratelimited(mapping);
	} while (written < holepunchbytes);

	return written ? written : status;
}



//taken from kernel so that we can do sync around write_begin
//and write_end and the actual data copy that happens between them
ssize_t simplefs_generic_perform_write(struct file *file,
				struct iov_iter *i, loff_t pos)
{
	struct address_space *mapping = file->f_mapping;
	const struct address_space_operations *a_ops = mapping->a_ops;
	long status = 0;
	ssize_t written = 0;
	unsigned int flags = 0;
	int result = 0;
	int j; //for zero out part
	unsigned int currentpage;
	struct inode * inode; 
	uintptr_t inode_pages_address;
	struct page_lock_status temp; 
	int holepunchbytes = -1; //replaces bytes
	int holepunchpos = -1; // replaces pos 
	int holepunchend = -1;
	int holepunchstartpage = -1;
	int holepunchendpage = -1;

	do {
		struct page *page;
		unsigned long offset;	/* Offset into pagecache page */
		unsigned long bytes;	/* Bytes to write to page */
		size_t copied;		/* Bytes copied from user */
		void *fsdata;

		offset = (pos & (PAGE_SIZE - 1));

		bytes = min_t(unsigned long, PAGE_SIZE - offset,
						iov_iter_count(i));

again:

		/*
		 * Bring in the user page that we will copy from _first_.
		 * Otherwise there's a nasty deadlock on copying from the
		 * same page as we're writing to, without it being marked
		 * up-to-date.
		 *
		 * Not only is this an optimisation, but it is also required
		 * to check that the address is actually valid, when atomic
		 * usercopies are used, below.
		 */
		if (unlikely(iov_iter_fault_in_readable(i, bytes))) {
			pr_info("iov iter fault i %d bytes %ld", i, bytes);
			status = -EFAULT;
			break;
		}

		int retrycount = 0;
		if (fatal_signal_pending(current)) {
			status = -EINTR;
			break;
		}

		//this sets up the addresses that we need for remote page sync
		currentpage = pos / PAGE_SIZE;
		inode = mapping->host;

		//hole punch fix
		if(pos > i_size_read(inode) && holepunchpos == -1){
			holepunchpos = i_size_read(inode);
			holepunchbytes = pos - i_size_read(inode);
			holepunchend = pos;
			holepunchstartpage = holepunchpos / PAGE_SIZE;
			holepunchendpage = holepunchend / PAGE_SIZE;
			pr_info("HOLE PUNCH BY %lld", pos - i_size_read(inode));
		hole_punch_fix(file, i, pos, holepunchbytes, holepunchpos, holepunchend, holepunchstartpage, holepunchendpage);

		}

		inode_pages_address = shmem_address[inode->i_ino] +
			(PAGE_SIZE * (currentpage));

retry:
		//acquire page lock acquires the local lock that is used to sync local reads, writes and 
		//invalidations	
		temp = acquire_page_lock(file, inode, currentpage, inode_pages_address, mapping, DFS_WRITE);

		status = a_ops->write_begin(file, mapping, pos, bytes, flags,
						&page, &fsdata);

		if (unlikely(status < 0)){

			break;
		}

		if (mapping_writably_mapped(mapping)){
			flush_dcache_page(page);

		}


		//acquire page lock tells us the status that it used to have before updating it
		//if that status is *lower* than what we need (we need it in write mode) then 
		//we have to go through the process of upgrading it with the remote system.	
		if (temp.old_state < DFS_WRITE){
			if(temp.old_state < DFS_READ || !PageRemoteValid(page)){

				//request access to page and then read the page 
				//if we don't have in read mode then we are missing data
				//that means that we need to copy the data from the received copy
				//into the page before performing the write


				int page_counter_start = page_counter[inode->i_ino][page->index] + 1;
				int page_counter_end = page_counter_start + 1;

				WRITE_ONCE(page_counter[inode->i_ino][page->index], page_counter_start);
				smp_wmb();
				result = invalidate_page_write(page, file, inode, currentpage, true);
				smp_wmb();
				WRITE_ONCE(page_counter[inode->i_ino][page->index], page_counter_end);


				//set page as up to date since we don't want it to zero out what it 
				//thinks are new buffers if a short write happens after hitting this code
				//path
				SetPageUptodate(page);    

			}else{
				result = invalidate_page_write(page, file, inode, currentpage, false);
			}
		}

		//if we fail...
		if(result == -1){
			//downgrade the state (only used for sync on write path)
			temp.state->state = temp.old_state;

			//release the lock from acquire_page_lock
			release_temp_lock(&temp);

			//page hasn't been marked as remote valid yet
			//on failure data in the page hasn't been changed yet
		
			//unlock the page locally 	
			unlock_page(page);
			retrycount++;
			uint64_t start_cycle = test_rdtsc(retrycount);

			goto retry;

		}

		//if we didn't fail, mark the page as remote valid (for read path to use)
		//now writes proceed as they would locally
		SetPageRemoteValid(page);

		//if this is a hole punched write, then zero out the data in the gap
		if(offset > i_size_read(inode)){	
			int temp_size = i_size_read(inode);
			char * zero_buf = kmalloc(offset - (temp_size  % PAGE_SIZE), GFP_KERNEL);
			if(zero_buf == NULL){
				pr_err("failed to malloc");
				BUG_ON(1);
			}
			for(j = 0; j < offset - (temp_size  % PAGE_SIZE); j++){
				zero_buf[j] = 0;	
			}
			simplefs_kernel_page_write(page, zero_buf, offset - (temp_size % PAGE_SIZE), temp_size % PAGE_SIZE);
			kfree(zero_buf);
		}
		//perform the write
		copied = iov_iter_copy_from_user_atomic(page, i, offset, bytes);

		flush_dcache_page(page);

		status = a_ops->write_end(file, mapping, pos, bytes, copied,
						page, fsdata);

		//release the lock from acquire page lock
		release_temp_lock(&temp);


		if (unlikely(status < 0))
			break;
		copied = status;

		cond_resched();

		iov_iter_advance(i, copied);
		if (unlikely(copied == 0)) {
			/*
			 * If we were unable to copy any data at all, we must
			 * fall back to a single segment length write.
			 *
			 * If we didn't fallback here, we could livelock
			 * because not all segments in the iov can be copied at
			 * once without a pagefault.
			 */
			bytes = min_t(unsigned long, PAGE_SIZE - offset,
						iov_iter_single_seg_count(i));
			goto again;
		}
		pos += copied;
		written += copied;

		balance_dirty_pages_ratelimited(mapping);
	} while (iov_iter_count(i));

	return written ? written : status;
}


//taken from kernel so that we can use our simplefs_generic_perform_write
//function instead of the normal version. 
ssize_t __simplefs_generic_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct address_space * mapping = file->f_mapping;
	struct inode 	*inode = mapping->host;
	ssize_t		written = 0;
	ssize_t		err;
	ssize_t		status;


	//most of this function is unchanged, we just want it to 
	//call into our version of simplefs_generic_perform_write
	//instead of the default generic_perform_write


	/* We can write back this queue in page reclaim */
	current->backing_dev_info = inode_to_bdi(inode);
	err = file_remove_privs(file);
	if (err)
		goto out;

	err = file_update_time(file);
	if (err)
		goto out;

	if (iocb->ki_flags & IOCB_DIRECT) {
		loff_t pos, endbyte;
		pr_info("direct writes not supported");
		BUG_ON(1);

		written = generic_file_direct_write(iocb, from);
		/*
		 * If the write stopped short of completing, fall back to
		 * buffered writes.  Some filesystems do this for writes to
		 * holes, for example.  For DAX files, a buffered write will
		 * not succeed (even if it did, DAX does not handle dirty
		 * page-cache pages correctly).
		 */
		if (written < 0 || !iov_iter_count(from) || IS_DAX(inode))
			goto out;

		status = simplefs_generic_perform_write(file, from, pos = iocb->ki_pos);
		/*
		 * If generic_perform_write() returned a synchronous error
		 * then we want to return the number of bytes which were
		 * direct-written, or the error code if that was zero.  Note
		 * that this differs from normal direct-io semantics, which
		 * will return -EFOO even if some bytes were written.
		 */
		if (unlikely(status < 0)) {
			err = status;
			goto out;
		}
		/*
		 * We need to ensure that the page cache pages are written to
		 * disk and invalidated to preserve the expected O_DIRECT
		 * semantics.
		 */
		endbyte = pos + status - 1;
		err = filemap_write_and_wait_range(mapping, pos, endbyte);
		if (err == 0) {
			iocb->ki_pos = endbyte + 1;
			written += status;
			invalidate_mapping_pages(mapping,
						 pos >> PAGE_SHIFT,
						 endbyte >> PAGE_SHIFT);
		} else {
			/*
			 * We don't know how much we wrote, so just return
			 * the number of bytes which were direct-written
			 */
		}
	} else {
		written = simplefs_generic_perform_write(file, from, iocb->ki_pos);
		if (likely(written > 0))
			iocb->ki_pos += written;
	}
out:
	current->backing_dev_info = NULL;
	return written ? written : err;
}

inline ssize_t simplefs_generic_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file->f_mapping->host;
	ssize_t ret;
	bool write = false;
	int cur_size;
	int new_size;

	//acquire local inode lock (syncs local writes
	down_write(&inode->i_rwsem);

	//read the current size using the synced function 
	//makes sure we have the most up to date size
	cur_size = i_size_read(inode);

	//this was borrowed from elsewhere in the kernel
	new_size = iocb->ki_pos + iov_length(from->iov, from->nr_segs);
	//taken from generic_write_checks

	//check to see if we are going to be increasing the size
	//or appending to the end of the file
	if ((new_size > cur_size)|| (iocb->ki_flags & IOCB_APPEND)){
		write = true;
	}	

	while(1){
	//first try to acquire remote inode lock in read mode 	
	//only need in read mode if we aren't changing the size
	//(note that write = false and the top of this function)
	lock_loop(inode->i_ino, write);

	if(!write){
		//double check to make sure the size didn't change
		//again using the synced function
		if ((new_size > i_size_read(inode))|| (iocb->ki_flags & IOCB_APPEND)){
			up_write(inode_rwlock[inode->i_ino]);
			//if it will change acquire remote lock in write mode
			write = true;
			continue;
		}
	}
	break;
	}
	//generic_write_checks will realign stuff if 
	//we are trying to append to the end
	ret = generic_write_checks(iocb, from);

	//proceed to the next step
	if (ret > 0)
		ret = __simplefs_generic_file_write_iter(iocb, from);


	up_write(inode_rwlock[inode->i_ino]);	
		
		
	up_write(&inode->i_rwsem);

	if (ret > 0)
		ret = generic_write_sync(iocb, ret);
	return ret;
}

void simplefs_invalidatepage (struct page * p, unsigned int x, unsigned int y){
}


int dummy_writepages(struct address_space *mapping, struct writeback_control *wbc){
	//dummy version to prevent crashing

	return 0;
}

const struct address_space_operations simplefs_aops = {
    .readpage = simplefs_readpage,
    .writepage = simplefs_writepage,
    .writepages = dummy_writepages,
    .write_begin = simplefs_write_begin,
    .write_end = simplefs_write_end,
    .invalidatepage = simplefs_invalidatepage,
};

const struct file_operations simplefs_file_ops = {
    .llseek = generic_file_llseek,
    .owner = THIS_MODULE,
    .read_iter = simplefs_generic_file_read_iter,
    .write_iter = simplefs_generic_file_write_iter,
    .fsync = generic_file_fsync,
};

