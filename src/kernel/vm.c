#include "vm.h"
#include "share.h"

#include "debug.h"
#include "efi.h"
#include "efi_kernel.h"
#include "mem.h"
#include "string.h"

/* x86-64 paging, four levels of it: a virtual address is four nine-bit
 * indexes and an offset, and each level holds physical addresses of the next.
 * One slot of the top level is the programs', which is half a terabyte of
 * address space - far more than anything here will use, and free, which is
 * what matters: nothing of the firmware's is anywhere near it.
 *
 * Every process has its own tables under that slot, and the slot points at
 * whichever process is running: a switch is one entry rewritten, so a
 * process costs its own tables and nothing more - no top table of its own,
 * no copy of the kernel's mappings to keep in step. */

#define PRESENT 0x001
#define WRITE   0x002
#define USER    0x004
#define BIG     0x080
#define ADDR    0x000FFFFFFFFFF000ull

#define ENTRIES   512
#define SLOT_SIZE (1ull << 39)      /* what one top-level entry covers */

#define SPACES 32                   /* processes at once */

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

static struct space *spaces[SPACES];    /* every one there is */
static struct space *cur;               /* the one the slot points at */
static unsigned slot;                   /* the programs' slot, once taken */
static uint64_t region;                 /* where it starts */

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
#define DEVICE    0x400             /* an entry for a device's memory: nobody's to give back */
#define SHARED    0x800             /* a file's page every process has in common (share.c):
                                       read-only, copied on the first write */

static uint64_t *low_pd;            /* the firmware's directory for it */
static uint64_t *low_orig;          /* what that said before anything changed it -
                                       none on the kernel's own tables, which
                                       say two megabytes one to one */
