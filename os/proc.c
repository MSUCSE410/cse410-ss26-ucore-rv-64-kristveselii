#include "proc.h"
#include "defs.h"
#include "loader.h"
#include "trap.h"
#include "timer.h"   // Project 2: NEW - for get_msec() function
#include "vm.h"      // Project 2: NEW - for virtual memory functions

// Global process management structures
struct proc pool[NPROC];                    // Array of all process control blocks
__attribute__((aligned(16))) char kstack[NPROC][PAGE_SIZE];  // Project 2: Kernel stacks (aligned to 16 bytes)
// Project 2: REMOVED user stack array - user stacks now allocated via page tables
__attribute__((aligned(4096))) char trapframe[NPROC][TRAP_PAGE_SIZE];  // Project 2: Trapframes (changed size)
// This is an array that holds all 16 (defined in proc.h) possible processes. Process 0 uses task_info_pool[0], process 1 uses task_info_pool[1], etc.
TaskInfo task_info_pool[NPROC];             // LAB1: Task info storage for each process

extern char boot_stack_top[];
struct proc *current_proc;  // Pointer to currently executing process
struct proc idle;           // Special idle process (runs when no user processes ready)

/**
 * threadid() - Get current thread/process ID
 * Returns: PID of currently running process
 */
int threadid()
{
	return curr_proc()->pid;
}

/**
 * curr_proc() - Get pointer to current process
 * Returns: Pointer to the currently executing process control block
 */
struct proc *curr_proc()
{
	return current_proc;
}

/**
 * proc_init() - Initialize the process management subsystem
 * 
 * Called once at boot time to:
 * 1. Mark all process slots as UNUSED
 * 2. Assign pre-allocated memory regions to each process
 * 3. Initialize the idle process
 */
void proc_init(void)
{
	struct proc *p;
	
	// Initialize each process slot in the pool
	for (p = pool; p < &pool[NPROC]; p++) {
		// Mark slot as available
		p->state = UNUSED;
		
		// Assign pre-allocated kernel and user stacks
		// Calculate index: (p - pool) gives position in array
		int process_index = p - pool;
		p->kstack = (uint64)kstack[process_index];
		// Project 2: REMOVED - No longer assigning physical user stack
		// p->ustack = (uint64)ustack[process_index];  
		p->trapframe = (struct trapframe *)trapframe[process_index];
		
		// Project 2: Initialize time tracking fields
		// start_time = -1 means "process has never run yet"
		p->start_time = -1;
		
		// Project 2: Zero out syscall counter array
		// This replaces the per-field initialization from Project 1
		memset(p->syscall_times, 0, sizeof(p->syscall_times));
	}
	
	// Initialize the special idle process (PID 0)
	idle.kstack = (uint64)boot_stack_top;
	idle.pid = 0;
	current_proc = &idle;  // Start with idle process as current
}

/**
 * allocpid() - Allocate a unique process ID
 * 
 * Uses a static counter to ensure each process gets a unique PID.
 * Returns: Next available PID (starts at 1, increments forever)
 */
int allocpid()
{
	static int next_pid = 1;  // Persistent across function calls
	return next_pid++;         // Return current value, then increment
}

/**
 * allocproc() - Allocate and initialize a process control block
 * 
 * Searches the process pool for an unused slot, then:
 * 1. Assigns a unique PID
 * 2. Clears all memory (context, trapframe, kernel stack)
 * 3. Sets up initial context to return to usertrapret
 * 
 * Returns: Pointer to allocated process, or NULL if pool is full
 */
struct proc *allocproc(void)
{
	struct proc *p;
	
	// Search for an unused process slot
	for (p = pool; p < &pool[NPROC]; p++) {
		if (p->state == UNUSED) {
			goto found;  // Found an available slot
		}
	}
	return 0;  // Pool is full, no available slots

found:
	// Initialize the newly allocated process
	p->pid = allocpid();    // Assign unique process ID
	p->state = USED;        // Mark as allocated (not yet runnable)
	
