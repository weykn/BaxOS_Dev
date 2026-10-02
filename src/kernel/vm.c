#include "vm.h"

#include "debug.h"
#include "efi.h"
#include "efi_kernel.h"
#include "mem.h"
#include "string.h"

/* x86-64 paging, four levels of it: a virtual address is four nine-bit
 * indexes and an offset, and each level holds physical addresses of the next.
 * Only one slot of the top level is ours per region, which is half a terabyte
 * of address space - far more than anything here will use, and free, which is
 * what matters: nothing of the firmware's is anywhere near it. */

#define PRESENT 0x001
#define WRITE   0x002
#define USER    0x004
#define BIG     0x080
#define ADDR    0x000FFFFFFFFFF000ull

#define ENTRIES   512
#define SLOT_SIZE (1ull << 39)      /* what one top-level entry covers */

/* Programs inside programs. One more than the deepest nesting allowed, since
   the outermost region is a level too. */
#define LEVELS 5

/* Pages a forked child may change before its parent can no longer be put
   back exactly. A child that has not started a program of its own is a few
   lines of a shell's own code - a dozen or so pages - so this is far more
   than one ever touches. */
#define UNDO_MAX 512

/* One page the child wrote to, and what was under it. copy is a page holding
   what the parent had there; zero means the parent had nothing there, and
   the page goes away again. */
struct undo {
    uint64_t at, copy;
};

/* Pages are bought from the firmware a chunk at a time rather than one at a
   time. A call to firmware costs about the same whatever it is for, and a C
   library is five hundred pages: asking for each of them separately was most
   of what starting a program cost.

   A chunk's own first page holds where the chunk before it was, so that the
   whole lot can be handed back at the end without a list of them anywhere
   else. */
#define CHUNK_FIRST 8               /* what the first one costs a small program:
                                       the shell's code, data, stack and the
                                       two tables under them, and the chunk's
                                       own page */
#define CHUNK_MAX   64              /* and a quarter of a megabyte once it is
                                       clearly big: past that the tail of the
                                       last chunk wastes more than the extra
                                       calls cost */

struct chunk {
    uint64_t before;                /* the chunk allocated before this one */
    uint64_t pages;                 /* what was asked for, to give back */
};

static struct level {
    uint64_t base;              /* the region's first address */
    unsigned slot;              /* its slot in the top-level table */
    uint64_t bought;            /* taken from the firmware for it, chunks and
                                   all - which is what it really costs, not
                                   what it has been handed so far */
    uint64_t tables;            /* its own top-level table, bought on its own */
    uint64_t chunks;            /* the last chunk bought, 0 for none */
    uint64_t next, left;        /* the next page in it, and how many are left */
    uint64_t grow;              /* how big the next chunk should be */
    uint64_t spare;             /* pages given back, chained through their
                                   first word, to hand out again */
    struct undo *undo;          /* what it has changed since a fork, if any */
    unsigned  undo_count;
    bool      undo_full;        /* more than the log could hold */

    /* Low memory, for a program linked to run at a fixed address: the range
       it may use, and a page listing the page table for each two megabytes
       of it, which stand in for the firmware's own mapping while it runs. */
    uint64_t  low_start, low_end;
    uint64_t *low_tables;
    uint64_t  low_owned;        /* pages it has at their own address */
} levels[LEVELS];

/* ---- low memory ------------------------------------------------------------
 *
 * A program linked to run at a fixed address - 0x400000, where a plain `ld`
 * puts things - has to be there, and there is where the firmware maps the
 * first gigabyte of the machine one to one. So while it runs, the firmware's
 * page directory for that gigabyte has the program's own tables in place of
 * the two-megabyte stretches it uses, and they are put back when it stops.
 *
 * That hides whatever is at those physical addresses, so nothing else may be
 * there: each page the program is given is the one at its own address, taken
 * so that nothing else can have it - or, where a program underneath already
 * has that page or the firmware keeps it for its runtime services and ACPI,
 * any page at all: those are not touched but through calls that put the
 * firmware's view back first (vm_firmware_view). A page held by anything
 * else means the program cannot run. The kernel takes its own memory from the top down, so
 * on any real machine the bottom is free for this. */

