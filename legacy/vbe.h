#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Graphics modes, through the display adapter's DISPI registers.
 *
 * The VGA text modes in vga.c are set by programming the chip directly, and
 * the same trick does not reach beyond VGA's own timings - 320x200 and a few
 * relatives. Anything larger is the video BIOS's job, and the BIOS is
 * real-mode code the kernel cannot call once it is in long mode.
 *
 * What it can do is talk to the adapter itself. The Bochs display interface,
 * which QEMU's standard VGA implements, exposes the mode as two I/O ports:
 * write a register index to one and its value to the other. Width, height and
 * depth are plain numbers rather than a list of mode numbers, so any size the
 * card has memory for can be set, at any time, without leaving long mode.
 *
 * The framebuffer itself is a PCI memory window - found by reading the
 * adapter's first base address register - which vbe_set maps before
 * returning. */

/* Video memory mapped past the visible image, for the caller to use as
   scratch, and real memory rather than just mapped address space. Mapping
   happens in 2 MiB pages, so the slack is already there and costs nothing;
   putting the text console's cells in it keeps tens of kilobytes of system
   RAM free on a machine that may only have a megabyte. */
#define VBE_SPARE 65536

struct vbe_mode {
    volatile uint8_t *base;     /* the framebuffer, mapped and ready to write */
    unsigned width, height;
    unsigned pitch;             /* bytes from one scan line to the next */
    unsigned bpp;               /* bits per pixel; always 32 here */
    uint32_t size;              /* bytes the visible image takes */
    uint32_t mapped;            /* bytes mapped: at least size + VBE_SPARE */
};

/* True if the adapter has the DISPI registers at all. */
bool vbe_available(void);

/* Video memory in bytes, which is what caps the size of a mode. */
uint32_t vbe_memory(void);

/* Switches to width x height in 32-bit colour and maps the framebuffer.
   Returns false if the adapter has no DISPI registers, the size is not one it
   will take, or there is not enough video memory for it. */
bool vbe_set(unsigned width, unsigned height);

/* Back to text mode. */
void vbe_off(void);

/* The mode in use, or NULL in text mode. */
const struct vbe_mode *vbe_current(void);
