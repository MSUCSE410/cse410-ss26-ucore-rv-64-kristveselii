#include "syscall.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "proc.h"

/**
 * sys_write() - Write data to a file descriptor
 * @fd: File descriptor (must be STDOUT for this implementation)
 * @str: Pointer to data to write
 * @len: Number of bytes to write
 * 
 * Currently only supports writing to standard output (console).
 * Returns: Number of bytes written, or -1 on error
 */
uint64 sys_write(int fd, char *str, uint len)
{
	debugf("sys_write fd = %d str = %x, len = %d", fd, str, len);
	
	// Only support writing to stdout
	if (fd != STDOUT)
		return -1;
	
	// Write each character to console
	for (int i = 0; i < len; ++i) {
		console_putchar(str[i]);
	}
	
	return len;
}

/**
 * sys_exit() - Terminate the calling process
 * @exit_code: Exit status code (0 = success, non-zero = error)
 * 
 * This function never returns.
 */
__attribute__((noreturn)) void sys_exit(int exit_code)
{
	exit(exit_code);
	__builtin_unreachable();  // Tell compiler this never returns
}

/**
 * sys_sched_yield() - Voluntarily yield the CPU
 * 
 * Allows other processes to run. The calling process remains RUNNABLE
 * and will be scheduled again in the future.
 * 
 * Returns: 0 on success
 */
uint64 sys_sched_yield()
{
	yield();
	return 0;
}

/**
 * sys_gettimeofday() - Get current time
 * @val: Pointer to TimeVal structure to fill
 * @_tz: Timezone (unused, for compatibility)
 * 
 * Returns current time based on CPU cycle counter.
 * Returns: 0 on success
 */
uint64 sys_gettimeofday(TimeVal *val, int _tz)
{
	// Read CPU cycle counter
	uint64 cycle = get_cycle();
	
	// Convert to seconds and microseconds
	val->sec = cycle / CPU_FREQ;
	val->usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	
	return 0;
}

/**
 * sys_task_info() - Get information about current task
 * @ti: Pointer to TaskInfo structure to fill with task information
 * 
 * LAB1: This system call retrieves:
 * - Current task status
 * - Per-syscall call counts
 * - Total running time in milliseconds
 * 
 * Returns: 0 on success
 */
int sys_task_info(TaskInfo *ti)
{
	struct proc *p = curr_proc();
	
	// Copy current task status
	ti->status = p->info->status;
	
	// Copy all syscall counters
	// Calculate array size to ensure we copy all elements
	int syscall_array_size = sizeof(p->info->syscall_times) / sizeof(p->info->syscall_times[0]);
	for (int i = 0; i < syscall_array_size; i++) {
		ti->syscall_times[i] = p->info->syscall_times[i];
	}
	
	// Calculate running time in milliseconds from CPU cycles
	// Formula: (cycles % CPU_FREQ) * 1000 / CPU_FREQ
	// This gives milliseconds for the current second
	uint64 cycle = get_cycle();
	ti->time = (cycle % CPU_FREQ) * 1000 / CPU_FREQ;
	
	return 0;
}

extern char trap_page[];

/**
 * syscall() - Main system call dispatcher
 * 
 * Called from trap handler when user program executes ecall instruction.
 * Reads syscall number from a7 register and arguments from a0-a5.
 * Dispatches to appropriate handler and returns result in a0.
 */
void syscall()
{
	struct proc *p = curr_proc();
	struct trapframe *tf = p->trapframe;
	
	// Extract syscall number and arguments from saved registers
	int syscall_id = tf->a7;
	uint64 args[6] = {
		tf->a0, tf->a1, tf->a2,
		tf->a3, tf->a4, tf->a5
	};
	
	// Log syscall for debugging
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]",
	       syscall_id, args[0], args[1], args[2], args[3], args[4], args[5]);
	
	/*
	 * LAB1: Increment syscall counter
	 * Track how many times each syscall has been called by this process.
	 * Only increment if syscall_id is valid (within array bounds).
	 */
	if (syscall_id > 0 && syscall_id < MAX_SYSCALL_NUM) {
		p->info->syscall_times[syscall_id]++;
	}
	
	// Dispatch to appropriate syscall handler
	int ret;  // Return value
	switch (syscall_id) {
	case SYS_write:
		ret = sys_write(args[0], (char *)args[1], args[2]);
		break;
		
	case SYS_exit:
		sys_exit(args[0]);
		// Never reaches here
		
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
		
	case SYS_gettimeofday:
		ret = sys_gettimeofday((TimeVal *)args[0], args[1]);
		break;
		
	/*
	 * LAB1: Handle sys_task_info system call
	 * Syscall number 410 - retrieve task information
	 */
	case SYS_task_info:
		ret = sys_task_info((TaskInfo *)args[0]);
		break;
		
	default:
		// Unknown syscall number
		ret = -1;
		errorf("unknown syscall %d", syscall_id);
	}
	
	// Store return value in a0 register (standard RISC-V calling convention)
	tf->a0 = ret;
	
	tracef("syscall ret %d", ret);
}