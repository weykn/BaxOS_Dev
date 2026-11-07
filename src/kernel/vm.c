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
 * no copy of the kernel's mappings to keep in step.
 *
 * On i386 it is PAE paging: the same entries, three levels, four gigabytes.
 * There is no slot to give programs a region in, so there are no regions:
 * every program has low memory alone - the first gigabyte - which is what a
 * 32-bit program on x86-64 has too. REGIONS says which. */

#define PRESENT 0x001
#define WRITE   0x002
#define USER    0x004
#define BIG     0x080
#define ADDR    0x000FFFFFFFFFF000ull

#define ENTRIES   512
#define SLOT_SIZE (1ull << 39)      /* what one top-level entry covers */

#ifdef __i386__
#define REGIONS 0
#else
#define REGIONS 1
#endif

#define SPACES 128                  /* processes at once */

/* Pages are bought from the firmware a chunk at a time rather than one at a
   time. A call to firmware costs about the same whatever it is for, and a C
   library is five hundred pages: asking for each of them separately was most
   of what starting a program cost.

   A chunk's own first page holds where the chunk before it was, so that the
   whole lot can be handed back at the end without a list of them anywhere
   else. */
#define CHUNK_FIRST 5               /* what the first one costs a small program:
                                       tsh's one page of code and data, its
                                       stack, the two tables under them and
                                       the chunk's own page */
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
/* What the program asked a page to be (vm_protect), in bits the processor
   ignores - kept in an entry whether or not a page is mapped there yet. The
   processor's own WRITE and USER bits are what enforce them.
   PAE ignores none of the high bits of an entry that is present, only bits
   8 to 11 - 8 being the global bit, which is ignored too while CR4.PGE is
   off, as i386's kernel keeps it. Low memory never has SHARED pages (see
   vm_map_shared), so on i386 its bit says NOACCESS, and SHARED and
   COMMON_RW are bits never set. */
#ifdef __i386__
#undef SHARED
#define SHARED    (1ull << 52)
#define NOWRITE   0x100ull
#define NOACCESS  0x800ull
#else
#define NOWRITE   (1ull << 52)
#define NOACCESS  (1ull << 53)
#endif
#define COMMON_RW (1ull << 54)      /* a SHARED page written in common (MAP_SHARED) */
#define PROT_BITS (NOWRITE | NOACCESS)

/* The machine's own page at its own address, for the kernel alone: what a
   fault of the kernel's own in a program's low memory gets where the page
   is neither free nor any program's - the kernel's data or the firmware's,
   which the program's tables would otherwise hide from it (vm_fault). A
   device's entry, so nobody gives it back; no USER, so the program cannot
   reach it. */
#define KERNEL_VIEW(e) (((e) & (PRESENT | USER | DEVICE)) == (PRESENT | DEVICE))

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
    uintptr_t cr3;

    __asm__ volatile("mov %%cr3, %0\n\tmov %0, %%cr3" : "=r"(cr3) : : "memory");
}

/* The firmware write-protects its own tables, CR0.WP, which binds even ring
   0; editing one means turning that off for as long as the edit takes. Once
   the kernel is on tables of its own (vm_move) there is no need: a process
   switch edits them, and two CR0 writes a switch - each one serialising,
   and a trip out of a virtual machine - were most of what a switch cost. */
static bool tables_ours = !REGIONS;  /* i386's are the kernel's from the start */

static uintptr_t write_protect_off(void) {
    uintptr_t cr0;

    if (tables_ours) {
        return 0;
    }
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0 & ~(uintptr_t)0x10000) : "memory");
    return cr0;
}

static void write_protect_back(uintptr_t cr0) {
    if (cr0 != 0) {
        __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    }
}

