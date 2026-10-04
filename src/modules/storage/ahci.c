/* storage/ahci: a SATA disk on an AHCI controller - what every PC's
 * chipset has had in place of IDE since the late 2000s, and what a 2.5"
 * SSD or a hard disk is plugged into. Polled, one command at a time, no
 * interrupts. Every port of every controller with a disk on it is looked
 * at, and the one holding the filesystem kept; a controller without it is
 * handed back to the firmware as it was.
 *
 * After SeaBIOS's ahci.c. A port needs one page - its command list, the
 * FIS the drive answers into and one command table - and each command
 * moves at most 64 KiB, through pages borrowed below 4 GiB for the call,
 * as storage/ide does. */

#include "driver.h"
#include "efi_kernel.h"
#include "mem.h"
#include "module.h"
#include "pci.h"
#include "string.h"

#define ENODEV 19
#define EBUSY  16

/* The controller's registers, then each port's at PORT(n). */
#define CAP   0x00
#define GHC   0x04
#define PI    0x0C
#define CAP2  0x24
#define BOHC  0x28
#define PORT(n) (0x100 + 0x80 * (n))
#define P_CLB  0x00
#define P_FB   0x08
#define P_IS   0x10
#define P_IE   0x14
#define P_CMD  0x18
#define P_TFD  0x20
#define P_SIG  0x24
#define P_SSTS 0x28
#define P_SERR 0x30
#define P_CI   0x38

#define GHC_AE    (1u << 31)
#define CMD_ST    (1u << 0)
#define CMD_SUD   (1u << 1)
#define CMD_FRE   (1u << 4)
#define CMD_FR    (1u << 14)
#define CMD_CR    (1u << 15)
#define IS_TFES   (1u << 30)
#define TFD_ERR   0x01
#define TFD_DRQ   0x08
#define TFD_DF    0x20
#define TFD_BSY   0x80
#define SIG_ATA   0x00000101

#define CMD_READ  0x25              /* READ DMA EXT */
#define CMD_WRITE 0x35              /* WRITE DMA EXT */
#define CMD_FLUSH 0xEA              /* FLUSH CACHE EXT */

#define DMA_PAGES 16                /* 64 KiB a command */
#define TIMEOUT_MS 5000

static volatile uint8_t *hba;
static uint32_t at;                 /* the controller's PCI address */
static unsigned port;
static uint64_t page;               /* the port's command list, FIS, table */

static uint32_t reg(unsigned r) { return *(volatile uint32_t *)(hba + r); }
static void set(unsigned r, uint32_t v) { *(volatile uint32_t *)(hba + r) = v; }
static uint32_t preg(unsigned r) { return reg(PORT(port) + r); }
static void pset(unsigned r, uint32_t v) { set(PORT(port) + r, v); }

/* Waits for the port register r to have the bits in mask equal to want. */
static bool wait(unsigned r, uint32_t mask, uint32_t want) {
    for (uint64_t until = efi_uptime_ms() + TIMEOUT_MS; efi_uptime_ms() < until;) {
        if ((preg(r) & mask) == want) {
            return true;
        }
    }
    return false;
}

/* One command from slot 0: count sectors from lba, to or from data. */
static int command(uint8_t op, uint64_t lba, unsigned count, uint64_t data, bool write) {
    volatile uint32_t *header = (volatile uint32_t *)page;
    volatile uint8_t *table = (volatile uint8_t *)page + 0x500;
    volatile uint32_t *prd = (volatile uint32_t *)(table + 0x80);
    uint64_t t = (uint64_t)table;

    for (unsigned i = 0; i < 0x80; i++) {
        table[i] = 0;
    }
    table[0] = 0x27;                    /* register FIS, host to device */
    table[1] = 0x80;                    /* a command */
    table[2] = op;
    table[4] = (uint8_t)lba;
    table[5] = (uint8_t)(lba >> 8);
    table[6] = (uint8_t)(lba >> 16);
    table[7] = 0x40;                    /* by LBA */
    table[8] = (uint8_t)(lba >> 24);
    table[9] = (uint8_t)(lba >> 32);
    table[10] = (uint8_t)(lba >> 40);
    table[12] = (uint8_t)count;
    table[13] = (uint8_t)(count >> 8);
    prd[0] = (uint32_t)data;
    prd[1] = (uint32_t)(data >> 32);
    prd[2] = 0;
    prd[3] = count * 512 - 1;
    header[0] = 5 | (write ? 1u << 6 : 0) | (data != 0 ? 1u << 16 : 0);
    header[1] = 0;
    header[2] = (uint32_t)t;
    header[3] = (uint32_t)(t >> 32);

    pset(P_IS, preg(P_IS));
    pset(P_CI, 1);
    for (uint64_t until = efi_uptime_ms() + TIMEOUT_MS; efi_uptime_ms() < until;) {
        if (preg(P_IS) & IS_TFES) {
            break;
        }
        if ((preg(P_CI) & 1) == 0) {
            return (preg(P_TFD) & (TFD_ERR | TFD_DF)) ? -1 : 0;
        }
    }
    /* An error stops the port; it is started again for the next one. */
    pset(P_CMD, preg(P_CMD) & ~CMD_ST);
    wait(P_CMD, CMD_CR, 0);
    pset(P_SERR, preg(P_SERR));
    pset(P_IS, preg(P_IS));
    pset(P_CMD, preg(P_CMD) | CMD_ST);
    return -1;
}

