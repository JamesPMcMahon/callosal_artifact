#include "memory_management.h"
#include "rbtree_ftns.h"
#include "fault.h"
#include <linux/vmalloc.h>

//from arch/x86/include/asm/traps.h
#include "../../arch/x86/include/asm/traps.h"

// page fault handler
unsigned long mn_handle_fault(struct task_struct *tsk, unsigned long error_code,
			unsigned long addr, unsigned long flags, 
            void **buf, unsigned long *data_size, unsigned long* vm_flags)
{
    struct vm_area_struct *vma = NULL;
    unsigned long aligned_addr = addr & PAGE_MASK;
    int err = DISAGG_FAULT_NONE;
    int copy_data = 1;

    *data_size = 0;
    // ASSUME currently, fault are anonymous only, may have VMA
    // only expection is expanding stack
    if (down_read_killable(&tsk->mm->mmap_sem)) {
        return DISAGG_FAULT_ERR_LOCK;
    }	

    vma = mn_find_vma(tsk->mm, addr);
    if (!vma || aligned_addr < vma->vm_start){
        /* Try STACK EXPANSION */
        if (!vma || mn_expand_downwards(tsk, vma, addr))    // non-zero means error here
        {
            DEBUG_print_vma(tsk->mm);
            err = DISAGG_FAULT_ERR_NO_VMA;
            goto error_fault_handler;
        }else{
            copy_data = 0;
            pr_info("PgFault - VMA found (stack): tgid: %d, addr: 0x%lx, vma: 0x%lx - 0x%lx\n", 
                (int)tsk->tgid, addr, vma->vm_start, vma->vm_end);
            // permission will be just follow the error code
            err = DISAGG_FAULT_WRITE;
            if (!(error_code & X86_PF_WRITE))
                err = DISAGG_FAULT_READ;
            goto copy_data_and_return;
        }
    }
    else if (vma && vma->vm_start <= aligned_addr)
    {

        if (!vma_is_anonymous(vma))
        {
            err = DISAGG_FAULT_ERR_NO_ANON;
            goto error_fault_handler;
        }else if ((error_code & X86_PF_WRITE) && (vma->vm_flags & VM_WRITE))
        {
            /* WRITE */
            pr_info("PgFault - VMA found (write): tgid: %d, addr: 0x%lx, vma: 0x%lx - 0x%lx [0x%lx]\n", 
                (int)tsk->tgid, addr, vma->vm_start, vma->vm_end, vma->vm_flags);
            err = DISAGG_FAULT_WRITE;
            goto copy_data_and_return;
            
        }else if (!(error_code & X86_PF_WRITE) && (vma->vm_flags & VM_READ))
        {
            /* READ */

            pr_info("PgFault - VMA found (read): tgid: %d, addr: 0x%lx, vma: 0x%lx - 0x%lx [0x%lx]\n", 
                (int)tsk->tgid, addr, vma->vm_start, vma->vm_end, vma->vm_flags);
            err = DISAGG_FAULT_READ;
            goto copy_data_and_return;
        }else{
            DEBUG_print_one_vma(vma, -1);   // print only current vma info
            err = DISAGG_FAULT_ERR_PERM;   // wrong permission
            goto error_fault_handler;
        }
    }

error_fault_handler:
    up_read(&tsk->mm->mmap_sem);
    return err;

copy_data_and_return:

    if(vma->vm_private_data 
        && (aligned_addr + (PAGE_SIZE) <= vma->vm_end))
    {
        *buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
        if (*buf)
        {
            unsigned long offset = aligned_addr - vma->vm_start;
            if (copy_data)
            {
                memcpy(*buf, vma->vm_private_data + offset, PAGE_SIZE);
            }
            *data_size = PAGE_SIZE;
            *vm_flags = vma->vm_flags;
        }else{
            err =  DISAGG_FAULT_ERR_NO_MEM;
        }
    }else{
        err = DISAGG_FAULT_DEBUG_DATA_MISS;
    }
    up_read(&tsk->mm->mmap_sem);
    return err;
}