/* The top table: x86-64's PML4, or i386's four-entry PDPT. */
static uint64_t *pml4(void) {
    uintptr_t cr3;

    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return (uint64_t *)(cr3 & (uintptr_t)ADDR & ~(uintptr_t)31);
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

        /* Each chunk is twice the one before it, up to CHUNK_MAX: a program
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

    if (!REGIONS) {
        return true;
    }
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
    uintptr_t cr0;

    if (low_pd != NULL) {
        return true;
    }
    if (!REGIONS) {
        /* The kernel's own PDPT, whose first entry is a directory of
           two-megabyte pages (mem_own_tables) - and an entry that holds
           neither USER nor WRITE, both of which PAE reserves there. */
        if (!(top[0] & PRESENT)) {
            return false;
        }
        low_pd = table_at(top[0]);
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
            pdpt[0] = (uint64_t)(uintptr_t)pd | PRESENT | WRITE;
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
        pdpt[0] = (uint64_t)(uintptr_t)pd | PRESENT | WRITE;
        low_fixed += VM_PAGE;
    }
    pdpt[0] |= USER;
    low_pd = table_at(pdpt[0]);
    memcpy(low_orig, low_pd, VM_PAGE);
    write_protect_back(cr0);
    flush_tlb();
    return true;
}

/* The space whose tables for low memory are in the directory now, and the
   one vm_firmware_view has taken them out of for a call into the firmware -
   to put back only what it took: a space being emptied takes its own out
   for good, and makes calls of its own while it does. */
static struct space *applied, *hidden;

/* Puts level's tables for its low range in the directory, or takes them out
   again for what the firmware had there. */
static void low_apply(struct space *level, bool on) {
    uintptr_t cr0;

    if (level->low_tables == NULL) {
        return;
    }
    applied = on ? level : applied == level ? NULL : applied;
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
    applied = level;                /* empty yet: its tables go in as they are made */
    return true;
}

bool vm_low_shadowed(uint64_t page) {
    for (unsigned i = 0; i < SPACES; i++) {
        struct space *l = spaces[i];

        if (l != NULL && l->low_tables != NULL && page >= l->low_start && page < l->low_end &&
            l->low_tables[page >> 21] != 0) {
            uint64_t e = ((uint64_t *)l->low_tables[page >> 21])[(page >> 12) & (ENTRIES - 1)];

            if ((e & PRESENT) && (e & ADDR) != page) {
                return true;
            }
        }
    }
    return false;
}

bool vm_low_used(void) {
    return low_pd != NULL;
}

void vm_move(uint64_t *top, uint64_t *pd) {
    uint64_t *old = pml4();

    tables_ours = true;

    if (slot != 0) {
        top[slot] = old[slot];
    }
    if (low_pd == NULL) {
        return;
    }
    if (!REGIONS) {
        pd = table_at(top[0]);      /* i386's: the same directory, moved */
        for (unsigned i = 0; i < ENTRIES; i++) {
            if (low_pd[i] != low_plain(i)) {
                pd[i] = low_pd[i];
            }
        }
        low_pd = pd;
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
    if (on && applied != NULL) {
        hidden = applied;
        low_apply(hidden, false);
    } else if (!on && hidden != NULL) {
        low_apply(hidden, true);
        hidden = NULL;
    }
}

size_t vm_fixed_tables(void) {
    return (size_t)low_fixed;
}

bool vm_start(void) {
    return cur != NULL;
}

uint64_t vm_base(void) {
    return cur != NULL && REGIONS ? region : 0;
}

uint64_t vm_end(void) {
    return cur != NULL && REGIONS ? region + SLOT_SIZE : 0;
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
            uintptr_t cr0;

            if (!make || (pt = page_from(level)) == NULL) {
                return NULL;
            }
            level->low_tables[addr >> 21] = (uint64_t)(uintptr_t)pt;
            level->tables += VM_PAGE;
            if (level == applied) {
                cr0 = write_protect_off();
                low_pd[addr >> 21] = (uint64_t)(uintptr_t)pt | PRESENT | WRITE | USER;
                write_protect_back(cr0);
                flush_tlb();
            }
        }
        pt = (uint64_t *)level->low_tables[addr >> 21];
        return &pt[(addr >> 12) & (ENTRIES - 1)];
    }

    if (!REGIONS) {
        return NULL;
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
            *at = (uint64_t)(uintptr_t)next | PRESENT | WRITE | USER;
            level->tables += VM_PAGE;
        }
        table = table_at(*at);
    }
    return &table[index[2]];
}

