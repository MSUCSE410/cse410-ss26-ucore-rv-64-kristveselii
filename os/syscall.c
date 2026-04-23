#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

#include "proc.h"
#include "vm.h"

// PROJECT 3, STEP 16: sys_mmap() — map anonymous pages into user virtual memory.
//
// Implemented in Project 2, used unchanged in Project 3.
//
// Lets a user process request a range of virtual addresses backed by freshly
// allocated physical pages. Two-pass design:
//   Pass 1 — validate that none of the requested pages are already mapped.
//             Failing here avoids partial allocation before we know the range is free.
//   Pass 2 — allocate one physical page per virtual page and map it in.
//             On failure mid-way, already-mapped pages are cleaned up with uvmunmap().
//
// Permission bits (port argument):
//   bit 0 = read   → PTE_R
//   bit 1 = write  → PTE_W
//   bit 2 = execute→ PTE_X
//   PTE_U is always added so user-mode code can access the pages.
uint64 sys_mmap(uint64 start, uint64 len, int port, int flag, int fd)
{
	struct proc *p = curr_proc();
	if (!p) return -1;

	// A zero-length mapping is treated as a successful no-op.
	if (len == 0) return 0;

	// The start address must be page-aligned.
	if (start % PGSIZE != 0) return -1; // start must be page-aligned
	if (len > (1UL << 30)) return -1; // limit the maximum mapping size to 1GB
	if ((port & ~0x7) != 0) return -1; // prot must be a combination of PROT_READ, PROT_WRITE, PROT_EXEC
	if ((port & 0X7) == 0) return -1; // at least one of PROT_READ, PROT_WRITE, PROT_EXEC must be set

	// Round the requested length up to a whole number of pages.
	uint64 sz = PGROUNDUP(len);

	// Build the PTE permission flags
	// PTE_U is required so the mapped pages are accessible in user mode.
	int perm = PTE_U;
	if (port & 0x1) perm |= PTE_R;
	if (port & 0x2) perm |= PTE_W;
	if (port & 0x4) perm |= PTE_X;

	// first pass: make sure no page is already mapped
	for (uint64 va = start; va < start + sz; va += PGSIZE) {
		pte_t *pte = walk(p->pagetable, va, 0);
		if (pte && (*pte & PTE_V) != 0) {
			return -1; // page already mapped
		}
	}

	// second pass: allocate and map page by page
	for (uint64 va = start; va < start + sz; va += PGSIZE) {
		void *pa = kalloc();
		if (pa == 0) {
			uint64 done = (va - start) / PGSIZE;
			if (done > 0) {
				uvmunmap(p->pagetable, start, done, 1); // unmap and free already mapped pages
			}
			return -1; // allocation failed
		}
		
		// Zero-fill the new page so the anonymous mapping starts clean.
		memset(pa, 0, PGSIZE);
		
		// Map the new physical page into the process page table.
		if (mappages(p->pagetable, va, PGSIZE, (uint64)pa, perm) < 0) {
			kfree(pa);
			
			// Clean up pages already mapped before returning failure.
			uint64 done = (va - start) / PGSIZE;
			if (done > 0) {
				uvmunmap(p->pagetable, start, done, 1); // un
			}
			return -1; // mapping failed
		}
	}
	return 0;	

}
	

// PROJECT 3, STEP 17: sys_munmap() — remove a virtual memory mapping.
//
// Implemented in Project 2, used unchanged in Project 3.
//
// Removes a range of virtual-to-physical mappings and frees the physical pages.
// Two-pass design (same reasoning as sys_mmap):
//   Pass 1 — verify every page in the range IS currently mapped.
//             Fail fast before touching anything to avoid partial teardown.
//   Pass 2 — call uvmunmap(do_free=1) to clear PTEs and free physical pages.
uint64 sys_munmap(uint64 start, uint64 len)
{
	struct proc *p = curr_proc();
	if (!p) return -1;

	// A zero-length unmap is treated as a succesful no-op.
	if (len == 0) return 0;


	if (start % PGSIZE != 0) return -1; // start must be page-aligned

	// Round the length up so we unmap the full pages.
	uint64 sz = PGROUNDUP(len);
	uint64 npages = sz / PGSIZE;

	// verify the whole range is mapped first
	for (uint64 va = start; va < start + sz; va += PGSIZE) {
		pte_t *pte = walk(p->pagetable, va, 0);
		if (pte == 0 || (*pte & PTE_V) == 0) {
			return -1; // page not mapped
		}
	}

	// Remove the mappings and free the underlying physical pages.
	uvmunmap(p->pagetable, start, npages, 1); // unmap and free physical pages
	return 0;
}

uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d str = %x, len = %d", fd, va, len);
	if (fd != STDOUT)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	debugf("size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return size;
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	debugf("sys_read fd = %d str = %x, len = %d", fd, va, len);
	if (fd != STDIN)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
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
	struct proc *p = curr_proc(); // gets the currently running process
	if (!p || !val) return -1; // Return error if no process is running or the provided pointer is invalid

	// Translate the user virtual address into a kernel-usable physical address.
	uint64 dst = useraddr(p->pagetable, (uint64)val);
	if (dst == 0) return -1;


	uint64 cycle = get_cycle();
	TimeVal t;
	t.sec = cycle / CPU_FREQ;
	t.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	copyout(p->pagetable, val, (char *)&t, sizeof(TimeVal));

	// Write the result into user memory.
	*(TimeVal *)dst = t;


	return 0;
}

uint64 sys_getpid()
{
	return curr_proc()->pid;
}

