#include "mem.h"
#include "thread.h"

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
#include "ia32.h"

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
#define RUNS  512           /* a program at a fixed address hands its pages
                                   back one by one, between other programs'
                                   own: every hole is a run (8 KiB in all) */

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

/* Single pages, chained through their own first word: what processes are
   made of, taken and given back one at a time in no order at all. Kept apart
   from the list of runs, they cannot break it into thousands of pieces - and
   a list that overflowed lost pages, a fixed-address program's among them.
   It is filled sixteen at a time from the top run, and also takes what the
   list ever has no room for. Only pages above the gigabyte a program at a
   fixed address can hide (FIXED_TOP below): one down there could be under
   such a program while its first word is written - and, on i386, below
   IA32_EXEC too, the pool where no program goes (take), which on a machine
   with no more than a gigabyte is where every single page of the kernel's
   comes from. */
#define LOOSE_FLOOR (1ull << 30)

#ifdef __i386__
#define LOOSE_OK(at) ((at) >= LOOSE_FLOOR || ((at) < IA32_EXEC && (at) >= 0x100000))
#else
#define LOOSE_OK(at) ((at) >= LOOSE_FLOOR)
#endif

#define LOOSE_FILL  16

static uint64_t loose, loose_count;

/* Pages of the 32-bit programs' gigabyte (take, below) that the list of
   runs has no room for: a program frees its pages there one by one, every
   other one where two programs took turns, and each is a run of its own.
   They cannot be chained - a program may want any of them back at its own
   address - so they are a bit each in a map instead, made out of the pool
   the first time one is needed: 28 KiB for the whole gigabyte. */
#define POOL_TOP  IA32_EXEC
#define FIXED_END (1ull << 30)
#define WIN_PAGES ((uint32_t)((FIXED_END - POOL_TOP) / PAGE))

static uint8_t *spill;
static uint32_t spill_count, spill_next;

static uint64_t take_below(size_t count, uint64_t limit);

/* The map, made while the pool still has room for it in one piece: as the
   kernel takes the machine's memory, where there is RAM in the gigabyte. */
static void spill_make(uint64_t top) {
    uint64_t map;

    if (spill == NULL && top > POOL_TOP &&
        (map = take_below((WIN_PAGES / 8 + PAGE - 1) / PAGE, POOL_TOP)) != 0) {
        spill = (uint8_t *)(uintptr_t)map;
        memset(spill, 0, WIN_PAGES / 8);
    }
}

static bool spill_put(uint64_t at) {
    static bool making;

    if (at < POOL_TOP || at >= FIXED_END) {
        return false;
    }
    if (spill == NULL) {
        uint64_t map;

        if (making) {
            return false;
        }
        making = true;
        map = take_below((WIN_PAGES / 8 + PAGE - 1) / PAGE, POOL_TOP);
        making = false;
        if (map == 0) {
            return false;
        }
        spill = (uint8_t *)(uintptr_t)map;
        memset(spill, 0, WIN_PAGES / 8);
    }
    uint32_t i = (uint32_t)((at - POOL_TOP) / PAGE);

    if (!(spill[i >> 3] & 1u << (i & 7))) {
        spill[i >> 3] |= (uint8_t)(1u << (i & 7));
        spill_count++;
    }
    return true;
}

static bool spill_take(uint64_t at) {
    uint32_t i = (uint32_t)((at - POOL_TOP) / PAGE);

    if (spill == NULL || at < POOL_TOP || at >= FIXED_END || !(spill[i >> 3] & 1u << (i & 7))) {
        return false;
    }
    spill[i >> 3] &= (uint8_t)~(1u << (i & 7));
    spill_count--;
    return true;
}

