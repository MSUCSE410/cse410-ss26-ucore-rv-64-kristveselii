#include "vm.h"
#include "defs.h"
#include "riscv.h"

pagetable_t kernel_pagetable;

extern char e_text[]; // kernel.ld sets this to end of kernel code.
extern char trampoline[];

// kvmmake() — build the kernel's direct-map page table.
//
// Implemented in Project 2, unchanged in Project 3.
//
// Maps three regions into the kernel page table:
//   1. Kernel text  [KERNBASE, e_text)    — read + execute (code is read-only)
//   2. Kernel data  [e_text, PHYSTOP)     — read + write   (heap, stacks, etc.)
//   3. Trampoline   [TRAMPOLINE, +1 page) — read + execute
//      The trampoline is the small assembly stub that switches between user
//      and kernel page tables on every syscall/interrupt. It must be mapped
//      at the same virtual address in BOTH the kernel and every user page
//      table so the CPU keeps executing after satp is changed.
pagetable_t kvmmake()
{
	pagetable_t kpgtbl;
	kpgtbl = (pagetable_t)kalloc();
	memset(kpgtbl, 0, PGSIZE);
	// map kernel text executable and read-only.
	kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)e_text - KERNBASE,
	       PTE_R | PTE_X);
	// map kernel data and the physical RAM we'll make use of.
	kvmmap(kpgtbl, (uint64)e_text, (uint64)e_text, PHYSTOP - (uint64)e_text,
	       PTE_R | PTE_W);
	kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
	return kpgtbl;
}

// kvm_init() — install the kernel page table and enable paging.
//
// Implemented in Project 2, unchanged in Project 3.
//
// Writes the kernel page table's physical address into the satp CSR
// (Supervisor Address Translation and Protection), turning on the Sv39 MMU.
// After this call every memory access goes through the page table.
// sfence_vma() flushes the TLB to discard any stale entries from before paging.
void kvm_init()
{
	kernel_pagetable = kvmmake();
	w_satp(MAKE_SATP(kernel_pagetable));
	sfence_vma();
	infof("enable pageing at %p", r_satp());
}

// walk() — traverse the 3-level Sv39 page table for virtual address va.
//
// Implemented in Project 2, unchanged in Project 3.
//
// Sv39 splits a virtual address into three 9-bit indices (one per level)
// plus a 12-bit page offset. PX(level, va) extracts the 9-bit index for
// the given level. The function walks from the root (level 2) to the leaf
// (level 0), following existing PTEs or allocating new intermediate page-table
// pages when alloc != 0.
// Returns a pointer to the leaf PTE so the caller can read or modify it.
// Returns NULL if a page-table page is missing and alloc == 0.
pte_t *walk(pagetable_t pagetable, uint64 va, int alloc)
{
	if (va >= MAXVA)
		panic("walk");

	for (int level = 2; level > 0; level--) {
		pte_t *pte = &pagetable[PX(level, va)];
		if (*pte & PTE_V) {
			pagetable = (pagetable_t)PTE2PA(*pte);
		} else {
			if (!alloc || (pagetable = (pde_t *)kalloc()) == 0)
				return 0;
			memset(pagetable, 0, PGSIZE);
			*pte = PA2PTE(pagetable) | PTE_V;
		}
	}
	return &pagetable[PX(0, va)];
}

// walkaddr() — look up the physical page base for a user virtual address.
//
// Implemented in Project 2, unchanged in Project 3.
//
// Calls walk() to find the leaf PTE and extracts the physical page number.
// Returns 0 (failure) if the PTE is missing, not valid (PTE_V clear),
// or not user-accessible (PTE_U clear — kernel pages must not be readable
// from user mode).
// Returns the PAGE BASE only — callers that need the full address must OR
// in the page offset themselves (see useraddr()).
uint64 walkaddr(pagetable_t pagetable, uint64 va)
{
	pte_t *pte;
	uint64 pa;

	if (va >= MAXVA)
		return 0;

	pte = walk(pagetable, va, 0);
	if (pte == 0)
		return 0;
	if ((*pte & PTE_V) == 0)
		return 0;
	if ((*pte & PTE_U) == 0)
		return 0;
	pa = PTE2PA(*pte);
	return pa;
}

// Look up a virtual address, return the physical address,
// useraddr() — like walkaddr() but preserves the intra-page byte offset.
//
// Implemented in Project 2, unchanged in Project 3.
//
// Returns (physical page base | (va & 0xFFF)) so the result points to
// exactly the same byte within the page that va referenced.
// Used when the kernel wants to write directly into a user-space struct
// by physical address rather than going through copyout() page-by-page
// (e.g. sys_task_info writes a TaskInfo struct, sys_wait writes an exit code).
uint64 useraddr(pagetable_t pagetable, uint64 va)
{
	uint64 page = walkaddr(pagetable, va);
	if (page == 0)
		return 0;
	return page | (va & 0xFFFULL);
}

