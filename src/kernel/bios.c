#include "bios.h"

#include <stddef.h>

#include "debug.h"
#include "driver.h"
#include "efi_kernel.h"
#include "fs.h"
#include "io.h"
#include "mem.h"
#include "string.h"
#include "vm.h"

/* What the BIOS loader left: where to call to reach real mode, the
   registers it takes and gives back, and 64 KiB below 1 MiB for INT 13h to
   read into and write from - the BIOS can reach nothing higher. */
static struct boot_info *boot;
static struct bios_regs *regs;
static uint8_t          *buffer;
static uint32_t          fs_base;   /* the filesystem's first sector */
static bool              gone;

/* The disk address packet INT 13h's extended calls take, in low memory
   too: just past the registers. */
struct dap {
    uint8_t  size, zero;
    uint16_t count, offset, segment;
    uint64_t lba;
} __attribute__((packed));

#define CARRY      0x0001
#define ZERO       0x0040
#define RUN        64               /* sectors a call: 32 KiB, any BIOS takes it */

/* One interrupt in real mode, with regs as they stand. The low memory a
   program linked to a fixed address may have mapped over is put back for
   the call, since the BIOS lives there. */
static void call(uint8_t vector) {
    regs->vector = vector;
    vm_firmware_view(true);
    ((void (*)(void))boot->bios_call)();
    vm_firmware_view(false);
}

/* count sectors from the start of the disk, at most RUN, through the
   buffer. */
static int disk(bool write, uint64_t lba, unsigned count) {
    struct dap *dap = (struct dap *)(boot->bios_regs + 64);

    *dap = (struct dap){ sizeof *dap, 0, (uint16_t)count, 0,
                         (uint16_t)(boot->bios_buffer >> 4), lba };
    memset(regs, 0, sizeof *regs);
    regs->eax = write ? 0x4300 : 0x4200;
    regs->edx = boot->bios_drive;
    regs->esi = (uint32_t)((uint64_t)dap & 0xF);
    regs->ds = (uint16_t)((uint64_t)dap >> 4);
    call(0x13);
    return (regs->flags & CARRY) ? -1 : 0;
}

static int transfer(bool write, uint32_t lba, unsigned count, uint8_t *data) {
    if (gone || fs_base == 0) {
        return -1;
    }
    while (count > 0) {
        unsigned n = count < RUN ? count : RUN;

        if (write) {
            memcpy(buffer, data, (size_t)n * 512);
        }
        if (disk(write, fs_base + (uint64_t)lba, n) < 0) {
            return -1;
        }
        if (!write) {
            memcpy(data, buffer, (size_t)n * 512);
        }
        data += (size_t)n * 512;
        lba += n;
        count -= n;
    }
    return 0;
}

int bios_read(uint32_t lba, unsigned count, void *out) {
    return transfer(false, lba, count, out);
}

int bios_write(uint32_t lba, unsigned count, const void *data) {
    return transfer(true, lba, count, (uint8_t *)data);
}

/* Whole-disk sectors, for finding the filesystem's partition. */
static int whole_disk(uint64_t lba, unsigned count, void *out) {
    if (count > RUN || disk(false, lba, count) < 0) {
        return -1;
    }
    memcpy(out, buffer, (size_t)count * 512);
    return 0;
}

/* In syscall_entry.asm: the RFLAGS a program starts with. */
extern uint64_t user_flags;

void bios_init(struct boot_info *info) {
    boot = info;
    regs = (struct bios_regs *)info->bios_regs;
    buffer = (uint8_t *)info->bios_buffer;

    /* Nothing in the IDT answers the BIOS's interrupts, so nothing may
       take one outside a call into it: programs run without them. */
    user_flags = 0x002;
    mem_take_bios((const struct e820 *)info->e820, info->e820_count, info);
    mem_pages_free(0x1000, 6);      /* the loader's tables */
    uint64_t base = gpt_find(whole_disk, FS_LBA, FS_MAGIC);

    fs_base = base <= 0xFFFFFFFF ? (uint32_t)base : 0;
    dbg("bios: drive %x, filesystem at %u\n", (uint64_t)info->bios_drive, fs_base);
}

void bios_leave(void) {
    gone = true;
    mem_pages_free(boot->bios_buffer, 16);
}

/* ---- the keyboard -------------------------------------------------------- */

static const char *pending;
static uint64_t    next_poll;

/* What a key with no character sends, by its scan code - the sequences
   efi.c and input/ps2 send too. */
