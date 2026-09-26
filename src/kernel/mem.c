#include "mem.h"

#include "ata.h"
#include "bg.h"
#include "boot.h"
#include "debug.h"
#include "efi.h"
#include "efi_kernel.h"
#include "string.h"
#include "syscall.h"
#include "vga.h"

/* ---- the pages ------------------------------------------------------------
 *
 * Free memory is a list of runs, sorted by address, first fit. Freeing puts
 * a run back and joins it to its neighbours, so the list stays about as
 * long as the memory map was. Should it ever fill, the smallest run is let
 * go of rather than refusing the free - a few pages lost, never a crash. */

#define PAGE  4096
#define RUNS  128

static struct run {
    uint64_t at, count;
} runs[RUNS];
static unsigned run_count;
static bool     ours;

/* The fixed window's pages that are free, one bit each. */
#define WINDOW_PAGES ((PROGRAM_STACK - PROGRAM_BASE) / PAGE)
static uint64_t window_free[WINDOW_PAGES / 64];

bool mem_ours(void) {
    return ours;
}

static struct efi_boot_services *firmware(void) {
    return efi_boot()->system->boot;
}

static void run_remove(unsigned i) {
    memmove(&runs[i], &runs[i + 1], (run_count - i - 1) * sizeof runs[0]);
    run_count--;
}

/* Puts at .. at + count back, joined to whatever it touches. */
static void run_add(uint64_t at, uint64_t count) {
    unsigned i = 0;

    if (count == 0) {
        return;
    }
    while (i < run_count && runs[i].at < at) {
        i++;
    }
    if (i > 0 && runs[i - 1].at + runs[i - 1].count * PAGE == at) {
        runs[i - 1].count += count;             /* onto the end of the one before */
        if (i < run_count && at + count * PAGE == runs[i].at) {
            runs[i - 1].count += runs[i].count; /* and it closes a gap */
            run_remove(i);
        }
        return;
    }
    if (i < run_count && at + count * PAGE == runs[i].at) {
        runs[i].at = at;                        /* onto the front of the next */
        runs[i].count += count;
        return;
    }
    if (run_count == RUNS) {
        unsigned smallest = 0;

        for (unsigned j = 1; j < RUNS; j++) {
            if (runs[j].count < runs[smallest].count) {
                smallest = j;
            }
        }
        if (runs[smallest].count >= count) {
            return;                             /* this one is the smallest */
        }
        run_remove(smallest);
        if (smallest < i) {
            i--;
        }
    }
    memmove(&runs[i + 1], &runs[i], (run_count - i) * sizeof runs[0]);
    runs[i] = (struct run){ at, count };
    run_count++;
}

/* Takes page out of whatever run holds it, if any does. */
static void run_take(uint64_t page) {
    for (unsigned i = 0; i < run_count; i++) {
        uint64_t end = runs[i].at + runs[i].count * PAGE;

        if (page < runs[i].at || page >= end) {
            continue;
        }
        uint64_t after = (end - page) / PAGE - 1;

        runs[i].count = (page - runs[i].at) / PAGE;
        if (runs[i].count == 0) {
            run_remove(i);
        }
        run_add(page + PAGE, after);
        return;
    }
}

uint64_t mem_pages(size_t count) {
    if (!ours) {
        uint64_t at = 0;

        return EFI_ERROR(firmware()->allocate_pages(EFI_ALLOCATE_ANY, EFI_LOADER_DATA,
                                                    count, &at)) ? 0 : at;
    }
    for (unsigned i = 0; i < run_count; i++) {
        if (runs[i].count >= count) {
            uint64_t at = runs[i].at;

            runs[i].at += count * PAGE;
            runs[i].count -= count;
            if (runs[i].count == 0) {
                run_remove(i);
            }
            return at;
        }
    }
    return 0;
}

void mem_pages_free(uint64_t at, size_t count) {
    if (!ours) {
        firmware()->free_pages(at, count);
        return;
    }
    run_add(at, count);
}

/* Each allocation starts with how many pages it is, sixteen bytes before
   what the caller gets, so that freeing needs nothing but the pointer. */
#define HEADER 16

