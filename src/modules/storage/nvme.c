/* storage/nvme: an NVMe SSD - what the disk of almost every PC made in the
 * last few years is - driven by the kernel itself: one queue pair for
 * setting it up and one for reading and writing, polled, no interrupts.
 * Every NVMe controller there is gets looked at in turn, and the one whose
 * first namespace holds the filesystem is kept; the rest are handed back
 * to the firmware as they were.
 *
 * After SeaBIOS's nvme.c, cut down to what this needs. Each command moves
 * at most 64 KiB, through pages borrowed below 4 GiB for the call - as
 * storage/ide does - so nothing but the four queues is held while the disk
 * is idle. Only 512-byte sectors: the filesystem counts in them. */

#include "debug.h"
#include "driver.h"
#include "efi_kernel.h"
#include "mem.h"
#include "module.h"
#include "pci.h"
#include "string.h"

#define ENODEV 19
#define EBUSY  16

/* Controller registers. */
#define CAP   0x00
#define CC    0x14
#define CSTS  0x1C
#define AQA   0x24
#define ASQ   0x28
#define ACQ   0x30

#define CC_EN     1u
#define CC_QUEUES (6u << 16 | 4u << 20)    /* 64-byte commands, 16-byte answers */
#define CSTS_RDY  1u
#define CSTS_CFS  2u

#define DEPTH 16                    /* entries a queue: one page holds it */
#define PAGES 4                     /* admin SQ and CQ, I/O SQ and CQ */
#define DMA_PAGES 16                /* 64 KiB a command */

enum {
    ADMIN_CREATE_SQ = 0x01, ADMIN_CREATE_CQ = 0x05, ADMIN_IDENTIFY = 0x06,
    IO_FLUSH = 0x00, IO_WRITE = 0x01, IO_READ = 0x02,
};

struct command {
    uint32_t cdw0, nsid;
    uint64_t reserved, metadata, prp1, prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};

struct answer {
    uint32_t result, reserved;
    uint16_t sq_head, sq_id, id, status;
};

struct queue {
    volatile struct command *sq;
    volatile struct answer  *cq;
    volatile uint32_t       *sq_bell, *cq_bell;
    unsigned tail, head;
    uint16_t phase;
};

static volatile uint8_t *regs;
static uint32_t          at;        /* the controller's PCI address */
static uint64_t          pages;     /* the queues */
static struct queue      admin, io;
static uint32_t          max_sectors = DMA_PAGES * 8;
static uint64_t          timeout_ms;

static uint32_t reg32(unsigned r) { return *(volatile uint32_t *)(regs + r); }
static uint64_t reg64(unsigned r) { return *(volatile uint64_t *)(regs + r); }
static void set32(unsigned r, uint32_t v) { *(volatile uint32_t *)(regs + r) = v; }
static void set64(unsigned r, uint64_t v) {
    set32(r, (uint32_t)v);
    set32(r + 4, (uint32_t)(v >> 32));
}

static bool wait_ready(bool ready) {
    for (uint64_t until = efi_uptime_ms() + timeout_ms; efi_uptime_ms() < until;) {
        uint32_t st = reg32(CSTS);

        if (st & CSTS_CFS) {
            return false;
        }
        if (((st & CSTS_RDY) != 0) == ready) {
            return true;
        }
    }
    return false;
}

static void queue_init(struct queue *q, uint64_t sq, uint64_t cq, unsigned id, unsigned stride) {
    q->sq = (volatile struct command *)sq;
    q->cq = (volatile struct answer *)cq;
    q->sq_bell = (volatile uint32_t *)(regs + 0x1000 + (2 * id) * stride);
    q->cq_bell = (volatile uint32_t *)(regs + 0x1000 + (2 * id + 1) * stride);
    q->tail = q->head = 0;
    q->phase = 1;
}

/* Puts c on q and waits for its answer: 0, or -1 for an error or none. */
static int submit(struct queue *q, struct command *c) {
    c->cdw0 |= q->tail << 16;           /* the slot is the command's id */
    q->sq[q->tail] = *c;
    q->tail = (q->tail + 1) % DEPTH;
    *q->sq_bell = q->tail;

    for (uint64_t until = efi_uptime_ms() + timeout_ms; efi_uptime_ms() < until;) {
        volatile struct answer *a = &q->cq[q->head];
        uint16_t status = a->status;

        if ((status & 1) != q->phase) {
            continue;
        }
        q->head = (q->head + 1) % DEPTH;
        if (q->head == 0) {
            q->phase ^= 1;
        }
        *q->cq_bell = q->head;
        return (status >> 1) == 0 ? 0 : -1;
    }
    return -1;
}

/* ---- reading and writing ----------------------------------------------- */

/* count sectors, at most max_sectors, through borrowed pages: the first
   holds the list of the rest, which NVMe wants for more than two. */
static int command(uint8_t op, uint64_t lba, unsigned count, void *data) {
    uint64_t base = mem_pages_below(1 + DMA_PAGES, 1ull << 32);
    size_t bytes = (size_t)count * 512;
    unsigned used = (unsigned)((bytes + 4095) / 4096);
    int err;

    if (base == 0) {
        return -1;
    }
    uint64_t *list = (uint64_t *)base, first = base + 4096;
    struct command c = { .cdw0 = op, .nsid = 1, .prp1 = first,
                         .cdw10 = (uint32_t)lba, .cdw11 = (uint32_t)(lba >> 32),
                         .cdw12 = count - 1 };

    for (unsigned i = 1; i < used; i++) {
        list[i - 1] = first + (uint64_t)i * 4096;
    }
    c.prp2 = used == 2 ? first + 4096 : used > 2 ? base : 0;
    if (op == IO_WRITE) {
        memcpy((void *)first, data, bytes);
    }
    err = submit(&io, &c);
    if (err == 0 && op == IO_READ) {
        memcpy(data, (void *)first, bytes);
    }
    mem_pages_free(base, 1 + DMA_PAGES);
    return err;
}

