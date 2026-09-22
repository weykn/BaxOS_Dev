#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The address space a program runs in.
 *
 * The 2 MiB window a program used to get was enough for something written
 * for this machine, and nowhere near enough for something taken off a Linux
 * system: a dynamically linked program arrives with a loader, a C library and
 * a heap, each wanting its own stretch of memory at an address of its own.
 *
 * So a program gets a region of its own instead, hung off a slot of the
 * firmware's top-level page table that nothing else uses. The pages in it are
 * ordinary memory bought from the firmware and mapped wherever the program
 * needs them, rather than mapped where they happen to sit - which is what
 * lets a library be placed at the address its loader picked. Only the pages
 * actually touched are ever bought.
 *
 * The kernel runs in the same page tables, so a pointer a program passes to
 * a syscall is one the kernel can simply follow. */

#define VM_PAGE 4096

/* Finds room in the firmware's tables. False if there is none, which leaves
   only the old fixed window working. */
bool vm_start(void);

/* Where the program's region begins and ends. Zero until vm_start. */
uint64_t vm_base(void);
uint64_t vm_end(void);

/* Whether addr .. addr + size lies inside it. */
bool vm_holds(uint64_t addr, uint64_t size);

/* Maps the page holding addr, if it is in the region and not mapped yet -
   what the page fault handler calls. */
bool vm_fault(uint64_t addr);

/* Maps addr .. addr + size now, zeroed, rounded out to whole pages. */
bool vm_reserve(uint64_t addr, uint64_t size);

/* Unmaps that range and hands its pages back. */
void vm_release(uint64_t addr, uint64_t size);

/* Hands back everything the program had, region and tables both. */
void vm_reset(void);

/* What the region costs: the pages lent to the program, and the page tables
   describing them. */
size_t vm_memory(void);
size_t vm_tables(void);