void *mem_alloc(size_t bytes) {
    size_t count = (bytes + HEADER + PAGE - 1) / PAGE;
    uint64_t at = mem_pages(count);

    if (at == 0) {
        return NULL;
    }
    *(uint64_t *)at = count;
    return (void *)(at + HEADER);
}

void mem_free(void *memory) {
    if (memory != NULL) {
        uint64_t at = (uint64_t)memory - HEADER;

        mem_pages_free(at, *(uint64_t *)at);
    }
}

/* The page table pages the processor is walking right now, which the
   firmware made in memory that is about to be called free. Two-megabyte and
   one-gigabyte entries map memory rather than naming another table. */
#define PRESENT 0x01
#define WRITE   0x02
#define HUGE    0x80
#define NX      (1ull << 63)
#define ADDR    0x000FFFFFFFFFF000ull

/* Makes page reachable at its own address, readable, writable and not
   refused for executing. The firmware's tables map all of memory that way
   except where it chose not to: guard pages around its own allocations left
   out, its code made read-only, its data made unexecutable. Those are ours
   now. False if no table reaches it at all. */
static bool open_page(uint64_t cr3, uint64_t page) {
    uint64_t *entry = (uint64_t *)(cr3 & ADDR);

    for (unsigned shift = 39; ; shift -= 9) {
        uint64_t *e = &entry[(page >> shift) & 511];

        if (shift == 12) {
            *e = page | PRESENT | WRITE;
            return true;
        }
        if (!(*e & PRESENT)) {
            return false;
        }
        *e = (*e | WRITE) & ~NX;
        if (*e & HUGE) {
            return true;            /* two megabytes or a gigabyte at once */
        }
        entry = (uint64_t *)(*e & ADDR);
    }
}

static void keep_tables(uint64_t table, unsigned level) {
    const uint64_t *entry = (const uint64_t *)table;

    run_take(table);
    if (level == 1) {
        return;
    }
    for (unsigned i = 0; i < 512; i++) {
        if ((entry[i] & PRESENT) && !(level < 4 && (entry[i] & HUGE))) {
            keep_tables(entry[i] & ADDR, level - 1);
        }
    }
}

void mem_take_over(const void *map, size_t size, size_t stride) {
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idtr;
    uint64_t cr3;

    uint64_t kept[16] = { 0 };      /* KiB not taken, by type: for the log */

    for (size_t at = 0; at < size; at += stride) {
        const struct efi_memory_descriptor *d = (const void *)((const char *)map + at);
        uint64_t start = d->physical, count = d->pages;

        if (d->type < 16) {
            kept[d->type] += count * 4;
        }

        /* Free, or the firmware's for as long as it was running - its code,
           its data, and the loader it started. Not loader data: that is
           every page the kernel has, itself included. */
        if (d->type != EFI_CONVENTIONAL_MEMORY && d->type != 1 &&
            d->type != 3 && d->type != 4) {
            continue;
        }
        /* Page 0 would be taken for "none". */
        if (start == 0) {
            if (--count == 0) {
                continue;
            }
            start += PAGE;
        }
        run_add(start, count);
    }
    /* The fixed window (syscall.c) is mapped over whatever the firmware's
       tables said was at 4 MiB, so the free pages there are kept for it
       rather than handed out to be reached the ordinary way. */
    for (uint64_t page = PROGRAM_BASE; page < PROGRAM_STACK; page += PAGE) {
        for (unsigned i = 0; i < run_count; i++) {
            if (page >= runs[i].at && page < runs[i].at + runs[i].count * PAGE) {
                unsigned bit = (unsigned)((page - PROGRAM_BASE) / PAGE);

                window_free[bit / 64] |= 1ull << (bit % 64);
                run_take(page);
                break;
            }
        }
    }
    dbg("mem: kept KiB - loader data %u, runtime code %u, runtime data %u, "
        "ACPI %u, ACPI NVS %u, reserved %u\n", kept[2], kept[5], kept[6], kept[9],
        kept[10], kept[0]);
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    keep_tables(cr3 & ADDR, 4);
    __asm__ volatile("sidt %0" : "=m"(idtr));
    for (uint64_t page = idtr.base & ~(uint64_t)(PAGE - 1); page <= idtr.base + idtr.limit;
         page += PAGE) {
        run_take(page);
    }

    /* Every page now free has to be one the kernel can use. A page no table
       reaches is dropped rather than handed out to fault. The firmware may
       have made its tables read-only as well, so write protection is off
       while they are changed. */
    uint64_t cr0;

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0 & ~(1ull << 16)) : "memory");
    for (unsigned i = 0; i < run_count; i++) {
        for (uint64_t n = 0; n < runs[i].count; n++) {
            uint64_t page = runs[i].at + n * PAGE;

            if (!open_page(cr3, page)) {
                run_take(page);
                i = (unsigned)-1;   /* the list changed: start over */
                break;
            }
        }
    }
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");    /* flush */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    ours = true;
}

