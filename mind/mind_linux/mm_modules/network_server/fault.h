#ifndef __MN_FAULT_MODULE_H__
#define __MN_FAULT_MODULE_H__

#include "../../include/disagg/network_disagg.h"
#include "../../include/disagg/fork_disagg.h"
#include "../../include/disagg/exec_disagg.h"
#include "../../include/disagg/mmap_disagg.h"
#include "../../include/disagg/fault_disagg.h"

// page fault handler
unsigned long mn_handle_fault(struct task_struct *tsk, unsigned long error_code,
			unsigned long addr, unsigned long flags, 
			void **buf, unsigned long *data_size, unsigned long *vm_flag);
#endif /* __MN_FAULT_MODULE_H__ */