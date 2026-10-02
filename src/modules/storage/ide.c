/* storage/ide: the first IDE disk, driven by the kernel itself once the
 * firmware is gone, through the legacy ports, polled, no interrupts: by
 * bus-master DMA where the controller has it, and programmed I/O where it
 * does not. It is what QEMU gives a machine by default, and what the
 * firmware's disk driver is replaced with - only if this finds the
 * filesystem on it, which is what decides whether the firmware can be let
 * go at all.
 *
 * Sectors are counted from the start of the disk. Each call returns 0, or -1
 * if the drive reports an error or does not answer. */

#include "driver.h"
#include "io.h"
#include "mem.h"
#include "module.h"
#include "pci.h"
#include "string.h"

#define ENODEV 19
#define EBUSY  16

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
#define CMD_READ_DMA  0x25          /* READ DMA EXT */
#define CMD_WRITE_DMA 0x35          /* WRITE DMA EXT */
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

/* ---- DMA ------------------------------------------------------------------
 *
 * Programmed I/O costs a status wait and 256 port reads a sector, and in a
 * virtual machine each sector is a trip of its own out to the emulator. The
 * controller's bus master moves a whole command's worth into memory with the
 * processor out of it: a table of the pages to fill, one command, and a
 * status bit to watch. The pages are borrowed for the call - below 4 GiB,
 * where the controller can reach - so nothing is held while the disk is
 * idle. */

#define BM_COMMAND   0              /* the primary channel's, from BAR4 */
#define BM_STATUS    2
#define BM_TABLE     4
#define BM_START     0x01
#define BM_TO_MEMORY 0x08
#define BM_ACTIVE    0x01
#define BM_ERROR     0x02
#define BM_IRQ       0x04
#define DMA_PAGES    16             /* 64 KiB a command */

struct prd {
    uint32_t addr;
    uint16_t bytes, flags;          /* 0x8000 on the last */
};

static uint16_t bm;                 /* the bus master's ports; 0, none */

/* count sectors, at most DMA_PAGES * 8. 0, -1 for an error, or 1 if there
   was no memory to do it with. */
static int dma(bool write, uint64_t lba, unsigned count, void *buffer) {
    uint64_t base = mem_pages_below(1 + DMA_PAGES, 1ull << 32);
    size_t bytes = (size_t)count * 512;
    uint8_t to = write ? 0 : BM_TO_MEMORY;
    int err = -1;

    if (base == 0) {
        return 1;
    }
    struct prd *table = (struct prd *)base;
    char *data = (char *)base + 4096;

    for (unsigned i = 0; i * 4096 < bytes; i++) {
        size_t left = bytes - (size_t)i * 4096;

        table[i] = (struct prd){ (uint32_t)(uintptr_t)(data + i * 4096),
                                 (uint16_t)(left < 4096 ? left : 4096),
                                 left <= 4096 ? 0x8000 : 0 };
    }
    if (write) {
        memcpy(data, buffer, bytes);
    }
    outb(bm + BM_COMMAND, to);
    outb(bm + BM_STATUS, inb(bm + BM_STATUS) | BM_ERROR | BM_IRQ);
    outl(bm + BM_TABLE, (uint32_t)base);
    if (start(write ? CMD_WRITE_DMA : CMD_READ_DMA, lba, count) == 0) {
        outb(bm + BM_COMMAND, to | BM_START);
        for (unsigned i = 0; i < SPINS; i++) {
            uint8_t st = inb(bm + BM_STATUS);

            if (st & BM_ERROR) {
                break;
            }
            if ((st & BM_ACTIVE) == 0) {
                err = wait(false);
                break;
            }
        }
        outb(bm + BM_COMMAND, to);
    }
    if (err == 0 && !write) {
        memcpy(buffer, data, bytes);
    }
    mem_pages_free(base, 1 + DMA_PAGES);
    return err;
}

/* By DMA while it works: a drive or controller that fails it once is left
   to programmed I/O from then on. Returns true if it did the whole chunk. */
static bool by_dma(bool write, uint64_t lba, unsigned n, void *buffer) {
    int err;

    if (bm == 0) {
        return false;
    }
    if ((err = dma(write, lba, n, buffer)) < 0) {
        bm = 0;
    }
    return err == 0;
}

/* The controller's bus master, if it has one and it can be turned on. */
static void find_bus_master(void) {
    uint32_t at = pci_find_class(0x01, 0x01);
    uint32_t bar;

    if (at == 0 || ((bar = pci_read(at, 0x20)) & 1) == 0 || (bar & 0xFFFC) == 0) {
        return;
    }
    pci_write(at, 0x04, (pci_read(at, 0x04) & 0xFFFF) | 0x5);  /* ports, bus master */
    bm = (uint16_t)(bar & 0xFFFC);
}

/* ---- the calls --------------------------------------------------------------- */

#define RUN 256                     /* sectors in one programmed command */

static int ide_read(uint64_t lba, unsigned count, void *buffer) {
    while (count > 0) {
        unsigned n = count < RUN ? count : RUN;

        if (by_dma(false, lba, n < DMA_PAGES * 8 ? n : DMA_PAGES * 8, buffer)) {
            n = n < DMA_PAGES * 8 ? n : DMA_PAGES * 8;
            buffer = (char *)buffer + (size_t)n * 512;
            lba += n;
            count -= n;
            continue;
        }
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

static int ide_write(uint64_t lba, unsigned count, const void *buffer) {
    while (count > 0) {
        unsigned n = count < RUN ? count : RUN;

        if (by_dma(true, lba, n < DMA_PAGES * 8 ? n : DMA_PAGES * 8, (void *)buffer)) {
            n = n < DMA_PAGES * 8 ? n : DMA_PAGES * 8;
            buffer = (const char *)buffer + (size_t)n * 512;
            lba += n;
            count -= n;
            continue;
        }
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

static int ide_flush(void) {
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

static uint64_t ide_find(uint32_t offset, uint32_t magic) {
    uint8_t *sector;
    uint64_t first;

    if (inb(IDE_COMMAND) == 0xFF || (sector = mem_alloc(512)) == NULL) {
        return 0;                   /* nothing on the channel */
    }
    first = find_in(sector, offset, magic);
    mem_free(sector);
    return first;
}

/* ---- the module ------------------------------------------------------- */

static const struct disk_driver driver = { ide_read, ide_write, ide_flush, ide_find };

MODULE_EXPORT int module_init(void) {
    if (inb(IDE_COMMAND) == 0xFF) {
        return -ENODEV;             /* nothing on the channel */
    }
    find_bus_master();
    disk_register(&driver);
    return 0;
}

/* Once the firmware is gone this is the only way to the disk. */
MODULE_EXPORT int module_exit(void) {
    if (disk_driver == &driver && mem_ours()) {
        return -EBUSY;
    }
    disk_register(NULL);
    return 0;
}
