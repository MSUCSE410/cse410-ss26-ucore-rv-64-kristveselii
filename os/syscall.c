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
 * @va: Project 2: VIRTUAL address of data to write (was physical pointer in Project 1)
 * @len: Number of bytes to write
 * 
 * Project 2: Now handles virtual addresses using copyin
 * Returns: Number of bytes written, or -1 on error
 */
uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d va = %x, len = %d", fd, va, len);
	
	// Only support writing to stdout
	if (fd != STDOUT)
		return -1;
	
	// Project 2: User pointer is now a VIRTUAL address
	// We must copy from user virtual memory to kernel buffer
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];  // Kernel buffer
	
	// Project 2: copyinstr - Copy string from user virtual address to kernel
	// Arguments: (page table, destination, source VA, max length)
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	debugf("size = %d", size);
	
	// Write each character to console
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	
	return size;
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
 * @val: Project 2: VIRTUAL address of TimeVal structure to fill
 * @_tz: Timezone (unused, for compatibility)
 * 
 * Project 2: Now uses copyout to write to user virtual memory
 * Returns: 0 on success, negative on error
 */
uint64 sys_gettimeofday(TimeVal *val, int _tz)
{
	// Project 2: Build result in KERNEL space first
	TimeVal pval;  // Kernel copy of TimeVal
	
	// Read CPU cycle counter
	uint64 cycle = get_cycle();
	
	// Convert to seconds and microseconds
	pval.sec = cycle / CPU_FREQ;
	pval.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	
	// Project 2: Copy from kernel to user VIRTUAL address
	// copyout(page table, destination VA, source buffer, size)
	// Returns 0 on success, -1 on error
	return copyout(curr_proc()->pagetable, (uint64)val, (char*)&pval, sizeof(pval));
}

/**
 * sys_task_info() - Get information about current task
 * @ti: Project 2: VIRTUAL address of TaskInfo structure to fill
 * 
 * Project 2 changes:
 * - Now tracks ACTUAL elapsed time (current - start_time)
 * - Uses copyout to write to virtual address
 * 
 * Returns: 0 on success, -1 on error
 */
int sys_task_info(TaskInfo* ti)
{
	struct proc *target_proc = curr_proc();
	
	// Project 2: Check if process has ever run
	// start_time < 0 means process never started
	if (target_proc->start_time < 0) {
		return -1;  // Error: can't get time for process that never ran
	}
	
	// Project 2: Calculate ACTUAL elapsed time in milliseconds
	uint64 time_ms = get_msec();  // Current time
	printf("time_ms: %d, tp start time: %d", time_ms, target_proc->start_time);
	int time_elapsed_ms = time_ms - target_proc->start_time;  // Elapsed time
	
	// Sanity check: time shouldn't go backwards
	if (time_elapsed_ms < 0) {
		return -1;
	}
	
	// Project 2: Build TaskInfo in KERNEL space
	TaskInfo pti;  // Kernel copy
	pti.status = Running;
	pti.time = time_elapsed_ms;  // ACTUAL elapsed time (not cycle-based)
	
	// Copy syscall counters from process to kernel TaskInfo
	memmove(pti.syscall_times, target_proc->syscall_times, sizeof(pti.syscall_times));
	
	// Project 2: Copy from kernel to user VIRTUAL address
	// copyout(page table, destination VA, source buffer, size)
	return copyout(curr_proc()->pagetable, (uint64)ti, (char*)&pti, sizeof(pti));
}

// ===================== PROJECT 2: NEW SYSCALLS =====================

/**
 * sys_mmap() - Map virtual memory pages
 * @start: Virtual address to start mapping (must be page-aligned)
 * @len: Length of memory region to map
 * @port: Permission bits (bit 0=READ, bit 1=WRITE, bit 2=EXECUTE)
 * @_flag: Flags (unused)
 * @_fd: File descriptor (unused)
 * 
 * Project 2: Allocates physical pages and maps them to virtual addresses
 * Returns: 0 on success, -1 on error
 */
