#include "mem.h"

#include "ata.h"
#include "driver.h"
#include "boot.h"
#include "debug.h"
#include "efi.h"
#include "efi_kernel.h"
#include "string.h"
#include "module.h"
#include "net.h"
#include "syscall.h"
#include "vga.h"
#include "vm.h"

/* ---- the pages ------------------------------------------------------------
 *
 * Free memory is a list of runs, sorted by address, and taken from the top
 * down: the bottom of memory is where a program linked to a fixed address
 * has to be (vm.c), so the kernel keeps out of it for as long as it can.
 * Freeing puts
 * a run back and joins it to its neighbours, so the list stays about as
 * long as the memory map was. Should it ever fill, the smallest run is let
 * go of rather than refusing the free - a few pages lost, never a crash. */

#define PAGE  4096
#define RUNS  64

static struct run {
    uint64_t at, count;
} runs[RUNS];
static unsigned run_count;
static bool     ours;

/* Low memory the firmware keeps for good - its runtime services, ACPI, and
   what it reserves - which a program linked to a fixed address may map over
   (vm.c), since nothing but a call into the firmware ever touches it. */
#define KEPT_LOW 32

static struct run kept_low[KEPT_LOW];
static unsigned   kept_low_count;

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

/* Takes page out of whatever run holds it. False if none does. */
static bool run_take(uint64_t page) {
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
        return true;
    }
    return false;
}

static uint64_t take(size_t count);

/* Memory the disk cache holds is free memory lent out: before an allocation
   fails, the cache gives some back, as Linux's page cache does. */
uint64_t mem_pages(size_t count) {
    uint64_t at;

    while ((at = take(count)) == 0 && disk_cache != NULL &&
           disk_cache->shrink(count * PAGE) > 0) {
    }
    return at;
}

static uint64_t take(size_t count) {
    if (!ours) {
        uint64_t at = 0;

        return EFI_ERROR(firmware()->allocate_pages(EFI_ALLOCATE_ANY, EFI_LOADER_DATA,
                                                    count, &at)) ? 0 : at;
    }
    for (unsigned i = run_count; i-- > 0;) {
        if (runs[i].count >= count) {
            runs[i].count -= count;             /* off the top end of it */
            uint64_t at = runs[i].at + runs[i].count * PAGE;

            if (runs[i].count == 0) {
                run_remove(i);
            }
            return at;
        }
    }
    return 0;
}

static uint64_t take_below(size_t count, uint64_t limit);

uint64_t mem_pages_below(size_t count, uint64_t limit) {
    uint64_t at;

    while ((at = take_below(count, limit)) == 0 && disk_cache != NULL &&
           disk_cache->shrink(count * PAGE) > 0) {
    }
    return at;
}

static uint64_t take_below(size_t count, uint64_t limit) {
    if (!ours) {
        uint64_t at = limit - 1;

        return EFI_ERROR(firmware()->allocate_pages(EFI_ALLOCATE_MAX, EFI_LOADER_DATA,
                                                    count, &at)) ? 0 : at;
    }
    for (unsigned i = run_count; i-- > 0;) {
        uint64_t end = runs[i].at + runs[i].count * PAGE;

        if (end > limit) {
            end = limit;
        }
        if (end >= runs[i].at + count * PAGE) {
            uint64_t at = end - count * PAGE;

            for (size_t k = 0; k < count; k++) {
                run_take(at + k * PAGE);
            }
            return at;
        }
    }
    return 0;
}

bool mem_take_page(uint64_t page) {
    uint64_t at = page;

    if (!ours) {
        return !EFI_ERROR(firmware()->allocate_pages(EFI_ALLOCATE_ADDRESS,
                                                     EFI_LOADER_DATA, 1, &at));
    }
    return run_take(page);
}

bool mem_firmware_kept(uint64_t page) {
    for (unsigned i = 0; i < kept_low_count; i++) {
        if (page >= kept_low[i].at && page < kept_low[i].at + kept_low[i].count * PAGE) {
            return true;
        }
    }
    return false;
}

