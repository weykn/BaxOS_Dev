#include "debug.h"
#include "string.h"
#include "efi.h"

#include <stddef.h>

#include "ata.h"
#include "boot.h"
#include "efi_kernel.h"
#include "vm.h"
#include "fs.h"
#include "io.h"
#include "mem.h"
#include "keyboard.h"
#include "driver.h"
#include "vga.h"

/* The kernel's side of the firmware.
 *
 * Until efi_leave, boot services are still running, and the keyboard and
 * the disk are the firmware's drivers rather than ours. After it, they are
 * the modules' that registered for them (driver.h) - storage/ide and
 * input/ps2 - and all that is left of the firmware is its runtime services:
 * the clock, and switching the machine off. A machine with no module for
 * its disk or its keyboard - a USB keyboard, an NVMe disk - never leaves,
 * and goes on as before.
 *
 * Everything here is a call back into firmware, so it costs a switch to
 * Microsoft's calling convention and nothing else. */

/* A copy: the loader's own lies in memory that is handed back with it. */
static struct boot_info kept;
static struct boot_info *boot;
static uint64_t         disk_base;  /* where the partition starts, once a disk
                                       module has the disk; 0 while the
                                       firmware does */

const struct disk_driver *disk_driver;

/* A disk module takes the disk over the moment it registers, firmware or no
   firmware: a firmware read costs ten milliseconds, one of ours a few
   microseconds, and everything the boot does after the first module - the
   rest of them, the scripts, the fonts - is reads. The firmware's driver
   only touches the controller when it is called, and it is not called
   again. Taking the module away before the firmware is gone hands the disk
   back to it. */
void disk_register(const struct disk_driver *d) {
    ata_sync();                     /* what the old way has, on the disk */
    disk_driver = d;
    disk_base = d != NULL ? d->find(FS_LBA, FS_MAGIC) : 0;
    fs_cache(disk_base == 0);       /* a read is quick from here on */
    dbg("efi: disk %s\n", disk_base != 0 ? "is the module's" : "is the firmware's");
}

void efi_init(struct boot_info *info) {
    kept = *info;
    boot = &kept;
}

const struct boot_info *efi_boot(void) {
    return boot;
}

/* ---- memory -------------------------------------------------------------- */

uint64_t efi_free_kib(void) {
    struct efi_boot_services *bs = boot->system->boot;
    struct efi_memory_descriptor *map = NULL;
    efi_uintn size = 0, key, stride;
    uint64_t pages = 0;
    uint32_t version;

    /* Asked once for the size, then for the map: the pool it goes in may add
       an entry or two of its own. */
    bs->get_memory_map(&size, map, &key, &stride, &version);
    size += 4 * stride;
    if (EFI_ERROR(bs->allocate_pool(EFI_LOADER_DATA, size, (void **)&map))) {
        return 0;
    }
    if (!EFI_ERROR(bs->get_memory_map(&size, map, &key, &stride, &version))) {
        for (efi_uintn at = 0; at < size; at += stride) {
            const struct efi_memory_descriptor *d = (const void *)((char *)map + at);

            if (d->type == EFI_CONVENTIONAL_MEMORY) {
                pages += d->pages;
            }
        }
    }
    bs->free_pool(map);
    return pages * 4;
}

/* In syscall_entry.asm: the RFLAGS a program starts with. */
extern uint64_t user_flags;

/* ---- letting the firmware go --------------------------------------------- */

