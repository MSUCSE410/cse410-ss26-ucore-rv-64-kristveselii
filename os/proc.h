#ifndef PROC_H
#define PROC_H

#include "types.h"

// System constants
#define NPROC (16)              // Maximum number of concurrent processes
#define MAX_SYSCALL_NUM 500     // Size of syscall counter array

/**
 * struct context - Saved kernel registers for context switching
 * 
 * When a process gives up the CPU (yield/block), these registers are saved.
 * When the process is scheduled again, these registers are restored.
 * Only callee-saved registers need to be saved (caller-saved are on stack).
 */
struct context {
	uint64 ra;  // Return address - where to resume execution
	uint64 sp;  // Stack pointer - kernel stack location

	// Callee-saved registers (s0-s11)
	// These must be preserved across function calls
	uint64 s0;
	uint64 s1;
	uint64 s2;
	uint64 s3;
	uint64 s4;
	uint64 s5;
	uint64 s6;
	uint64 s7;
	uint64 s8;
	uint64 s9;
	uint64 s10;
	uint64 s11;
};

/**
 * enum procstate - Process lifecycle states
 * 
 * A process transitions through these states:
 * UNUSED -> USED -> RUNNABLE -> RUNNING -> (back to RUNNABLE or UNUSED)
 */
enum procstate {
	UNUSED,    // Process slot is available (not allocated)
	USED,      // Process allocated but not yet ready to run
	SLEEPING,  // Process is blocked waiting for an event
	RUNNABLE,  // Process is ready to run (in ready queue)
	RUNNING,   // Process is currently executing
	ZOMBIE     // Process has exited but not yet cleaned up
};

/**
 * TaskStatus - User-visible task states for sys_task_info
 * 
 * These are the simplified states reported to user programs.
 * Maps from internal procstate to user-facing status.
 */
typedef enum {
	UnInit,   // Task not yet initialized
	Ready,    // Task ready to run or running
	Running,  // Task currently executing
	Exited,   // Task has terminated
} TaskStatus;

/**
 * TaskInfo - Task information structure for sys_task_info system call
 * 
 * This structure is filled by the kernel and returned to user programs
 * when they call sys_task_info.
 */
typedef struct {
	TaskStatus status;  // Current task state
	
	// Array of syscall counters - one counter per syscall number
	// syscall_times[N] = number of times syscall N was called
	unsigned int syscall_times[MAX_SYSCALL_NUM];
	
	// Total running time in milliseconds
	// This is calculated from CPU cycles when sys_task_info is called
	int time;
} TaskInfo;

/**
 * struct proc - Process Control Block (PCB)
 * 
 * Contains all information needed to manage a process.
 * One proc structure exists for each potential process in the system.
 */
struct proc {
	enum procstate state;  // Current state in process lifecycle
	int pid;               // Process ID (unique identifier)
	
	// Memory management
	uint64 ustack;         // Virtual address of user stack
	uint64 kstack;         // Virtual address of kernel stack
	
	// CPU state management
	struct trapframe *trapframe;  // Saved user-mode CPU registers
	struct context context;       // Saved kernel-mode CPU registers
	
	/*
	 * LAB1: Task information for sys_task_info system call
	 * Points to this process's TaskInfo structure in task_info_pool
	 */
	TaskInfo *info;
};

// Function declarations
struct proc *curr_proc();                           // Get current process
void exit(int);                                     // Terminate current process
void proc_init();                                   // Initialize process subsystem
void scheduler() __attribute__((noreturn));         // Main scheduler loop
void sched();                                       // Switch to scheduler
void yield();                                       // Voluntarily give up CPU
struct proc *allocproc();                           // Allocate a new process
void swtch(struct context *, struct context *);     // Low-level context switch (in swtch.S)

#endif // PROC_H