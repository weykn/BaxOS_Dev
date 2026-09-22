#include "ata.h"

#include "io.h"

/* Primary bus registers. */
#define ATA_DATA       0x1F0
#define ATA_COUNT      0x1F2
#define ATA_LBA_LOW    0x1F3
#define ATA_LBA_MID    0x1F4
#define ATA_LBA_HIGH   0x1F5
#define ATA_DRIVE      0x1F6
#define ATA_STATUS     0x1F7    /* on read */
#define ATA_COMMAND    0x1F7    /* on write */
#define ATA_ALT_STATUS 0x3F6

#define STATUS_ERR 0x01
#define STATUS_DF  0x20
#define STATUS_BSY 0x80

#define CMD_READ  0x20
#define CMD_WRITE 0x30
#define CMD_FLUSH 0xE7

/* Waits out BSY, then fails if the drive flagged an error. */
static int wait(void) {
    uint8_t status;

    /* The status register takes ~400ns to reflect a new command; each read of
       the alternate status port costs about 100ns. */
    for (int i = 0; i < 4; i++) {
        (void)inb(ATA_ALT_STATUS);
    }
    while ((status = inb(ATA_STATUS)) & STATUS_BSY) {
        __asm__ volatile("pause");
    }
    return status & (STATUS_ERR | STATUS_DF) ? -1 : 0;
}

static int issue(uint32_t lba, uint8_t command) {
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));   /* master, LBA mode */
    outb(ATA_COUNT, 1);
    outb(ATA_LBA_LOW, (uint8_t)lba);
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HIGH, (uint8_t)(lba >> 16));
    outb(ATA_COMMAND, command);
    return wait();
}

int ata_read(uint32_t lba, void *buffer) {
    if (issue(lba, CMD_READ) < 0) {
        return -1;
    }
    insw(ATA_DATA, buffer, 256);
    return 0;
}

int ata_write(uint32_t lba, const void *buffer) {
    if (issue(lba, CMD_WRITE) < 0) {
        return -1;
    }
    outsw(ATA_DATA, buffer, 256);
    if (wait() < 0) {
        return -1;
    }

    /* Push the drive's write cache out so the data is really on disk. */
    outb(ATA_COMMAND, CMD_FLUSH);
    return wait();
}