bool mem_window_take(uint64_t page) {
    unsigned bit = (unsigned)((page - PROGRAM_BASE) / PAGE);
    uint64_t at = page;

    if (!ours) {
        return !EFI_ERROR(firmware()->allocate_pages(EFI_ALLOCATE_ADDRESS,
                                                     EFI_LOADER_DATA, 1, &at));
    }
    if (bit >= WINDOW_PAGES || !(window_free[bit / 64] & 1ull << (bit % 64))) {
        return false;
    }
    window_free[bit / 64] &= ~(1ull << (bit % 64));
    return true;
}

void mem_window_give(uint64_t page) {
    unsigned bit = (unsigned)((page - PROGRAM_BASE) / PAGE);

    if (!ours) {
        firmware()->free_pages(page, 1);
    } else if (bit < WINDOW_PAGES) {
        window_free[bit / 64] |= 1ull << (bit % 64);
    }
}

uint64_t mem_free_kib(void) {
    uint64_t pages = 0;

    if (ours) {
        for (unsigned i = 0; i < run_count; i++) {
            pages += runs[i].count;
        }
        for (unsigned i = 0; i < WINDOW_PAGES / 64; i++) {
            for (uint64_t bits = window_free[i]; bits != 0; bits &= bits - 1) {
                pages++;
            }
        }
        return pages * 4;
    }
    return efi_free_kib();
}

/* ---- what is where ----------------------------------------------------- */

/* Defined by kernel.ld and start.asm; only their addresses mean anything. */
extern char __kernel_start[], __bss_start[], __bss_end[], stack_bottom[], stack_top[];

static uint32_t span(const char *start, const char *end) {
    return (uint32_t)((uintptr_t)end - (uintptr_t)start);
}

void mem_get_stats(struct mem_stats *stats) {
    stats->total_kib = (uint32_t)efi_boot()->ram_kib;
    stats->free_kib = (uint32_t)mem_free_kib();
    stats->used_kib = stats->total_kib > stats->free_kib ?
                      stats->total_kib - stats->free_kib : 0;
    stats->image = span(__kernel_start, __bss_start);
    stats->stack = span(stack_bottom, stack_top);
    stats->data = span(__bss_start, __bss_end) - stats->stack;
    stats->page_tables = (uint32_t)program_tables();
    stats->console = (uint32_t)vga_memory();
    stats->disk_cache = (uint32_t)ata_cache_memory();
    stats->wallpaper = bg_memory();
    stats->window = (uint32_t)program_memory();
    /* What the machine costs, which is everything above but the window: that
       is what whatever is running has borrowed, and it comes and goes with
       it. Leaving it in made this figure depend on who was asking - the
       title bar, drawn only while the machine waits for a key, could never
       agree with a program that measured memory while running. The window is
       reported on its own, by whatever wants to show it. */
    stats->kernel_kib = (stats->image + stats->data + stats->stack + stats->page_tables +
                       stats->console + stats->wallpaper + stats->disk_cache +
                       1023) / 1024;

    /* The stack started out zeroed, so its deepest non-zero byte marks how
       far it has ever grown. */
    uint32_t untouched = 0;
    while (untouched < stats->stack && stack_bottom[untouched] == 0) {
        untouched++;
    }
    stats->stack_peak = stats->stack - untouched;
}