static int transfer(bool write, uint64_t lba, unsigned count, uint8_t *data) {
    uint64_t base = mem_pages_below(DMA_PAGES, 1ull << 32);
    int err = 0;

    if (base == 0) {
        return -1;
    }
    while (count > 0 && err == 0) {
        unsigned n = count < DMA_PAGES * 8 ? count : DMA_PAGES * 8;

        if (write) {
            memcpy((void *)base, data, (size_t)n * 512);
        }
        err = command(write ? CMD_WRITE : CMD_READ, lba, n, base, write);
        if (err == 0 && !write) {
            memcpy(data, (void *)base, (size_t)n * 512);
        }
        data += (size_t)n * 512;
        lba += n;
        count -= n;
    }
    mem_pages_free(base, DMA_PAGES);
    return err;
}

static int ahci_read(uint64_t lba, unsigned count, void *buffer) {
    return transfer(false, lba, count, buffer);
}

static int ahci_write(uint64_t lba, unsigned count, const void *buffer) {
    return transfer(true, lba, count, (uint8_t *)buffer);
}

static int ahci_flush(void) {
    return command(CMD_FLUSH, 0, 0, 0, false);
}

static uint64_t ahci_find(uint32_t offset, uint32_t magic) {
    return gpt_find(ahci_read, offset, magic);
}

static const struct disk_driver driver = { ahci_read, ahci_write, ahci_flush, ahci_find };

/* ---- ports ----------------------------------------------------------------- */

static void port_stop(void) {
    pset(P_CMD, preg(P_CMD) & ~CMD_ST);
    wait(P_CMD, CMD_CR, 0);
    pset(P_CMD, preg(P_CMD) & ~CMD_FRE);
    wait(P_CMD, CMD_FR, 0);
}

/* The port's lists in page, receiving on, and running once the drive is
   no longer busy. */
static bool port_start(void) {
    port_stop();
    memset((void *)page, 0, 4096);
    pset(P_CLB, (uint32_t)page);
    pset(P_CLB + 4, (uint32_t)(page >> 32));
    pset(P_FB, (uint32_t)(page + 0x400));
    pset(P_FB + 4, (uint32_t)((page + 0x400) >> 32));
    pset(P_IE, 0);
    pset(P_SERR, preg(P_SERR));
    pset(P_IS, preg(P_IS));
    pset(P_CMD, preg(P_CMD) | CMD_FRE | CMD_SUD);
    if (!wait(P_TFD, TFD_BSY | TFD_DRQ, 0)) {
        return false;
    }
    pset(P_CMD, preg(P_CMD) | CMD_ST);
    return true;
}

/* Firmware that says it may own the controller is asked to let it go. */
static void handoff(void) {
    if ((reg(CAP2) & 1) == 0) {
        return;
    }
    set(BOHC, reg(BOHC) | 2);
    for (uint64_t until = efi_uptime_ms() + 2000; efi_uptime_ms() < until;) {
        if ((reg(BOHC) & 1) == 0) {
            break;
        }
    }
}

static bool try_controller(void) {
    uint32_t ports;

    handoff();
    set(GHC, reg(GHC) | GHC_AE);
    ports = reg(PI);
    for (port = 0; port < 32; port++) {
        if (!(ports & (1u << port)) || (preg(P_SSTS) & 0xF) != 3 || preg(P_SIG) != SIG_ATA) {
            continue;                   /* nothing, or no disk: a CD, a multiplier */
        }
        if (port_start() && disk_register(&driver)) {
            return true;
        }
        port_stop();
    }
    return false;
}

MODULE_EXPORT int module_init(void) {
    if ((page = mem_pages_below(1, 1ull << 32)) == 0) {
        return -ENODEV;
    }
    for (at = pci_next_class(0x01, 0x06, 0); at != 0; at = pci_next_class(0x01, 0x06, at)) {
        uint64_t bar;

        if ((pci_read(at, 0x08) >> 8 & 0xFF) != 0x01 || (bar = pci_memory(at, 5)) == 0) {
            continue;                   /* not AHCI's interface, or no registers */
        }
        hba = (volatile uint8_t *)bar;
        efi_pci(at, true);
        if (try_controller()) {
            return 0;
        }
        efi_pci(at, false);
    }
    mem_pages_free(page, 1);
    return -ENODEV;
}

/* Its disk is the firmware's no longer, so while it has the filesystem
   it stays. */
MODULE_EXPORT int module_exit(void) {
    if (disk_driver == &driver) {
        return -EBUSY;
    }
    return 0;
}