const char *efi_leave(void) {
    static bool gone;
    struct efi_boot_services *bs = boot->system->boot;
    efi_uintn size = 0, key, stride, room;
    uint32_t version;
    uint64_t map;

    if (gone) {
        return "already done";
    }
    /* Only if what it does can be done without it: the filesystem on a disk
       a module can drive, and a keyboard one can. */
    if (disk_base == 0) {
        return "no disk driver";
    }
    if (keyboard_driver == NULL || !keyboard_driver->start()) {
        return "no keyboard driver";
    }
    efi_uptime_ms();                /* the clock is timed against the firmware */

    /* The map, asked for until ExitBootServices takes it: anything the
       firmware does in between - even allocating this - changes it. */
    bs->get_memory_map(&size, NULL, &key, &stride, &version);
    room = size + 16 * stride;
    map = mem_pages((room + 4095) / 4096);
    if (map == 0) {
        return "out of memory";
    }
    /* The kernel's own page tables, while there is still a firmware to give
       memory for them: as far up as there is RAM, or the screen. */
    uint64_t top = vga_framebuffer_end();

    size = room;
    if (!EFI_ERROR(bs->get_memory_map(&size, (void *)map, &key, &stride, &version))) {
        for (efi_uintn at = 0; at < size; at += stride) {
            const struct efi_memory_descriptor *d = (const void *)(map + at);
            uint64_t end = d->physical + d->pages * 4096;

            top = efi_is_ram(d->type) && end > top ? end : top;
        }
    }
    if (!mem_own_tables(top)) {
        mem_pages_free(map, (room + 4095) / 4096);
        return "out of memory";
    }
    for (unsigned tries = 0; ; tries++) {
        size = room;
        if (EFI_ERROR(bs->get_memory_map(&size, (void *)map, &key, &stride, &version))) {
            if (tries == 0) {
                mem_pages_free(map, (room + 4095) / 4096);
            }
            return "no memory map";
        }
        if (!EFI_ERROR(bs->exit_boot_services(boot->image, key))) {
            break;
        }
        if (tries == 3) {
            return "firmware refused";  /* gone half way: nothing to call to undo */
        }
    }

    /* Nothing of the firmware's may run now, interrupts least of all: its
       handlers are in memory that is about to be reused. Programs run with
       them off too; nothing here uses one. */
    __asm__ volatile("cli");
    outb(0x21, 0xFF);               /* both PICs, every line masked */
    outb(0xA1, 0xFF);
    user_flags = 0x002;             /* and programs start without them */
    boot->disk = NULL;
    vga_firmware_gone();
    mem_take_over((const void *)map, size, stride);
    mem_pages_free(map, (room + 4095) / 4096);
    gone = true;
    dbg("efi: left the firmware; %u KiB free\n", mem_free_kib());
    return NULL;
}

/* ---- the keyboard ------------------------------------------------------- */

/* A key with no character of its own arrives from the firmware as a scan
   code, and goes on as the escape sequence a terminal sends for it - which is
   what a program doing its own line editing watches for. The sequence is
   handed over a character at a time, since that is how a key is read. */
static const char *pending;

static const char *firmware_key(uint16_t scan) {
    switch (scan) {
    case 0x01: return "\033[A";     /* up */
    case 0x02: return "\033[B";     /* down */
    case 0x03: return "\033[C";     /* right */
    case 0x04: return "\033[D";     /* left */
    case 0x05: return "\033[H";     /* home */
    case 0x06: return "\033[F";     /* end */
    case 0x07: return "\033[2~";    /* insert */
    case 0x08: return "\033[3~";    /* delete */
    case 0x09: return "\033[5~";    /* page up */
    case 0x0A: return "\033[6~";    /* page down */
    case 0x17: return "\033";       /* escape */
    default:   return NULL;
    }
}

/* UEFI hands back a key as a character where it has one, and as a scan code
   where it has not. */
char keyboard_poll_char(char (*idle)(void)) {
    struct efi_text_input *in = boot->system->con_in;
    struct efi_input_key key;
    char c = idle != NULL ? idle() : 0;

    if (c != 0) {
        return c;
    }
    if (pending != NULL) {
        c = *pending++;
        if (*pending == '\0') {
            pending = NULL;
        }
        return c;
    }
    /* A PS/2 keyboard is read here; one on USB still by the firmware, while
       there is one. */
    c = keyboard_driver != NULL ? keyboard_driver->key() : 0;
    if (c != 0 || mem_ours()) {
        return c;
    }
    if (EFI_ERROR(in->read_key(in, &key))) {
        return 0;
    }
    if (key.unicode_char == 0) {
        const char *text = firmware_key(key.scan_code);

        if (text == NULL) {
            return 0;
        }
        pending = text[1] != '\0' ? text + 1 : NULL;
        return text[0];
    }
    if (key.unicode_char == '\r') {
        return '\n';                    /* firmware reports Enter as a return */
    }
    /* A control character counts: Ctrl-D ends a program's input, and a
       shell of its own binds the rest. */
    if (key.unicode_char > 0 && key.unicode_char < 0x7F) {
        return (char)key.unicode_char;
    }
    return 0;
}

char keyboard_read_char(char (*idle)(void)) {
    for (;;) {
        char c = keyboard_poll_char(idle);

        if (c != 0) {
            return c;
        }
        __asm__ volatile("pause");
    }
}