#define LOW_LIMIT (1ull << 30)      /* the gigabyte one page directory covers */
#define OWNED     0x200             /* an entry whose page is at its own address */

static uint64_t *low_pd;            /* the firmware's directory for it */
static uint64_t *low_orig;          /* what that said before anything changed it -
                                       none on the kernel's own tables, which
                                       say two megabytes one to one */
static uint64_t  low_fixed;         /* pages bought for those two, for good */

static unsigned depth;          /* levels[depth] is the one in use */

/* Pages from mem.c: the firmware's while it is running, the kernel's own
   after. EFI_SUCCESS or not, as the callers were written for. */
static efi_status pages_from_firmware(uint64_t pages, uint64_t *at) {
    *at = mem_pages(pages);
    return *at != 0 ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
}

static void pages_back_to_firmware(uint64_t at, uint64_t pages) {
    mem_pages_free(at, pages);
}

static uint64_t *table_at(uint64_t entry) {
    return (uint64_t *)(entry & ADDR);
}

static void flush_tlb(void) {
    __asm__ volatile("mov %%cr3, %%rax\n\tmov %%rax, %%cr3" : : : "rax", "memory");
}

/* The firmware write-protects its own tables, CR0.WP, which binds even ring
   0; editing one means turning that off for as long as the edit takes. */
static uint64_t write_protect_off(void) {
    uint64_t cr0;

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0 & ~0x10000ull) : "memory");
    return cr0;
}

