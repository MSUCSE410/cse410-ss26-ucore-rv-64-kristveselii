#include "loader.h"
#include "defs.h"
#include "trap.h"

// Global variables for application management
static uint64 app_num;        // Total number of user applications to load
static uint64 *app_info_ptr;  // Pointer to application metadata array
extern char _app_num[], ekernel[];  // Symbols from link_app.S

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
	static int finished_count = 0;  // Persistent counter across function calls
	
	// Increment and check if all applications have finished
	if (++finished_count >= app_num)
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
	// Safety check: ensure kernel doesn't overlap with application space
	if ((uint64)ekernel >= BASE_ADDRESS) {
		panic("kernel too large...\n");
	}
	
	// Read application metadata from linker-provided symbols
	app_info_ptr = (uint64 *)_app_num;  // Point to start of app info array
	app_num = *app_info_ptr;            // First element is the app count
	app_info_ptr++;                     // Move pointer to actual app addresses
}

/**
 * load_app() - Load a single application into memory
 * @app_index: Which application to load (0-indexed)
 * @app_info: Array of application start/end addresses
 * 
 * Each app gets a fixed memory region:
 * [BASE_ADDRESS + n*MAX_APP_SIZE, BASE_ADDRESS + (n+1)*MAX_APP_SIZE)
 * 
 * Returns: Size of the loaded application in bytes
 */
int load_app(int app_index, uint64 *app_info)
{
	// Calculate source memory region for this app
	uint64 app_start = app_info[app_index];
	uint64 app_end = app_info[app_index + 1];
	uint64 app_length = app_end - app_start;
	
	// Calculate destination address for this app
	void *dest_addr = (void *)BASE_ADDRESS + app_index * MAX_APP_SIZE;
	
	// Clear the entire app memory slot (for security/cleanliness)
	memset(dest_addr, 0, MAX_APP_SIZE);
	
	// Copy the app binary to its designated memory location
	memmove(dest_addr, (void *)app_start, app_length);
	
	return app_length;
}

/**
 * run_all_app() - Load all applications and initialize their process structures
 * 
 * This function:
 * 1. Allocates a process control block (PCB) for each app
 * 2. Loads each app binary into memory
 * 3. Initializes the trapframe (user registers) for each process
 * 4. Sets up initial process state
 * 
 * Returns: 0 on success
 */
int run_all_app()
{
	// Iterate through all applications
	for (int app_index = 0; app_index < app_num; ++app_index) {
		// Allocate a new process control block from the process pool
		struct proc *process = allocproc();
		struct trapframe *tf = process->trapframe;
		
		// Load this application's binary into memory
		load_app(app_index, app_info_ptr);
		
		// Calculate entry point (where the app should start executing)
		uint64 entry_point = BASE_ADDRESS + app_index * MAX_APP_SIZE;
		tracef("load app %d at %p", app_index, entry_point);
		
		// Initialize the trapframe (user-mode CPU state)
		tf->epc = entry_point;  // Program counter: start of app code
		tf->sp = (uint64)process->ustack + USER_STACK_SIZE;  // Stack pointer: top of user stack
		
		// Set initial process state
		process->state = RUNNABLE;  // Process is ready to run
		
		/*
		 * LAB1: Initialize task info fields
		 * - Set initial status to Running (will be updated by scheduler)
		 * - Reset running time counter to 0
		 */
		process->info->status = Running;
		process->info->time = 0;
	}
	
	return 0;
}