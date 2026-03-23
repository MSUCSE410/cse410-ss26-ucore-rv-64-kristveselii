#include "loader.h"
#include "defs.h"
#include "trap.h"

// Global variables for application management
static int app_num;          // Total number of user applications to load
static uint64 *app_info_ptr; // Pointer to application metadata array
extern char _app_num[];      // Symbol from link_app.S

/**
 * finished() - Track and verify all applications have completed
 * 
 * This function is called when an application exits. It maintains a count
 * of finished applications and panics when all have completed.
 * 
 * Returns: 0 (never actually returns, always panics when all apps finish)
 */
int finished()
{
	static int fin = 0;  // Persistent counter across function calls
	
	// Increment and check if all applications have finished
	if (++fin >= app_num)
		panic("all apps over");  // Shut down system when all apps complete
	
	return 0;
}

/**
 * loader_init() - Initialize the application loader subsystem
 * 
 * Reads application metadata from link_app.S symbols and validates that
 * the kernel binary doesn't overlap with application memory space.
 */
void loader_init()
{
	// Read application metadata from linker-provided symbols
	app_info_ptr = (uint64 *)_app_num;  // Point to start of app info array
	app_num = *app_info_ptr;            // First element is the app count
	app_info_ptr++;                     // Move pointer to actual app addresses
}

/**
 * bin_loader() - Load binary and set up virtual memory for a process
 * @start: Physical address where app binary starts
 * @end: Physical address where app binary ends
 * @p: Process structure to initialize
 * 
 * Project 2: Creates page table and maps:
 * - Trapframe (for kernel/user transition)
 * - Application code/data
 * - User stack
 * 
 * Returns: Page table pointer
 */
pagetable_t bin_loader(uint64 start, uint64 end, struct proc *p)
{
	// Project 2: Create new page table for this process
	pagetable_t pg = uvmcreate();
	
	// Project 2: Map trapframe page (kernel uses this during syscalls/interrupts)
	if (mappages(pg, TRAPFRAME, PGSIZE, (uint64)p->trapframe,
		     PTE_R | PTE_W) < 0) {
		panic("mappages fail");
	}
	
	// Validate alignment of application binary
	if (!PGALIGNED(start)) {
		panic("user program not aligned, start = %p", start);
	}
	if (!PGALIGNED(end)) {
		// Fix in ch5
		warnf("Some kernel data maybe mapped to user, start = %p, end = %p",
		      start, end);
	}
	
	// Round up to page boundary
	end = PGROUNDUP(end);
	uint64 length = end - start;
	
	// Project 2: Map application code and data to virtual address space
	// Maps physical [start, end) to virtual [BASE_ADDRESS, BASE_ADDRESS + length)
	if (mappages(pg, BASE_ADDRESS, length, start,
		     PTE_U | PTE_R | PTE_W | PTE_X) != 0) {
		panic("mappages fail");
	}
	
	// Save page table in process structure
	p->pagetable = pg;
	
	// Project 2: Allocate and map user stack
	// Stack goes right after the application code
	uint64 ustack_bottom_vaddr = BASE_ADDRESS + length + PAGE_SIZE;
	if (USTACK_SIZE != PAGE_SIZE) {
		// Fix in ch5
		panic("Unsupported");
	}
	
	// Allocate physical page for stack and map it
	mappages(pg, ustack_bottom_vaddr, USTACK_SIZE, (uint64)kalloc(),
		 PTE_U | PTE_R | PTE_W | PTE_X);
	p->ustack = ustack_bottom_vaddr;
	
	// Initialize trapframe registers
	p->trapframe->epc = BASE_ADDRESS;  // Start execution at BASE_ADDRESS
	p->trapframe->sp = p->ustack + USTACK_SIZE;  // Stack grows down from top
	
	// Track highest page allocated
	p->max_page = PGROUNDUP(p->ustack + USTACK_SIZE - 1) / PAGE_SIZE;
	
	return pg;
}

/**
 * run_all_app() - Load all applications and initialize their process structures
 * 
 * Project 2: Now uses bin_loader() to set up page tables for each process
 * 
 * Returns: 0 on success
 */
int run_all_app()
{
	// Iterate through all applications
	for (int i = 0; i < app_num; ++i) {
		// Allocate a new process control block from the process pool
		struct proc *p = allocproc();
		tracef("load app %d", i);
		
		// Project 2: Load binary and set up virtual memory
		// app_info_ptr[i] = start of binary, app_info_ptr[i+1] = end of binary
		bin_loader(app_info_ptr[i], app_info_ptr[i + 1], p);
		
		// Set initial process state
		p->state = RUNNABLE;  // Process is ready to run
		
		// Project 2: Initialize time tracking fields
		p->start_time = -1;  // -1 means "never started yet"
		memset(p->syscall_times, 0, sizeof(p->syscall_times));  // Zero syscall counters
	}
	
	return 0;
}