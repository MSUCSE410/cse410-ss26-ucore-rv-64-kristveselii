#ifndef PROC_H
#define PROC_H

#include "riscv.h"
#include "types.h"
#include "queue.h"

#define NPROC (512)
#define FD_BUFFER_SIZE (16)

// PROJECT 3, STEP 1: Maximum syscall ID we track per process.
// syscall_times[] is indexed by syscall ID, so it must be large enough
// to cover every ID dispatched in syscall.c (highest used is ~410).
#define MAX_SYSCALL_NUM 500

// PROJECT 3, STEP 2: Constant for the stride scheduling algorithm.
// Each time a process is selected to run its stride is incremented by:
//     stride += BIG_STRIDE / priority
// Higher priority → smaller increment → stride grows slowly → selected more often.
// 65536 (2^16) gives good resolution across priority range [2, isize_max]
// without overflowing a 32-bit int during normal operation.
#define BIG_STRIDE 65536 // to reduce integer division errors


struct file;

// Saved registers for kernel context switches.
struct context {
	uint64 ra;
	uint64 sp;

	// callee-saved
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

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
struct proc {
	enum procstate state; // Process state
	int pid; // Process ID
	pagetable_t pagetable; // User page table
	uint64 ustack; // Virtual address of kernel stack
	uint64 kstack; // Virtual address of kernel stack
	struct trapframe *trapframe; // data page for trampoline.S
	struct context context; // swtch() here to run process
	uint64 max_page; // highest virtual page number allocated (set by bin_loader)

	// PROJECT 3, STEP 3: Per-process syscall tracking.
	// syscall_times[i] is incremented in syscall() every time syscall i fires.
	// Reset to zero in proc_init() and bin_loader() so each new program starts clean.
	unsigned int syscall_times[MAX_SYSCALL_NUM]; // record the times of each system call

	// start_time holds the CPU cycle count when this process was first scheduled.
	// sys_task_info computes elapsed ms as: (get_cycle() - start_time) * 1000 / CPU_FREQ
	uint64 start_time; // record the start time of the process	

	struct proc *parent; // Parent process (NULL if orphan or top-level init)
	uint64 exit_code;    // Passed to the waiting parent through wait()
	struct file *files[FD_BUFFER_SIZE];

	// PROJECT 3, STEP 4: Stride scheduling fields.
	// priority: controlled by sys_set_priority(), default 16, minimum 2.
	//   A higher value means more CPU time (smaller stride increment per round).
	// stride: accumulates each time this process is chosen by the scheduler.
	//   stride += BIG_STRIDE / priority on every selection.
	//   The scheduler always picks the RUNNABLE process with the smallest stride,
	//   so processes with high priority stay near the bottom and get chosen often.
	int priority; // priority of the process, higher means more CPU time
	int stride; // tracks how much CPU time the process has used
};

// PROJECT 3, STEP 5: TaskStatus and TaskInfo for sys_task_info.
// These mirror the user-space struct definitions so the kernel can populate
// a TaskInfo and copy it back to the calling user process.
typedef enum{
	UnInit,
	Ready,
	Running,
	Exited,
} TaskStatus;

typedef struct{
	TaskStatus status; // current task state
	// Count how many times this process has invoked each syscall.
	unsigned int syscall_times[MAX_SYSCALL_NUM];
	int time; // running time in ms
} TaskInfo;

int cpuid();
struct proc *curr_proc();
void exit(int);
void proc_init();
void scheduler() __attribute__((noreturn));
void sched();
void yield();
int fork();
int exec(char *);
int wait(int, int *);
void add_task(struct proc *);
struct proc *pop_task();
struct proc *allocproc();
int fdalloc(struct file *);
// swtch.S
void swtch(struct context *, struct context *);

// PROJECT 3, STEP 6: spawn() — declared here, defined in proc.c.
// Creates a child process and loads the named program directly into it.
// Equivalent to fork() + exec() but without copying the parent's address space.
// Called by sys_spawn() in syscall.c after copying the filename from user memory.
int spawn(char *filename);

#endif // PROC_H