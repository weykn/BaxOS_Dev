#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Memory for the kernel and its programs.
 *
 * While the firmware is still running, every page comes from it. Once the
 * kernel has taken the machine over (mem_take_over, after ExitBootServices)
 * it hands pages out itself, from runs of free pages made out of the
 * firmware's last memory map - which by then includes everything the
 * firmware had for itself. A page taken either way is freed the same way. */

/* count contiguous pages, page-aligned, not zeroed. Returns the address, or
   0 if there is no run that long. */
uint64_t mem_pages(size_t count);
void     mem_pages_free(uint64_t at, size_t count);

/* bytes of memory, not zeroed, or NULL; given back with mem_free. Whole
   pages underneath, so for things that are big or do not stay long. */
void *mem_alloc(size_t bytes);
void  mem_free(void *memory);

/* Takes the free memory in the firmware's final memory map for the kernel's
   own - the map got just before ExitBootServices succeeded - except the
   pages still in use as page tables or the interrupt table. */
void mem_take_over(const void *map, size_t size, size_t stride);

/* Whether mem_take_over has happened: the firmware is gone. */
bool mem_ours(void);

/* A page of the fixed window (syscall.c), which is mapped one to one and so
   has to be that exact page: taken if it is free, given back when the
   program is done with it. The window's pages are kept for it alone, since
   its mapping hides whatever else was there. */
bool mem_window_take(uint64_t page);
void mem_window_give(uint64_t page);

/* RAM free right now, in KiB. */
uint64_t mem_free_kib(void);


/* Sizes in bytes unless named otherwise. */
struct mem_stats {
    uint32_t total_kib;     /* all the RAM there is */
    uint32_t free_kib;      /* what nobody has right now */
    uint32_t image;         /* kernel code and data, as loaded from kernel.bin */
    uint32_t data;          /* zero-filled kernel data (.bss), minus the stack */
    uint32_t stack;         /* the kernel stack */
    uint32_t stack_peak;    /* the most of the stack ever in use */
    uint32_t page_tables;   /* the fixed window's, hung off the firmware's */
    uint32_t console;       /* the console's cells, taken from the firmware */
    uint32_t disk_cache;    /* sectors kept so the next program need not read */
    uint32_t wallpaper;     /* the picture behind the text, if there is one */
    uint32_t window;        /* what the running program has, the tables
                               describing it counted in */
    uint32_t kernel_kib;    /* all of the above but the window, in KiB
                               rounded up: what the machine costs, whoever is
                               asking and whatever is running */
    uint32_t used_kib;      /* everything that is not free: the firmware's,
                               the kernel's and the program's */
};

void mem_get_stats(struct mem_stats *stats);