// Forward declarations — these functions are defined later in this file.
// Required because syscall() calls them and C requires functions to be
// declared before use when -Werror=implicit-function-declaration is set.
uint64 sys_task_info(TaskInfo *ti);
uint64 sys_mmap(uint64 start, uint64 len, int prot, int flag, int fd);
uint64 sys_munmap(uint64 start, uint64 len);



uint64 sys_getppid()
{
	struct proc *p = curr_proc();
	return p->parent == NULL ? IDLE_PID : p->parent->pid;
}

uint64 sys_clone()
{
	debugf("fork!\n");
	return fork();
}

uint64 sys_exec(uint64 va)
{
	struct proc *p = curr_proc();
	char name[200];
	copyinstr(p->pagetable, name, va, 200);
	debugf("sys_exec %s\n", name);
	return exec(name);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

// -------------------------------------------------------------------------
// PROJECT 3, STEP 18: sys_spawn() — syscall wrapper for spawn().
//
// The user calls spawn(char *name) which triggers syscall ID 400 (SYS_spawn).
// The trap handler passes args[0] = virtual address of the filename string.
// This wrapper:
//   1. Uses copyinstr() to safely copy the filename from user virtual memory
//      into a kernel buffer (user pointers cannot be dereferenced directly).
//   2. Delegates to spawn() in proc.c for the actual process creation and load.
uint64 sys_spawn(uint64 va)
{
	// Get curr process
	struct proc *p = curr_proc();
	char filename[200];

	// Copy the filename from user space to kernel space
	// Delegate to proc.c
	if (copyinstr(p->pagetable, filename, va, 200) < 0){
		return -1;
	}
	return spawn(filename);
}
// -------------------------------------------------------------------------

// -------------------------------------------------------------------------
// PROJECT 3, STEP 19: sys_set_priority() — set this process's stride priority.
//
// The user calls setpriority(long long prio) which triggers syscall ID 140 (SYS_setpriority).
// Rules (from spec):
//   - prio must be >= 2 (prio = 1 would give max stride increment, behaving as lowest priority)
//   - Returns prio on success, -1 on invalid input.
// Effect: the stride scheduler computes pass = BIG_STRIDE / priority each round,
// so a higher prio means a smaller pass and more frequent selection.
uint64 sys_set_priority(long long prio){
    // Get the currently running process
	struct proc *p = curr_proc();

	// Return error if no process is currently running (should never happen in practice)
	if (p == 0) return -1;
	
	// Priority must be >= 2 
	if (prio < 2) return -1;

	p->priority = prio; // update process priority
    return prio;
}
// -------------------------------------------------------------------------


extern char trap_page[];

// syscall() — main system call dispatcher.
//
// Called from the trap handler whenever a user process executes ecall.
// Reads the syscall number from register a7 and arguments from a0-a5
// (all snapshotted into the trapframe by the trampoline before we get here).
// Dispatches to the appropriate handler and writes the return value into a0.
void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);

	// PROJECT 3, STEP 20: Increment per-process syscall counter.
	// Every syscall invocation is counted here, before dispatch.
	// sys_task_info reads these counts back to populate TaskInfo.syscall_times[].
	// Count how many times each syscall has been used by this process
	if (id < MAX_SYSCALL_NUM) {
		curr_proc()->syscall_times[id]++;
	}
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
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
	case SYS_clone: // SYS_fork
		ret = sys_clone();
		break;
	case SYS_execve:
		ret = sys_exec(args[0]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_spawn:
		ret = sys_spawn(args[0]);
		break;

	// Return task status, syscall stats, and running time.
	case SYS_task_info:
		ret = sys_task_info((TaskInfo *)args[0]);
		break;

	// Create an anonmyous memory mapping in user space.
	case SYS_mmap:
		ret = sys_mmap(args[0], args[1], args[2], args[3], args[4]);
		break;

	// Remove a previously mapped user-space memory region.
	case SYS_munmap:
		ret = sys_munmap(args[0], args[1]);
		break;

	// -------------------------------------------------------------------------
	// PROJECT 3, STEP 21: Dispatch SYS_setpriority (ID 140) to sys_set_priority().
	// args[0] holds the priority value (long long prio) passed by the user program.
	case SYS_setpriority:
		ret = sys_set_priority(args[0]);
		break;
	// -------------------------------------------------------------------------

	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}


// PROJECT 3, STEP 22: sys_task_info() — report runtime statistics to user space.
//
// Fills a TaskInfo struct (defined in proc.h) with:
//   status        — always Running (we can only query the currently running process)
//   syscall_times — copied from p->syscall_times[], accumulated since program load
//   time          — elapsed milliseconds since first schedule:
//                   (get_cycle() - start_time) * 1000 / CPU_FREQ
//
// The user passes a virtual address (ti). useraddr() translates it to a
// physical address so the kernel can write the struct directly into user memory.
uint64 sys_task_info(TaskInfo *ti)
{
	// Get the current running process
	struct proc *p = curr_proc();

	// Return error if no process is running or the provided pointer is invalid
	if (!p || !ti) return -1;

	// Translate the user virtual address into a kernel-usable physical address.
	uint64 dst = useraddr(p->pagetable, (uint64)ti);
	if (dst == 0) return -1;

	// Since we are querying the current task, we set the status to Running
	TaskInfo info;
	info.status = Running;

	// Copy the syscall times and calculate the running time of the process
	for (int i = 0; i < MAX_SYSCALL_NUM; i++) {
		info.syscall_times[i] = p->syscall_times[i];
	}

	// Calculate the running time of the process in milliseconds
	uint64 now = get_cycle();
	info.time = (int)((now - p->start_time) * 1000 / CPU_FREQ);

	*(TaskInfo *)dst = info;
	return 0; // Success
}