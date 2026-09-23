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
 * A program may start a program, so the regions are a stack of them: one
 * slot each, so that the one underneath is untouched rather than copied out
 * of the way. Half a terabyte a slot and two hundred and fifty slots going
 * spare makes that the cheapest way to have two address spaces at once.
 *
 * The kernel runs in the same page tables, so a pointer a program passes to
 * a syscall is one the kernel can simply follow. */

#define VM_PAGE 4096

/* Finds room in the firmware's tables. False if there is none. */
bool vm_start(void);

/* Where the program's region begins and ends. Zero until vm_start. */
uint64_t vm_base(void);
uint64_t vm_end(void);

/* Whether addr .. addr + size lies inside the region in use. */
bool vm_holds(uint64_t addr, uint64_t size);

/* Maps the page holding addr, if it is in the region and not mapped yet -
   what the page fault handler calls. A page that was write-protected for
   vm_undo_begin is copied aside here and made writable again. */
bool vm_fault(uint64_t addr);

/* Maps addr .. addr + size now, zeroed, rounded out to whole pages. */
bool vm_reserve(uint64_t addr, uint64_t size);

/* Whether the page holding addr is already there. */
bool vm_mapped(uint64_t addr);

/* Unmaps that range and hands its pages back. */
void vm_release(uint64_t addr, uint64_t size);

/* Hands back everything the program in the current region had, keeping the
   region itself. */
void vm_reset(void);

/* A region of its own for a program that is starting inside another, and
   the way back. vm_push leaves the one underneath exactly as it was; vm_pop
   gives the new one up entirely. False if there is no room for another. */
bool vm_push(void);
void vm_pop(void);

/* How many regions deep we are, and the way back to a given depth: what a
   program killed inside another leaves behind. */
unsigned vm_level(void);
void vm_unwind(unsigned to);

/* Write-protects everything the current region has mapped, so that whatever
   is written from here on can be put back: what fork needs, since the child
   runs in its parent's memory until it starts a program of its own. False
   if there is no room to record it, which changes nothing.

   vm_undo_end(true) puts every changed page back as it was and unmaps every
   page that was not there before; vm_undo_end(false) keeps the changes. */
bool vm_undo_begin(void);
void vm_undo_end(bool restore);

/* What the regions cost: the pages lent to programs, and the page tables
   describing them. */
size_t vm_memory(void);
size_t vm_tables(void);
