#include "vm.h"

#include "debug.h"
#include "efi.h"
#include "efi_kernel.h"
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
#define CHUNK_FIRST 8               /* what the first one costs a small program */
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
} levels[LEVELS];

static unsigned depth;          /* levels[depth] is the one in use */

static struct efi_boot_services *services(void) {
    return efi_boot()->system->boot;
}

static efi_status pages_from_firmware(uint64_t pages, uint64_t *at) {
    return services()->allocate_pages(EFI_ALLOCATE_ANY, EFI_LOADER_DATA, pages, at);
}

static void pages_back_to_firmware(uint64_t at, uint64_t pages) {
    services()->free_pages(at, pages);
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

bool vm_start(void) {
    if (levels[0].base != 0) {
        return true;
    }
    return slot_take(&levels[0]);
}

uint64_t vm_base(void) {
    return levels[depth].base;
}

uint64_t vm_end(void) {
    uint64_t base = levels[depth].base;

    return base == 0 ? 0 : base + SLOT_SIZE;
}

bool vm_holds(uint64_t addr, uint64_t size) {
    uint64_t base = levels[depth].base;

    return base != 0 && addr >= base && addr <= vm_end() && size <= vm_end() - addr;
}

/* Walks down to the entry for addr, making the tables on the way if make is
   set. Returns NULL if there is none, or if one could not be made. */
static uint64_t *entry_for(uint64_t addr, bool make) {
    struct level *level = &levels[depth];
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
            page_back(level, *at & ADDR);
            *at = 0;
        }
    }
    flush_tlb();
}

/* Runs visit over every mapped page of the region, handing it the entry and
   the address it covers. */
static void walk(struct level *level, void (*visit)(uint64_t *at, uint64_t addr)) {
    uint64_t *pdpt = table_at(pml4()[level->slot]);

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
    if (EFI_ERROR(services()->allocate_pool(EFI_LOADER_DATA,
                                            UNDO_MAX * sizeof(struct undo), &block))) {
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
            page_back(level, *at & ADDR);           /* the parent had none */
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
    services()->free_pool(log);
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
    level_empty(level, false);
}

bool vm_push(void) {
    if (depth + 1 >= LEVELS) {
        return false;
    }
    depth++;
    levels[depth] = (struct level){ 0 };
    if (!slot_take(&levels[depth])) {
        depth--;
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
        total += (size_t)levels[i].bought;
    }
    return total;
}

size_t vm_tables(void) {
    size_t total = 0;

    for (unsigned i = 0; i <= depth; i++) {
        total += (size_t)levels[i].tables;
    }
    return total;
}
