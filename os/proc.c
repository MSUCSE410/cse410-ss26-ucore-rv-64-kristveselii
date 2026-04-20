#include "proc.h"
#include "defs.h"
#include "loader.h"
#include "trap.h"
#include "vm.h"
#include "queue.h"

struct proc pool[NPROC];
__attribute__((aligned(16))) char kstack[NPROC][PAGE_SIZE];
__attribute__((aligned(4096))) char trapframe[NPROC][TRAP_PAGE_SIZE];

extern char boot_stack_top[];
struct proc *current_proc;
struct proc idle;
struct queue task_queue; // kept for compatibility; not used by stride scheduler

int threadid()
{
	return curr_proc()->pid;
}

struct proc *curr_proc()
{
	return current_proc;
}

// initialize the proc table at boot time.
void proc_init()
{
	struct proc *p;
	for (p = pool; p < &pool[NPROC]; p++) {
		p->state = UNUSED;
		p->kstack = (uint64)kstack[p - pool];
		p->trapframe = (struct trapframe *)trapframe[p - pool];

		// PROJECT 3, STEP 7: Zero syscall counters and start_time at boot.
		// Ensures no stale data from a previous kernel run can bleed into
		// a newly allocated process slot.
		memset(p->syscall_times, 0, sizeof(p->syscall_times)); // Reset all syscall counters to 0
		p->start_time = 0; // records the first scheduled time (in cycles)
	}
	idle.kstack = (uint64)boot_stack_top;
	idle.pid = IDLE_PID;
	current_proc = &idle;
	init_queue(&task_queue);
}

int allocpid()
{
	static int PID = 1;
	return PID++;
}

// fetch_task() — kept for compatibility but no longer called by the scheduler.
// The stride scheduler scans the pool directly to find the minimum-stride
// RUNNABLE process, so popping from the FIFO queue is not needed.
struct proc *fetch_task()
{
	int index = pop_queue(&task_queue);
	if (index < 0) {
		debugf("No task to fetch\n");
		return NULL;
	}
	debugf("fetch task %d(pid=%d) to task queue\n", index, pool[index].pid);
	return pool + index;
}

