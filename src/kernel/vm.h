#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The address space a process runs in.
 *
 * A program taken off a Linux system arrives with a loader, a C library and
 * a heap, each wanting its own stretch of memory at an address of its own.
 * So every process has a region: one slot of the top-level page table that
 * nothing else uses, half a terabyte, with tables of its own under it. The
 * pages in it are ordinary memory mapped wherever the program needs them,
 * and only the pages actually touched are ever bought.
 *
 * Every process's region is at the same address, and the slot points at
 * whichever one is running (vm_space_use): a fork's child has its parent's
 * addresses, as a fork's child must, and switching costs one entry.
 *
 * The kernel runs in the same page tables, so a pointer a program passes to
 * a syscall is one the kernel can simply follow. */

#define VM_PAGE 4096

/* One process's memory. It lives in the process; vm.c keeps track of it
   from vm_space_new to vm_space_free. */
struct space {
    uint64_t *pdpt;             /* its table under the slot */
    uint64_t bought;            /* taken from the firmware for it, chunks and
                                   all - which is what it really costs, not
                                   what it has been handed so far */
    uint64_t tables;            /* bytes of tables in that */
    uint64_t chunks;            /* the last chunk bought, 0 for none */
    uint64_t next, left;        /* the next page in it, and how many are left */
    uint64_t grow;              /* how big the next chunk should be */
    uint64_t spare;             /* pages given back, chained through their
                                   first word, to hand out again */

    /* Low memory, for a program linked to run at a fixed address: the range
       it may use, and a page listing the page table for each two megabytes
       of it, which stand in for the firmware's own mapping while it runs. */
    uint64_t  low_start, low_end;
    uint64_t *low_tables;
    uint64_t  low_owned;        /* pages it has at their own address */
};

/* A new, empty space; false if there is no room for one. */
bool vm_space_new(struct space *s);

/* The space mapped from now on, NULL for none: what a process switch does. */
void vm_space_use(struct space *s);
struct space *vm_space(void);

/* Every page it has goes back, and it is forgotten. */
void vm_space_free(struct space *s);

/* A new space holding a copy of everything the current one has, at the same
   addresses: a fork's child. False, and nothing made, if memory runs out. */
bool vm_fork(struct space *child);

/* Whether the current process has a region to run in. */
bool vm_start(void);

/* Where the program's region begins and ends. Zero without one. */
uint64_t vm_base(void);
uint64_t vm_end(void);

/* Gives the program in the current region start .. end of low memory too,
   for one linked to run at a fixed address: mapped over the firmware's one
   to one mapping while it runs, from pages at those very addresses. False if
   that cannot be had. Its pages come as it touches them, like the region's. */
bool vm_low(uint64_t start, uint64_t end);

/* Puts the firmware's own view of low memory back while it is on, for a call
   into its runtime services, and the running program's when it is off. */
void vm_firmware_view(bool on);

/* The kernel's own tables taking over from the firmware's (mem_take_over):
   top is the new top-level table, pd its directory for the first gigabyte.
   What programs have hung off the firmware's is carried across. */
void vm_move(uint64_t *top, uint64_t *pd);

/* Whether a program linked to a fixed address has had the first gigabyte's
   directory: then vm_move needs a pd to carry it to. */
bool vm_low_used(void);

/* The page tables the machine has for good: the first region's top table,
   and what low memory has cost. */
size_t vm_fixed_tables(void);

/* Whether addr .. addr + size lies inside the region in use, or its low
   memory. */
bool vm_holds(uint64_t addr, uint64_t size);

/* Maps the page holding addr, if it is in the region and not mapped yet -
   what the page fault handler calls. A page that was write-protected for
   vm_undo_begin is copied aside here and made writable again. */
bool vm_fault(uint64_t addr);

/* Maps addr .. addr + size now, zeroed, rounded out to whole pages. */
bool vm_reserve(uint64_t addr, uint64_t size);

/* Maps page - one of share.c's - at addr, read-only until written. False if
   something is there already, or the address is not one a shared page may
   go: then the caller makes a private copy as before. */
bool vm_map_shared(uint64_t addr, uint64_t page, bool writable);

/* Whether the page holding addr is already there. */
bool vm_mapped(uint64_t addr);

/* Maps addr .. addr + size to a device's memory at phys - the screen, for
   a program drawing on it. Those pages are never handed back to anyone. */
bool vm_map_device(uint64_t addr, uint64_t phys, uint64_t size);

/* Unmaps that range and hands its pages back. */
void vm_release(uint64_t addr, uint64_t size);
void vm_discard(uint64_t addr, uint64_t size);   /* madvise DONTNEED: private pages only */

/* Hands back everything the program in the current region had, keeping the
   region itself. */
void vm_reset(void);

/* What the regions cost: every page bought for them, the tables under the
   top one included, and the top tables, which are bought on their own. */
size_t vm_memory(void);
size_t vm_tables(void);
