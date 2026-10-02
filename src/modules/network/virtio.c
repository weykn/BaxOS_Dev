/* network/virtio: the virtual card of KVM and most clouds (virtio-net), in
 * its legacy form, which every such host still offers: registers in I/O
 * ports and two queues in memory the device reads and writes itself.
 *
 * A queue is a ring of descriptors - each a buffer - and two rings of
 * their numbers: the driver lists buffers it hands over in one, the device
 * lists them in the other once it is done with them. Receiving keeps eight
 * buffers handed over; sending hands over one and waits for it back. */

#include "module.h"
#include "net.h"
#include "efi_kernel.h"
#include "io.h"
#include "mem.h"
#include "pci.h"
#include "string.h"

#define ENODEV 19

#define FEATURES   0x00
#define GUEST      0x04
#define QUEUE_PFN  0x08
#define QUEUE_SIZE 0x0C
#define QUEUE_SEL  0x0E
#define NOTIFY     0x10
#define STATUS     0x12
#define CONFIG     0x14             /* the MAC, with MSI-X off */

#define F_MAC      (1u << 5)
#define S_ACK      1
#define S_DRIVER   2
#define S_OK       4

#define HDR        10               /* virtio_net_hdr, ahead of each frame */
#define BUFS       8
#define BUF_SIZE   2048

struct desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags, next;
};

struct avail {
    uint16_t flags, idx, ring[];
};

struct used_elem {
    uint32_t id, len;
};

struct used {
    uint16_t flags, idx;
    struct used_elem ring[];
};

struct queue {
    uint16_t size, seen;            /* how far through used we have got */
    uint64_t at;
    uint32_t pages;
    volatile struct desc  *desc;
    volatile struct avail *avail;
    volatile struct used  *used;
};

static const uint16_t ids[] = { 0x1000 };

static uint16_t io;
static struct queue rxq, txq;
static uint64_t bufs;               /* BUFS receive buffers, then the send one */

#define BUF_PAGES ((BUFS + 1) * BUF_SIZE / 4096 + 1)

static void barrier(void) {
    __asm__ volatile("" ::: "memory");
}

static bool queue_start(struct queue *q, uint16_t which) {
    outw(io + QUEUE_SEL, which);
    q->size = inw(io + QUEUE_SIZE);
    if (q->size == 0) {
        return false;
    }
    uint32_t ring = ((16u * q->size + 6 + 2u * q->size) + 4095) & ~4095u;

    q->pages = (ring + 6 + 8u * q->size + 4095) / 4096;
    if ((q->at = mem_pages(q->pages)) == 0) {
        return false;
    }
    memset((void *)q->at, 0, q->pages * 4096u);
    q->desc = (volatile struct desc *)q->at;
    q->avail = (volatile struct avail *)(q->at + 16u * q->size);
    q->used = (volatile struct used *)(q->at + ring);
    q->seen = 0;
    outl(io + QUEUE_PFN, (uint32_t)(q->at >> 12));
    return true;
}

static void hand_over(struct queue *q, uint16_t id) {
    q->avail->ring[q->avail->idx % q->size] = id;
    barrier();
    q->avail->idx++;
}

static void send(const uint8_t *frame, size_t len) {
    uint8_t *buf = (uint8_t *)(bufs + BUFS * BUF_SIZE);
    uint64_t until = efi_uptime_ms() + 50;

    memset(buf, 0, HDR);
    memcpy(buf + HDR, frame, len);
    txq.desc[0] = (struct desc){ (uint64_t)buf, (uint32_t)(HDR + len), 0, 0 };
    hand_over(&txq, 0);
    outw(io + NOTIFY, 1);
    while (txq.used->idx == txq.seen && efi_uptime_ms() < until) {
        __asm__ volatile("pause");
    }
    txq.seen = txq.used->idx;
}

static void poll(void (*input)(const uint8_t *frame, size_t len)) {
    bool handed = false;

    while (rxq.used->idx != rxq.seen) {
        volatile struct used_elem *e = &rxq.used->ring[rxq.seen % rxq.size];
        uint32_t id = e->id, len = e->len;

        if (id < BUFS && len > HDR) {
            input((const uint8_t *)(bufs + id * BUF_SIZE + HDR), len - HDR);
        }
        rxq.seen++;
        hand_over(&rxq, (uint16_t)id);
        handed = true;
    }
    if (handed) {
        outw(io + NOTIFY, 0);
    }
}

static uint32_t at;                 /* the card's PCI address */

static void stop(void) {
    if (io == 0) {
        return;
    }
    outb(io + STATUS, 0);           /* a reset: the device lets go of it all */
    if (rxq.at != 0) {
        mem_pages_free(rxq.at, rxq.pages);
    }
    if (txq.at != 0) {
        mem_pages_free(txq.at, txq.pages);
    }
    if (bufs != 0) {
        mem_pages_free(bufs, BUF_PAGES);
    }
    rxq = txq = (struct queue){ 0 };
    bufs = 0;
}

static struct net_card card;

static bool start(void) {
    io = (uint16_t)(pci_read(at, 0x10) & ~3u);
    pci_write(at, 0x04, (pci_read(at, 0x04) & 0xFFFF) | 0x5);     /* ports, bus master */

    outb(io + STATUS, 0);
    outb(io + STATUS, S_ACK | S_DRIVER);
    outl(io + GUEST, inl(io + FEATURES) & F_MAC);
    if (!queue_start(&rxq, 0) || !queue_start(&txq, 1) ||
        (bufs = mem_pages(BUF_PAGES)) == 0) {
        stop();
        return false;
    }
    for (unsigned i = 0; i < 6; i++) {
        card.mac[i] = inb(io + CONFIG + i);
    }
    for (uint16_t i = 0; i < BUFS; i++) {
        rxq.desc[i] = (struct desc){ bufs + i * BUF_SIZE, BUF_SIZE, 2, 0 };    /* the device writes */
        hand_over(&rxq, i);
    }
    outb(io + STATUS, S_ACK | S_DRIVER | S_OK);
    outw(io + NOTIFY, 0);
    card.memory = (rxq.pages + txq.pages + BUF_PAGES) * 4096u;
    return true;
}

static struct net_card card = { "virtio", { 0 }, 0, start, send, poll };

MODULE_EXPORT int module_init(void) {
    at = pci_find(0x1AF4, ids, 1);
    if (at == 0 || (pci_read(at, 0x10) & 1) == 0) {
        return -ENODEV;             /* none, or only the modern interface */
    }
    return net_card_register(&card);
}

MODULE_EXPORT int module_exit(void) {
    net_card_register(NULL);
    stop();
    return 0;
}
