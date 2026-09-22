#pragma once

#include <stdint.h>

#include "efi.h"

/* What the loader hands the kernel, and the only thing that knows how the
 * machine was started.
 *
 * The loader does not call ExitBootServices: the firmware keeps its drivers
 * running, and the kernel reaches the keyboard, the disk and the clock
 * through them. That is what makes this run on a machine whose keyboard is
 * USB and whose disk is NVMe without a line of driver code for either.
 *
 * The price is that firmware's memory has to stay mapped and its watchdog
 * has to be turned off, both of which the loader sees to. */

#define BOOT_MAGIC 0x536F7861426ULL     /* "BaxoS" */

/* Where the loader puts things. Both are whole 2 MiB pages, which is what
   the kernel's own page tables map them with. */
#define KERNEL_BASE   0x200000          /* the kernel image, its bss and stack */
#define KERNEL_BYTES  0x200000

/* The ring 3 window, at PROGRAM_BASE. The loader does not reserve it: the
   kernel buys it from the firmware a page at a time as a program touches it,
   so an idle machine holds none of it. */
#define PROGRAM_BYTES 0x200000

struct boot_info {
    uint64_t magic;

    struct efi_system_table *system;    /* firmware, still in boot services */
    efi_handle               image;     /* the loader's own image handle */

    uint64_t framebuffer;               /* physical address, already mapped */
    uint32_t width, height;
    uint32_t pitch;                     /* bytes from one scan line to the next */
    uint32_t pixel_format;              /* EFI_PIXEL_BGRX or EFI_PIXEL_RGBX */

    struct efi_block_io *disk;          /* the partition holding the filesystem */
    uint32_t             media_id;
    uint32_t             reserved;

    uint64_t memory_kib;                /* RAM the firmware says is usable */

    /* The timestamp counter as the loader was entered, which is the earliest
       moment this machine can be asked about. Uptime counts from here, so it
       covers loading the kernel as well as running it. */
    uint64_t started;
};
