#include "vm.h"

#include "efi.h"
#include "efi_kernel.h"
#include "string.h"

/* x86-64 paging, four levels of it: a virtual address is four nine-bit
 * indexes and an offset, and each level holds physical addresses of the next.
 * Only one slot of the top level is ours, which is half a terabyte of address
 * space - far more than anything here will use, and free, which is what
 * matters: nothing of the firmware's is anywhere near it. */

#define PRESENT 0x001
#define WRITE   0x002
#define USER    0x004
#define BIG     0x080
#define ADDR    0x000FFFFFFFFFF000ull

#define ENTRIES   512
#define SLOT_SIZE (1ull << 39)      /* what one top-level entry covers */

static uint64_t base;               /* the region's first address */
static unsigned slot;               /* its slot in the top-level table */
static uint64_t pages;              /* lent to the program */
static uint64_t tables;             /* spent describing them */

static struct efi_boot_services *services(void) {
    return efi_boot()->system->boot;
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

/* One page of memory for a table, or for the program: any page the firmware
   has going, cleared before it is used for anything. */
static uint64_t *fresh_page(void) {
    uint64_t at = 0;

    if (EFI_ERROR(services()->allocate_pages(EFI_ALLOCATE_ANY, EFI_LOADER_DATA, 1, &at))) {
        return NULL;
    }
    memset((void *)at, 0, VM_PAGE);
    return (uint64_t *)at;
}

bool vm_start(void) {
    uint64_t *top = pml4();

    if (base != 0) {
        return true;
    }
    /* The lower half of the address space, which is where a program may run,
       is the first 256 slots. The firmware maps what the machine has at the
       bottom of it; the first slot it leaves alone is ours. */
    for (unsigned i = 1; i < 256; i++) {
        if (!(top[i] & PRESENT)) {
            uint64_t *pdpt = fresh_page();
            uint64_t cr0;

            if (pdpt == NULL) {
                return false;
            }
            cr0 = write_protect_off();
            top[i] = (uint64_t)pdpt | PRESENT | WRITE | USER;
            write_protect_back(cr0);
            tables += VM_PAGE;
            slot = i;
            base = (uint64_t)i * SLOT_SIZE;
            flush_tlb();
            return true;
        }
    }
    return false;
}

uint64_t vm_base(void) {
    return base;
}

uint64_t vm_end(void) {
    return base == 0 ? 0 : base + SLOT_SIZE;
}

bool vm_holds(uint64_t addr, uint64_t size) {
    return base != 0 && addr >= base && addr <= vm_end() && size <= vm_end() - addr;
}

/* Walks down to the entry for addr, making the tables on the way if make is
   set. Returns NULL if there is none, or if one could not be made. */
static uint64_t *entry_for(uint64_t addr, bool make) {
    uint64_t *table = table_at(pml4()[slot]);
    unsigned index[3] = {
        (unsigned)(addr >> 30) & (ENTRIES - 1),
        (unsigned)(addr >> 21) & (ENTRIES - 1),
        (unsigned)(addr >> 12) & (ENTRIES - 1),
    };

    for (unsigned level = 0; level < 2; level++) {
        uint64_t *at = &table[index[level]];

        if (!(*at & PRESENT)) {
            uint64_t *next;

            if (!make || (next = fresh_page()) == NULL) {
                return NULL;
            }
            *at = (uint64_t)next | PRESENT | WRITE | USER;
            tables += VM_PAGE;
        }
        table = table_at(*at);
    }
    return &table[index[2]];
}

bool vm_fault(uint64_t addr) {
    uint64_t *at;
    uint64_t *page;

    if (!vm_holds(addr, 0)) {
        return false;
    }
    at = entry_for(addr, true);
    if (at == NULL) {
        return false;
    }
    if (*at & PRESENT) {
        return true;                /* someone else got there first */
    }
    page = fresh_page();
    if (page == NULL) {
        return false;
    }
    *at = (uint64_t)page | PRESENT | WRITE | USER;
    pages += VM_PAGE;
    __asm__ volatile("invlpg (%0)" : : "r"(addr & ~(uint64_t)(VM_PAGE - 1)) : "memory");
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

void vm_release(uint64_t addr, uint64_t size) {
    uint64_t first = (addr + VM_PAGE - 1) & ~(uint64_t)(VM_PAGE - 1);
    uint64_t last = (addr + size) & ~(uint64_t)(VM_PAGE - 1);

    if (!vm_holds(addr, size)) {
        return;
    }
    for (uint64_t page = first; page < last; page += VM_PAGE) {
        uint64_t *at = entry_for(page, false);

        if (at != NULL && (*at & PRESENT)) {
            services()->free_pages(*at & ADDR, 1);
            *at = 0;
            pages -= VM_PAGE;
        }
    }
    flush_tlb();
}

void vm_reset(void) {
    uint64_t *pdpt;

    if (base == 0) {
        return;
    }
    pdpt = table_at(pml4()[slot]);
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
                    services()->free_pages(pt[k] & ADDR, 1);
                }
            }
            services()->free_pages(pd[j] & ADDR, 1);
            pd[j] = 0;
            tables -= VM_PAGE;
        }
        services()->free_pages(pdpt[i] & ADDR, 1);
        pdpt[i] = 0;
        tables -= VM_PAGE;
    }
    pages = 0;
    flush_tlb();
}

size_t vm_memory(void) {
    return (size_t)pages;
}

size_t vm_tables(void) {
    return (size_t)tables;
}