static uint64_t *entry_for(uint64_t addr, bool make) {
    return entry_in(cur, addr, make);
}

bool vm_low_moved(uint64_t addr, uint64_t size) {
    return cur != NULL && cur->low_tables != NULL && addr < cur->low_end &&
           addr + size > cur->low_start;
}

bool vm_fault(uint64_t addr) {
    return vm_fault_as(addr, true);
}

bool vm_low_unfilled(uint64_t addr) {
    uint64_t *at;

    if (cur == NULL || cur->low_tables == NULL || addr < cur->low_start || addr >= cur->low_end) {
        return false;
    }
    at = entry_in(cur, addr, false);
    return at == NULL || !(*at & PRESENT) || KERNEL_VIEW(*at);
}

bool vm_fault_as(uint64_t addr, bool user) {
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

        uint64_t kept = *at & PROT_BITS;

        if (level->chunks == 0 && page >= LOW_LIMIT && shared_take(common)) {
            *at = common | PRESENT | WRITE | USER | kept;
            level->bought += VM_PAGE;
            __asm__ volatile("invlpg (%0)" : : "r"((uintptr_t)page) : "memory");
            return true;
        }
        if ((fresh = page_from(level)) == NULL) {
            return false;
        }
        memcpy(fresh, (const void *)page, VM_PAGE);
        *at = (uint64_t)(uintptr_t)fresh | PRESENT | WRITE | USER | kept;
        __asm__ volatile("invlpg (%0)" : : "r"((uintptr_t)page) : "memory");
        shared_drop(common);
        return true;
    }
    if (KERNEL_VIEW(*at) && user) {
        /* The program wants a page the kernel has been reading as the
           machine's: only if it has become free since. */
        if (!mem_take_page(page)) {
            return false;
        }
        level->low_owned++;
        *at = page | PRESENT | WRITE | USER | OWNED;
        __asm__ volatile("invlpg (%0)" : : "r"((uintptr_t)page) : "memory");
        memset((void *)page, 0, VM_PAGE);
        return true;
    }
    if (*at & PRESENT) {
        return (*at & WRITE) != 0;  /* someone else got there first, or not ours */
    }
    if (page < LOW_LIMIT) {
        /* The page at its own address, so that nothing else is hidden -
           or, over one a program underneath has, any page at all. A
           page that is the kernel's or the running firmware's is a
           program's only where the program reached for it; where the
           kernel did, it is reading its own, or the firmware's, data -
           user_range has made every page of a program's that it reaches
           for the program's first. */
        uint64_t owned = 0;

        /* While the firmware runs the kernel cannot ask it whether the page
           is free: what it would read to ask may be the very page. */
        if (!user && !mem_ours()) {
            *at = page | PRESENT | WRITE | DEVICE;
            __asm__ volatile("invlpg (%0)" : : "r"((uintptr_t)page) : "memory");
            return true;
        }
        if (mem_take_page(page)) {
            owned = OWNED;
            level->low_owned++;
            fresh = (uint64_t *)page;
        } else if (!user && !(low_below(page) || mem_firmware_kept(page) || mem_beyond_ram(page))) {
            *at = page | PRESENT | WRITE | DEVICE;
            __asm__ volatile("invlpg (%0)" : : "r"((uintptr_t)page) : "memory");
            return true;
        } else if (!(low_below(page) || mem_firmware_kept(page) || mem_beyond_ram(page) ||
                     mem_firmware_has(page)) ||
                   (fresh = page_from(level)) == NULL) {
            dbg("vm: low page %x not ours: below %u kept %u\n", page, (uint64_t)low_below(page), (uint64_t)mem_firmware_kept(page));
            return false;           /* someone's, and in use: not to be hidden */
        }
        *at = (uint64_t)(uintptr_t)fresh | PRESENT | WRITE | USER | owned | (*at & PROT_BITS);
        __asm__ volatile("invlpg (%0)" : : "r"((uintptr_t)page) : "memory");
        memset((void *)page, 0, VM_PAGE);
        return true;
    }
    fresh = page_from(level);
    if (fresh == NULL) {
        return false;
    }
    *at = (uint64_t)(uintptr_t)fresh | PRESENT | WRITE | USER | (*at & PROT_BITS);
    __asm__ volatile("invlpg (%0)" : : "r"((uintptr_t)page) : "memory");
    return true;
}