void mem_give_page(uint64_t page) {
    mem_pages_free(page, 1);
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

/* ---- the kernel's own page tables -----------------------------------------
 *
 * The firmware's tables map memory four kilobytes at a time wherever it
 * guarded or write-protected something of its own: over a hundred pages of
 * tables, for protection that is nobody's once it is gone. The kernel's are
 * all of memory one to one, readable, writable and runnable, a gigabyte an
 * entry: two pages. The first gigabyte gets a directory of two-megabyte
 * entries only once a program linked to a fixed address wants it (vm.c) -
 * here, if one already has. A processor without gigabyte pages gets a
 * directory a gigabyte up to the top of memory - four at least, for what is
 * mapped below 4 GiB. */

#define PRESENT 0x01
#define WRITE   0x02
#define HUGE    0x80

static uint64_t own_cr3;            /* the tables, contiguous: top, pdpt, directories */
static size_t   own_pages;

#define ACPI_RECLAIM 9              /* the memory type of ACPI's tables */

static uint64_t reclaimed_kib;      /* of what the firmware kept, taken back */

bool mem_own_tables(uint64_t top) {
    uint32_t a = 0x80000001, b, c = 0, d;

    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    bool gig = (d >> 26) & 1;
    uint64_t gigs = gig ? (vm_low_used() ? 1 : 0) : (top + (1ull << 30) - 1) >> 30;

    if (!gig && gigs < 4) {
        gigs = 4;
    }
    if (gigs > 512) {
        gigs = 512;
    }
    size_t count = 2 + (size_t)gigs;
    uint64_t at = mem_pages(count);

    if (at == 0) {
        return false;
    }
    uint64_t *pml4 = (uint64_t *)at, *pdpt = pml4 + 512, *pd = pdpt + 512;

    memset(pml4, 0, PAGE);
    for (uint64_t i = 0; i < 512; i++) {
        pdpt[i] = i < gigs ? (uint64_t)&pd[i * 512] | PRESENT | WRITE
                : gig      ? (i << 30) | PRESENT | WRITE | HUGE : 0;
    }
    for (uint64_t i = 0; i < gigs * 512; i++) {
        pd[i] = (i << 21) | PRESENT | WRITE | HUGE;
    }
    pml4[0] = (uint64_t)pdpt | PRESENT | WRITE;
    own_cr3 = at;
    own_pages = count;
    return true;
}

/* The loader takes KERNEL_BYTES for the kernel, which is what it may grow
   to, not what it is: whatever lies past the end of its .bss is handed back
   as soon as it runs. */
void mem_trim_kernel(void) {
    extern char __kernel_start[], __bss_end[];
    uint64_t from = ((uint64_t)__bss_end + PAGE - 1) & ~(uint64_t)(PAGE - 1);
    uint64_t to = (uint64_t)__kernel_start + KERNEL_BYTES;

    if (to > from) {
        mem_pages_free(from, (to - from) / PAGE);
    }
}

void mem_take_over(const void *map, size_t size, size_t stride) {
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idtr;
    uint64_t kept[16] = { 0 };      /* KiB not taken, by type: for the log */

    /* Onto the kernel's own tables, and what programs had hung off the
       firmware's with them: the firmware's are then as free as the rest of
       its memory. */
    vm_move((uint64_t *)own_cr3, own_pages > 2 ? (uint64_t *)(own_cr3 + 2 * PAGE) : NULL);
    __asm__ volatile("mov %0, %%cr3" : : "r"(own_cr3) : "memory");

    for (size_t at = 0; at < size; at += stride) {
        const struct efi_memory_descriptor *d = (const void *)((const char *)map + at);
        uint64_t start = d->physical, count = d->pages;

        if (d->type < 16) {
            kept[d->type] += count * 4;
        }

        if (d->type != EFI_CONVENTIONAL_MEMORY && d->type > 4 && d->type != ACPI_RECLAIM &&
            start < (1ull << 30) && kept_low_count < KEPT_LOW) {
            kept_low[kept_low_count++] = (struct run){ start, count };
        }
        /* Free, or the firmware's for as long as it was running - its code,
           its data, and the loader it started - or ACPI's tables, which
           nothing here reads. Not loader data: that is every page the
           kernel has, itself included. */
        if (d->type == ACPI_RECLAIM) {
            reclaimed_kib += count * 4;
        } else if (d->type != EFI_CONVENTIONAL_MEMORY && d->type != 1 &&
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
    dbg("mem: kept KiB - loader data %u, runtime code %u, runtime data %u, "
        "ACPI %u, ACPI NVS %u, reserved %u\n", kept[2], kept[5], kept[6], kept[9],
        kept[10], kept[0]);
    /* The firmware's IDT, which the kernel's handlers were put in. */
    __asm__ volatile("sidt %0" : "=m"(idtr));
    for (uint64_t page = idtr.base & ~(uint64_t)(PAGE - 1); page <= idtr.base + idtr.limit;
         page += PAGE) {
        run_take(page);
    }
    ours = true;
}

uint64_t mem_free_kib(void) {
    uint64_t pages = 0;

    if (ours) {
        for (unsigned i = 0; i < run_count; i++) {
            pages += runs[i].count;
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
    stats->ram_kib = (uint32_t)efi_boot()->ram_kib;
    stats->firmware_kib = (uint32_t)(efi_boot()->firmware_kib - reclaimed_kib);
    stats->total_kib = stats->ram_kib - stats->firmware_kib;
    stats->free_kib = (uint32_t)mem_free_kib();
    stats->used_kib = stats->ram_kib > stats->free_kib ?
                      stats->ram_kib - stats->free_kib : 0;
    stats->image = span(__kernel_start, __bss_start);
    stats->stack = span(stack_bottom, stack_top);
    stats->data = span(__bss_start, __bss_end) - stats->stack;
    stats->page_tables = (uint32_t)(program_tables() + own_pages * PAGE);
    stats->console = (uint32_t)vga_memory();
    stats->disk_cache = disk_cache != NULL ? (uint32_t)disk_cache->memory() : 0;
    stats->modules = module_memory();
    stats->network = net != NULL ? net->memory() : 0;
    stats->window = (uint32_t)program_memory();
    /* What the machine costs, which is everything above but the window: that
       is what whatever is running has borrowed, and it comes and goes with
       it. Leaving it in made this figure depend on who was asking - the
       title bar, drawn only while the machine waits for a key, could never
       agree with a program that measured memory while running. The window is
       reported on its own, by whatever wants to show it. */
    stats->kernel_kib = (stats->image + stats->data + stats->stack + stats->page_tables +
                       stats->console + stats->network + stats->modules +
                       1023) / 1024;

    /* The stack started out zeroed, so its deepest non-zero byte marks how
       far it has ever grown. */
    uint32_t untouched = 0;
    while (untouched < stats->stack && stack_bottom[untouched] == 0) {
        untouched++;
    }
    stats->stack_peak = stats->stack - untouched;
}
