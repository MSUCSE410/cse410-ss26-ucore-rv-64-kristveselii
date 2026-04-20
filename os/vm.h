#ifndef VM_H
#define VM_H

#include "riscv.h"
#include "types.h"

// vm.h — Virtual Memory API
//
// Implemented in Project 2, unchanged in Project 3.
//
// Declares every function in vm.c that the rest of the kernel uses to
// create, populate, and tear down user page tables.
//
// Background — RISC-V Sv39 paging:
//   Each process gets a 39-bit virtual address space backed by a 3-level
//   page table tree. A virtual address is decoded as:
//     bits 38-30 → 9-bit index into the level-2 (root) page table
//     bits 29-21 → 9-bit index into the level-1 page table
//     bits 20-12 → 9-bit index into the level-0 (leaf) page table
//     bits 11-0  → 12-bit byte offset within the 4 KB page
//   Each table holds 512 PTEs. A leaf PTE stores the physical page number
//   plus permission flags (R/W/X/U/V).

// kvm_init() — build the kernel's direct-map page table and enable paging.
// Called once at boot before any user processes exist.
void kvm_init();

// kvmmap() — add a single VA→PA mapping to the kernel page table.
// Used only during boot to set up the kernel's own address space.
void kvmmap(pagetable_t, uint64, uint64, uint64, int);

// walk() — traverse the 3-level page table for virtual address va.
// Returns a pointer to the leaf PTE.
// If alloc != 0, missing intermediate page-table pages are allocated on the way down.
// Called internally and exposed so sys_mmap/sys_munmap can inspect existing mappings.
pte_t *walk(pagetable_t, uint64, int); 

// mappages() — create PTEs mapping [va, va+size) → [pa, pa+size).
// Returns 0 on success, -1 if a page-table page could not be allocated
// or if any page in the range is already mapped (remap).
int mappages(pagetable_t, uint64, uint64, uint64, int);

// uvmcreate() — allocate a fresh user page table.
// Pre-maps the TRAMPOLINE and TRAPFRAME pages that every process needs
// for kernel/user transitions. Takes the physical address of the process's
// trapframe so it can be installed at the fixed TRAPFRAME virtual address.
pagetable_t uvmcreate(uint64);

// uvmcopy() — deep-copy a parent's user address space into a child's.
// Allocates new physical pages, copies contents, and installs matching PTEs.
// Used by fork().
int uvmcopy(pagetable_t, pagetable_t, uint64);

// uvmfree() — free all user pages then free the page-table pages themselves.
// Step 1: uvmunmap frees physical pages up to max_page.
// Step 2: freewalk frees the now-empty page-table structure.
// Called by freepagetable() on process exit or exec().
void uvmfree(pagetable_t, uint64);

// uvmunmap() — remove npages mappings starting at page-aligned va.
// If do_free != 0, the underlying physical pages are also freed via kfree().
// Missing PTEs are silently skipped rather than panicking.
void uvmunmap(pagetable_t, uint64, uint64, int);

// walkaddr() — look up the physical page base for a user virtual address.
// Returns 0 if va is unmapped, not valid, or not user-accessible (PTE_U clear).
uint64 walkaddr(pagetable_t, uint64);

// useraddr() — like walkaddr() but preserves the intra-page byte offset.
// Returns (physical page base | page offset) so the result points to exactly
// the same byte that va referenced. Used when the kernel writes directly
// into a user-space struct by physical address (e.g. sys_task_info, sys_wait).
uint64 useraddr(pagetable_t, uint64);

// copyout() — copy len bytes from kernel buffer src to user VA dstva.
// Handles ranges that span page boundaries. Returns 0 on success, -1 on error.
int copyout(pagetable_t, uint64, char *, uint64);

// copyin() — copy len bytes from user VA srcva to kernel buffer dst.
// Handles ranges that span page boundaries. Returns 0 on success, -1 on error.
int copyin(pagetable_t, char *, uint64, uint64);

// copyinstr() — copy a null-terminated string from user VA srcva to kernel dst.
// Stops at '\0' or after max bytes. Returns bytes copied (excluding '\0'), or -1.
// Used by sys_exec, sys_spawn, and sys_write to read string arguments from user space.
int copyinstr(pagetable_t, char *, uint64, uint64);

#endif // VM_H