/* ---- the disk ----------------------------------------------------------- */

/* The filesystem is in a partition of its own, which firmware presents as a
   block device starting at its own sector 0 - so the numbers fs.c uses go
   straight through. */

int ata_read(uint32_t lba, void *buffer) {
    return ata_read_many(lba, 1, buffer);   /* through the cache, like the rest */
}

/* Sectors in one request.
 *
 * What a read costs is the round trip, not the bytes: firmware drivers were
 * written for a machine that reads a loader once and then gets out of the
 * way, and asking one for a sector costs about what asking it for a thousand
 * does. So everything is asked for in as few calls as it can be - the two
 * megabytes of C library behind every program is the difference between a
 * fifth of a second and a fiftieth.
 *
 * How much a driver will take at once is its own business and the protocol
 * does not say: a virtio disk answers "bogus descriptor or out of resources"
 * when asked for a whole library in one go. So the size is found rather than
 * assumed - it starts high and halves whenever a driver refuses, which costs
 * one failed call on a machine that cannot take the full amount. */
#define BLOCK_RUN_MAX 8192      /* four megabytes */
#define BLOCK_RUN_MIN 8

static unsigned block_run = BLOCK_RUN_MAX;

/* Straight to the disk, with no cache in the way. */
static int read_now(uint32_t lba, unsigned count, void *buffer) {
    struct efi_block_io *disk = boot->disk;

    if (disk_base != 0) {
        return count == 0 ? -1 : disk_driver->read(disk_base + lba, count, buffer);
    }
    if (disk == NULL || count == 0) {
        return -1;
    }
    while (count > 0) {
        unsigned n = count < block_run ? count : block_run;

        if (EFI_ERROR(disk->read_blocks(disk, boot->media_id, lba,
                                        (efi_uintn)n * 512, buffer))) {
            if (block_run > BLOCK_RUN_MIN) {
                block_run /= 2;     /* more than this driver will take */
                continue;
            }
            return -1;
        }
        buffer = (char *)buffer + (size_t)n * 512;
        lba += n;
        count -= n;
    }
    return 0;
}

/* ---- through the cache --------------------------------------------------- */

/* Straight to the disk, or through the cache module when there is one. */
int ata_read_many(uint32_t lba, unsigned count, void *buffer) {
    if (count == 0) {
        return -1;
    }
    return disk_cache != NULL ? disk_cache->read(lba, count, buffer, read_now)
                              : read_now(lba, count, buffer);
}

int ata_write(uint32_t lba, const void *buffer) {
    return ata_write_many(lba, 1, buffer);
}

/* Whether anything written has not been flushed since. A flush makes the
   drive put its own cache on the platters, and costs a trip to the host's
   fsync under an emulator, so it waits - as on Linux - for the machine to go
   idle, for a program to ask (fsync, sync), or for the power to go. */
static bool unflushed;

int ata_write_many(uint32_t lba, unsigned count, const void *buffer) {
    struct efi_block_io *disk = boot->disk;

    unflushed = true;

    if (count == 0 || (disk == NULL && disk_base == 0)) {
        return -1;
    }
    if (disk_cache != NULL) {
        disk_cache->written(lba, count, buffer);
    }
    if (disk_base != 0) {
        return disk_driver->write(disk_base + lba, count, buffer);
    }
    while (count > 0) {
        unsigned n = count < block_run ? count : block_run;

        if (EFI_ERROR(disk->write_blocks(disk, boot->media_id, lba,
                                         (efi_uintn)n * 512, buffer))) {
            if (block_run > BLOCK_RUN_MIN) {
                block_run /= 2;
                continue;
            }
            return -1;
        }
        buffer = (const char *)buffer + (size_t)n * 512;
        lba += n;
        count -= n;
    }
    return 0;
}

/* Flushing is what a write actually costs: the firmware takes the sector in
   hand at once and then spends milliseconds putting it on the disk. Doing it
   after every sector made saving the file table - sixteen of them - sixteen
   times slower than it had to be, and that table is saved by every write
   there is. One flush at the end of an operation is as durable: nothing has
   returned to the program until it is through. */
void ata_sync(void) {
    struct efi_block_io *disk = boot->disk;

    if (!unflushed) {
        return;
    }
    unflushed = false;
    if (disk_base != 0) {
        disk_driver->flush();
    } else if (disk != NULL) {
        disk->flush_blocks(disk);
    }
}

