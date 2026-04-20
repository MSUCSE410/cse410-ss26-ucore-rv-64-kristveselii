#ifndef PROC_H
#define PROC_H

#include "riscv.h"
#include "types.h"
#include "sync.h"

#define NPROC (128)
#define NTHREAD (16)
#define FD_BUFFER_SIZE (16)
#define LOCK_POOL_SIZE (8)

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

enum threadstate { T_UNUSED, T_USED, SLEEPING, RUNNABLE, RUNNING, EXITED };
struct thread {
	enum threadstate state; // Thread state
	int tid; // Thread ID
	struct proc *process;
	uint64 ustack; // Virtual address of user stack
	uint64 kstack; // Virtual address of kernel stack
	struct trapframe *trapframe; // data page for trampoline.S
	struct context context; // swtch() here to run process
	uint64 exit_code;
};

enum procstate { P_UNUSED, P_USED, ZOMBIE };

// Per-process state
struct proc {
	enum procstate state; // Process state
	int pid; // Process ID
	pagetable_t pagetable; // User page table
	uint64 max_page;
	uint64 ustack_base; // Virtual address of user stack base
	struct proc *parent; // Parent process
	uint64 exit_code;
	//File descriptor table, using to record the files opened by the process
	struct file *files[FD_BUFFER_SIZE];
	struct thread threads[NTHREAD];
	// Use dummy increasing id as index index of lock pool because we don't have destroy method yet
	uint next_mutex_id, next_semaphore_id, next_condvar_id;
	struct mutex mutex_pool[LOCK_POOL_SIZE];
	struct semaphore semaphore_pool[LOCK_POOL_SIZE];
	struct condvar condvar_pool[LOCK_POOL_SIZE];

	/*
	 * Project 5: Deadlock Detection State (Step 1)
	 *
	 * Each process maintains its own deadlock detection state independently.
	 * This is because locks/semaphores are per-process resources — threads
	 * within a process share the same mutex_pool and semaphore_pool, so
	 * deadlock detection only needs to consider threads within one process.
	 *
	 * The detection algorithm used is the Banker's Algorithm (resource
	 * allocation graph / work-finish method):
	 *   1. Initialize work[] = available[]
	 *   2. Find a thread i where request[i] <= work (can safely finish)
	 *   3. If found, simulate releasing its resources: work += allocation[i]
	 *   4. Repeat until no more threads can finish
	 *   5. If any thread is still unfinished, a deadlock exists
	 *
	 * Three matrices are tracked for both mutexes and semaphores:
	 *   - available[j]:         how many units of resource j are currently free
	 *   - allocation[i][j]:     how many units of resource j thread i currently holds
	 *   - request[i][j]:        how many units of resource j thread i is currently waiting for
	 *
	 * For mutexes: values are binary (0 or 1) since a mutex is either held or not.
	 * For semaphores: values can be > 1 since semaphores track a count of resources.
	 */

	// Flag to enable or disable deadlock detection for this process.
	// Set via sys_enable_deadlock_detect(). When 0, lock/semaphore
	// operations proceed without running the detection algorithm.
	int deadlock_detect_enabled;

	// --- Mutex deadlock detection matrices ---
	// available[j] = 1 if mutex j is currently unlocked (not held by any thread), 0 if locked.
	// Initialized to 1 when a mutex is created (it starts unlocked).
	int mutex_available[LOCK_POOL_SIZE];

	// allocation[i][j] = number of times thread i holds mutex j.
	// Incremented in sys_mutex_lock() after successfully acquiring the lock.
	// Decremented in sys_mutex_unlock() when the thread releases the lock.
	int mutex_allocation[NTHREAD][LOCK_POOL_SIZE];

	// request[i][j] = 1 if thread i is currently blocked waiting for mutex j, 0 otherwise.
	// Set to 1 before running deadlock detection in sys_mutex_lock().
	// Reset to 0 after either detecting a deadlock (returning early) or
	// successfully acquiring the lock.
	int mutex_request[NTHREAD][LOCK_POOL_SIZE];

	// --- Semaphore deadlock detection matrices ---
	// available[j] = current count of free resources for semaphore j.
	// Initialized to res_count when the semaphore is created.
	// Decremented on semaphore_down(), incremented on semaphore_up().
	int semaphore_available[LOCK_POOL_SIZE];

	// allocation[i][j] = number of semaphore j resources thread i currently holds.
	// Incremented in sys_semaphore_down() after successfully acquiring a resource.
	// Decremented in sys_semaphore_up() when the thread releases a resource.
	int semaphore_allocation[NTHREAD][LOCK_POOL_SIZE];

	// request[i][j] = 1 if thread i is currently trying to acquire semaphore j, 0 otherwise.
	// Set to 1 before running deadlock detection in sys_semaphore_down().
	// Reset to 0 after detecting a deadlock or successfully acquiring the resource.
	int semaphore_request[NTHREAD][LOCK_POOL_SIZE];
};

int cpuid();
struct proc *curr_proc();
struct thread *curr_thread(void);
void exit(int);
void proc_init();
void scheduler() __attribute__((noreturn));
void sched();
void yield();
int fork();
int exec(char *, char **);
int wait(int, int *);
void add_task(struct thread *);
struct thread *id_to_task(int);
int task_to_id(struct thread *);
struct thread *pop_task();
struct proc *allocproc();
int allocthread(struct proc *p, uint64 entry, int alloc_user_res);
uint64 get_thread_trapframe_va(int tid);
int fdalloc(struct file *);
int init_stdio(struct proc *);
int push_argv(struct proc *, char **);
// swtch.S
void swtch(struct context *, struct context *);

#endif // PROC_H