static const char *grey(uint8_t scan) {
    static const char *const function[] = {
        "\033[[A", "\033[[B", "\033[[C", "\033[[D", "\033[[E",
        "\033[17~", "\033[18~", "\033[19~", "\033[20~", "\033[21~",
    };

    switch (scan) {
    case 0x48: return "\033[A";
    case 0x50: return "\033[B";
    case 0x4D: return "\033[C";
    case 0x4B: return "\033[D";
    case 0x47: return "\033[H";
    case 0x4F: return "\033[F";
    case 0x52: return "\033[2~";
    case 0x53: return "\033[3~";
    case 0x49: return "\033[5~";
    case 0x51: return "\033[6~";
    case 0x85: return "\033[23~";
    case 0x86: return "\033[24~";
    default:
        return scan >= 0x3B && scan <= 0x44 ? function[scan - 0x3B] : NULL;
    }
}

char bios_key(void) {
    if (pending != NULL) {
        char c = *pending++;

        if (*pending == '\0') {
            pending = NULL;
        }
        return c;
    }
    if (gone) {
        return 0;
    }
    /* No more often than the BIOS's own clock ticks: its keyboard is fed
       from its timer interrupt, which only arrives inside a call, and a
       BIOS asked faster than that sees its time run fast. */
    uint64_t now = efi_uptime_ms();

    if (now < next_poll) {
        return 0;
    }
    memset(regs, 0, sizeof *regs);
    regs->eax = 0x1100;                 /* a key waiting? */
    call(0x16);
    if (regs->flags & ZERO) {
        next_poll = now + 55;
        return 0;
    }
    memset(regs, 0, sizeof *regs);
    regs->eax = 0x1000;                 /* take it */
    call(0x16);

    uint8_t ch = (uint8_t)regs->eax, scan = (uint8_t)(regs->eax >> 8);

    if (ch == 0 || ch == 0xE0) {
        const char *text = grey(scan);

        if (text == NULL) {
            return 0;
        }
        pending = text[1] != '\0' ? text + 1 : NULL;
        return text[0];
    }
    if (ch == '\r') {
        return '\n';
    }
    return ch < 0x80 ? (char)ch : 0;
}

/* ---- the clock ------------------------------------------------------------ */

static unsigned bcd(uint8_t value, bool binary) {
    return binary ? value : (value >> 4) * 10 + (value & 0xF);
}

struct rtc {
    unsigned second, minute, hour, day, month, year;
};

/* Read twice, until two readings agree: the clock may tick in between. */
static void rtc(struct rtc *now) {
    uint8_t raw[7], again[7];
    static const uint8_t at[7] = { 0x00, 0x02, 0x04, 0x07, 0x08, 0x09, 0x32 };

    for (unsigned tries = 0; tries < 4; tries++) {
        for (unsigned i = 0; i < 1000000 && (cmos_read(0x0A) & 0x80); i++) {
        }
        for (unsigned i = 0; i < 7; i++) {
            raw[i] = cmos_read(at[i]);
        }
        for (unsigned i = 0; i < 7; i++) {
            again[i] = cmos_read(at[i]);
        }
        if (memcmp(raw, again, 7) == 0) {
            break;
        }
    }
    uint8_t b = cmos_read(0x0B);
    bool binary = b & 0x04, pm = !(b & 0x02) && (raw[2] & 0x80);

    now->second = bcd(raw[0], binary);
    now->minute = bcd(raw[1], binary);
    now->hour = bcd(raw[2] & 0x7F, binary) % 12 + (pm ? 12 : 0);
    if (b & 0x02) {
        now->hour = bcd(raw[2], binary);
    }
    now->day = bcd(raw[3], binary);
    now->month = bcd(raw[4], binary);
    unsigned century = bcd(raw[6], binary);

    now->year = (century >= 19 && century <= 30 ? century : 20) * 100 + bcd(raw[5], binary);
}

unsigned bios_seconds(void) {
    struct rtc now;

    rtc(&now);
    return (now.hour * 60 + now.minute) * 60 + now.second;
}

uint64_t bios_epoch(void) {
    static const unsigned before[] = { 0, 31, 59, 90, 120, 151,
                                       181, 212, 243, 273, 304, 334 };
    struct rtc now;
    uint64_t days;

    rtc(&now);
    if (now.year < 1970 || now.month < 1 || now.month > 12) {
        return 0;
    }
    days = (uint64_t)(now.year - 1970) * 365 + before[now.month - 1] + now.day - 1;
    for (unsigned y = 1970; y < now.year; y++) {
        days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 1 : 0;
    }
    if (now.month > 2 && now.year % 4 == 0 && (now.year % 100 != 0 || now.year % 400 == 0)) {
        days++;
    }
    return ((days * 24 + now.hour) * 60 + now.minute) * 60 + now.second;
}

/* The PIT's channel 2, gated through port 0x61 and counting down 11932
   ticks of its 1.193182 MHz: ten milliseconds, read off the processor's
   counter. No interrupt, no speaker. */
