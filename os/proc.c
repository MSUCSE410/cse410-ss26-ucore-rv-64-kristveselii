#include "proc.h"
#include "defs.h"
#include "loader.h"
#include "trap.h"

// Global process management structures
struct proc pool[NPROC];                    // Array of all process control blocks
char kstack[NPROC][PAGE_SIZE];              // Kernel stacks (one per process)
__attribute__((aligned(4096))) char ustack[NPROC][PAGE_SIZE];      // User stacks
__attribute__((aligned(4096))) char trapframe[NPROC][PAGE_SIZE];   // Trapframes (saved user registers)
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
		p->ustack = (uint64)ustack[process_index];
		p->trapframe = (struct trapframe *)trapframe[process_index];
		
		/*
		 * LAB1: Initialize task info pointer and status
		 * Each process gets its own TaskInfo structure from the pool
		 */
		p->info = &task_info_pool[process_index];
		p->info->status = UnInit;  // Process not yet initialized
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
	
	// Clear all process memory for security and consistency
	memset(&p->context, 0, sizeof(p->context));  // Zero saved kernel registers
	memset(p->trapframe, 0, PAGE_SIZE);          // Zero saved user registers
	memset((void *)p->kstack, 0, PAGE_SIZE);     // Zero kernel stack
	
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
				/*
				 * LAB1: Reset task time when process starts running
				 * This tracks time for the current execution period
				 */
				p->info->time = 0;
				
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
 * exit() - Terminate the current process
 * @exit_code: Exit status code (0 = success, non-zero = error)
 * 
 * This function:
 * 1. Logs the exit
 * 2. Marks process as UNUSED (frees the slot)
 * 3. Calls finished() to track completion
 * 4. Switches to scheduler (never returns)
 */
void exit(int exit_code)
{
	struct proc *p = curr_proc();
	
	// Log process termination
	infof("proc %d exit with %d", p->pid, exit_code);
	
	// Mark process slot as available for reuse
	p->state = UNUSED;
	
	// Track that another app has finished (may panic if all done)
	finished();
	
	// Switch to scheduler - this process will never run again
	sched();
}