/* An entry made to say what prot asks, mapped or not. */
static uint64_t protected(uint64_t e, unsigned prot) {
    uint64_t want = ((prot & 2) == 0 ? NOWRITE : 0) | ((prot & 7) == 0 ? NOACCESS : 0);

    if (!(e & PRESENT)) {
        return want;                /* nothing mapped: only the wish */
    }
    e = (e & ~(PROT_BITS | WRITE | USER)) | want;
    if (!(want & NOACCESS)) {
        e |= USER;
    }
    if (!(want & NOWRITE) && (!(e & SHARED) || (e & COMMON_RW))) {
        e |= WRITE;                 /* a page shared to copy stays read-only */
    }
    return e;
}

/* Tables are made to hold the wish for pages not there yet only for a
   read-only stretch of a few megabytes - a library's relocations, a
   program's data. A reservation of gigabytes made inaccessible would cost
   megabytes of tables to say so, and gets it said only where it has some. */
#define PROTECT_MAKE (16ull << 20)

void vm_protect(uint64_t addr, uint64_t size, unsigned prot) {
    uint64_t first = addr & ~(uint64_t)(VM_PAGE - 1);
    bool open = (prot & 2) != 0;
    bool make = !open && (prot & 7) != 0 && size <= PROTECT_MAKE;

    if (cur == NULL || !vm_holds(first, size) || (open && !cur->protects)) {
        return;
    }
    if (!open) {
        cur->protects = true;
    }
    for (uint64_t page = first; page < addr + size; page += VM_PAGE) {
        uint64_t *at = entry_for(page, make);

        if (at != NULL) {
            *at = protected(*at, prot);
        } else {
            page |= (1ull << 21) - VM_PAGE;     /* no table: on to the next one */
        }
    }
    flush_tlb();
}

bool vm_denied(uint64_t addr, bool write, bool user) {
    uint64_t *at;

    if (cur == NULL || !cur->protects || !vm_holds(addr, 0) ||
        (at = entry_for(addr, false)) == NULL) {
        return false;
    }
    if ((*at & NOACCESS) && (user || !(*at & PRESENT))) {
        return true;
    }
    return write && (*at & NOWRITE);
}

void vm_settle(uint64_t addr, uint64_t size) {
    bool changed = false;

    if (cur == NULL || !cur->protects) {
        return;
    }
    for (uint64_t page = addr & ~(uint64_t)(VM_PAGE - 1); page < addr + size; page += VM_PAGE) {
        uint64_t *at = vm_holds(page, VM_PAGE) ? entry_for(page, false) : NULL;

        if (at != NULL && (*at & PRESENT) && (*at & PROT_BITS) &&
            (*at & (WRITE | USER)) != (protected(*at, (*at & NOACCESS) ? 0 : 1) & (WRITE | USER))) {
            *at = protected(*at, (*at & NOACCESS) ? 0 : 1);
            changed = true;
        }
    }
    if (changed) {
        flush_tlb();
    }
}

