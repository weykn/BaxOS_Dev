#include "efi.h"

#include <stddef.h>

#include "ata.h"
#include "boot.h"
#include "keyboard.h"
#include "ps2.h"

/* The kernel's side of the firmware.
 *
 * Boot services are still running, so the keyboard, the disk and the clock
 * are the firmware's drivers rather than ours. That is the whole reason this
 * boots on a machine whose keyboard is on USB and whose disk is NVMe: those
 * drivers are already written, already loaded, and already know the machine.
 *
 * Everything here is a call back into firmware, so it costs a switch to
 * Microsoft's calling convention and nothing else. */

static struct boot_info *boot;

void efi_init(struct boot_info *info) {
    boot = info;
}

const struct boot_info *efi_boot(void) {
    return boot;
}

/* ---- the devices --------------------------------------------------------
 *
 * The firmware binds drivers only to what it needed to boot: the screen, the
 * keyboard and the disk it was read from. Everything else - a mouse most of
 * all - turns up only when it is asked to bind the rest, and on a machine
 * with much plugged in that takes most of half a second.
 *
 * So it is not done on the way up. The shell asks for it once it is waiting
 * for its first key, by which time the prompt is on screen and the wait
 * belongs to nobody. */

void efi_connect_devices(void) {
    struct efi_boot_services *bs = boot->system->boot;
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

/* ---- the keyboard ------------------------------------------------------- */

/* UEFI hands back a key as a character where it has one, which is all this
   shell wants; the arrow and function keys arrive as scan codes with no
   character and are dropped, as they were before. */
char keyboard_poll_char(char (*idle)(void)) {
    struct efi_text_input *in = boot->system->con_in;
    struct efi_input_key key;
    char c = idle != NULL ? idle() : 0;

    if (c != 0) {
        return c;
    }
    /* A PS/2 keyboard is read here; one on USB still by the firmware. */
    c = ps2_key();
    if (c != 0) {
        return c;
    }
    if (EFI_ERROR(in->read_key(in, &key)) || key.unicode_char == 0) {
        return 0;
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
    struct efi_block_io *disk = boot->disk;

    if (disk == NULL ||
        EFI_ERROR(disk->read_blocks(disk, boot->media_id, lba, 512, buffer))) {
        return -1;
    }
    return 0;
}

/* Sectors in one request. A firmware driver has a limit of its own on how
   much it will take at once - a virtio disk answers "bogus descriptor or out
   of resources" when asked for a whole library in one go - and the protocol
   does not say what that limit is, so everything here is asked for in
   helpings this size: large enough that a megabyte is a handful of calls,
   small enough that no driver has to find room for it all at once. */
#define BLOCK_RUN 256

int ata_read_many(uint32_t lba, unsigned count, void *buffer) {
    struct efi_block_io *disk = boot->disk;

    if (disk == NULL || count == 0) {
        return -1;
    }
    while (count > 0) {
        unsigned n = count < BLOCK_RUN ? count : BLOCK_RUN;

            if (EFI_ERROR(disk->read_blocks(disk, boot->media_id, lba,
                                        (efi_uintn)n * 512, buffer))) {
            return -1;
        }
        buffer = (char *)buffer + (size_t)n * 512;
        lba += n;
        count -= n;
    }
    return 0;
}

int ata_write(uint32_t lba, const void *buffer) {
    return ata_write_many(lba, 1, buffer);
}

int ata_write_many(uint32_t lba, unsigned count, const void *buffer) {
    struct efi_block_io *disk = boot->disk;

    if (disk == NULL || count == 0) {
        return -1;
    }
    while (count > 0) {
        unsigned n = count < BLOCK_RUN ? count : BLOCK_RUN;

        if (EFI_ERROR(disk->write_blocks(disk, boot->media_id, lba,
                                         (efi_uintn)n * 512, buffer))) {
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

    if (disk != NULL) {
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


/* Seconds past midnight, from the firmware's own reading of the clock. */
unsigned efi_seconds(void) {
    struct efi_time now;

    if (EFI_ERROR(boot->system->runtime->get_time(&now, NULL))) {
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

    if (EFI_ERROR(boot->system->runtime->get_time(&now, NULL)) ||
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
    boot->system->runtime->reset_system(EFI_RESET_SHUTDOWN, EFI_SUCCESS, 0, NULL);
}

void efi_restart(void) {
    boot->system->runtime->reset_system(EFI_RESET_COLD, EFI_SUCCESS, 0, NULL);
}