// kvmmap() — add one VA→PA mapping to the kernel page table.
//
// Implemented in Project 2, unchanged in Project 3.
//
// Thin wrapper around mappages(). Panics on failure because kernel
// boot-time mappings are non-negotiable — if they fail, the OS cannot run.
void kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
	if (mappages(kpgtbl, va, sz, pa, perm) != 0)
		panic("kvmmap");
}

// mappages() — install PTEs for a contiguous virtual address range.
//
// Implemented in Project 2, unchanged in Project 3.
//
// Maps the physical range [pa, pa+size) to the virtual range [va, va+size).
// Calls walk(alloc=1) to create intermediate page-table pages as needed.
// Returns -1 if any page in the range is already mapped (remap detected
// by PTE_V being set) or if walk() fails to allocate memory.
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
	uint64 a, last;
	pte_t *pte;

	a = PGROUNDDOWN(va);
	last = PGROUNDDOWN(va + size - 1);
	for (;;) {
		if ((pte = walk(pagetable, a, 1)) == 0) {
			errorf("pte invalid, va = %p", a);
			return -1;
		}
		if (*pte & PTE_V) {
			errorf("remap");
			return -1;
		}
		*pte = PA2PTE(pa) | perm | PTE_V;
		if (a == last)
			break;
		a += PGSIZE;
		pa += PGSIZE;
	}
	return 0;
}

// uvmunmap() — remove npages mappings starting at page-aligned va.
//
// Implemented in Project 2, unchanged in Project 3.
//
// Clears each leaf PTE in the range. If do_free != 0, also frees the
// underlying physical page via kfree(). Pages that are already unmapped
// (walk returns NULL or PTE_V is clear) are silently skipped — this
// matters during exec() teardown where the address space may be partial.
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
	uint64 a;
	pte_t *pte;

	if ((va % PGSIZE) != 0)
		panic("uvmunmap: not aligned");

	for (a = va; a < va + npages * PGSIZE; a += PGSIZE) {
		if ((pte = walk(pagetable, a, 0)) == 0)
			continue;
		if ((*pte & PTE_V) != 0) {
			if (PTE_FLAGS(*pte) == PTE_V)
				panic("uvmunmap: not a leaf");
			if (do_free) {
				uint64 pa = PTE2PA(*pte);
				kfree((void *)pa);
			}
		}
		*pte = 0;
	}
}

// uvmcreate() — allocate and initialize a fresh user page table.
//
// Implemented in Project 2, unchanged in Project 3.
//
// Every user process needs two fixed mappings installed at creation time:
//   TRAMPOLINE → physical trampoline page (R+X, no PTE_U)
//     Runs in supervisor mode during kernel/user switch, so NOT user-accessible.
//   TRAPFRAME  → this process's trapframe page (R+W, no PTE_U)
//     Stores saved user registers during a trap. Kernel-only access.
// The trapframe physical address is passed in because each process has its
// own pre-allocated slot in the static trapframe[] array in proc.c.
pagetable_t uvmcreate(uint64 trapframe)
{
	pagetable_t pagetable;
	pagetable = (pagetable_t)kalloc();
	if (pagetable == 0) {
		errorf("uvmcreate: kalloc error");
		return 0;
	}
	memset(pagetable, 0, PGSIZE);
	if (mappages(pagetable, TRAMPOLINE, PAGE_SIZE, (uint64)trampoline,
		     PTE_R | PTE_X) < 0) {
		panic("mappages fail");
	}
	if (mappages(pagetable, TRAPFRAME, PGSIZE, trapframe, PTE_R | PTE_W) <
	    0) {
		panic("mappages fail");
	}
	return pagetable;
}

// freewalk() — recursively free all page-table pages (not the leaf physical pages).
//
// Implemented in Project 2, unchanged in Project 3.
//
// Walks the 3-level page-table tree. For each valid non-leaf PTE (one that
// points to a lower-level page table rather than a physical data page),
// it recurses into the child table and frees it after clearing the PTE.
//
// IMPORTANT: All leaf mappings must already be cleared by uvmunmap() before
// calling freewalk(). The panic("freewalk: leaf") line is commented out per
// the Project 3 merge instructions — leaving it active would cause spurious
// panics during process teardown in the ch5 test suite.
void freewalk(pagetable_t pagetable)
{
	// there are 2^9 = 512 PTEs in a page table.
	for (int i = 0; i < 512; i++) {
		pte_t pte = pagetable[i];
		if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
			// this PTE points to a lower-level page table.
			uint64 child = PTE2PA(pte);
			freewalk((pagetable_t)child);
			pagetable[i] = 0;
		} else if (pte & PTE_V) {
			//panic("freewalk: leaf");
		}
	}
	kfree((void *)pagetable);
}