// add_task() — kept because load_init_app() still calls it.
// With stride scheduling the scheduler finds RUNNABLE processes by scanning
// the pool, so pushing to the queue here has no effect on scheduling order.
void add_task(struct proc *p)
{
	push_queue(&task_queue, p - pool);
	debugf("add task %d(pid=%d) to task queue\n", p - pool, p->pid);
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel.
// If there are no free procs, or a memory allocation fails, return 0.
struct proc *allocproc()
{
	struct proc *p;
	for (p = pool; p < &pool[NPROC]; p++) {
		if (p->state == UNUSED) {
			goto found;
		}
	}
	return 0;

found:
	// init proc
	p->pid = allocpid();
	p->state = USED;
	p->ustack = 0;
	p->max_page = 0;
	p->parent = NULL;
	p->exit_code = 0;
	p->pagetable = uvmcreate((uint64)p->trapframe);
	memset(&p->context, 0, sizeof(p->context));
	memset((void *)p->kstack, 0, KSTACK_SIZE);
	memset((void *)p->trapframe, 0, TRAP_PAGE_SIZE);
	p->context.ra = (uint64)usertrapret;
	p->context.sp = p->kstack + KSTACK_SIZE;

	// PROJECT 3, STEP 8: Initialize stride scheduling fields.
	// priority = 16: default gives equal share with all other default processes.
	// stride = 0: spec requires all processes start at zero so the first round
	//             of scheduling is effectively fair (all tied at the minimum).
	p->priority = 16; // default priority
	p->stride = 0; // default stride, starts at 0 
	return p;
}

// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
//
// PROJECT 3, STEP 9: Replaced round-robin with stride scheduling.
//
// ALGORITHM:
//   The stride algorithm allocates CPU time proportional to each process's
//   priority by tracking a "stride" value per process:
//     - Each scheduling round, pick the RUNNABLE process with the smallest stride.
//     - After selecting process P: P.stride += BIG_STRIDE / P.priority
//     - A high-priority process has a large divisor so its stride grows slowly,
//       causing it to be selected again sooner than a low-priority process.
//
// WHY A LINEAR SCAN INSTEAD OF THE QUEUE:
//   Round-robin used a FIFO queue (O(1) pop). Stride scheduling must find the
//   global minimum stride across all RUNNABLE processes, which requires a scan.
//   The pool is bounded at 512 entries so the O(n) scan is acceptable.
void scheduler()
{
	struct proc *p;
	for (;;) {
		// stride scheduling: picking the process with the smallest stride
		struct proc *best = 0;
		//*int has_proc = 0;
		for (p = pool; p < &pool[NPROC]; p++) {
			if (p->state == RUNNABLE) {
				
				// PROJECT 3, STEP 10: Record the first-schedule timestamp.
				// r_time() reads the RISC-V hardware timer (mtime).
				// We set start_time exactly once — on the very first scheduling
				// of this process — so sys_task_info can compute elapsed time.
				if (p->start_time == 0) {
					p->start_time = r_time(); // record the start time of the process
				}

				// Find process with smallest stride value.
				// When strides are equal the first found wins (arbitrary but fair).
				if (best == 0 || p->stride < best->stride) {
					best = p;
				}
			}
		}

		if (best == 0) {
			panic("all app are over!\n");
		}

		// PROJECT 3, STEP 11: Increment stride BEFORE running the process.
		// stride += BIG_STRIDE / priority
		// The process "pays" for its upcoming time slice up front.
		// Next round another process with a smaller accumulated stride will
		// likely be chosen, producing the proportional CPU-sharing effect.
		// High priority -> smaller increment -> runs more often
		best->stride += BIG_STRIDE / best->priority; // update stride
		tracef("swtich to proc %d", best - pool);
		best->state = RUNNING;
		current_proc = best;
		swtch(&idle.context, &best->context);
		// Execution resumes here after the process yields, blocks, or exits.
		
		/*
				has_proc = 1;
				tracef("swtich to proc %d", p - pool);
				p->state = RUNNING;
				current_proc = p;
				swtch(&idle.context, &p->context);
			}
		}
		if(has_proc == 0) {
			panic("all app are over!\n");
		}
		p = fetch_task();
		if (p == NULL) {
			panic("all app are over!\n");
		}
		tracef("swtich to proc %d", p - pool);
		p->state = RUNNING;
		current_proc = p;
		swtch(&idle.context, &p->context);*/
	}
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void sched()
{
	struct proc *p = curr_proc();
	if (p->state == RUNNING)
		panic("sched running");
	swtch(&p->context, &idle.context);
}

// Give up the CPU for one scheduling round.
void yield()
{
	// Mark current process as runnable and give up CPU
	current_proc->state = RUNNABLE;
	// PROJECT 3, STEP 12: add_task() removed from yield().
	// The old round-robin scheduler needed the process pushed back onto the
	// FIFO queue so it would be found again. The stride scheduler finds all
	// RUNNABLE processes by scanning the pool directly, so setting
	// state = RUNNABLE is sufficient — no queue push needed.
	//add_task(current_proc); // removed because stride scheduling doesn't use a queue
	sched();
}

// Free a process's page table, and free the
// physical memory it refers to.
void freepagetable(pagetable_t pagetable, uint64 max_page)
{
	uvmunmap(pagetable, TRAMPOLINE, 1, 0);
	uvmunmap(pagetable, TRAPFRAME, 1, 0);
	uvmfree(pagetable, max_page);
}

void freeproc(struct proc *p)
{
	if (p->pagetable)
		freepagetable(p->pagetable, p->max_page);
	p->pagetable = 0;
	p->state = UNUSED;
}

int fork()
{
	struct proc *np;
	struct proc *p = curr_proc();
	// Allocate process.
	if ((np = allocproc()) == 0) {
		panic("allocproc\n");
	}
	// Copy user memory from parent to child.
	if (uvmcopy(p->pagetable, np->pagetable, p->max_page) < 0) {
		panic("uvmcopy\n");
	}
	np->max_page = p->max_page;
	// copy saved user registers.
	*(np->trapframe) = *(p->trapframe);
	// Cause fork to return 0 in the child.
	np->trapframe->a0 = 0;
	np->parent = p;
	// PROJECT 3, STEP 13: Set child RUNNABLE without add_task().
	// The stride scheduler discovers new RUNNABLE processes by scanning the pool.
	// The child inherits default priority (16) and stride (0) from allocproc(),
	// placing it at the bottom of the stride order for a fair first round.
	np->state = RUNNABLE; // child is ready to run (no queue needed for stride scheduling)
	//add_task(np);
	return np->pid;
}

int exec(char *name)
{
	int id = get_id_by_name(name);
	if (id < 0)
		return -1;
	struct proc *p = curr_proc();
	uvmunmap(p->pagetable, 0, p->max_page, 1);
	p->max_page = 0;
	loader(id, p);
	return 0;
}

int wait(int pid, int *code)
{
	struct proc *np;
	int havekids;
	struct proc *p = curr_proc();

	for (;;) {
		// Scan through table looking for exited children.
		havekids = 0;
		for (np = pool; np < &pool[NPROC]; np++) {
			if (np->state != UNUSED && np->parent == p &&
			    (pid <= 0 || np->pid == pid)) {
				havekids = 1;
				if (np->state == ZOMBIE) {
					// Found one.
					np->state = UNUSED;
					pid = np->pid;
					*code = np->exit_code;
					return pid;
				}
			}
		}
		if (!havekids) {
			return -1;
		}

		// Put current proces back to runnable state and reschedule
		p->state = RUNNABLE;
		// PROJECT 3, STEP 14: add_task() removed from wait() for the same reason
		// as yield() — stride scheduler scans pool, no queue push needed.
		//add_task(p); // no queue needed for stride scheduling
		sched();
	}
}

// Exit the current process.
void exit(int code)
{
	struct proc *p = curr_proc();
	p->exit_code = code;
	debugf("proc %d exit with %d\n", p->pid, code);
	freeproc(p);
	if (p->parent != NULL) {
		// Parent should `wait`
		p->state = ZOMBIE;
	}
	// Set the `parent` of all children to NULL
	struct proc *np;
	for (np = pool; np < &pool[NPROC]; np++) {
		if (np->parent == p) {
			np->parent = NULL;
		}
	}
	sched();
}

// PROJECT 3, STEP 15: spawn() — create a child and load a program into it directly.
//
// spawn() is equivalent to fork() + exec() but avoids copying the parent's
// address space. Instead of duplicating the parent's pages (fork) and then
// immediately discarding them (exec), spawn() allocates a fresh process and
// loads the target binary straight into it.
//
// Steps:
//   1. get_id_by_name() — look up the program in the embedded app table.
//   2. allocproc()      — get a fresh proc slot with a new page table, PID,
//                         and default stride/priority fields.
//   3. Set np->parent   — so wait() / exit() parent-child logic works correctly.
//   4. loader()         — maps the binary and stack, sets epc/sp/max_page,
//                         and sets np->state = RUNNABLE.
//   5. Return np->pid   — the child's PID is returned to the parent via a0.
//
// On any failure return -1 so the user program can detect the error.
int spawn(char *filename)
{
	int id = get_id_by_name(filename);
	if (id < 0){
		return -1; // invalid program name
	}
	struct proc *parent = curr_proc();
	struct proc *np = allocproc();
	if (np == NULL){
		return -1;
	}

	np->parent = parent; // set parent-child relationship

	// load program into new process
	if (loader(id, np) < 0){
		np->state = UNUSED;
		return -1;
	}

	return np->pid; // return child PID
}