static uint64_t  low_fixed;         /* pages bought for those two, for good */


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
static uint64_t *page_from(struct space *level) {
    uint64_t at;

    /* Once memory is the kernel's, a page is one call and nothing to hand
       back but itself: one at a time, and back the moment it is let go, so
       what a process has is what it holds now rather than the most it ever
       did. Chunks are for while the firmware still owns memory, where every
       call is a trip to it - and a region that started on them stays on
       them. */
    if (level->chunks == 0 && mem_ours()) {
        if ((at = mem_pages(1)) == 0) {
            return NULL;
        }
        level->bought += VM_PAGE;
        memset((void *)at, 0, VM_PAGE);
        return (uint64_t *)at;
    }

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
static void page_back(struct space *level, uint64_t at) {
    if (level->chunks == 0) {
        mem_pages_free(at, 1);
        level->bought -= VM_PAGE;
        return;
    }
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
static void chunks_back(struct space *level) {
    uint64_t chunk = level->chunks;

    while (chunk != 0) {
        uint64_t before = ((struct chunk *)chunk)->before;

        pages_back_to_firmware(chunk, ((struct chunk *)chunk)->pages);
        chunk = before;
    }
    level->bought = 0;
    level->chunks = level->next = level->left = level->spare = level->grow = 0;
}

/* Claims a free top-level slot for programs, the first time one is wanted. */
static bool slot_init(void) {
    uint64_t *top = pml4();

    /* The lower half of the address space, which is where a program may run,
       is the first 256 slots. The firmware maps what the machine has at the
       bottom of it; a slot it leaves alone is ours. */
    for (unsigned i = 1; slot == 0 && i < 256; i++) {
        if (!(top[i] & PRESENT)) {
            slot = i;
            region = (uint64_t)i * SLOT_SIZE;
        }
    }
    return slot != 0;
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
static void low_apply(struct space *level, bool on) {
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

/* Whether another process has the page at addr at its own address, and so
   whether this one may have a page there without hiding anyone's. */
static bool low_below(uint64_t addr) {
    for (unsigned i = 0; i < SPACES; i++) {
        struct space *l = spaces[i];

        if (l == NULL || l == cur) {
            continue;
        }
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
    struct space *level = cur;

    start &= ~(uint64_t)(VM_PAGE - 1);
    end = (end + VM_PAGE - 1) & ~(uint64_t)(VM_PAGE - 1);
    if (level == NULL || level->low_tables != NULL || start < (2ull << 20) ||
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

    if (slot != 0) {
        top[slot] = old[slot];
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
    if (low_pd != NULL && cur != NULL) {
        low_apply(cur, !on);
    }
}

size_t vm_fixed_tables(void) {
    return (size_t)low_fixed;
}

bool vm_start(void) {
    return cur != NULL;
}

uint64_t vm_base(void) {
    return cur != NULL ? region : 0;
}

uint64_t vm_end(void) {
    return cur != NULL ? region + SLOT_SIZE : 0;
}

bool vm_holds(uint64_t addr, uint64_t size) {
    struct space *level = cur;
    uint64_t base = vm_base();

    if (level == NULL) {
        return false;
    }
    if (level->low_tables != NULL && addr >= level->low_start && addr <= level->low_end &&
        size <= level->low_end - addr) {
        return true;
    }
    return base != 0 && addr >= base && addr <= vm_end() && size <= vm_end() - addr;
}

/* Walks down to space level's entry for addr, making the tables on the way
   if make is set. Returns NULL if there is none, or if one could not be made. */
static uint64_t *entry_in(struct space *level, uint64_t addr, bool make) {

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
            if (level == cur) {
                cr0 = write_protect_off();
                low_pd[addr >> 21] = (uint64_t)pt | PRESENT | WRITE | USER;
                write_protect_back(cr0);
                flush_tlb();
            }
        }
        pt = (uint64_t *)level->low_tables[addr >> 21];
        return &pt[(addr >> 12) & (ENTRIES - 1)];
    }

    uint64_t *table = level->pdpt;
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

static uint64_t *entry_for(uint64_t addr, bool make) {
    return entry_in(cur, addr, make);
}

bool vm_fault(uint64_t addr) {
    struct space *level = cur;
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
    if ((*at & (PRESENT | SHARED)) == (PRESENT | SHARED)) {
        /* A write to a page every process has: this one gets its own - the
           page itself, if nobody else has it any more. */
        uint64_t common = *at & ADDR;

        if (level->chunks == 0 && page >= LOW_LIMIT && shared_take(common)) {
            *at = common | PRESENT | WRITE | USER;
            level->bought += VM_PAGE;
            __asm__ volatile("invlpg (%0)" : : "r"(page) : "memory");
            return true;
        }
        if ((fresh = page_from(level)) == NULL) {
            return false;
        }
        memcpy(fresh, (const void *)page, VM_PAGE);
        *at = (uint64_t)fresh | PRESENT | WRITE | USER;
        __asm__ volatile("invlpg (%0)" : : "r"(page) : "memory");
        shared_drop(common);
        return true;
    }
    if (*at & PRESENT) {
        return (*at & WRITE) != 0;  /* someone else got there first, or not ours */
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
        *at = (uint64_t)fresh | PRESENT | WRITE | USER | owned;
        __asm__ volatile("invlpg (%0)" : : "r"(page) : "memory");
        memset((void *)page, 0, VM_PAGE);
        return true;
    }
    fresh = page_from(level);
    if (fresh == NULL) {
        return false;
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

bool vm_map_shared(uint64_t addr, uint64_t page) {
    uint64_t *at;

    /* Not under a program at a fixed address: its pages are the ones at their
       own address there, or ones nothing else could be hiding (vm_fault). */
    if (addr < LOW_LIMIT || !vm_holds(addr, VM_PAGE) || (at = entry_for(addr, true)) == NULL ||
        (*at & PRESENT)) {
        return false;
    }
    *at = page | PRESENT | USER | SHARED;
    __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
    return true;
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
static void low_page_back(struct space *level, uint64_t entry) {
    if (entry & DEVICE) {
        return;
    }
    if (entry & SHARED) {
        shared_drop(entry & ADDR);
        return;
    }
    if (entry & OWNED) {
        mem_give_page(entry & ADDR);
        level->low_owned--;
    } else {
        page_back(level, entry & ADDR);
    }
}

bool vm_map_device(uint64_t addr, uint64_t phys, uint64_t size) {
    struct space *level = cur;

    if (!vm_holds(addr, size)) {
        return false;
    }
    for (uint64_t off = 0; off < size; off += VM_PAGE) {
        uint64_t *at = entry_for(addr + off, true);

        if (at == NULL) {
            return false;
        }
        if (*at & PRESENT) {
            low_page_back(level, *at);
        }
        *at = (phys + off) | PRESENT | WRITE | USER | DEVICE;
    }
    flush_tlb();
    return true;
}

void vm_release(uint64_t addr, uint64_t size) {
    struct space *level = cur;
    uint64_t first = (addr + VM_PAGE - 1) & ~(uint64_t)(VM_PAGE - 1);
    uint64_t last = (addr + size) & ~(uint64_t)(VM_PAGE - 1);

    if (!vm_holds(addr, size)) {
        return;
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
static void walk(struct space *level, void (*visit)(uint64_t *at, uint64_t addr)) {
    uint64_t *pdpt = level->pdpt;

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
                    visit(&pt[k], region + ((uint64_t)i << 30) +
                                  ((uint64_t)j << 21) + ((uint64_t)k << 12));
                }
            }
        }
    }
}

/* Empties the region: every page it lent the program goes back onto its own
   free list, and the tables describing them with it. With whole set the
   region itself goes too, and every chunk it ever bought is handed to the
   firmware - which is one call per megabyte rather than one per page. */
static void level_empty(struct space *level, bool whole) {
    uint64_t *pdpt = level->pdpt;

    if (level->low_tables != NULL) {
        if (level == cur) {
            low_apply(level, false);
        }
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
                if ((pt[k] & (PRESENT | DEVICE | SHARED)) == (PRESENT | SHARED)) {
                    shared_drop(pt[k] & ADDR);
                } else if ((pt[k] & (PRESENT | DEVICE)) == PRESENT) {
                    page_back(level, pt[k] & ADDR);
                }
                pt[k] = 0;
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
        pages_back_to_firmware((uint64_t)pdpt, 1);
        level->pdpt = NULL;
    }
    flush_tlb();
}

void vm_reset(void) {
    if (cur != NULL) {
        level_empty(cur, false);
    }
}

bool vm_space_new(struct space *s) {
    unsigned i = 0;

    *s = (struct space){ 0 };
    while (i < SPACES && spaces[i] != NULL) {
        i++;
    }
    if (i == SPACES || !slot_init() || (s->pdpt = (uint64_t *)table_page()) == NULL) {
        return false;
    }
    spaces[i] = s;
    return true;
}

void vm_space_use(struct space *s) {
    uint64_t cr0;

    if (s == cur) {
        return;
    }
    if (cur != NULL) {
        low_apply(cur, false);
    }
    cr0 = write_protect_off();
    pml4()[slot] = s != NULL ? (uint64_t)s->pdpt | PRESENT | WRITE | USER : 0;
    write_protect_back(cr0);
    cur = s;
    if (s != NULL) {
        low_apply(s, true);
    }
    flush_tlb();
}

struct space *vm_space(void) {
    return cur;
}

void vm_space_free(struct space *s) {
    if (s->pdpt == NULL) {
        return;
    }
    level_empty(s, true);
    for (unsigned i = 0; i < SPACES; i++) {
        if (spaces[i] == s) {
            spaces[i] = NULL;
        }
    }
    if (s == cur) {
        vm_space_use(NULL);
    }
}

/* ---- fork ---------------------------------------------------------------
 *
 * The child gets a copy of every page the parent has, at the same address,
 * in tables of its own: the two then run side by side, each changing only
 * its own. A page is a page whoever asks, so only what the parent has
 * actually touched is copied - a shell is a megabyte or two of it, and the
 * copy goes again the moment the child starts a program of its own. The
 * screen, mapped by a program drawing on it, is shared rather than copied. */

static struct space *fork_to;
static bool fork_failed;

static void fork_page(uint64_t *at, uint64_t addr) {
    uint64_t *to, *page;

    if (fork_failed || (to = entry_in(fork_to, addr, true)) == NULL) {
        fork_failed = true;
        return;
    }
    if (*at & DEVICE) {
        *to = *at;
        return;
    }
    /* The parent's own page, where it is one at a time: shared from now on,
       read-only to both, until one of them writes (vm_fault). */
    if (!(*at & SHARED) && addr >= LOW_LIMIT && cur->chunks == 0 && fork_to->chunks == 0 &&
        shared_adopt(*at & ADDR)) {
        *at = (*at & ADDR) | PRESENT | USER | SHARED;
        cur->bought -= VM_PAGE;
    }
    if (*at & SHARED) {
        *to = *at;                  /* the same page, counted once more */
        shared_hold(*at & ADDR);
        return;
    }
    if ((page = page_from(fork_to)) == NULL) {
        fork_failed = true;
        return;
    }
    memcpy(page, (const void *)addr, VM_PAGE);
    *to = (uint64_t)page | PRESENT | WRITE | USER;
}

bool vm_fork(struct space *child) {
    if (cur == NULL || !vm_space_new(child)) {
        return false;
    }
    if (cur->low_tables != NULL) {
        child->low_start = cur->low_start;
        child->low_end = cur->low_end;
        if ((child->low_tables = page_from(child)) == NULL) {
            vm_space_free(child);
            return false;
        }
    }
    fork_to = child;
    fork_failed = false;
    walk(cur, fork_page);
    flush_tlb();                    /* the parent's pages it now shares are read-only */
    if (fork_failed) {
        vm_space_free(child);
        return false;
    }
    return true;
}

size_t vm_memory(void) {
    size_t total = 0;

    for (unsigned i = 0; i < SPACES; i++) {
        if (spaces[i] != NULL) {
            total += (size_t)spaces[i]->bought + (size_t)spaces[i]->low_owned * VM_PAGE;
        }
    }
    return total + shared_bytes();
}

/* Only each process's top table for the slot: the rest are out of its
   chunks, and so are already in vm_memory. */
size_t vm_tables(void) {
    size_t total = 0;

    for (unsigned i = 0; i < SPACES; i++) {
        total += spaces[i] != NULL ? VM_PAGE : 0;
    }
    return total;
}