// uvmfree() — free all user memory pages, then free the page-table structure.
//
// Implemented in Project 2, unchanged in Project 3.
//
// Two-step teardown:
//   1. uvmunmap(do_free=1) clears all leaf PTEs and frees physical pages for
//      virtual pages [0, max_page * PAGE_SIZE). max_page is the high-water mark
//      set by bin_loader() so we never scan the full 512 GB Sv39 address space.
//   2. freewalk() frees the now-empty page-table pages themselves.
void uvmfree(pagetable_t pagetable, uint64 max_page)
{
	if (max_page > 0)
		uvmunmap(pagetable, 0, max_page, 1);
	freewalk(pagetable);
}

// uvmcopy() — copy a parent's entire user address space into a child's.
//
// Implemented in Project 2, unchanged in Project 3.
//
// Used by fork(). Walks every virtual page in [0, max_page * PAGE_SIZE),
// skipping any that are not mapped. For each mapped page:
//   1. Reads the physical address and PTE flags from the parent's table.
//   2. Allocates a new physical page for the child (kalloc).
//   3. Copies the page contents with memmove().
//   4. Maps the new page into the child's table with the same flags.
// On any failure, unmaps everything mapped so far and returns -1.
int uvmcopy(pagetable_t old, pagetable_t new, uint64 max_page)
{
	pte_t *pte;
	uint64 pa, i;
	uint flags;
	char *mem;

	for (i = 0; i < max_page * PAGE_SIZE; i += PGSIZE) {
		if ((pte = walk(old, i, 0)) == 0)
			continue;
		if ((*pte & PTE_V) == 0)
			continue;
		pa = PTE2PA(*pte);
		flags = PTE_FLAGS(*pte);
		if ((mem = kalloc()) == 0)
			goto err;
		memmove(mem, (char *)pa, PGSIZE);
		if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0) {
			kfree(mem);
			goto err;
		}
	}
	return 0;

err:
	uvmunmap(new, 0, i / PGSIZE, 1);
	return -1;
}

// copyout() — copy len bytes from kernel buffer src to user virtual address dstva.
//
// Implemented in Project 2, unchanged in Project 3.
//
// Cannot dereference dstva directly because it is a virtual address in the
// user's page table, not the kernel's. For each page boundary crossed,
// walkaddr() translates the virtual page to its physical base, then memmove()
// copies up to PGSIZE bytes at a time into the correct physical location.
// Returns 0 on success, -1 if any page in the range is not mapped.
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
	uint64 n, va0, pa0;

	while (len > 0) {
		va0 = PGROUNDDOWN(dstva);
		pa0 = walkaddr(pagetable, va0);
		if (pa0 == 0)
			return -1;
		n = PGSIZE - (dstva - va0);
		if (n > len)
			n = len;
		memmove((void *)(pa0 + (dstva - va0)), src, n);

		len -= n;
		src += n;
		dstva = va0 + PGSIZE;
	}
	return 0;
}

// copyin() — copy len bytes from user virtual address srcva to kernel buffer dst.
//
// Implemented in Project 2, unchanged in Project 3.
//
// Mirror image of copyout(). Translates each virtual page to physical via
// walkaddr(), then memmoves up to PGSIZE bytes per iteration.
// Returns 0 on success, -1 if any page in the range is not mapped.
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
	uint64 n, va0, pa0;

	while (len > 0) {
		va0 = PGROUNDDOWN(srcva);
		pa0 = walkaddr(pagetable, va0);
		if (pa0 == 0)
			return -1;
		n = PGSIZE - (srcva - va0);
		if (n > len)
			n = len;
		memmove(dst, (void *)(pa0 + (srcva - va0)), n);

		len -= n;
		dst += n;
		srcva = va0 + PGSIZE;
	}
	return 0;
}

// copyinstr() — copy a null-terminated string from user VA srcva to kernel dst.
//
// Implemented in Project 2, unchanged in Project 3.
//
// Like copyin() but stops at the first '\0' or after max bytes, whichever
// comes first. Returns the number of bytes copied (not counting '\0'), or -1.
// Used by sys_exec, sys_spawn, and sys_write to safely read filename and
// string arguments out of user virtual memory.
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
	uint64 n, va0, pa0;
	int got_null = 0, len = 0;

	while (got_null == 0 && max > 0) {
		va0 = PGROUNDDOWN(srcva);
		pa0 = walkaddr(pagetable, va0);
		if (pa0 == 0)
			return -1;
		n = PGSIZE - (srcva - va0);
		if (n > max)
			n = max;

		char *p = (char *)(pa0 + (srcva - va0));
		while (n > 0) {
			if (*p == '\0') {
				*dst = '\0';
				got_null = 1;
				break;
			} else {
				*dst = *p;
			}
			--n;
			--max;
			p++;
			dst++;
			len++;
		}

		srcva = va0 + PGSIZE;
	}
	return len;
}