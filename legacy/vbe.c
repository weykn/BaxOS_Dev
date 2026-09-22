#include "vbe.h"

#include <stddef.h>

#include "io.h"
#include "string.h"

/* ---- the DISPI registers ------------------------------------------------ */

#define DISPI_INDEX 0x01CE
#define DISPI_DATA  0x01CF

enum {
    DISPI_ID, DISPI_XRES, DISPI_YRES, DISPI_BPP, DISPI_ENABLE, DISPI_BANK,
    DISPI_VIRT_WIDTH, DISPI_VIRT_HEIGHT, DISPI_X_OFFSET, DISPI_Y_OFFSET,
    DISPI_VIDEO_MEMORY_64K,
};

#define DISPI_DISABLED 0x00
#define DISPI_ENABLED  0x01
#define DISPI_LFB      0x40     /* put the framebuffer in the PCI window */

/* The interface announces itself with one of a short run of version numbers.
   Anything from the first version that has a linear framebuffer will do. */
#define DISPI_ID0 0xB0C0
#define DISPI_ID5 0xB0C5

#define BPP 32                  /* the only depth here: one pixel, one uint32 */

static void dispi_write(uint16_t reg, uint16_t value) {
    outw(DISPI_INDEX, reg);
    outw(DISPI_DATA, value);
}

static uint16_t dispi_read(uint16_t reg) {
    outw(DISPI_INDEX, reg);
    return inw(DISPI_DATA);
}

bool vbe_available(void) {
    uint16_t id = dispi_read(DISPI_ID);
    return id >= DISPI_ID0 && id <= DISPI_ID5;
}

uint32_t vbe_memory(void) {
    return (uint32_t)dispi_read(DISPI_VIDEO_MEMORY_64K) * 64 * 1024;
}

/* ---- finding the framebuffer -------------------------------------------- */

#define PCI_ADDRESS 0xCF8
#define PCI_DATA    0xCFC

#define PCI_VENDOR  0x00
#define PCI_CLASS   0x08        /* revision, then the three class bytes */
#define PCI_BAR0    0x10

#define CLASS_DISPLAY 0x03
#define BAR_IO        0x01      /* set in a BAR that describes ports, not memory */
#define NO_DEVICE     0xFFFF

static uint32_t pci_read(unsigned slot, unsigned func, unsigned offset) {
    outl(PCI_ADDRESS, 0x80000000u | slot << 11 | func << 8 | (offset & 0xFC));
    return inl(PCI_DATA);
}

/* The display adapter's framebuffer window, or 0 if there is no adapter.
   Only bus 0 is searched: the emulated machines put it there. */
static uint64_t framebuffer_address(void) {
    for (unsigned slot = 0; slot < 32; slot++) {
        if ((pci_read(slot, 0, PCI_VENDOR) & 0xFFFF) == NO_DEVICE) {
            continue;
        }
        if (pci_read(slot, 0, PCI_CLASS) >> 24 != CLASS_DISPLAY) {
            continue;
        }
        uint32_t bar = pci_read(slot, 0, PCI_BAR0);
        if ((bar & BAR_IO) == 0) {
            return bar & ~0xFu;     /* the low bits are flags, not address */
        }
    }
    return 0;
}

/* ---- mapping it --------------------------------------------------------- */

#define PAGE_2MIB   0x200000
#define PD_ENTRIES  512
#define GIB         0x40000000ull

/* present | writable | 2 MiB page | write-through. Write-through rather than
   the usual write-back: what lands in the framebuffer is meant to be looked
   at, so it should reach the card rather than sit in the cache. */
#define PAGE_FB 0x8B

extern uint64_t pdpt[PD_ENTRIES];    /* boot.asm's, through kernel.ld */
extern uint64_t fb_pd[PD_ENTRIES];

/* Identity-maps bytes of the framebuffer at phys. Returns false if it spans
   more than the one gigabyte fb_pd covers. */
static bool map_framebuffer(uint64_t phys, uint64_t bytes) {
    uint64_t first = phys & ~(uint64_t)(PAGE_2MIB - 1);
    uint64_t last = (phys + bytes - 1) & ~(uint64_t)(PAGE_2MIB - 1);

    if (phys / GIB != (phys + bytes - 1) / GIB) {
        return false;
    }
    memset(fb_pd, 0, sizeof fb_pd);
    for (uint64_t page = first; page <= last; page += PAGE_2MIB) {
        fb_pd[page % GIB / PAGE_2MIB] = page | PAGE_FB;
    }
    pdpt[phys / GIB] = (uint64_t)fb_pd | 0x03;      /* present | writable */

    /* Reloading CR3 drops the stale entries the walker cached. */
    __asm__ volatile("mov %%cr3, %%rax\n\tmov %%rax, %%cr3" : : : "rax", "memory");
    return true;
}

/* ---- modes -------------------------------------------------------------- */

static struct vbe_mode mode;
static bool active;

const struct vbe_mode *vbe_current(void) {
    return active ? &mode : NULL;
}

bool vbe_set(unsigned width, unsigned height) {
    /* A whole number of 8-pixel glyphs across keeps the text console simple,
       and the interface itself only takes multiples of 8 wide. */
    if (!vbe_available() || width == 0 || height == 0 || width % 8 != 0) {
        return false;
    }
    uint64_t phys = framebuffer_address();
    if (phys == 0) {
        return false;
    }

    /* The size registers only take effect while the mode is off. */
    dispi_write(DISPI_ENABLE, DISPI_DISABLED);
    dispi_write(DISPI_XRES, (uint16_t)width);
    dispi_write(DISPI_YRES, (uint16_t)height);
    dispi_write(DISPI_BPP, BPP);
    dispi_write(DISPI_VIRT_WIDTH, (uint16_t)width);
    dispi_write(DISPI_X_OFFSET, 0);
    dispi_write(DISPI_Y_OFFSET, 0);
    dispi_write(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB);

    /* An adapter that cannot do the size asked for quietly settles on one it
       can, so believe the registers rather than the request. */
    if (dispi_read(DISPI_XRES) != width || dispi_read(DISPI_YRES) != height ||
        dispi_read(DISPI_BPP) != BPP) {
        dispi_write(DISPI_ENABLE, DISPI_DISABLED);
        return false;
    }

    /* Believe the pitch the adapter settled on rather than assuming it is the
       width: it is free to make scan lines wider than the image. */
    uint32_t pitch = dispi_read(DISPI_VIRT_WIDTH) * (BPP / 8);
    uint64_t size = (uint64_t)pitch * height;
    uint64_t mapped = (size + VBE_SPARE + PAGE_2MIB - 1) & ~(uint64_t)(PAGE_2MIB - 1);

    /* The spare has to be real video memory, not just mapped address space. */
    if (size + VBE_SPARE > vbe_memory() || !map_framebuffer(phys, mapped)) {
        dispi_write(DISPI_ENABLE, DISPI_DISABLED);
        return false;
    }

    mode = (struct vbe_mode){
        .base   = (volatile uint8_t *)phys,
        .width  = width,
        .height = height,
        .pitch  = pitch,
        .bpp    = BPP,
        .size   = (uint32_t)size,
        .mapped = (uint32_t)mapped,
    };
    active = true;
    return true;
}

void vbe_off(void) {
    if (active) {
        dispi_write(DISPI_ENABLE, DISPI_DISABLED);
        active = false;
    }
}