	// Project 2: Initialize virtual memory fields to 0
	// These will be set up when the process is actually loaded
	p->pagetable = 0;   // No page table yet
	p->ustack = 0;      // No user stack virtual address yet
	p->max_page = 0;    // No pages allocated yet
	
	// Clear all process memory for security and consistency
	memset(&p->context, 0, sizeof(p->context));  // Zero saved kernel registers
	memset((void *)p->kstack, 0, PAGE_SIZE);   // Zero kernel stack
	memset((void *)p->trapframe, 0, TRAP_PAGE_SIZE);  // Project 2: Use TRAP_PAGE_SIZE
	
	// Set up initial kernel context
	// When this process is first scheduled, it will "return" to usertrapret
	p->context.ra = (uint64)usertrapret;  // Return address
	p->context.sp = p->kstack + PAGE_SIZE;  // Stack pointer (top of kernel stack)
	
	return p;
}

/**
 * scheduler() - Main scheduling loop (never returns)
 * 
 * This function implements a simple round-robin scheduler:
 * 1. Scan through all processes looking for RUNNABLE ones
 * 2. Switch to each runnable process in order
 * 3. When process yields/blocks, return here and continue scanning
 * 
 * The scheduler runs in the context of the idle process.
 */
void scheduler(void)
{
	struct proc *p;
	
	// Infinite loop - scheduler never exits
	for (;;) {
		// Scan through entire process pool
		for (p = pool; p < &pool[NPROC]; p++) {
			// Only schedule processes that are ready to run
			if (p->state == RUNNABLE) {
				// Project 2: Track when process first starts running
				// If start_time is -1 (never started), record current time
				if (p->start_time < 0) {
					p->start_time = get_msec();  // Get current time in milliseconds
					printf("SCHEDULED AT TIME : %d\n", p->start_time);
				}
				
				// Update process state and make it current
				p->state = RUNNING;
				current_proc = p;
				
				// Context switch: save idle context, restore process context
				// This transfers control to the process
				// When the process yields, we return here
				swtch(&idle.context, &p->context);
				
				// Process has yielded/blocked, continue scanning
			}
		}
	}
}

/**
 * sched() - Switch from current process back to scheduler
 * 
 * This function is called by yield(), exit(), and blocking operations.
 * It performs a context switch from the current process to the idle/scheduler.
 * 
 * Precondition: Process state must NOT be RUNNING (should be RUNNABLE or other)
 */
void sched(void)
{
	struct proc *p = curr_proc();
	
	// Sanity check: shouldn't call sched() while still marked as running
	if (p->state == RUNNING)
		panic("sched running");
	
	// Context switch back to scheduler (idle process)
	swtch(&p->context, &idle.context);
	// Execution returns here when process is scheduled again
}

/**
 * yield() - Voluntarily give up the CPU
 * 
 * Called by processes that want to cooperatively multitask.
 * Marks process as RUNNABLE and switches to scheduler.
 */
void yield(void)
{
	current_proc->state = RUNNABLE;  // Still ready to run, just giving up CPU
	sched();  // Switch back to scheduler
	// Returns here when we're scheduled again
}

/**
 * freeproc() - Free process resources
 * @p: Process to free
 * 
 * Project 2: Marks process slot as available for reuse.
 * In a complete implementation, this would also free the page table.
 */
void freeproc(struct proc *p)
{
	p->state = UNUSED;  // Mark slot as available
	// Project 2: uvmfree(p->pagetable, p->max_page);  // Would free page table (commented out)
}

/**
 * exit() - Terminate the current process
 * @exit_code: Exit status code (0 = success, non-zero = error)
 * 
 * This function:
 * 1. Logs the exit
 * 2. Frees process resources
 * 3. Calls finished() to track completion
 * 4. Switches to scheduler (never returns)
 */
void exit(int exit_code)
{
	struct proc *p = curr_proc();
	
	// Log process termination
	infof("proc %d exit with %d", p->pid, exit_code);
	
	// Project 2: Use freeproc() to clean up instead of just setting state
	freeproc(p);  // Frees resources and marks as UNUSED
	
	// Track that another app has finished (may panic if all done)
	finished();
	
	// Switch to scheduler - this process will never run again
	sched();
}