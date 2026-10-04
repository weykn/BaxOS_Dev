#pragma once

#include <stdint.h>

#include "efi.h"

/* What the loader hands the kernel, and the only thing that knows how the
 * machine was started: by UEFI (src/boot/uefi.c), or by a PC BIOS
 * (src/boot/bios.asm), in which case system is NULL and the bios fields
 * say the rest.
 *
 * The UEFI loader does not call ExitBootServices: the firmware keeps its drivers
 * running, and the kernel reaches the keyboard, the disk and the clock
 * through them. That is what makes this run on a machine whose keyboard is
 * USB and whose disk is NVMe without a line of driver code for either.
 *
 * The price is that firmware's memory has to stay mapped and its watchdog
 * has to be turned off, both of which the loader sees to. */

#define BOOT_MAGIC 0x536F7861426ULL     /* "BaxoS" */

/* What the loader takes for the kernel: the image, its bss and its stack,
   wherever the firmware has room. The kernel is position-independent and
   applies its own relocations first thing, so no address is fixed - a
   fixed one is exactly what a firmware short of memory has already used. */
#define KERNEL_BYTES  0x20000

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

    uint64_t memory_kib;                /* RAM free as the loader started */
    uint64_t ram_kib;                   /* all the RAM there is, the firmware's
                                           own included */
    uint64_t firmware_kib;              /* of that, what the firmware keeps
                                           for good */

    /* The timestamp counter as the loader was entered, which is the earliest
       moment this machine can be asked about. Uptime counts from here, so it
       covers loading the kernel as well as running it. */
    uint64_t started;

    /* Only from the BIOS loader. The memory map as INT 15h E820 gave it;
       the drive it booted from, as INT 13h numbers it; and the way back
       to real mode: bios_call runs INT bios_regs->vector with the
       registers in bios_regs, and leaves there what came back. Both live
       below 1 MiB, in the loader, and stay there. */
    uint64_t e820;                      /* struct e820 entries */
    uint32_t e820_count;
    uint32_t bios_drive;
    uint64_t bios_call;                 /* void (*)(void), SysV */
    uint64_t bios_regs;                 /* struct bios_regs */
    uint64_t bios_buffer;               /* 64 KiB below 1 MiB, for INT 13h */
};

struct e820 {
    uint64_t base, length;
    uint32_t type;                      /* 1 is RAM */
    uint32_t pad;
} __attribute__((packed));

struct bios_regs {
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp;
    uint16_t ds, es;
    uint16_t flags;                     /* back out: carry is bit 0 */
    uint8_t  vector;
} __attribute__((packed));