bool vm_writable(uint64_t addr, uint64_t size) {
    if (cur == NULL || !cur->protects) {
        return true;
    }
    for (uint64_t page = addr & ~(uint64_t)(VM_PAGE - 1); page < addr + size; page += VM_PAGE) {
        uint64_t *at = vm_holds(page, 0) ? entry_for(page, false) : NULL;

        if (at != NULL && (*at & PROT_BITS)) {
            return false;
        }
    }
    return true;
}

bool vm_mapped(uint64_t addr) {
    uint64_t *at;

    if (!vm_holds(addr, 0)) {
        return false;
    }
    at = entry_for(addr, false);
    return at != NULL && (*at & PRESENT) != 0 && !KERNEL_VIEW(*at);
}

bool vm_map_shared(uint64_t addr, uint64_t page, bool writable) {
    uint64_t *at;

    /* Not under a program at a fixed address: its pages are the ones at their
       own address there, or ones nothing else could be hiding (vm_fault). */
    if (addr < LOW_LIMIT || !vm_holds(addr, VM_PAGE) || (at = entry_for(addr, true)) == NULL ||
        (*at & PRESENT)) {
        return false;
    }
    /* Writable is memory processes share on purpose (MAP_SHARED): every
       write is everyone's, so it is never copied. */
    *at = page | PRESENT | USER | SHARED | (writable ? WRITE | COMMON_RW : 0) | (*at & PROT_BITS);
    __asm__ volatile("invlpg (%0)" : : "r"((uintptr_t)addr) : "memory");
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

/* Private pages under addr .. addr + size given back (madvise DONTNEED):
   touched again they come back empty, or refilled from their file. A shared
   page, and a device's, stay. */
void vm_discard(uint64_t addr, uint64_t size) {
    struct space *level = cur;

    if (!vm_holds(addr, size)) {
        return;
    }
    for (uint64_t page = addr; page < addr + size; page += VM_PAGE) {
        uint64_t *at = entry_for(page, false);

        if (at != NULL && (*at & PRESENT) && !(*at & (SHARED | DEVICE))) {
            low_page_back(level, *at);
            *at &= PROT_BITS;       /* gone, but still what it was asked to be */
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

    for (unsigned i = 0; REGIONS && i < ENTRIES; i++) {
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
        if (level == applied) {
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
                page_back(level, (uint64_t)(uintptr_t)pt);
                level->tables -= VM_PAGE;
            }
        }
        page_back(level, (uint64_t)(uintptr_t)level->low_tables);
        level->low_tables = NULL;
        level->low_start = level->low_end = 0;
    }
    for (unsigned i = 0; REGIONS && i < ENTRIES; i++) {
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
    level->protects = false;
    if (whole) {
        if (REGIONS) {
            pages_back_to_firmware((uint64_t)(uintptr_t)pdpt, 1);
        }
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
    /* With no regions the space has no table of its own, and pdpt only
       says it is in use: it points at nothing that is ever read. */
    if (i == SPACES || !slot_init() ||
        (s->pdpt = REGIONS ? (uint64_t *)table_page() : (uint64_t *)s) == NULL) {
        return false;
    }
    spaces[i] = s;
    return true;
}

void vm_space_use(struct space *s) {
    uintptr_t cr0;

    if (s == cur) {
        return;
    }
    if (cur != NULL) {
        low_apply(cur, false);
    }
    if (REGIONS) {
        cr0 = write_protect_off();
        pml4()[slot] = s != NULL ? (uint64_t)(uintptr_t)s->pdpt | PRESENT | WRITE | USER : 0;
        write_protect_back(cr0);
    }
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
        *at = (*at & (ADDR | PROT_BITS | USER)) | PRESENT | SHARED;
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
    *to = (uint64_t)(uintptr_t)page | (*at & (PRESENT | WRITE | USER | PROT_BITS));
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
    child->protects = cur->protects;
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

    for (unsigned i = 0; REGIONS && i < SPACES; i++) {
        total += spaces[i] != NULL ? VM_PAGE : 0;
    }
    return total;
}