static void write_protect_back(uint64_t cr0) {
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

static uint64_t *pml4(void) {
    uint64_t cr3;

    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return (uint64_t *)(cr3 & ADDR);
}

/* One page of memory for a table, or for the program: one the region has
   already bought where there is one, and a fresh chunk of them where there is
   not. Cleared before it is used for anything. */
static uint64_t *page_from(struct level *level) {
    uint64_t at;

    if (level->spare != 0) {
        at = level->spare;
        level->spare = *(uint64_t *)at;
        memset((void *)at, 0, VM_PAGE);
        return (uint64_t *)at;
    }
    if (level->left == 0) {
        uint64_t want = level->grow != 0 ? level->grow : CHUNK_FIRST;
        uint64_t got = 0;
        struct chunk *head;

        /* Each chunk is twice the one before it, up to a megabyte: a program
           that only ever wants a dozen pages is charged for a dozen, and one
           loading a C library gets there in a handful of calls. */
        level->grow = want < CHUNK_MAX ? want * 2 : CHUNK_MAX;
        while (EFI_ERROR(pages_from_firmware(want, &got))) {
            if (want == 2) {
                return NULL;
            }
            want = want / 2 < 2 ? 2 : want / 2;
        }
        head = (struct chunk *)got;
        head->pages = want;
        head->before = level->chunks;
        level->bought += want * VM_PAGE;
        level->chunks = got;
        level->next = got + VM_PAGE;            /* past the chunk's own page */
        level->left = want - 1;
    }
    at = level->next;
    level->next += VM_PAGE;
    level->left--;
    memset((void *)at, 0, VM_PAGE);
    return (uint64_t *)at;
}

/* Gives one back to the region it came from, to be handed out again. */
static void page_back(struct level *level, uint64_t at) {
    *(uint64_t *)at = level->spare;
    level->spare = at;
}

/* One page from the firmware, on its own and cleared. */
static uint64_t table_page(void) {
    uint64_t at = 0;

    if (EFI_ERROR(pages_from_firmware(1, &at))) {
        return 0;
    }
    memset((void *)at, 0, VM_PAGE);
    return at;
}

/* Hands every chunk the region bought back to the firmware. Everything in
   them goes with it, so nothing may still be mapped when this runs. */
static void chunks_back(struct level *level) {
    uint64_t chunk = level->chunks;

    while (chunk != 0) {
        uint64_t before = ((struct chunk *)chunk)->before;

        pages_back_to_firmware(chunk, ((struct chunk *)chunk)->pages);
        chunk = before;
    }
    level->bought = 0;
    level->chunks = level->next = level->left = level->spare = level->grow = 0;
}

/* Claims a free top-level slot and points it at a table of its own. */
static bool slot_take(struct level *level) {
    uint64_t *top = pml4();

    /* The lower half of the address space, which is where a program may run,
       is the first 256 slots. The firmware maps what the machine has at the
       bottom of it; a slot it leaves alone is ours. */
    for (unsigned i = 1; i < 256; i++) {
        uint64_t *pdpt;
        uint64_t cr0;

        if (top[i] & PRESENT) {
            continue;
        }
        /* The region's own table is bought on its own rather than out of a
           chunk: the chunks are handed back whenever the region is emptied,
           and this has to outlive that. */
        pdpt = (uint64_t *)table_page();
        if (pdpt == NULL) {
            return false;
        }
        cr0 = write_protect_off();
        top[i] = (uint64_t)pdpt | PRESENT | WRITE | USER;
        write_protect_back(cr0);
        level->tables = VM_PAGE;
        level->slot = i;
        level->base = (uint64_t)i * SLOT_SIZE;
        flush_tlb();
        return true;
    }
    return false;
}

/* What the directory says for the two megabytes at i when no program has
   them. */
static uint64_t low_plain(unsigned i) {
    return low_orig != NULL ? low_orig[i] : ((uint64_t)i << 21) | PRESENT | WRITE | BIG;
}

/* Finds the firmware's page directory for the first gigabyte, splitting a
   one-gigabyte page into two-megabyte ones if that is how it was mapped, and
   keeps a copy of it to put back from. Once; false if it cannot be had. */
static bool low_setup(void) {
    uint64_t *top = pml4(), *pdpt;
    uint64_t cr0;

    if (low_pd != NULL) {
        return true;
    }
    if (!(top[0] & PRESENT) || !((pdpt = table_at(top[0]))[0] & PRESENT)) {
        return false;
    }
    if (mem_ours()) {
        /* The kernel's own tables: a gigabyte page, split here into
           two-megabyte ones the first time it is wanted. */
        if (pdpt[0] & BIG) {
            uint64_t *pd = (uint64_t *)table_page();

            if (pd == NULL) {
                return false;
            }
            for (unsigned i = 0; i < ENTRIES; i++) {
                pd[i] = low_plain(i);
            }
            pdpt[0] = (uint64_t)pd | PRESENT | WRITE;
            low_fixed = VM_PAGE;
        }
        top[0] |= USER;
        pdpt[0] |= USER;
        low_pd = table_at(pdpt[0]);
        flush_tlb();
        return true;
    }
    if ((low_orig = (uint64_t *)table_page()) == NULL) {
        return false;
    }
    low_fixed = VM_PAGE;
    cr0 = write_protect_off();
    top[0] |= USER;                 /* ring 3 is let through at every level */
    if (pdpt[0] & BIG) {
        uint64_t base = pdpt[0] & ADDR, flags = pdpt[0] & 0xFFF;
        uint64_t *pd = (uint64_t *)table_page();

        if (pd == NULL) {
            write_protect_back(cr0);
            return false;
        }
        for (unsigned i = 0; i < ENTRIES; i++) {
            pd[i] = (base + ((uint64_t)i << 21)) | flags | BIG;
        }
        pdpt[0] = (uint64_t)pd | PRESENT | WRITE;
        low_fixed += VM_PAGE;
    }
    pdpt[0] |= USER;
    low_pd = table_at(pdpt[0]);
    memcpy(low_orig, low_pd, VM_PAGE);
    write_protect_back(cr0);
    flush_tlb();
    return true;
}

/* Puts level's tables for its low range in the directory, or takes them out
   again for what the firmware had there. */
static void low_apply(struct level *level, bool on) {
    uint64_t cr0;

    if (level->low_tables == NULL) {
        return;
    }
    cr0 = write_protect_off();
    for (uint64_t i = level->low_start >> 21; i <= (level->low_end - 1) >> 21; i++) {
        uint64_t table = level->low_tables[i];

        low_pd[i] = on && table != 0 ? table | PRESENT | WRITE | USER : low_plain(i);
    }
    write_protect_back(cr0);
    flush_tlb();
}

/* Whether a program underneath has the page at addr at its own address, and
   so whether this one may have a page there without hiding anyone's. */
static bool low_below(uint64_t addr) {
    for (unsigned i = 0; i < depth; i++) {
        struct level *l = &levels[i];

        if (l->low_tables != NULL && addr >= l->low_start && addr < l->low_end) {
            uint64_t table = l->low_tables[addr >> 21];

            if (table != 0 && (((uint64_t *)table)[(addr >> 12) & (ENTRIES - 1)] & OWNED)) {
                return true;
            }
        }
    }
    return false;
}

bool vm_low(uint64_t start, uint64_t end) {
    struct level *level = &levels[depth];

    start &= ~(uint64_t)(VM_PAGE - 1);
    end = (end + VM_PAGE - 1) & ~(uint64_t)(VM_PAGE - 1);
    if (level->base == 0 || level->low_tables != NULL || start < (2ull << 20) ||
        end > LOW_LIMIT || start >= end || !low_setup() ||
        (level->low_tables = page_from(level)) == NULL) {
        return false;
    }
    level->low_start = start;
    level->low_end = end;
    return true;
}

bool vm_low_used(void) {
    return low_pd != NULL;
}

void vm_move(uint64_t *top, uint64_t *pd) {
    uint64_t *old = pml4();

    for (unsigned i = 0; i < LEVELS; i++) {
        if (levels[i].base != 0) {
            top[levels[i].slot] = old[levels[i].slot];
        }
    }
    if (low_pd == NULL) {
        return;
    }
    /* The directory is all two-megabyte pages, which is what is put back from
       now on; whatever a program has in place of one stays. */
    for (unsigned i = 0; i < ENTRIES; i++) {
        uint64_t plain = pd[i];

        if (low_pd[i] != low_orig[i]) {
            pd[i] = low_pd[i];
        }
        low_orig[i] = plain;
    }
    low_pd = pd;
    top[0] |= USER;
    table_at(top[0])[0] |= USER;
}

void vm_firmware_view(bool on) {
    if (low_pd != NULL) {
        low_apply(&levels[depth], !on);
    }
}

size_t vm_fixed_tables(void) {
    return (size_t)low_fixed + (levels[0].base != 0 ? VM_PAGE : 0);
}

bool vm_start(void) {
    if (levels[depth].base != 0) {
        return true;
    }
    return slot_take(&levels[depth]);
}

uint64_t vm_base(void) {
    return levels[depth].base;
}

uint64_t vm_end(void) {
    uint64_t base = levels[depth].base;

    return base == 0 ? 0 : base + SLOT_SIZE;
}

bool vm_holds(uint64_t addr, uint64_t size) {
    struct level *level = &levels[depth];
    uint64_t base = level->base;

    if (level->low_tables != NULL && addr >= level->low_start && addr <= level->low_end &&
        size <= level->low_end - addr) {
        return true;
    }
    return base != 0 && addr >= base && addr <= vm_end() && size <= vm_end() - addr;
}

/* Walks down to the entry for addr, making the tables on the way if make is
   set. Returns NULL if there is none, or if one could not be made. */
static uint64_t *entry_for(uint64_t addr, bool make) {
    struct level *level = &levels[depth];

    if (addr < LOW_LIMIT) {
        uint64_t *pt;

        if (level->low_tables == NULL) {
            return NULL;
        }
        if (level->low_tables[addr >> 21] == 0) {
            uint64_t cr0;

            if (!make || (pt = page_from(level)) == NULL) {
                return NULL;
            }
            level->low_tables[addr >> 21] = (uint64_t)pt;
            level->tables += VM_PAGE;
            cr0 = write_protect_off();
            low_pd[addr >> 21] = (uint64_t)pt | PRESENT | WRITE | USER;
            write_protect_back(cr0);
            flush_tlb();
        }
        pt = (uint64_t *)level->low_tables[addr >> 21];
        return &pt[(addr >> 12) & (ENTRIES - 1)];
    }

    uint64_t *table = table_at(pml4()[level->slot]);
    unsigned index[3] = {
        (unsigned)(addr >> 30) & (ENTRIES - 1),
        (unsigned)(addr >> 21) & (ENTRIES - 1),
        (unsigned)(addr >> 12) & (ENTRIES - 1),
    };

    for (unsigned n = 0; n < 2; n++) {
        uint64_t *at = &table[index[n]];

        if (!(*at & PRESENT)) {
            uint64_t *next;

            if (!make || (next = page_from(level)) == NULL) {
                return NULL;
            }
            *at = (uint64_t)next | PRESENT | WRITE | USER;
            level->tables += VM_PAGE;
        }
        table = table_at(*at);
    }
    return &table[index[2]];
}

/* Notes that the page at addr is about to change, keeping what is under it
   if there is anything - which is what lets vm_undo_end put it back. */
static void undo_note(struct level *level, uint64_t addr, bool mapped) {
    struct undo *entry;

    if (level->undo_count == UNDO_MAX) {
        level->undo_full = true;
        return;
    }
    entry = &level->undo[level->undo_count];
    entry->at = addr;
    entry->copy = 0;
    if (mapped) {
        uint64_t *copy = page_from(level);

        if (copy == NULL) {
            level->undo_full = true;
            return;
        }
        memcpy(copy, (const void *)addr, VM_PAGE);
        entry->copy = (uint64_t)copy;
    }
    level->undo_count++;
}

bool vm_fault(uint64_t addr) {
    struct level *level = &levels[depth];
    uint64_t page = addr & ~(uint64_t)(VM_PAGE - 1);
    uint64_t *at;
    uint64_t *fresh;

    if (!vm_holds(addr, 0)) {
        return false;
    }
    at = entry_for(addr, true);
    if (at == NULL) {
        return false;
    }
    if (*at & PRESENT) {
        if (*at & WRITE) {
            return true;            /* someone else got there first */
        }
        /* Write-protected for a fork: the parent's page is copied aside and
           the child gets the original to scribble on. */
        if (level->undo == NULL) {
            return false;           /* not ours to make writable */
        }
        undo_note(level, page, true);
        *at |= WRITE;
        __asm__ volatile("invlpg (%0)" : : "r"(page) : "memory");
        return true;
    }
    if (page < LOW_LIMIT) {
        /* The page at its own address, so that nothing else is hidden -
           or, over one a program underneath has, any page at all. */
        uint64_t owned = 0;

        if (mem_take_page(page)) {
            owned = OWNED;
            level->low_owned++;
            fresh = (uint64_t *)page;
        } else if (!(low_below(page) || mem_firmware_kept(page)) ||
                   (fresh = page_from(level)) == NULL) {
            return false;           /* someone's, and in use: not to be hidden */
        }
        if (level->undo != NULL) {
            undo_note(level, page, false);
        }
        *at = (uint64_t)fresh | PRESENT | WRITE | USER | owned;
        __asm__ volatile("invlpg (%0)" : : "r"(page) : "memory");
        memset((void *)page, 0, VM_PAGE);
        return true;
    }
    fresh = page_from(level);
    if (fresh == NULL) {
        return false;
    }
    if (level->undo != NULL) {
        undo_note(level, page, false);
    }
    *at = (uint64_t)fresh | PRESENT | WRITE | USER;
    __asm__ volatile("invlpg (%0)" : : "r"(page) : "memory");
    return true;
}

bool vm_mapped(uint64_t addr) {
    uint64_t *at;

    if (!vm_holds(addr, 0)) {
        return false;
    }
    at = entry_for(addr, false);
    return at != NULL && (*at & PRESENT) != 0;
}

bool vm_reserve(uint64_t addr, uint64_t size) {
    uint64_t first = addr & ~(uint64_t)(VM_PAGE - 1);
    uint64_t last = (addr + size + VM_PAGE - 1) & ~(uint64_t)(VM_PAGE - 1);

    if (!vm_holds(addr, size)) {
        return false;
    }
    for (uint64_t page = first; page < last; page += VM_PAGE) {
        if (!vm_fault(page)) {
            return false;
        }
    }
    return true;
}

/* Hands back the page an entry maps: to the machine if it was one at its own
   address, to the region's spares otherwise. */
static void low_page_back(struct level *level, uint64_t entry) {
    if (entry & OWNED) {
        mem_give_page(entry & ADDR);
        level->low_owned--;
    } else {
        page_back(level, entry & ADDR);
    }
}

void vm_release(uint64_t addr, uint64_t size) {
    struct level *level = &levels[depth];
    uint64_t first = (addr + VM_PAGE - 1) & ~(uint64_t)(VM_PAGE - 1);
    uint64_t last = (addr + size) & ~(uint64_t)(VM_PAGE - 1);

    if (!vm_holds(addr, size) || level->undo != NULL) {
        return;                     /* nothing goes back while a fork is out */
    }
    for (uint64_t page = first; page < last; page += VM_PAGE) {
        uint64_t *at = entry_for(page, false);

        if (at != NULL && (*at & PRESENT)) {
            low_page_back(level, *at);
            *at = 0;
        }
    }
    flush_tlb();
}

/* Runs visit over every mapped page of the region, handing it the entry and
   the address it covers. */
static void walk(struct level *level, void (*visit)(uint64_t *at, uint64_t addr)) {
    uint64_t *pdpt = table_at(pml4()[level->slot]);

    if (level->low_tables != NULL) {
        for (uint64_t i = level->low_start >> 21; i <= (level->low_end - 1) >> 21; i++) {
            uint64_t *pt = (uint64_t *)level->low_tables[i];

            for (unsigned k = 0; pt != NULL && k < ENTRIES; k++) {
                if (pt[k] & PRESENT) {
                    visit(&pt[k], (i << 21) + ((uint64_t)k << 12));
                }
            }
        }
    }

    for (unsigned i = 0; i < ENTRIES; i++) {
        uint64_t *pd;

        if (!(pdpt[i] & PRESENT)) {
            continue;
        }
        pd = table_at(pdpt[i]);
        for (unsigned j = 0; j < ENTRIES; j++) {
            uint64_t *pt;

            if (!(pd[j] & PRESENT)) {
                continue;
            }
            pt = table_at(pd[j]);
            for (unsigned k = 0; k < ENTRIES; k++) {
                if (pt[k] & PRESENT) {
                    visit(&pt[k], level->base + ((uint64_t)i << 30) +
                                  ((uint64_t)j << 21) + ((uint64_t)k << 12));
                }
            }
        }
    }
}

static void unprotect(uint64_t *at, uint64_t addr) {
    (void)addr;
    *at |= WRITE;
}

static void protect(uint64_t *at, uint64_t addr) {
    (void)addr;
    *at &= ~(uint64_t)WRITE;
}

bool vm_undo_begin(void) {
    struct level *level = &levels[depth];
    void *block = NULL;

    if (level->base == 0 || level->undo != NULL) {
        return false;               /* one fork out at a time in a region */
    }
    if ((block = mem_alloc(UNDO_MAX * sizeof(struct undo))) == NULL) {
        return false;
    }
    level->undo = block;
    level->undo_count = 0;
    level->undo_full = false;
    walk(level, protect);
    flush_tlb();
    return true;
}

void vm_undo_end(bool restore) {
    struct level *level = &levels[depth];
    struct undo *log = level->undo;

    if (log == NULL) {
        return;
    }
    /* Taken down before anything is put back: restoring writes to the very
       pages that are protected, and the fault handler must not treat those
       writes as the child's. */
    level->undo = NULL;
    for (unsigned i = 0; i < level->undo_count; i++) {
        uint64_t *at = entry_for(log[i].at, false);

        if (restore && log[i].copy != 0) {
            memcpy((void *)log[i].at, (const void *)log[i].copy, VM_PAGE);
        }
        if (restore && log[i].copy == 0 && at != NULL && (*at & PRESENT)) {
            low_page_back(level, *at);              /* the parent had none */
            *at = 0;
            continue;
        }
        if (log[i].copy != 0) {
            page_back(level, log[i].copy);
        }
    }
    if (level->undo_full) {
        dbg("fork: more pages changed than could be kept\n");
    }
    walk(level, unprotect);
    flush_tlb();
    mem_free(log);
}

/* Empties the region: every page it lent the program goes back onto its own
   free list, and the tables describing them with it. With whole set the
   region itself goes too, and every chunk it ever bought is handed to the
   firmware - which is one call per megabyte rather than one per page. */
static void level_empty(struct level *level, bool whole) {
    uint64_t *pdpt;

    if (level->base == 0) {
        return;
    }
    if (level->low_tables != NULL) {
        low_apply(level, false);
        for (uint64_t i = level->low_start >> 21; i <= (level->low_end - 1) >> 21; i++) {
            uint64_t *pt = (uint64_t *)level->low_tables[i];

            for (unsigned k = 0; pt != NULL && k < ENTRIES; k++) {
                if (pt[k] & PRESENT) {
                    low_page_back(level, pt[k]);
                }
            }
            if (pt != NULL) {
                page_back(level, (uint64_t)pt);
                level->tables -= VM_PAGE;
            }
        }
        page_back(level, (uint64_t)level->low_tables);
        level->low_tables = NULL;
        level->low_start = level->low_end = 0;
    }
    pdpt = table_at(pml4()[level->slot]);
    for (unsigned i = 0; i < ENTRIES; i++) {
        uint64_t *pd;

        if (!(pdpt[i] & PRESENT)) {
            continue;
        }
        pd = table_at(pdpt[i]);
        for (unsigned j = 0; j < ENTRIES; j++) {
            uint64_t *pt;

            if (!(pd[j] & PRESENT)) {
                continue;
            }
            pt = table_at(pd[j]);
            for (unsigned k = 0; k < ENTRIES; k++) {
                if (pt[k] & PRESENT) {
                    page_back(level, pt[k] & ADDR);
                }
            }
            page_back(level, pd[j] & ADDR);
            pd[j] = 0;
            level->tables -= VM_PAGE;
        }
        page_back(level, pdpt[i] & ADDR);
        pdpt[i] = 0;
        level->tables -= VM_PAGE;
    }
    /* Nothing of the region is mapped any more, so its chunks are nobody's:
       an idle machine holds none of what the program had. */
    chunks_back(level);
    if (whole) {
        uint64_t cr0 = write_protect_off();

        pml4()[level->slot] = 0;
        write_protect_back(cr0);
        pages_back_to_firmware((uint64_t)pdpt, 1);
        *level = (struct level){ 0 };
    }
    flush_tlb();
}

void vm_reset(void) {
    struct level *level = &levels[depth];

    if (level->undo != NULL) {
        vm_undo_end(false);         /* whatever forked it is gone */
    }
    /* The outermost region goes whole, its top table too: an idle machine
       holds none of it, and the next program takes it again (vm_start). */
    level_empty(level, depth == 0);
}

bool vm_push(void) {
    if (depth + 1 >= LEVELS) {
        return false;
    }
    low_apply(&levels[depth], false);   /* the one underneath is put away */
    depth++;
    levels[depth] = (struct level){ 0 };
    if (!slot_take(&levels[depth])) {
        depth--;
        low_apply(&levels[depth], true);
        return false;
    }
    return true;
}

void vm_pop(void) {
    if (depth == 0) {
        return;
    }
    if (levels[depth].undo != NULL) {
        vm_undo_end(false);
    }
    level_empty(&levels[depth], true);
    depth--;
    low_apply(&levels[depth], true);
}

unsigned vm_level(void) {
    return depth;
}

void vm_unwind(unsigned to) {
    while (depth > to) {
        vm_pop();
    }
}

size_t vm_memory(void) {
    size_t total = 0;

    for (unsigned i = 0; i <= depth; i++) {
        total += (size_t)levels[i].bought + (size_t)levels[i].low_owned * VM_PAGE;
    }
    return total;
}

/* Only each region's top table: the rest are out of its chunks, and so are
   already in vm_memory. The first region's is counted with vm_fixed_tables. */
size_t vm_tables(void) {
    size_t total = 0;

    for (unsigned i = 1; i <= depth; i++) {
        total += levels[i].base != 0 ? VM_PAGE : 0;
    }
    return total;
}