int sys_mmap(void* start, unsigned long long len, int port, int _flag, int _fd)
{
	// Project 2 STEP 1: Validate permissions
	// port must be 3 bits (0-7), and at least one permission must be set
	if (((port & ~0x7) != 0) || ((port & 0x7) == 0)) {
		return -1;  // Invalid permissions
	}
	
	// Project 2 STEP 2: Validate alignment
	// Virtual addresses must be page-aligned (multiple of PAGE_SIZE)
	if ((((uint64)start) % PAGE_SIZE) != 0) {
		return -1;  // Not aligned
	}
	
	// Project 2 STEP 3: Calculate virtual address range
	uint64 start_va = (uint64)start;
	uint64 end_va = PGROUNDUP(start_va + len);  // Round up to page boundary
	uint64 cva;  // Current virtual address
	pagetable_t pagetable = curr_proc()->pagetable;
	
	// Project 2 STEP 4: Build PTE permission bits
	int perm = PTE_U;  // User accessible (required)
	if (port & 1) { perm |= PTE_R; }  // Readable
	if (port & 2) { perm |= PTE_W; }  // Writable
	if (port & 4) { perm |= PTE_X; }  // Executable
	
	// Project 2 STEP 5: Allocate and map pages
	for(cva = start_va; cva < end_va; cva += PAGE_SIZE) {
		// Allocate one physical page
		void* page = kalloc();
		if (page == 0) {
			return -1;  // Out of memory
		}
		
		// Zero the page for security
		memset(page, 0, PGSIZE);
		
		// Map virtual address to physical page in page table
		if (mappages(pagetable, cva, PAGE_SIZE, (uint64)page, perm) != 0) {
			return -1;  // Mapping failed
		}
	}
	
	return 0;  // Success
}

/**
 * sys_munmap() - Unmap virtual memory pages
 * @start: Virtual address to start unmapping (must be page-aligned)
 * @len: Length of memory region to unmap
 * 
 * Project 2: Removes virtual-to-physical mappings and frees physical pages
 * Returns: 0 on success, -1 on error
 */
int sys_munmap(void* start, unsigned long long len)
{
	// Project 2 STEP 1: Validate alignment
	if ((((uint64)start) % PAGE_SIZE) != 0) {
		return -1;  // Not aligned
	}
	
	// Project 2 STEP 2: Calculate virtual address range
	uint64 start_va = (uint64)start;
	uint64 end_va = PGROUNDUP(start_va + len);
	uint64 cva;
	pagetable_t pagetable = curr_proc()->pagetable;
	
	// Project 2 STEP 3: Unmap each page
	for(cva = start_va; cva < end_va; cva += PAGE_SIZE) {
		// Check if this virtual address is actually mapped
		if (useraddr(pagetable, cva) == 0) {
			return -1;  // Trying to unmap something not mapped!
		}
		
		// Unmap virtual address and free physical page
		// uvmunmap(page table, VA, num pages, do_free)
		uvmunmap(pagetable, cva, 1, 1);
	}
	
	return 0;  // Success
}

// ===================================================================

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
	 * Project 2: Changed from info->syscall_times to direct array access
	 * because syscall_times is now in proc struct, not TaskInfo
	 */
	// Every time ANY syscall happens, check if the ID is valid (between 0 & 500) 
	// and increment the counter for that specific syscall.
	curr_proc()->syscall_times[syscall_id]++;  // Update syscall counter
	
	// Dispatch to appropriate syscall handler
	int ret;  // Return value
	switch (syscall_id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
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
	
	// ============ PROJECT 2: NEW SYSCALLS ============
	case SYS_mmap:
		ret = sys_mmap((void*) args[0], args[1], args[2], args[3], args[4]);
		break;
		
	case SYS_munmap:
		ret = sys_munmap((void*) args[0], args[1]);
		break;
	// =================================================
		
	default:
		// Unknown syscall number
		ret = -1;
		errorf("unknown syscall %d", syscall_id);
	}
	
	// Store return value in a0 register (standard RISC-V calling convention)
	tf->a0 = ret;
	
	tracef("syscall ret %d", ret);
}