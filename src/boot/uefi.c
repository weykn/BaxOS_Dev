/* The Tuxlet OS UEFI loader: BOOTX64.EFI.
 *
 * Firmware hands us a machine already in long mode with everything mapped,
 * so there is no mode switching to do - only finding the things the kernel
 * cannot find for itself once it is running, and putting the kernel where it
 * expects to be.
 *
 * The kernel is built into this file rather than read from disk, so the boot
 * partition holds one file and the loader needs no filesystem code at all.
 *
 * Built by a compiler that speaks Microsoft's calling convention, which is
 * UEFI's; the kernel is built for SysV, so the jump across is marked. */

#include <stdbool.h>

#include "../kernel/boot.h"
#include "../kernel/efi.h"
#include "../kernel/syscall.h"

/* Put there by objcopy, from build/kernel.bin. */
extern const unsigned char kernel_start[], kernel_end[];

typedef __attribute__((sysv_abi, noreturn)) void (*kernel_entry)(struct boot_info *);

static struct efi_system_table *st;
static struct efi_boot_services *bs;
static struct boot_info info;

static void say(const uint16_t *text) {
    st->con_out->output_string(st->con_out, text);
}

/* Prints the message and stops, leaving it on screen to be read. */
static void fail(const uint16_t *text) {
    say(u"Tuxlet OS: ");
    say(text);
    say(u"\r\n");
    for (;;) {
        __asm__ volatile("hlt");
    }
}

static void *zero(void *dest, uint64_t count) {
    unsigned char *d = dest;

    while (count-- > 0) {
        *d++ = 0;
    }
    return dest;
}

static void copy(void *dest, const void *src, uint64_t count) {
    unsigned char *d = dest;
    const unsigned char *s = src;

    while (count-- > 0) {
        *d++ = *s++;
    }
}

/* Binds a driver to every device the firmware knows about. It is the
   slowest thing a boot does - most of half a second on a machine with much
   plugged into it - so it is a fallback here and the kernel does it once the
   shell is up, where nobody is kept waiting by it. */
static void connect_everything(void) {
    efi_handle *handles;
    efi_uintn count = 0;

    if (EFI_ERROR(bs->locate_handle_buffer(0 /* all handles */, 0, 0, &count, &handles))) {
        return;
    }
    for (efi_uintn i = 0; i < count; i++) {
        bs->connect_controller(handles[i], 0, 0, 1);
    }
    bs->free_pool(handles);
}

/* ---- the screen ---------------------------------------------------------
 *
 * Whatever mode the firmware is already in is the one we take - on a laptop
 * that is the panel's own resolution.
 *
 * Asking for a different one does not stick. The firmware's console driver
 * still owns the screen while boot services are running, and the next time
 * anything makes it look, it puts the mode back to the one it believes in
 * and leaves us drawing a 1024x768 picture into a 1280x800 scanout. So the
 * mode is the firmware's to choose, and the console is sized to fit it. */

static void find_screen(void) {
    struct efi_guid gop_guid = EFI_GOP_GUID;
    struct efi_gop *gop;
    struct efi_gop_info *mode;

    if (EFI_ERROR(bs->locate_protocol(&gop_guid, 0, (void **)&gop)) || gop == 0) {
        fail(u"no graphics output protocol");
    }
    mode = gop->mode->info;
    if (mode->pixel_format > EFI_PIXEL_BGRX) {
        fail(u"the screen has no framebuffer to draw in");
    }
    info.framebuffer  = gop->mode->framebuffer;
    info.width        = mode->width;
    info.height       = mode->height;
    info.pitch        = mode->pixels_per_scanline * 4;
    info.pixel_format = mode->pixel_format;
}

/* ---- the disk -----------------------------------------------------------
 *
 * The filesystem is in a partition of its own, which firmware presents as a
 * block device whose first sector is the partition's. Rather than parse a
 * partition table, every block device is asked for the sector the filesystem
 * keeps its table in, and the one that answers with the right magic is it. */

