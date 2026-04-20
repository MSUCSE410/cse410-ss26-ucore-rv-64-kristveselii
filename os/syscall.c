#include "console.h"
#include "defs.h"
#include "loader.h"
#include "sync.h"
#include "syscall.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

uint64 console_write(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	tracef("write size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return len;
}

uint64 console_read(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	tracef("read size = %d", len);
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
}

uint64 sys_write(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_write(va, len);
	case FD_PIPE:
		return pipewrite(f->pipe, va, len);
	case FD_INODE:
		return inodewrite(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_read(va, len);
	case FD_PIPE:
		return piperead(f->pipe, va, len);
	case FD_INODE:
		return inoderead(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

__attribute__((noreturn)) void sys_exit(int code)
{
	exit(code);
	__builtin_unreachable();
}

uint64 sys_sched_yield()
{
	yield();
	return 0;
}

uint64 sys_gettimeofday(uint64 val, int _tz)
{
	struct proc *p = curr_proc();
	uint64 cycle = get_cycle();
	TimeVal t;
	t.sec = cycle / CPU_FREQ;
	t.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	copyout(p->pagetable, val, (char *)&t, sizeof(TimeVal));
	return 0;
}

uint64 sys_getpid()
{
	return curr_proc()->pid;
}

uint64 sys_getppid()
{
	struct proc *p = curr_proc();
	return p->parent == NULL ? IDLE_PID : p->parent->pid;
}

uint64 sys_clone()
{
	debugf("fork!");
	return fork();
}

static inline uint64 fetchaddr(pagetable_t pagetable, uint64 va)
{
	uint64 *addr = (uint64 *)useraddr(pagetable, va);
	return *addr;
}

uint64 sys_exec(uint64 path, uint64 uargv)
{
	struct proc *p = curr_proc();
	char name[MAX_STR_LEN];
	copyinstr(p->pagetable, name, path, MAX_STR_LEN);
	uint64 arg;
	static char strpool[MAX_ARG_NUM][MAX_STR_LEN];
	char *argv[MAX_ARG_NUM];
	int i;
	for (i = 0; uargv && (arg = fetchaddr(p->pagetable, uargv));
	     uargv += sizeof(char *), i++) {
		copyinstr(p->pagetable, (char *)strpool[i], arg, MAX_STR_LEN);
		argv[i] = (char *)strpool[i];
	}
	argv[i] = NULL;
	return exec(name, (char **)argv);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

uint64 sys_pipe(uint64 fdarray)
{
	struct proc *p = curr_proc();
	uint64 fd0, fd1;
	struct file *f0, *f1;
	if (f0 < 0 || f1 < 0) {
		return -1;
	}
	f0 = filealloc();
	f1 = filealloc();
	if (pipealloc(f0, f1) < 0)
		goto err0;
	fd0 = fdalloc(f0);
	fd1 = fdalloc(f1);
	if (fd0 < 0 || fd1 < 0)
		goto err0;
	if (copyout(p->pagetable, fdarray, (char *)&fd0, sizeof(fd0)) < 0 ||
	    copyout(p->pagetable, fdarray + sizeof(uint64), (char *)&fd1,
		    sizeof(fd1)) < 0) {
		goto err1;
	}
	return 0;

err1:
	p->files[fd0] = 0;
	p->files[fd1] = 0;
err0:
	fileclose(f0);
	fileclose(f1);
	return -1;
}

uint64 sys_openat(uint64 va, uint64 omode, uint64 _flags)
{
	struct proc *p = curr_proc();
	char path[200];
	copyinstr(p->pagetable, path, va, 200);
	return fileopen(path, omode);
}

uint64 sys_close(int fd)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d", fd);
		return -1;
	}
	fileclose(f);
	p->files[fd] = 0;
	return 0;
}

int sys_thread_create(uint64 entry, uint64 arg)
{
	struct proc *p = curr_proc();
	int tid = allocthread(p, entry, 1);
	if (tid < 0) {
		errorf("fail to create thread");
		return -1;
	}
	struct thread *t = &p->threads[tid];
	t->trapframe->a0 = arg;
	t->state = RUNNABLE;
	add_task(t);
	return tid;
}

int sys_gettid()
{
	return curr_thread()->tid;
}

int sys_waittid(int tid)
{
	if (tid < 0 || tid >= NTHREAD) {
		errorf("unexpected tid %d", tid);
		return -1;
	}
	struct thread *t = &curr_proc()->threads[tid];
	if (t->state == T_UNUSED || tid == curr_thread()->tid) {
		return -1;
	}
	if (t->state != EXITED) {
		return -2;
	}
	memset((void *)t->kstack, 7, KSTACK_SIZE);
	t->tid = -1;
	t->state = T_UNUSED;
	return t->exit_code;
}

/*
 * Project 5: Deadlock Detection Algorithm (Step 3)
 *
 * Implements the Banker's Algorithm (work-finish method) to detect whether
 * a deadlock currently exists among the threads of the calling process.
 *
 * Parameters:
 *   available[LOCK_POOL_SIZE]              - units of each resource currently free
 *   allocation[NTHREAD][LOCK_POOL_SIZE]    - units of each resource held by each thread
 *   request[NTHREAD][LOCK_POOL_SIZE]       - units of each resource each thread is waiting for
 *
 * Returns 1 if a deadlock is detected, 0 if the system is in a safe state.
 *
 * The same function is reused for both mutex and semaphore detection because
 * the algorithm is identical — only the semantics of the matrices differ:
 *   - For mutexes:    values are binary (0 or 1)
 *   - For semaphores: values can be > 1 (resource counts)
 *
 * Algorithm:
 *   1. Copy available[] into work[] (simulates available resources)
 *   2. Mark all threads as unfinished
 *   3. Repeatedly find any unfinished thread whose request <= work
 *      (meaning it could run to completion with current resources)
 *   4. When such a thread is found, simulate it finishing: add its
 *      allocation back to work[], mark it finished
 *   5. Repeat until no more threads can be found
 *   6. If any thread is still unfinished, those threads are deadlocked
 */
static int deadlock_detect(const int available[LOCK_POOL_SIZE],
			   const int allocation[NTHREAD][LOCK_POOL_SIZE],
			   const int request[NTHREAD][LOCK_POOL_SIZE])
{
	// work[] simulates the pool of currently available resources.
	// It grows as we simulate threads finishing and releasing their allocations.
	int work[LOCK_POOL_SIZE];

	// finish[i] = 1 means thread i can complete (safely finish) given work[].
	// Starts at 0 (unfinished) for all threads.
	int finish[NTHREAD];

	// Initialize work to the current available resources.
	for (int j = 0; j < LOCK_POOL_SIZE; j++) {
		work[j] = available[j];
	}

	// All threads start as unfinished.
	for (int i = 0; i < NTHREAD; i++) {
		finish[i] = 0;
	}

	// changed tracks whether we made progress in this pass.
	// If a full pass finds no thread that can finish, we're stuck — deadlock.
	int changed = 1;

	while (changed) {
		changed = 0;

		for (int i = 0; i < NTHREAD; i++) {
			if (finish[i]) continue; // already determined this thread can finish

			// Check if thread i's request can be satisfied by current work[].
			// If request[i][j] > work[j] for any resource j, thread i is blocked.
			int can_finish = 1;
			for (int j = 0; j < LOCK_POOL_SIZE; j++) {
				if (request[i][j] > work[j]) {
					can_finish = 0;
					break;
				}
			}

			// If thread i can finish, simulate it completing:
			// release all its held resources back into work[], mark it done.
			if (can_finish) {
				for (int j = 0; j < LOCK_POOL_SIZE; j++) {
					work[j] += allocation[i][j];
				}
				finish[i] = 1;
				changed = 1; // made progress, try another pass
			}
		}
	}

	// If any thread could not finish, those threads are in a deadlock cycle.
	for (int i = 0; i < NTHREAD; i++) {
		if (!finish[i]) {
			return 1; // deadlock detected
		}
	}
	return 0; // all threads can finish — safe state
}

/*
 * Project 5: Mutex syscalls (Step 4-1)
 *
 * sys_mutex_create, sys_mutex_lock, and sys_mutex_unlock each maintain
 * the three deadlock detection matrices (available, allocation, request)
 * so that deadlock_detect() always has an accurate view of resource state.
 */

// sys_mutex_create: allocate a new mutex and initialize its detection state.
// Returns the mutex ID (its index in mutex_pool[]), or -1 on failure.
int sys_mutex_create(int blocking)
{
	struct proc *p = curr_proc();
	struct mutex *m = mutex_create(blocking);

	if (m == NULL) {
		errorf("fail to create mutex: out of resource");
		return -1;
	}

	/*
	 * Project 5: Initialize detection state for this new mutex (Step 4-1)
	 *
	 * The mutex ID is derived from its position in the per-process mutex_pool[].
	 * A freshly created mutex is unlocked, so available[mutex_id] = 1.
	 * No thread holds or is waiting for it yet, so allocation and request
	 * rows for this mutex are zeroed across all threads.
	 */
	int mutex_id = m - p->mutex_pool;

	// New mutex is unlocked — one unit available.
	p->mutex_available[mutex_id] = 1;

	// No thread holds or is requesting this mutex yet.
	for (int i = 0; i < NTHREAD; i++) {
		p->mutex_allocation[i][mutex_id] = 0;
		p->mutex_request[i][mutex_id] = 0;
	}

	debugf("create mutex %d", mutex_id);
	return mutex_id;
}

// sys_mutex_lock: acquire a mutex, with deadlock detection if enabled.
// Returns 0 on success, -1 on invalid id, or -0xDEAD if deadlock detected.
int sys_mutex_lock(int mutex_id)
{
	struct proc *p = curr_proc();
	struct thread *t = curr_thread();
	int tid = t->tid;

	if (mutex_id < 0 || mutex_id >= curr_proc()->next_mutex_id) {
		errorf("Unexpected mutex id %d", mutex_id);
		return -1;
	}

	/*
	 * Project 5: Deadlock detection before blocking (Step 4-1)
	 *
	 * Before actually calling mutex_lock() (which may block), we first
	 * record this thread's intent to acquire the mutex in request[][],
	 * then run the detection algorithm on the updated state.
	 *
	 * If deadlock_detect() returns 1, granting this request would lead to
	 * a deadlock. We reject the request immediately by resetting request[][]
	 * and returning -0xDEAD, allowing user space to handle the situation.
	 *
	 * If detection passes (safe state), we proceed to actually acquire the
	 * lock, then update allocation[][] and available[] to reflect ownership.
	 */

	// Record that this thread is requesting the mutex.
	p->mutex_request[tid][mutex_id] = 1;

	// Run detection only if the user enabled it for this process.
	if (p->deadlock_detect_enabled &&
	    deadlock_detect(p->mutex_available,
	                    p->mutex_allocation,
	                    p->mutex_request)) {
		// Deadlock would occur — undo the request and reject.
		p->mutex_request[tid][mutex_id] = 0;
		errorf("deadlock detected on mutex %d", mutex_id);
		return -0xDEAD;
	}

	// Safe to proceed — actually acquire the lock (may block here).
	mutex_lock(&p->mutex_pool[mutex_id]);

	// Lock acquired: clear the request, mark the allocation, mark unavailable.
	p->mutex_request[tid][mutex_id] = 0;
	p->mutex_available[mutex_id]--;
	p->mutex_allocation[tid][mutex_id]++;
	return 0;
}

// sys_mutex_unlock: release a mutex and update detection state.
// Returns 0 on success, -1 on invalid id.
int sys_mutex_unlock(int mutex_id)
{
	struct proc *p = curr_proc();
	struct thread *t = curr_thread();
	int tid = t->tid;

	if (mutex_id < 0 || mutex_id >= p->next_mutex_id) {
		errorf("Unexpected mutex id %d", mutex_id);
		return -1;
	}

	/*
	 * Project 5: Update detection state on unlock (Step 4-1)
	 *
	 * When a thread releases a mutex, we decrement its allocation count
	 * and increment available[] so the detection algorithm knows this
	 * resource is free again. The guard (> 0) prevents underflow in
	 * case of unbalanced unlock calls.
	 */
	if (p->mutex_allocation[tid][mutex_id] > 0) {
		p->mutex_allocation[tid][mutex_id]--;
		p->mutex_available[mutex_id]++;
	}

	// Actually release the lock.
	mutex_unlock(&p->mutex_pool[mutex_id]);
	return 0;
}

/*
 * Project 5: Semaphore syscalls (Step 4-2)
 *
 * sys_semaphore_create, sys_semaphore_up, and sys_semaphore_down each
 * maintain the semaphore detection matrices in parallel with the mutex
 * matrices above. The logic mirrors the mutex implementation, but uses
 * resource counts instead of binary values.
 */

// sys_semaphore_create: allocate a semaphore with res_count initial resources.
// Returns the semaphore ID, or -1 on failure.
int sys_semaphore_create(int res_count)
{
	struct proc *p = curr_proc();
	struct semaphore *s = semaphore_create(res_count);

	if (s == NULL) {
		errorf("fail to create semaphore: out of resource");
		return -1;
	}

	/*
	 * Project 5: Initialize detection state for this semaphore (Step 4-2)
	 *
	 * Unlike mutexes (binary), semaphores start with res_count free units.
	 * available[sem_id] = res_count reflects however many threads can
	 * simultaneously acquire this semaphore before any block.
	 */
	int sem_id = s - p->semaphore_pool;

	// Initialize available to the semaphore's initial resource count.
	p->semaphore_available[sem_id] = res_count;

	// No thread holds or is requesting this semaphore yet.
	for (int i = 0; i < NTHREAD; i++) {
		p->semaphore_allocation[i][sem_id] = 0;
		p->semaphore_request[i][sem_id] = 0;
	}

	debugf("create semaphore %d", sem_id);
	return sem_id;
}

// sys_semaphore_up: release one unit of a semaphore (signal/V operation).
// Returns 0 on success, -1 on invalid id.
int sys_semaphore_up(int semaphore_id)
{
	struct proc *p = curr_proc();
	struct thread *t = curr_thread();
	int tid = t->tid;

	if (semaphore_id < 0 || semaphore_id >= p->next_semaphore_id) {
		errorf("Unexpected semaphore id %d", semaphore_id);
		return -1;
	}

	/*
	 * Project 5: Update detection state on semaphore release (Step 4-2)
	 *
	 * When a thread signals (up) a semaphore, it releases one resource unit.
	 * We decrement allocation[][] and increment available[] so the detection
	 * algorithm sees the resource as freed. The guard (> 0) prevents
	 * underflow for producer/consumer patterns where the signaling thread
	 * may not have called semaphore_down() itself.
	 */
	if (p->semaphore_allocation[tid][semaphore_id] > 0) {
		p->semaphore_allocation[tid][semaphore_id]--;
		p->semaphore_available[semaphore_id]++;
	}

	// Actually release one resource unit.
	semaphore_up(&p->semaphore_pool[semaphore_id]);
	return 0;
}

// sys_semaphore_down: acquire one unit of a semaphore (wait/P operation),
// with deadlock detection if enabled.
// Returns 0 on success, -1 on invalid id, or -0xDEAD if deadlock detected.
int sys_semaphore_down(int semaphore_id)
{
	struct proc *p = curr_proc();
	struct thread *t = curr_thread();
	int tid = t->tid;

	if (semaphore_id < 0 || semaphore_id >= p->next_semaphore_id) {
		errorf("Unexpected semaphore id %d", semaphore_id);
		return -1;
	}

	/*
	 * Project 5: Deadlock detection before blocking (Step 4-2)
	 *
	 * Same pattern as sys_mutex_lock: record the request, run detection,
	 * reject if deadlock is predicted, otherwise proceed to acquire.
	 *
	 * After successfully acquiring, update allocation[][] and available[]
	 * to reflect that this thread now holds one more unit of this semaphore.
	 */

	// Record that this thread is requesting one unit of this semaphore.
	p->semaphore_request[tid][semaphore_id] = 1;

	// Run detection only if the user enabled it for this process.
	if (p->deadlock_detect_enabled &&
	    deadlock_detect(p->semaphore_available,
	                    p->semaphore_allocation,
	                    p->semaphore_request)) {
		// Deadlock would occur — undo the request and reject.
		p->semaphore_request[tid][semaphore_id] = 0;
		errorf("deadlock detected on semaphore %d", semaphore_id);
		return -0xDEAD;
	}

	// Safe to proceed — actually acquire one resource unit (may block here).
	semaphore_down(&p->semaphore_pool[semaphore_id]);

	// Acquired: clear the request, update allocation and available.
	p->semaphore_request[tid][semaphore_id] = 0;
	p->semaphore_available[semaphore_id]--;
	p->semaphore_allocation[tid][semaphore_id]++;

	return 0;
}

int sys_condvar_create()
{
	struct condvar *c = condvar_create();
	if (c == NULL) {
		errorf("fail to create condvar: out of resource");
		return -1;
	}
	int cond_id = c - curr_proc()->condvar_pool;
	debugf("create condvar %d", cond_id);
	return cond_id;
}

int sys_condvar_signal(int cond_id)
{
	if (cond_id < 0 || cond_id >= curr_proc()->next_condvar_id) {
		errorf("Unexpected condvar id %d", cond_id);
		return -1;
	}
	cond_signal(&curr_proc()->condvar_pool[cond_id]);
	return 0;
}

int sys_condvar_wait(int cond_id, int mutex_id)
{
	if (cond_id < 0 || cond_id >= curr_proc()->next_condvar_id) {
		errorf("Unexpected condvar id %d", cond_id);
		return -1;
	}
	if (mutex_id < 0 || mutex_id >= curr_proc()->next_mutex_id) {
		errorf("Unexpected mutex id %d", mutex_id);
		return -1;
	}
	cond_wait(&curr_proc()->condvar_pool[cond_id],
		  &curr_proc()->mutex_pool[mutex_id]);
	return 0;
}

/*
 * Project 5: sys_enable_deadlock_detect (Step 2)
 *
 * This syscall lets user programs opt into deadlock detection at runtime.
 * Passing enabled=1 activates the Banker's Algorithm checks in
 * sys_mutex_lock() and sys_semaphore_down(). Passing enabled=0 disables
 * them, allowing lock operations to proceed without detection overhead.
 *
 * The flag is stored per-process in proc->deadlock_detect_enabled,
 * initialized to 0 in allocproc(). User programs call this before
 * creating threads and locks to ensure detection is active from the start.
 */
int sys_enable_deadlock_detect(int enabled)
{
	if (enabled != 0 && enabled != 1) {
		errorf("invalid deadlock detect flag %d", enabled);
		return -1;
	}
	curr_proc()->deadlock_detect_enabled = enabled;
	return 0;
}

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_thread()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	if (id != SYS_write && id != SYS_read && id != SYS_sched_yield) {
		debugf("syscall %d args = [%x, %x, %x, %x, %x, %x]", id,
		       args[0], args[1], args[2], args[3], args[4], args[5]);
	}
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_openat:
		ret = sys_openat(args[0], args[1], args[2]);
		break;
	case SYS_close:
		ret = sys_close(args[0]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday(args[0], args[1]);
		break;
	case SYS_getpid:
		ret = sys_getpid();
		break;
	case SYS_getppid:
		ret = sys_getppid();
		break;
	case SYS_clone:
		ret = sys_clone();
		break;
	case SYS_execve:
		ret = sys_exec(args[0], args[1]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_pipe2:
		ret = sys_pipe(args[0]);
		break;
	case SYS_thread_create:
		ret = sys_thread_create(args[0], args[1]);
		break;
	case SYS_gettid:
		ret = sys_gettid();
		break;
	case SYS_waittid:
		ret = sys_waittid(args[0]);
		break;
	case SYS_mutex_create:
		ret = sys_mutex_create(args[0]);
		break;
	case SYS_mutex_lock:
		ret = sys_mutex_lock(args[0]);
		break;
	case SYS_mutex_unlock:
		ret = sys_mutex_unlock(args[0]);
		break;
	case SYS_semaphore_create:
		ret = sys_semaphore_create(args[0]);
		break;
	case SYS_semaphore_up:
		ret = sys_semaphore_up(args[0]);
		break;
	case SYS_semaphore_down:
		ret = sys_semaphore_down(args[0]);
		break;
	case SYS_condvar_create:
		ret = sys_condvar_create();
		break;
	case SYS_condvar_signal:
		ret = sys_condvar_signal(args[0]);
		break;
	case SYS_condvar_wait:
		ret = sys_condvar_wait(args[0], args[1]);
		break;
	/*
	 * Project 5: SYS_enable_deadlock_detect dispatch (Step 2)
	 *
	 * Routes the enable_deadlock_detect syscall to its handler.
	 * args[0] is the enabled flag (0 or 1), passed from the user
	 * via register a0, snapshotted into args[] from the trapframe.
	 */
	case SYS_enable_deadlock_detect:
		ret = sys_enable_deadlock_detect(args[0]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	curr_thread()->trapframe->a0 = ret;
	if (id != SYS_write && id != SYS_read && id != SYS_sched_yield) {
		debugf("syscall %d ret %d", id, ret);
	}
}