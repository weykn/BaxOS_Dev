/* storage/usb: a USB stick or disk - bulk-only mass storage, SCSI commands
 * in a 31-byte wrapper - through the USB host (usb/xhci). Each such device
 * is looked at, and the one holding the filesystem kept.
 *
 * After SeaBIOS's usb-msc.c and blockcmd.c. A command moves at most 64 KiB,
 * through pages borrowed below 4 GiB for the call, where the controller
 * can reach; the wrapper and the status come back in the first of them. */

#include "driver.h"
#include "efi_kernel.h"
#include "mem.h"
#include "module.h"
#include "string.h"
#include "usb.h"

#define ENODEV 19
#define EBUSY  16

#define DMA_PAGES 16                /* 64 KiB a command */

struct cbw {
    uint32_t signature, tag, length;
    uint8_t  flags, lun, cb_length, cb[16];
} __attribute__((packed));

struct csw {
    uint32_t signature, tag, residue;
    uint8_t  status;
} __attribute__((packed));

static const struct usb_device *dev;
static uint32_t tag;

/* One SCSI command, its data to or from the borrowed pages at base. */
static int scsi(uint64_t base, const uint8_t *cb, unsigned cb_length, uint32_t bytes, bool in) {
    struct cbw *w = (struct cbw *)base;
    struct csw *s = (struct csw *)(base + 64);
    void *data = (void *)(base + 4096);

    memset(w, 0, sizeof *w);
    w->signature = 0x43425355;          /* "USBC" */
    w->tag = ++tag;
    w->length = bytes;
    w->flags = in ? 0x80 : 0;
    w->cb_length = (uint8_t)cb_length;
    memcpy(w->cb, cb, cb_length);
    if (usb_host->bulk(dev, dev->out, w, sizeof *w) != sizeof *w) {
        return -1;
    }
    if (bytes > 0) {
        usb_host->bulk(dev, in ? dev->in : dev->out, data, bytes);
    }
    if (usb_host->bulk(dev, dev->in, s, sizeof *s) != sizeof *s ||
        s->signature != 0x53425355 || s->tag != w->tag) {
        return -1;
    }
    return s->status == 0 ? 0 : -1;
}

static int transfer(bool write, uint64_t lba, unsigned count, uint8_t *data) {
    uint64_t base = mem_pages_below(1 + DMA_PAGES, 1ull << 32);
    int err = 0;

    if (base == 0) {
        return -1;
    }
    while (count > 0 && err == 0) {
        unsigned n = count < DMA_PAGES * 8 ? count : DMA_PAGES * 8;
        uint8_t cb[16] = { 0 };
        unsigned length = 10;

        if (lba + n > 0xFFFFFFFFull) {  /* READ(16), WRITE(16) */
            cb[0] = write ? 0x8A : 0x88;
            for (unsigned i = 0; i < 8; i++) {
                cb[2 + i] = (uint8_t)(lba >> (56 - 8 * i));
            }
            cb[12] = (uint8_t)(n >> 24);
            cb[13] = (uint8_t)(n >> 16);
            cb[14] = (uint8_t)(n >> 8);
            cb[15] = (uint8_t)n;
            length = 16;
        } else {                        /* READ(10), WRITE(10) */
            cb[0] = write ? 0x2A : 0x28;
            cb[2] = (uint8_t)(lba >> 24);
            cb[3] = (uint8_t)(lba >> 16);
            cb[4] = (uint8_t)(lba >> 8);
            cb[5] = (uint8_t)lba;
            cb[7] = (uint8_t)(n >> 8);
            cb[8] = (uint8_t)n;
        }
        if (write) {
            memcpy((void *)(base + 4096), data, (size_t)n * 512);
        }
        err = scsi(base, cb, length, n * 512, !write);
        if (err == 0 && !write) {
            memcpy(data, (void *)(base + 4096), (size_t)n * 512);
        }
        data += (size_t)n * 512;
        lba += n;
        count -= n;
    }
    mem_pages_free(base, 1 + DMA_PAGES);
    return err;
}

static int msc_read(uint64_t lba, unsigned count, void *buffer) {
    return transfer(false, lba, count, buffer);
}

static int msc_write(uint64_t lba, unsigned count, const void *buffer) {
    return transfer(true, lba, count, (uint8_t *)buffer);
}

static int msc_flush(void) {
    uint64_t base = mem_pages_below(1, 1ull << 32);
    uint8_t cb[10] = { 0x35 };          /* SYNCHRONIZE CACHE(10) */
    int err;

    if (base == 0) {
        return -1;
    }
    err = scsi(base, cb, sizeof cb, 0, false);
    mem_pages_free(base, 1);
    return err;
}

static uint64_t msc_find(uint32_t offset, uint32_t magic) {
    return gpt_find(msc_read, offset, magic);
}

static const struct disk_driver driver = { msc_read, msc_write, msc_flush, msc_find };

/* Waits for the medium, as a stick just reset wants, and checks its
   sectors are the filesystem's 512 bytes. */
static bool ready(void) {
    uint64_t base = mem_pages_below(2, 1ull << 32);
    bool ok = false;

    if (base == 0) {
        return false;
    }
    for (unsigned tries = 0; tries < 20 && !ok; tries++) {
        uint8_t unit[6] = { 0 }, sense[6] = { 0x03, 0, 0, 0, 18, 0 };

        ok = scsi(base, unit, sizeof unit, 0, false) == 0;
        if (!ok) {
            scsi(base, sense, sizeof sense, 18, true);  /* clears what it said */
            for (uint64_t until = efi_uptime_ms() + 50; efi_uptime_ms() < until;) {
            }
        }
    }
    uint8_t capacity[10] = { 0x25 };    /* READ CAPACITY(10) */

    if (ok && scsi(base, capacity, sizeof capacity, 8, true) == 0) {
        const uint8_t *answer = (const uint8_t *)(base + 4096);

        ok = (answer[4] << 24 | answer[5] << 16 | answer[6] << 8 | answer[7]) == 512;
    } else {
        ok = false;
    }
    mem_pages_free(base, 2);
    return ok;
}

MODULE_EXPORT int module_init(void) {
    const struct usb_device *d;

    if (usb_host == NULL || !usb_host->start()) {
        return -ENODEV;
    }
    for (unsigned i = 0; (d = usb_host->device(i)) != NULL; i++) {
        if (d->class != USB_CLASS_STORAGE || d->in == 0 || d->out == 0) {
            continue;
        }
        dev = d;
        if (ready() && disk_register(&driver)) {
            return 0;
        }
    }
    return -ENODEV;
}

/* Its disk is the firmware's no longer, so while it has the filesystem
   it stays. */
MODULE_EXPORT int module_exit(void) {
    return disk_driver == &driver ? -EBUSY : 0;
}
