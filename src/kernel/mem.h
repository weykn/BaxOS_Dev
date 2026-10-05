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
uint64_t mem_pages_lent(size_t count);  /* the disk cache's: above the fixed-address gigabyte */
void     mem_pages_free(uint64_t at, size_t count);

/* The same, all of it below limit: for a device that can only reach the
   first four gigabytes. */
uint64_t mem_pages_below(size_t count, uint64_t limit);

/* bytes of memory, not zeroed, or NULL; given back with mem_free. Whole
   pages underneath, so for things that are big or do not stay long. */
void *mem_alloc(size_t bytes);
void  mem_free(void *memory);

/* Builds the tables mem_take_over moves onto, while the firmware can still
   give memory for them; top is the highest address there is to map. */
bool mem_own_tables(uint64_t top);

/* Takes the free memory in the firmware's final memory map for the kernel's
   own - the map got just before ExitBootServices succeeded - except the
   interrupt table, after moving onto the tables mem_own_tables built. */
void mem_take_over(const void *map, size_t size, size_t stride);

struct e820;
struct boot_info;

/* The same for a machine a BIOS started: every page from the E820 map, the
   kernel's from the start, and its own tables at once. Fills in info's
   memory figures. */
void mem_take_bios(const struct e820 *map, unsigned count, struct boot_info *info);

/* Makes sure the device registers at at are mapped, one to one and
   uncached, in the kernel's own tables - now, or when they are built. */
void mem_map_io(uint64_t at);

/* Gives back the part of the kernel's own region it does not use. */
void mem_trim_kernel(void);

/* Whether mem_take_over has happened: the firmware is gone. */
bool mem_ours(void);

/* The page at page, taken if it is free - for low memory (vm.c), which has
   to be that exact page since its mapping hides whatever else was there -
   and given back when the program is done with it. */
bool mem_take_page(uint64_t page);

/* Whether the page is low memory the firmware keeps for good - its runtime
   services', ACPI's, or reserved - rather than anyone's in use. */
bool mem_firmware_kept(uint64_t page);
void mem_give_page(uint64_t page);

/* RAM free right now, in KiB. */
uint64_t mem_free_kib(void);


/* Sizes in bytes unless named otherwise. */
struct mem_stats {
    uint32_t ram_kib;       /* all the RAM there is */
    uint32_t firmware_kib;  /* of it, the firmware's for good */
    uint32_t total_kib;     /* the rest: the RAM the OS has, as Linux's
                               MemTotal counts it */
    uint32_t free_kib;      /* what nobody has right now */
    uint32_t image;         /* kernel code and data, as loaded from kernel.bin */
    uint32_t data;          /* zero-filled kernel data (.bss), minus the stack */
    uint32_t stack;         /* the kernel stack */
    uint32_t stack_peak;    /* the most of the stack ever in use */
    uint32_t page_tables;   /* the fixed window's, hung off the firmware's */
    uint32_t console;       /* the console's cells, taken from the firmware */
    uint32_t disk_cache;    /* what the disk cache holds: free memory lent out,
                               so not in kernel_kib */
    uint32_t modules;       /* the loaded modules' own code and data */
    uint32_t network;       /* the network's buffers, the card's and sockets' */
    uint32_t window;        /* what the running program has, the tables
                               describing it counted in */
    uint32_t kernel_kib;    /* all of the above but the window and the disk cache, in KiB
                               rounded up: what the machine costs, whoever is
                               asking and whatever is running */
    uint32_t used_kib;      /* all of ram_kib that is not free: the
                               firmware's, the kernel's and the program's */
};

void mem_get_stats(struct mem_stats *stats);
