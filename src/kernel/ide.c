#include "ide.h"

#include "io.h"
#include "mem.h"
#include "string.h"

/* The primary channel's ports, as every PC has had them since the AT. */
#define IDE_DATA      0x1F0
#define IDE_COUNT     0x1F2
#define IDE_LBA0      0x1F3
#define IDE_LBA1      0x1F4
#define IDE_LBA2      0x1F5
#define IDE_DRIVE     0x1F6
#define IDE_COMMAND   0x1F7         /* the status, when read */
#define IDE_CONTROL   0x3F6

#define ST_ERR  0x01
#define ST_DRQ  0x08
#define ST_DF   0x20
#define ST_BSY  0x80

#define CMD_READ   0x24             /* READ SECTORS EXT */
#define CMD_WRITE  0x34             /* WRITE SECTORS EXT */
#define CMD_FLUSH  0xEA             /* FLUSH CACHE EXT */

#define SPINS 10000000              /* polls before a drive counts as gone */

/* Waits for the drive to stop being busy, and for it to want data if want
   says so. Returns -1 on an error, a fault, or no answer at all - a channel
   with nothing on it reads back as all ones. */
static int wait(bool want) {
    for (unsigned i = 0; i < SPINS; i++) {
        uint8_t st = inb(IDE_COMMAND);

        if (st == 0xFF) {
            return -1;
        }
        if (st & ST_BSY) {
            continue;
        }
        if (st & (ST_ERR | ST_DF)) {
            return -1;
        }
        if (!want || (st & ST_DRQ)) {
            return 0;
        }
    }
    return -1;
}

/* Sets up count sectors (1 to 65536, 0 meaning the most) from lba and
   issues command. 48-bit addressing: the high halves go in first. */
static int start(uint8_t command, uint64_t lba, unsigned count) {
    if (wait(false) < 0) {
        return -1;
    }
    outb(IDE_CONTROL, 0x02);        /* no interrupts: it is polled */
    outb(IDE_DRIVE, 0x40);          /* the master, by LBA */
    outb(IDE_COUNT, (uint8_t)(count >> 8));
    outb(IDE_LBA0, (uint8_t)(lba >> 24));
    outb(IDE_LBA1, (uint8_t)(lba >> 32));
    outb(IDE_LBA2, (uint8_t)(lba >> 40));
    outb(IDE_COUNT, (uint8_t)count);
    outb(IDE_LBA0, (uint8_t)lba);
    outb(IDE_LBA1, (uint8_t)(lba >> 8));
    outb(IDE_LBA2, (uint8_t)(lba >> 16));
    outb(IDE_COMMAND, command);
    return 0;
}

#define RUN 256                     /* sectors in one command */

int ide_read(uint64_t lba, unsigned count, void *buffer) {
    while (count > 0) {
        unsigned n = count < RUN ? count : RUN;

        if (start(CMD_READ, lba, n) < 0) {
            return -1;
        }
        for (unsigned i = 0; i < n; i++) {
            if (wait(true) < 0) {
                return -1;
            }
            insw(IDE_DATA, buffer, 256);
            buffer = (char *)buffer + 512;
        }
        lba += n;
        count -= n;
    }
    return 0;
}

int ide_write(uint64_t lba, unsigned count, const void *buffer) {
    while (count > 0) {
        unsigned n = count < RUN ? count : RUN;

        if (start(CMD_WRITE, lba, n) < 0) {
            return -1;
        }
        for (unsigned i = 0; i < n; i++) {
            if (wait(true) < 0) {
                return -1;
            }
            outsw(IDE_DATA, buffer, 256);
            buffer = (const char *)buffer + 512;
        }
        if (wait(false) < 0) {
            return -1;
        }
        lba += n;
        count -= n;
    }
    return 0;
}

int ide_flush(void) {
    if (start(CMD_FLUSH, 0, 0) < 0) {
        return -1;
    }
    return wait(false);
}

/* ---- finding the partition -------------------------------------------- */

struct gpt_header {
    char     signature[8];          /* "EFI PART" */
    uint8_t  pad[64];
    uint64_t entries_lba;
    uint32_t entry_count, entry_size;
};

/* Walks the partition table in sector, a page borrowed for the purpose. */
static uint64_t find_in(uint8_t *sector, uint32_t offset, uint32_t magic) {
    struct gpt_header header;
    uint32_t per_sector;

    if (ide_read(1, 1, sector) < 0) {
        return 0;
    }
    memcpy(&header, sector, sizeof header);
    for (unsigned i = 0; i < 8; i++) {
        if (header.signature[i] != "EFI PART"[i]) {
            return 0;
        }
    }
    if (header.entry_size < 40 || header.entry_size > 512) {
        return 0;
    }
    per_sector = 512 / header.entry_size;
    for (uint32_t i = 0; i < header.entry_count && i < 128; i++) {
        uint64_t first;
        uint32_t found;

        if (ide_read(header.entries_lba + i / per_sector, 1, sector) < 0) {
            return 0;
        }
        memcpy(&first, sector + (i % per_sector) * header.entry_size + 32, sizeof first);
        if (first == 0 || ide_read(first + offset, 1, sector) < 0) {
            continue;
        }
        memcpy(&found, sector, sizeof found);
        if (found == magic) {
            return first;
        }
    }
    return 0;
}

uint64_t ide_find(uint32_t offset, uint32_t magic) {
    uint8_t *sector;
    uint64_t first;

    if (inb(IDE_COMMAND) == 0xFF || (sector = mem_alloc(512)) == NULL) {
        return 0;                   /* nothing on the channel */
    }
    first = find_in(sector, offset, magic);
    mem_free(sector);
    return first;
}