/* One of them that no program has another page at, or 0. */
static uint64_t spill_any(void) {
    for (uint32_t n = 0; spill_count > 0 && n < WIN_PAGES; n++) {
        uint32_t i = (spill_next + n) % WIN_PAGES;
        uint64_t at = POOL_TOP + (uint64_t)i * PAGE;

        if ((spill[i >> 3] & 1u << (i & 7)) && !vm_low_shadowed(at)) {
            spill_take(at);
            spill_next = i;
            return at;
        }
    }
    return 0;
}

static void loosen(uint64_t at, uint64_t count) {
    for (uint64_t i = 0; i < count; i++, at += PAGE) {
        if (!LOOSE_OK(at)) {
            if (!spill_put(at)) {
                dbg("mem: runs full, page %x lost\n", at);
            }
            continue;
        }
        *(uint64_t *)at = loose;
        loose = at;
        loose_count++;
    }
}

/* Puts at .. at + count back, joined to whatever it touches. On i386 only
   what is below 4 GiB, all the kernel's tables map. */
static void run_add(uint64_t at, uint64_t count) {
    unsigned i = 0;

#ifdef __i386__
    if (at >= (1ull << 32)) {
        return;
    }
    if (at + count * PAGE > (1ull << 32)) {
        count = ((1ull << 32) - at) / PAGE;
    }
#endif
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
        /* The smallest run goes onto the loose chain - one above the
           gigabyte if there is one, since only those can be chained: a low
           page with nowhere to go would be lost. */
        unsigned smallest = RUNS;

        for (unsigned j = 0; j < RUNS; j++) {
            bool high = LOOSE_OK(runs[j].at);
            bool best_high = smallest < RUNS && LOOSE_OK(runs[smallest].at);

            if (smallest == RUNS || (high && !best_high) ||
                (high == best_high && runs[j].count < runs[smallest].count)) {
                smallest = j;
            }
        }
        if (LOOSE_OK(at) && (!LOOSE_OK(runs[smallest].at) || runs[smallest].count >= count)) {
            loosen(at, count);                  /* this one is the smallest */
            return;
        }
        loosen(runs[smallest].at, runs[smallest].count);
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
#ifdef __i386__
static uint64_t take_outside(size_t count);
#endif

/* The gigabyte a program linked to a fixed address runs in. While one runs,
   its own tables stand in for that stretch of the kernel's view of memory
   (vm.c), so whatever the kernel keeps there reads as the program's pages
   instead. On a machine with memory above it, the kernel keeps out. */
#define FIXED_TOP (1ull << 30)

__attribute__((unused)) static uint64_t floor_kept(void) {
    return ours && efi_boot()->ram_kib > (FIXED_TOP >> 10) ? FIXED_TOP : 0;
}

/* Whether at .. count pages lands where a program in low memory would hide
   it, on a machine with memory to spare elsewhere: below the gigabyte - on
   i386, where every program is a 32-bit one, in its gigabyte from IA32_EXEC
   up, on any machine: the disk cache gives the pool back first, and lends
   nothing there at all, since a program there wants each page at its own
   address and cannot have one the cache holds. */
static bool where_hidden(uint64_t at, size_t count) {
#ifdef __i386__
    return at < (1ull << 30) && at + count * 4096 > IA32_EXEC;
#else
    (void)count;
    return at < floor_kept();
#endif
}

/* Memory the disk cache holds is free memory lent out: before an allocation
   fails - or lands where a program would hide it - the cache gives some
   back, as Linux's page cache does. */
uint64_t mem_pages(size_t count) {
    for (;;) {
        uint64_t at = take(count);

        if ((at != 0 && !where_hidden(at, count)) || disk_cache == NULL ||
            disk_cache->shrink(count * PAGE) == 0) {
            if (ours && at != 0 && at < (64u << 20) && sizeof(void *) == 8) {
                dbg("mem: %u pages at %x for %p\n", (uint64_t)count, at, __builtin_return_address(0));
            }
            return at;
        }
        if (at != 0) {
            run_add(at, count);
        }
    }
}

/* Pages for the disk cache: only from above that gigabyte, where there is
   memory there, so that lending it out never pushes the kernel below. */
uint64_t mem_pages_lent(size_t count) {
#ifdef __i386__
    return take_outside(count);
#endif
    uint64_t at = take(count);

    if (at != 0 && where_hidden(at, count)) {
        mem_pages_free(at, count);
        return 0;
    }
    return at;
}

/* A 32-bit program lives in the first gigabyte, from IA32_EXEC up, over
   the kernel's own view of it (vm.c) - on i386 every program does. So the
   kernel's pages are kept out of there: above the gigabyte where there is
   memory, below IA32_EXEC next - nothing goes there but what a program puts
   at a fixed address itself, and the pool is used from its top down - and
   only once both are gone among the programs, and never under a page some
   program has another page at. On i386, while the firmware runs, not among
   them at all: a program may map over what the firmware holds there (vm.c),
   which would be the kernel's too. */
/* count pages of the runs within [lo, hi), from the top, none of them one
   a program has another page at; 0 if there are none. */
static uint64_t take_clear(size_t count, uint64_t lo, uint64_t hi) {
    for (unsigned i = run_count; i-- > 0;) {
        uint64_t start = runs[i].at > lo ? runs[i].at : lo;
        uint64_t end = runs[i].at + runs[i].count * PAGE < hi ? runs[i].at + runs[i].count * PAGE : hi;

        while (end >= start + count * PAGE) {
            uint64_t at = end - count * PAGE, k = 0;

            while (k < count && !vm_low_shadowed(at + k * PAGE)) {
                k++;
            }
            if (k == count) {
                for (k = 0; k < count; k++) {
                    run_take(at + k * PAGE);
                }
                return at;
            }
            end = at + k * PAGE;    /* below the one in the way */
        }
    }
    return 0;
}

#ifdef __i386__

/* Outside the programs' gigabyte, or 0: all the disk cache may have. */
static uint64_t take_outside(size_t count);

static uint64_t take(size_t count) {
    uint64_t at = take_outside(count);

    if (at == 0 && ours) {
        if ((at = take_clear(count, POOL_TOP, FIXED_END)) == 0 && count == 1) {
            at = spill_any();
        }
        dbg("mem: %u pages among the programs, at %x\n", (uint64_t)count, at);
    }
    return at;
}

static uint64_t take_outside(size_t count) {
    uint64_t at;

    if (ours && count == 1 && loose != 0) {
        at = loose;                 /* a single page above the gigabyte */
        loose = *(uint64_t *)at;
        loose_count--;
        return at;
    }
    if (!ours) {
        at = POOL_TOP - 1;
        if (!EFI_ERROR(EFI_CALL(firmware()->allocate_pages, EFI_ALLOCATE_MAX, EFI_LOADER_DATA,
                                count, &at))) {
            return at;
        }
        if (EFI_ERROR(EFI_CALL(firmware()->allocate_pages, EFI_ALLOCATE_ANY, EFI_LOADER_DATA,
                               count, &at))) {
            return 0;
        }
        if (at < FIXED_END) {
            EFI_CALL(firmware()->free_pages, at, count);
            return 0;               /* the top is the programs' */
        }
        return at;
    }
    for (bool drained = false;; drained = true) {
        if ((at = take_clear(count, FIXED_END, ~0ull)) != 0 ||
            (at = take_below(count, POOL_TOP)) != 0) {
            return at;
        }
        if (drained || loose == 0) {
            break;
        }
        /* Nothing that long outside the programs' gigabyte: the single
           pages go back into runs first, where neighbours join up again,
           for as long as the list has room. */
        while (loose != 0 && run_count < RUNS - 1) {
            at = loose;
            loose = *(uint64_t *)at;
            loose_count--;
            run_add(at, 1);
        }
    }
    return 0;
}
#else
static uint64_t take_top(size_t count);

/* x86-64's: from the top, as ever - and where that is a 32-bit program's
   gigabyte, the pool below it first, as on i386. */
static uint64_t take(size_t count) {
    uint64_t at = 0;

    if (!ours) {
        if (EFI_ERROR(EFI_CALL(firmware()->allocate_pages, EFI_ALLOCATE_ANY, EFI_LOADER_DATA,
                               count, &at))) {
            return 0;
        }
        if (at >= POOL_TOP && at < FIXED_END) {
            uint64_t low = POOL_TOP - 1;

            if (!EFI_ERROR(EFI_CALL(firmware()->allocate_pages, EFI_ALLOCATE_MAX,
                                    EFI_LOADER_DATA, count, &low))) {
                EFI_CALL(firmware()->free_pages, at, count);
                at = low;
            }
        }
        return at;
    }
    if ((at = take_top(count)) == 0 || at >= FIXED_END || at + count * PAGE <= POOL_TOP) {
        return at;
    }
    mem_pages_free(at, count);
    if ((at = take_below(count, POOL_TOP)) != 0 || (at = take_clear(count, POOL_TOP, FIXED_END)) != 0 ||
        (count == 1 && (at = spill_any()) != 0)) {
        return at;
    }
    return take_top(count);
}

static uint64_t take_top(size_t count) {
    if (count == 1 && loose == 0) {
        for (unsigned i = run_count; i-- > 0;) {
            if (runs[i].count >= LOOSE_FILL && runs[i].at + runs[i].count * PAGE >
                LOOSE_FLOOR + LOOSE_FILL * PAGE) {
                runs[i].count -= LOOSE_FILL;    /* off the top end, as below */
                uint64_t at = runs[i].at + runs[i].count * PAGE;

                if (runs[i].count == 0) {
                    run_remove(i);
                }
                if (at < LOOSE_FLOOR) {
                    run_add(at, LOOSE_FILL);    /* straddles the floor: as it was */
                    break;
                }
                loosen(at, LOOSE_FILL);
                break;
            }
        }
    }
    if (count == 1 && loose != 0) {
        uint64_t at = loose;

        loose = *(uint64_t *)at;
        loose_count--;
        return at;
    }
    for (bool drained = false;; drained = true) {
        for (unsigned i = run_count; i-- > 0;) {
            if (runs[i].count >= count) {
                runs[i].count -= count;         /* off the top end of it */
                uint64_t at = runs[i].at + runs[i].count * PAGE;

                if (runs[i].count == 0) {
                    run_remove(i);
                }
                return at;
            }
        }
        if (drained || loose_count == 0) {
            return 0;
        }
        /* Nothing that long: the single pages go back into runs, where
           neighbours join up again, for as long as the list has room. */
        while (loose != 0 && run_count < RUNS - 1) {
            uint64_t at = loose;

            loose = *(uint64_t *)at;
            loose_count--;
            run_add(at, 1);
        }
    }
}
#endif

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

        return EFI_ERROR(EFI_CALL(firmware()->allocate_pages, EFI_ALLOCATE_MAX, EFI_LOADER_DATA,
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
        return !EFI_ERROR(EFI_CALL(firmware()->allocate_pages, EFI_ALLOCATE_ADDRESS,
                                      EFI_LOADER_DATA, 1, &at));
    }
    return run_take(page) || spill_take(page);
}

/* Where memory ends, as the map at take-over said; 0 until then. */
static uint64_t ram_top;

bool mem_beyond_ram(uint64_t page) {
    if (ram_top == 0) {
        ram_top = ours ? (uint64_t)-1 : efi_ram_top();
    }
    return page >= ram_top;
}

bool mem_firmware_kept(uint64_t page) {
    for (unsigned i = 0; i < kept_low_count; i++) {
        if (page >= kept_low[i].at && page < kept_low[i].at + kept_low[i].count * PAGE) {
            return true;
        }
    }
    return false;
}

bool mem_firmware_has(uint64_t page) {
#ifdef __i386__
    return !ours && page >= POOL_TOP;   /* the kernel takes none there (take) */
#else
    (void)page;
    return false;
#endif
}

void mem_give_page(uint64_t page) {
    mem_pages_free(page, 1);
}

void mem_pages_free(uint64_t at, size_t count) {
    if (!ours) {
        EFI_CALL(firmware()->free_pages, at, count);
        return;
    }
    if (count == 1 && LOOSE_OK(at)) {
        loosen(at, 1);                      /* back with the single pages */
        return;
    }
    run_add(at, count);
}

/* Each allocation starts with how many pages it is, sixteen bytes before
   what the caller gets, so that freeing needs nothing but the pointer. */
#define HEADER 16

/* Up to a kilobyte, allocations share pages instead: a page of one size -
   32 bytes to 1024, doubling - holds a struct slab at its front and as many
   of them after it as fit, each at a multiple of its size. None is ever at
   HEADER into its page, which is how mem_free tells the two kinds apart.
   A page goes back as soon as nothing in it is used. */
#define SMALL_CLASSES 6
#define SMALL_MAX     (32u << (SMALL_CLASSES - 1))

struct slab {
    struct slab *next;              /* pages of its size with room, as a list */
    void        *free;              /* its free ones, through their first word */
    uint32_t     size, used;
};

static struct slab *with_room[SMALL_CLASSES];
static size_t heap_pages;           /* all mem_alloc holds, for `mem` */

static void *small_alloc(size_t bytes) {
    unsigned c = 0;

    while ((32u << c) < bytes) {
        c++;
    }
    struct slab *s = with_room[c];

    if (s == NULL) {
        uint64_t page = mem_pages(1);
        uint32_t size = 32u << c;

        if (page == 0) {
            return NULL;
        }
        heap_pages++;
        s = (struct slab *)page;
        /* mem_pages may have had the disk cache free some first, onto
           this very list: so on to the front of it, not in place of it. */
        *s = (struct slab){ .next = with_room[c], .size = size };
        for (uint32_t at = PAGE - size; at >= size && at >= sizeof *s; at -= size) {
            *(void **)(page + at) = s->free;
            s->free = (void *)(page + at);
        }
        with_room[c] = s;
    }
    void *memory = s->free;

    s->free = *(void **)memory;
    s->used++;
    if (s->free == NULL) {
        with_room[c] = s->next;     /* full: off the list */
    }
    return memory;
}

static void small_free(void *memory) {
    struct slab *s = (struct slab *)((uint64_t)(uintptr_t)memory & ~(uint64_t)(PAGE - 1));
    unsigned c = 0;

    while ((32u << c) < s->size) {
        c++;
    }
    if (s->free == NULL) {
        s->next = with_room[c];     /* had no room: back on the list */
        with_room[c] = s;
    }
    *(void **)memory = s->free;
    s->free = memory;
    if (--s->used == 0) {
        struct slab **at = &with_room[c];

        while (*at != NULL && *at != s) {
            at = &(*at)->next;
        }
        if (*at == s) {
            *at = s->next;
        }
        heap_pages--;
        mem_pages_free((uint64_t)(uintptr_t)s, 1);
    }
}

void *mem_alloc(size_t bytes) {
    if (bytes <= SMALL_MAX) {
        return small_alloc(bytes);
    }
    size_t count = (bytes + HEADER + PAGE - 1) / PAGE;
    uint64_t at = mem_pages(count);

    if (at == 0) {
        return NULL;
    }
    heap_pages += count;
    *(uint64_t *)at = count;
    return (void *)(at + HEADER);
}

void mem_free(void *memory) {
    if (memory == NULL) {
        return;
    }
    if (((uint64_t)(uintptr_t)memory & (PAGE - 1)) != HEADER) {
        small_free(memory);
        return;
    }
    uint64_t at = (uint64_t)(uintptr_t)memory - HEADER;

    heap_pages -= *(uint64_t *)at;
    mem_pages_free(at, *(uint64_t *)at);
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

static bool gig_pages(void) {
    uint32_t a = 0x80000001, b, c = 0, d;

    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return (d >> 26) & 1;
}

/* Devices' registers can be anywhere in the address space - a firmware
   puts 64-bit windows far above any RAM, past the 512 GiB the kernel's
   tables map - so each one a driver asks for (pci_memory) is remembered
   and its gigabyte mapped, uncached, as the tables are built or straight
   away if they already are. */
#define IO_SPANS 8
#define NO_CACHE 0x18               /* PWT, PCD */

static uint64_t io_spans[IO_SPANS];
static unsigned io_count;
static size_t   io_pages;           /* tables bought for them */

static uint64_t *table_for(uint64_t *entry, bool *fresh) {
    if (*entry & PRESENT) {
        *fresh = false;
        return (uint64_t *)(*entry & 0x000FFFFFFFFFF000ull);
    }
    uint64_t page = mem_pages_below(1, 1ull << 32);

    if (page == 0) {
        return NULL;
    }
    memset((void *)page, 0, PAGE);
    io_pages++;
    *entry = page | PRESENT | WRITE;
    *fresh = true;
    return (uint64_t *)page;
}

static void map_gig(uint64_t *pml4, uint64_t gig) {
    bool fresh;
    uint64_t *pdpt = table_for(&pml4[gig >> 39 & 511], &fresh), *pd;

    if (pdpt == NULL || (pdpt[gig >> 30 & 511] & PRESENT)) {
        return;
    }
    if (gig_pages()) {
        pdpt[gig >> 30 & 511] = gig | PRESENT | WRITE | HUGE | NO_CACHE;
        return;
    }
    if ((pd = table_for(&pdpt[gig >> 30 & 511], &fresh)) != NULL) {
        for (uint64_t i = 0; i < 512; i++) {
            pd[i] = (gig + (i << 21)) | PRESENT | WRITE | HUGE | NO_CACHE;
        }
    }
}

static void reload_cr3(void) {
    uintptr_t cr3;

    __asm__ volatile("mov %%cr3, %0\n\tmov %0, %%cr3" : "=r"(cr3) : : "memory");
}

void mem_map_io(uint64_t at) {
    uint64_t gig = at & ~((1ull << 30) - 1);

#ifdef __i386__
    (void)gig;                      /* the four gigabytes there are: all mapped */
    return;
#endif
    if (gig < (512ull << 30) && gig_pages()) {
        return;                     /* the first table maps it already */
    }
    for (unsigned i = 0; i < io_count; i++) {
        if (io_spans[i] == gig) {
            return;
        }
    }
    if (io_count < IO_SPANS) {
        io_spans[io_count++] = gig;
    }
    if (own_cr3 != 0 && ours) {
        map_gig((uint64_t *)own_cr3, gig);
        reload_cr3();
    }
}

#ifdef __i386__
/* i386's: a PDPT and its four directories of two-megabyte pages, the four
   gigabytes one to one - five pages, the PDPT's first. What PAE reserves
   in a PDPT entry (WRITE, USER) is left out there. */
bool mem_own_tables(uint64_t top) {
    (void)top;
    if (own_cr3 != 0) {
        return true;                /* built at boot already (mem_paging_on) */
    }
    uint64_t at = mem_pages_below(5, POOL_TOP);

    if (at == 0) {
        return false;
    }
    uint64_t *pdpt = (uint64_t *)(uintptr_t)at, *pd = pdpt + 512;

    memset(pdpt, 0, PAGE);
    for (uint64_t i = 0; i < 4; i++) {
        pdpt[i] = ((uint64_t)(uintptr_t)&pd[i * 512]) | PRESENT;
    }
    for (uint64_t i = 0; i < 4 * 512; i++) {
        pd[i] = (i << 21) | PRESENT | WRITE | HUGE;
    }
    own_cr3 = at;
    own_pages = 5;
    return true;
}

/* Onto those tables now, with PAE paging on whatever was before it: a
   32-bit firmware runs with paging off, and the kernel needs it from its
   first program on. One to one, so the firmware runs on as it did. CR4.PGE
   goes off: vm.c keeps a bit of its own where the global bit would be. */
bool mem_paging_on(void) {
    uintptr_t cr0, cr4;

    if (!mem_own_tables(0)) {
        return false;
    }
    vm_move((uint64_t *)(uintptr_t)own_cr3, NULL);
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0 & ~0x80000000u) : "memory");
    __asm__ volatile("mov %0, %%cr4" : : "r"((cr4 | 0x20) & ~(uintptr_t)0x80) : "memory");
    __asm__ volatile("mov %0, %%cr3" : : "r"((uintptr_t)own_cr3) : "memory");
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0 | 0x80000000u) : "memory");
    return true;
}
#else

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
    /* Below 4 GiB: the BIOS's way into real mode (bios.asm) has to load
       CR3 again from 32-bit code on the way back. */
    uint64_t at = mem_pages_below(count, 1ull << 32);

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
    for (unsigned i = 0; i < io_count; i++) {
        map_gig(pml4, io_spans[i]);
    }
    return true;
}
#endif