/* ---- the clock ----------------------------------------------------------
 *
 * Two clocks, for the two questions. The firmware's own reading of the RTC
 * says what time it is; the processor's timestamp counter says how long we
 * have been up, because the loader reads it before it does anything else
 * and it does not care what midnight does.
 *
 * How fast that counter runs is worked out the first time anyone asks, by
 * timing a stall against the firmware - which is why the reading is taken
 * before the working out, so the cost of asking never lands in the answer,
 * and why nothing pays for it unless uptime is wanted. */

static uint64_t rdtsc(void) {
    uint32_t low, high;

    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return (uint64_t)high << 32 | low;
}

#define CALIBRATE_US 10000      /* the stall uptime is measured against */

static uint64_t ticks_per_second;

uint64_t efi_uptime_ms(void) {
    uint64_t now = rdtsc();

    if (ticks_per_second == 0) {
        uint64_t from = rdtsc();

        boot->system->boot->stall(CALIBRATE_US);
        ticks_per_second = (rdtsc() - from) * (1000000 / CALIBRATE_US);
        if (ticks_per_second == 0) {
            ticks_per_second = 1;       /* a counter that does not count */
        }
    }
    return (now - boot->started) * 1000 / ticks_per_second;
}

/* The same in microseconds, for what is over in a fraction of one - a
   syscall. Taken in two parts so the multiplication cannot overflow. */
uint64_t efi_uptime_us(void) {
    uint64_t ticks;

    efi_uptime_ms();                    /* calibrates, the first time */
    ticks = rdtsc() - boot->started;
    return ticks / ticks_per_second * 1000000 +
           ticks % ticks_per_second * 1000000 / ticks_per_second;
}


/* The firmware's runtime services keep their data in memory a program linked
   to a fixed address may have mapped over (vm.c), so they are called with
   the firmware's own view of low memory put back for the length of the call. */
static efi_status get_time(struct efi_time *now) {
    efi_status status;

    vm_firmware_view(true);
    status = boot->system->runtime->get_time(now, NULL);
    vm_firmware_view(false);
    return status;
}

/* Seconds past midnight, from the firmware's own reading of the clock. */
unsigned efi_seconds(void) {
    struct efi_time now;

    if (EFI_ERROR(get_time(&now))) {
        return 0;
    }
    return ((unsigned)now.hour * 60 + now.minute) * 60 + now.second;
}

/* The same clock counted the way every program expects it: seconds since the
   start of 1970. The firmware gives a date, so this is only arithmetic - days
   for the whole years since then, days for the whole months of this one, and
   the day of the month. Leap years are every fourth, bar centuries that are
   not fourth centuries, which is the whole of the rule inside this range. */
uint64_t efi_epoch(void) {
    static const unsigned before[] = { 0, 31, 59, 90, 120, 151,
                                       181, 212, 243, 273, 304, 334 };
    struct efi_time now;
    uint64_t days;

    if (EFI_ERROR(get_time(&now)) ||
        now.year < 1970 || now.month < 1 || now.month > 12) {
        return 0;
    }
    unsigned year = now.year;
    unsigned leaps = 0;

    for (unsigned y = 1970; y < year; y++) {
        leaps += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 1 : 0;
    }
    days = (uint64_t)(year - 1970) * 365 + leaps + before[now.month - 1];
    if (now.month > 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) {
        days++;                     /* this year's own, once February is past */
    }
    days += now.day - 1;

    /* The firmware keeps the offset from UTC in minutes, or says it does not
       know; either way the seconds are counted from there. */
    int64_t offset = now.time_zone == 0x07FF ? 0 : now.time_zone;

    return ((days * 24 + now.hour) * 60 + now.minute - (uint64_t)offset) * 60 + now.second;
}

/* ---- switching off ------------------------------------------------------ */

void efi_power_off(void) {
    ata_sync();
    vm_firmware_view(true);
    boot->system->runtime->reset_system(EFI_RESET_SHUTDOWN, EFI_SUCCESS, 0, NULL);
}

void efi_restart(void) {
    ata_sync();
    vm_firmware_view(true);
    boot->system->runtime->reset_system(EFI_RESET_COLD, EFI_SUCCESS, 0, NULL);
}