static bool find_disk(void) {
    struct efi_guid block_guid = EFI_BLOCK_IO_GUID;
    efi_handle *handles;
    efi_uintn count = 0;
    void *sector;

    if (EFI_ERROR(bs->locate_handle_buffer(2 /* by protocol */, &block_guid,
                                           0, &count, &handles))) {
        return false;
    }
    if (EFI_ERROR(bs->allocate_pool(EFI_LOADER_DATA, 512, &sector))) {
        return false;
    }
    for (efi_uintn i = 0; i < count; i++) {
        struct efi_block_io *disk;

        if (EFI_ERROR(bs->handle_protocol(handles[i], &block_guid, (void **)&disk))) {
            continue;
        }
        if (!disk->media->present || disk->media->block_size != 512) {
            continue;
        }
        if (EFI_ERROR(disk->read_blocks(disk, disk->media->media_id, FS_LBA, 512, sector))) {
            continue;
        }
        if (*(volatile uint32_t *)sector == FS_MAGIC) {
            info.disk = disk;
            info.media_id = disk->media->media_id;
            break;
        }
    }
    bs->free_pool(sector);
    return info.disk != 0;
}

/* ---- memory -------------------------------------------------------------- */

/* Usable RAM, in KiB: what the firmware has not already spoken for, plus the
   two regions we are about to take, which are the kernel's own. */
static void measure_memory(void) {
    struct efi_memory_descriptor *map = 0;
    efi_uintn size = 0, key, stride;
    uint64_t pages = 0, ram = 0;
    uint32_t version;

    bs->get_memory_map(&size, map, &key, &stride, &version);
    size += 8 * stride;                 /* the call itself may add entries */
    if (EFI_ERROR(bs->allocate_pool(EFI_LOADER_DATA, size, (void **)&map))) {
        return;
    }
    if (!EFI_ERROR(bs->get_memory_map(&size, map, &key, &stride, &version))) {
        for (efi_uintn off = 0; off < size; off += stride) {
            struct efi_memory_descriptor *d = (void *)((char *)map + off);
            if (d->type == EFI_CONVENTIONAL_MEMORY) {
                pages += d->pages;
            }
            if (efi_is_ram(d->type)) {
                ram += d->pages;
            }
        }
    }
    bs->free_pool(map);
    info.memory_kib = pages * 4;
    info.ram_kib = ram * 4;
}

/* ---- taking memory for the kernel ---------------------------------------- */

/* Anywhere below 4 GiB: the kernel runs wherever it is put. Returns where,
   or 0 if there is no room at all. */
static uint64_t take(uint64_t bytes) {
    uint64_t at = 0xFFFFFFFF;

    return EFI_ERROR(bs->allocate_pages(EFI_ALLOCATE_MAX, EFI_LOADER_DATA,
                                        bytes / 4096, &at)) ? 0 : at;
}

efi_status EFIAPI efi_main(efi_handle image, struct efi_system_table *table) {
    /* Before anything else, so that uptime counts the whole of the boot. */
    info.started = __builtin_ia32_rdtsc();

    st = table;
    bs = table->boot;

    /* Firmware resets the machine after five minutes of an application not
       finishing. This one never finishes. */
    bs->set_watchdog_timer(0, 0, 0, 0);

    info.magic  = BOOT_MAGIC;
    info.system = st;
    info.image  = image;

    find_screen();

    /* The firmware only binds drivers to what it needed to boot - which
       includes the disk it read this file off, so the filesystem beside us
       on it is usually there for the asking. Only a firmware that has not
       done that pays for binding the rest here. */
    if (!find_disk()) {
        connect_everything();
        find_disk();
    }
    measure_memory();

    uint64_t base = take(KERNEL_BYTES);

    if (base == 0) {
        fail(u"no memory for the kernel");
    }

    /* The image, then zeroes for its .bss - which the flat binary does not
       carry - and the stack that lives in it. */
    zero((void *)base, KERNEL_BYTES);
    copy((void *)base, kernel_start, (uint64_t)(kernel_end - kernel_start));

    ((kernel_entry)base)(&info);
}