/* The loader takes KERNEL_BYTES for the kernel, which is what it may grow
   to, not what it is: whatever lies past the end of its .bss is handed back
   as soon as it runs. */
void mem_trim_kernel(void) {
    extern char __kernel_start[], __bss_end[];
    uint64_t from = ((uint64_t)(uintptr_t)__bss_end + PAGE - 1) & ~(uint64_t)(PAGE - 1);
    uint64_t to = (uint64_t)(uintptr_t)__kernel_start + KERNEL_BYTES;

    if (to > from) {
        mem_pages_free(from, (to - from) / PAGE);
    }
}

void mem_take_over(const void *map, size_t size, size_t stride) {
    struct { uint16_t limit; uintptr_t base; } __attribute__((packed)) idtr;
    uint64_t kept[16] = { 0 };      /* KiB not taken, by type: for the log */

#ifdef __i386__
    /* i386's tables are the ones built at boot, which the firmware has been
       running on - and may have changed, a page it guarded unmapped: every
       entry made one to one again, but for a program's table in the first
       gigabyte (USER marks those). */
    uint64_t *pdpt = (uint64_t *)(uintptr_t)own_cr3, *pd = pdpt + 512;

    for (uint64_t i = 0; i < 4; i++) {
        pdpt[i] = ((uint64_t)(uintptr_t)&pd[i * 512]) | PRESENT;
    }
    for (uint64_t i = 0; i < 4 * 512; i++) {
        if (i >= 512 || !(pd[i] & 0x04)) {
            pd[i] = (i << 21) | PRESENT | WRITE | HUGE;
        }
    }
#endif
    /* Onto the kernel's own tables, and what programs had hung off the
       firmware's with them: the firmware's are then as free as the rest of
       its memory. */
    vm_move((uint64_t *)own_cr3, own_pages > 2 ? (uint64_t *)(own_cr3 + 2 * PAGE) : NULL);
    __asm__ volatile("mov %0, %%cr3" : : "r"((uintptr_t)own_cr3) : "memory");

    for (size_t at = 0; at < size; at += stride) {
        const struct efi_memory_descriptor *d = (const void *)((const char *)map + at);
        uint64_t start = d->physical, count = d->pages;

        if (d->type < 16) {
            kept[d->type] += count * 4;
        }
        if (d->type >= 1 && d->type <= 10 && d->type != 8 && start + count * PAGE > ram_top) {
            ram_top = start + count * PAGE;     /* memory, not reserved or a device's */
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
    for (uint64_t page = idtr.base & ~(uintptr_t)(PAGE - 1); page <= idtr.base + idtr.limit;
         page += PAGE) {
        run_take(page);
    }
    ours = true;
    spill_make(ram_top);
}

/* The BIOS path: no firmware to allocate from at all, so the kernel's own
   runs are made at once, from the RAM E820 reports above 1 MiB - below it
   is the BIOS's and the loader's, all but what the loader gave the kernel -
   and the loader's tables are left for the kernel's own. */
void mem_take_bios(const struct e820 *map, unsigned count, struct boot_info *info) {
    uint64_t top = vga_framebuffer_end(), ram = 0, kept = 0;

    for (unsigned i = 0; i < count; i++) {
        uint64_t start = (map[i].base + PAGE - 1) & ~(uint64_t)(PAGE - 1);
        uint64_t end = (map[i].base + map[i].length) & ~(uint64_t)(PAGE - 1);

        if (map[i].type == 1 || map[i].type == 3 || map[i].type == 4) {
            /* On i386, only what is below 4 GiB: the rest is not reached. */
            ram += (sizeof(void *) == 4 && end > (1ull << 32) ?
                    (start < (1ull << 32) ? (1ull << 32) - start : 0) : map[i].length) / 1024;
            top = end > top ? end : top;
            ram_top = end > ram_top ? end : ram_top;
        }
        if (map[i].type == 4) {
            kept += map[i].length / 1024;   /* ACPI NVS */
        }
        if (map[i].type != 1 && map[i].base < (1ull << 30) && kept_low_count < KEPT_LOW) {
            kept_low[kept_low_count++] = (struct run){ map[i].base & ~(uint64_t)(PAGE - 1),
                                                       (map[i].length + PAGE - 1) / PAGE };
        }
        if (map[i].type != 1) {
            continue;
        }
        if (start < 0x100000) {
            start = 0x100000;
        }
        if (end > start) {
            run_add(start, (end - start) / PAGE);
        }
    }
    ours = true;
    spill_make(ram_top);
    info->ram_kib = ram;
    info->firmware_kib = kept + 640;    /* and what is below 1 MiB */
    info->memory_kib = mem_free_kib();
    if (mem_own_tables(top)) {
        vm_move((uint64_t *)own_cr3, own_pages > 2 ? (uint64_t *)(own_cr3 + 2 * PAGE) : NULL);
        __asm__ volatile("mov %0, %%cr3" : : "r"((uintptr_t)own_cr3) : "memory");
    }
}

uint64_t mem_free_kib(void) {
    uint64_t pages = 0;

    if (ours) {
        for (unsigned i = 0; i < run_count; i++) {
            pages += runs[i].count;
        }
        return (pages + loose_count + spill_count) * 4;
    }
    return efi_free_kib();
}

uint64_t mem_used_kib(void) {
    uint64_t gone = efi_boot()->firmware_kib - reclaimed_kib + mem_free_kib() +
                    (disk_cache != NULL ? disk_cache->memory() / 1024 : 0);

    return efi_boot()->ram_kib > gone ? efi_boot()->ram_kib - gone : 0;
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
    stats->page_tables = (uint32_t)(program_tables() + (own_pages + io_pages) * PAGE);
    stats->console = (uint32_t)vga_memory();
    /* Less the console's cells, which are mem_alloc'd and counted above. */
    size_t console_pages = (stats->console + HEADER + PAGE - 1) / PAGE;

    stats->heap = (uint32_t)((heap_pages > console_pages ? heap_pages - console_pages : 0) * PAGE);
    stats->stack += (uint32_t)thread_stacks();
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
                       stats->console + stats->network + stats->modules + stats->heap +
                       1023) / 1024;

    /* The stack started out zeroed, so its deepest non-zero byte marks how
       far it has ever grown. */
    uint32_t untouched = 0, own = span(stack_bottom, stack_top);
    while (untouched < own && stack_bottom[untouched] == 0) {
        untouched++;
    }
    stats->stack_peak = own - untouched;
    stats->thread_peak = thread_stack_peak();
}