uint64_t bios_ticks_per_second(void) {
    uint32_t low, high;
    uint64_t from, to;

    outb(0x61, (inb(0x61) & ~0x02) | 0x01);
    outb(0x43, 0xB0);                   /* channel 2, both bytes, mode 0 */
    outb(0x42, 11932 & 0xFF);
    outb(0x42, 11932 >> 8);
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    from = (uint64_t)high << 32 | low;
    for (unsigned i = 0; i < 100000000 && !(inb(0x61) & 0x20); i++) {
    }
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    to = (uint64_t)high << 32 | low;
    return (to - from) * 100;
}

/* ---- power ------------------------------------------------------------------
 *
 * Switching off is ACPI's: sleep state 5, whose type the DSDT's \_S5 object
 * gives, written to the PM1 control registers the FADT names. Found the
 * way every small kernel finds it - the RSDP by its signature in the BIOS
 * area, the tables by theirs, and \_S5 by a byte search of the DSDT. */

static const uint8_t *rsdp_in(uint64_t from, uint64_t to) {
    for (uint64_t at = from; at < to; at += 16) {
        if (memcmp((const void *)at, "RSD PTR ", 8) == 0) {
            return (const uint8_t *)at;
        }
    }
    return NULL;
}

static const uint8_t *table(const char *name) {
    const uint8_t *rsdp = rsdp_in((uint64_t)*(uint16_t *)0x40E << 4,
                                  ((uint64_t)*(uint16_t *)0x40E << 4) + 1024);
    uint32_t rsdt;

    if (rsdp == NULL && (rsdp = rsdp_in(0xE0000, 0x100000)) == NULL) {
        return NULL;
    }
    memcpy(&rsdt, rsdp + 16, 4);
    uint32_t length = *(const uint32_t *)((uint64_t)rsdt + 4);

    for (uint32_t at = 36; at + 4 <= length; at += 4) {
        const uint8_t *t = (const uint8_t *)(uint64_t)*(const uint32_t *)((uint64_t)rsdt + at);

        if (memcmp(t, name, 4) == 0) {
            return t;
        }
    }
    return NULL;
}

void bios_power_off(void) {
    const uint8_t *fadt = table("FACP");

    if (fadt == NULL) {
        return;
    }
    uint32_t smi, pm1a, pm1b, dsdt_at;
    uint8_t enable = fadt[52];

    memcpy(&dsdt_at, fadt + 40, 4);
    memcpy(&smi, fadt + 48, 4);
    memcpy(&pm1a, fadt + 64, 4);
    memcpy(&pm1b, fadt + 68, 4);

    const uint8_t *dsdt = (const uint8_t *)(uint64_t)dsdt_at;
    uint32_t length = *(const uint32_t *)(dsdt + 4);
    unsigned a = 0, b = 0;
    bool found = false;

    /* Name(_S5_, Package(){a, b, ...}): 08 '_S5_' 12 len count, then each
       value a byte, or BytePrefix (0A) and a byte. */
    for (uint32_t i = 36; i + 12 < length && !found; i++) {
        const uint8_t *p = dsdt + i;

        if (memcmp(p, "_S5_", 4) != 0 || p[4] != 0x12 ||
            (p[-1] != 0x08 && !(p[-2] == 0x08 && p[-1] == '\\'))) {
            continue;
        }
        p += 5;
        p += ((*p & 0xC0) >> 6) + 2;        /* the package length, and the count */
        if (*p == 0x0A) {
            p++;
        }
        a = *p++;
        if (*p == 0x0A) {
            p++;
        }
        b = *p;
        found = true;
    }
    if (!found || pm1a == 0) {
        return;
    }
    if (smi != 0 && enable != 0 && !(inw((uint16_t)pm1a) & 1)) {
        outb((uint16_t)smi, enable);        /* ACPI on: SCI_EN */
        for (unsigned i = 0; i < 10000000 && !(inw((uint16_t)pm1a) & 1); i++) {
        }
    }
    outw((uint16_t)pm1a, (uint16_t)(a << 10 | 1 << 13));
    if (pm1b != 0) {
        outw((uint16_t)pm1b, (uint16_t)(b << 10 | 1 << 13));
    }
    for (unsigned i = 0; i < 100000000; i++) {
        __asm__ volatile("pause");
    }
}

void bios_restart(void) {
    outb(0xCF9, 0x02);                  /* the chipset's reset register */
    outb(0xCF9, 0x06);
    for (unsigned i = 0; i < 100000 && (inb(0x64) & 0x02); i++) {
    }
    outb(0x64, 0xFE);                   /* the keyboard controller's */
    for (unsigned i = 0; i < 10000000; i++) {
        __asm__ volatile("pause");
    }
    /* Neither: a fault with no table to handle it, which resets. */
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) none = { 0, 0 };

    __asm__ volatile("lidt %0; int3" : : "m"(none));
}