static int transfer(uint8_t op, uint64_t lba, unsigned count, uint8_t *data) {
    while (count > 0) {
        unsigned n = count < max_sectors ? count : max_sectors;

        if (command(op, lba, n, data) < 0) {
            return -1;
        }
        data += (size_t)n * 512;
        lba += n;
        count -= n;
    }
    return 0;
}

static int nvme_read(uint64_t lba, unsigned count, void *buffer) {
    return transfer(IO_READ, lba, count, buffer);
}

static int nvme_write(uint64_t lba, unsigned count, const void *buffer) {
    return transfer(IO_WRITE, lba, count, (uint8_t *)buffer);
}

static int nvme_flush(void) {
    struct command c = { .cdw0 = IO_FLUSH, .nsid = 1 };

    return submit(&io, &c);
}

static uint64_t nvme_find(uint32_t offset, uint32_t magic) {
    return gpt_find(nvme_read, offset, magic);
}

static const struct disk_driver driver = { nvme_read, nvme_write, nvme_flush, nvme_find };

/* ---- setting a controller up ------------------------------------------- */

/* The first namespace's sector size and the most a command may move,
   through a borrowed page. */
static bool identify(void) {
    uint64_t page = mem_pages_below(1, 1ull << 32);
    bool ok = false;

    if (page == 0) {
        return false;
    }
    struct command c = { .cdw0 = ADMIN_IDENTIFY, .prp1 = page, .cdw10 = 1 };

    if (submit(&admin, &c) == 0) {
        uint8_t mdts = ((uint8_t *)page)[77];   /* 2^mdts pages, 0: no limit */

        if (mdts != 0 && (1u << mdts) * 8 < max_sectors) {
            max_sectors = (1u << mdts) * 8;
        }
        c = (struct command){ .cdw0 = ADMIN_IDENTIFY, .nsid = 1, .prp1 = page, .cdw10 = 0 };
        if (submit(&admin, &c) == 0) {
            uint8_t format = ((uint8_t *)page)[26] & 0xF;
            uint32_t lbaf;

            memcpy(&lbaf, (uint8_t *)page + 128 + 4 * format, 4);
            ok = ((lbaf >> 16) & 0xFF) == 9;    /* 2^9: 512-byte sectors */
        }
    }
    mem_pages_free(page, 1);
    return ok;
}

static void stop(void) {
    set32(CC, 0);
    wait_ready(false);
}

static bool start(void) {
    uint64_t cap = reg64(CAP);
    unsigned stride = 4u << ((cap >> 32) & 0xF);

    timeout_ms = 500 * ((cap >> 24) & 0xFF) + 500;
    if (((cap >> 48) & 0xF) != 0) {
        return false;                   /* cannot do 4 KiB pages */
    }
    stop();
    if ((reg32(CSTS) & CSTS_RDY) != 0 || (pages = mem_pages_below(PAGES, 1ull << 32)) == 0) {
        return false;
    }
    memset((void *)pages, 0, PAGES * 4096);
    queue_init(&admin, pages, pages + 4096, 0, stride);
    queue_init(&io, pages + 2 * 4096, pages + 3 * 4096, 1, stride);

    set32(AQA, (DEPTH - 1) << 16 | (DEPTH - 1));
    set64(ASQ, pages);
    set64(ACQ, pages + 4096);
    set32(CC, CC_QUEUES | CC_EN);
    if (!wait_ready(true)) {
        dbg("nvme: not ready, %x\n", (uint64_t)reg32(CSTS));
        return false;
    }
    if (!identify()) {
        dbg("nvme: identify failed\n");
        return false;
    }
    /* The answers' queue first, then the commands' that answer there. */
    struct command c = { .cdw0 = ADMIN_CREATE_CQ, .prp1 = pages + 3 * 4096,
                         .cdw10 = (DEPTH - 1) << 16 | 1, .cdw11 = 1 };

    if (submit(&admin, &c) < 0) {
        return false;
    }
    c = (struct command){ .cdw0 = ADMIN_CREATE_SQ, .prp1 = pages + 2 * 4096,
                          .cdw10 = (DEPTH - 1) << 16 | 1, .cdw11 = 1u << 16 | 1 };
    return submit(&admin, &c) == 0;
}

static void let_go(void) {
    stop();
    if (pages != 0) {
        mem_pages_free(pages, PAGES);
        pages = 0;
    }
    efi_pci(at, false);
}

MODULE_EXPORT int module_init(void) {
    for (at = pci_next_class(0x01, 0x08, 0); at != 0; at = pci_next_class(0x01, 0x08, at)) {
        if ((pci_read(at, 0x08) >> 8 & 0xFF) != 0x02) {
            continue;                   /* not NVMe's programming interface */
        }
        uint64_t bar = pci_memory(at, 0);

        if (bar == 0) {
            continue;
        }
        regs = (volatile uint8_t *)bar;
        max_sectors = DMA_PAGES * 8;
        efi_pci(at, true);
        if (start() && disk_register(&driver)) {
            return 0;
        }
        let_go();
    }
    return -ENODEV;
}

/* Its disk is the firmware's no longer, so while it has the filesystem
   it stays. */
MODULE_EXPORT int module_exit(void) {
    if (disk_driver == &driver) {
        return -EBUSY;
    }
    let_go();
    return 0;
}
