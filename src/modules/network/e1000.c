/* network/e1000: Intel's 8254x and 82574 cards - what QEMU gives a machine
 * unless told otherwise, and what a great many real ones have had.
 *
 * Driven through its registers in memory, polled: one page holds both
 * descriptor rings and the buffer frames are sent from, and the card
 * receives into four pages more. A send waits for the card to have the
 * frame, so one buffer is all sending needs. */

#include "module.h"
#include "net.h"
#include "efi_kernel.h"
#include "mem.h"
#include "pci.h"
#include "string.h"

#define ENODEV 19

#define CTRL   0x0000
#define IMC    0x00D8
#define RCTL   0x0100
#define TCTL   0x0400
#define TIPG   0x0410
#define RDBAL  0x2800
#define RDBAH  0x2804
#define RDLEN  0x2808
#define RDH    0x2810
#define RDT    0x2818
#define TDBAL  0x3800
#define TDBAH  0x3804
#define TDLEN  0x3808
#define TDH    0x3810
#define TDT    0x3818
#define MTA    0x5200
#define RAL    0x5400
#define RAH    0x5404

#define CTRL_SLU (1u << 6)
#define CTRL_RST (1u << 26)

#define RING    8                   /* descriptors: 128 bytes, the least */
#define RX_SIZE 2048
#define PAGES   (1 + RING * RX_SIZE / 4096)

struct rx_desc {
    uint64_t addr;
    uint16_t len, sum;
    uint8_t  status, errors;
    uint16_t special;
};

struct tx_desc {
    uint64_t addr;
    uint16_t len;
    uint8_t  cso, cmd, status, css;
    uint16_t special;
};

static const uint16_t ids[] = { 0x100E, 0x100F, 0x1004, 0x10D3 };

static volatile uint32_t *regs;
static volatile struct rx_desc *rx;
static volatile struct tx_desc *tx;
static uint8_t *rx_buf, *tx_buf;
static unsigned rx_next, tx_next;
static uint64_t pages;

static uint32_t reg(unsigned r) { return regs[r / 4]; }
static void reg_set(unsigned r, uint32_t v) { regs[r / 4] = v; }

static void wait_ms(uint64_t ms) {
    for (uint64_t until = efi_uptime_ms() + ms; efi_uptime_ms() < until;) {
        __asm__ volatile("pause");
    }
}

static void send(const uint8_t *frame, size_t len) {
    volatile struct tx_desc *d = &tx[tx_next];
    uint64_t until = efi_uptime_ms() + 50;

    memcpy(tx_buf, frame, len);
    d->addr = (uint64_t)tx_buf;
    d->len = (uint16_t)len;
    d->cmd = 1 | 2 | 8;             /* end of packet, add the CRC, report */
    d->status = 0;
    tx_next = (tx_next + 1) % RING;
    reg_set(TDT, tx_next);
    while ((d->status & 1) == 0 && efi_uptime_ms() < until) {
        __asm__ volatile("pause");
    }
}

static void poll(void (*input)(const uint8_t *frame, size_t len)) {
    while (rx[rx_next].status & 1) {
        if ((rx[rx_next].status & 2) != 0 && rx[rx_next].errors == 0) {
            input(rx_buf + rx_next * RX_SIZE, rx[rx_next].len);
        }
        rx[rx_next].status = 0;
        reg_set(RDT, rx_next);
        rx_next = (rx_next + 1) % RING;
    }
}

static uint32_t at;                 /* the card's PCI address */
static struct net_card card;

static bool start(void) {
    uint32_t bar = pci_read(at, 0x10);
    uint64_t base = bar & ~0xFull;

    if ((bar & 6) == 4) {
        base |= (uint64_t)pci_read(at, 0x14) << 32;
    }
    if ((pages = mem_pages(PAGES)) == 0) {
        return false;
    }
    pci_write(at, 0x04, (pci_read(at, 0x04) & 0xFFFF) | 0x6);     /* memory, bus master */
    regs = (volatile uint32_t *)base;

    reg_set(IMC, ~0u);              /* polled: no interrupts */
    reg_set(CTRL, reg(CTRL) | CTRL_RST);
    wait_ms(10);
    reg_set(IMC, ~0u);
    reg_set(CTRL, (reg(CTRL) | CTRL_SLU) & ~(1u << 3 | 1u << 7 | 1u << 30 | 1u << 31));

    uint32_t low = reg(RAL), high = reg(RAH);

    for (unsigned i = 0; i < 4; i++) {
        card.mac[i] = (uint8_t)(low >> 8 * i);
    }
    card.mac[4] = (uint8_t)high;
    card.mac[5] = (uint8_t)(high >> 8);
    reg_set(RAH, (high & 0xFFFF) | 1u << 31);
    for (unsigned i = 0; i < 128; i++) {
        reg_set(MTA + 4 * i, 0);
    }

    rx = (volatile struct rx_desc *)pages;
    tx = (volatile struct tx_desc *)(pages + 128);
    tx_buf = (uint8_t *)pages + 2048;
    rx_buf = (uint8_t *)pages + 4096;
    rx_next = tx_next = 0;
    for (unsigned i = 0; i < RING; i++) {
        rx[i] = (struct rx_desc){ .addr = (uint64_t)rx_buf + i * RX_SIZE };
        tx[i] = (struct tx_desc){ 0 };
    }
    reg_set(RDBAL, (uint32_t)(uint64_t)rx);
    reg_set(RDBAH, (uint32_t)((uint64_t)rx >> 32));
    reg_set(RDLEN, RING * sizeof *rx);
    reg_set(RDH, 0);
    reg_set(RDT, RING - 1);
    /* On, broadcasts too, 2 KiB buffers, the CRC left off. */
    reg_set(RCTL, 1u << 1 | 1u << 15 | 1u << 26);

    reg_set(TDBAL, (uint32_t)(uint64_t)tx);
    reg_set(TDBAH, (uint32_t)((uint64_t)tx >> 32));
    reg_set(TDLEN, RING * sizeof *tx);
    reg_set(TDH, 0);
    reg_set(TDT, 0);
    /* On, short frames padded, the collision settings every driver uses. */
    reg_set(TCTL, 1u << 1 | 1u << 3 | 0x10u << 4 | 0x40u << 12);
    reg_set(TIPG, 0x0060200A);
    return true;
}

static struct net_card card = { "e1000", { 0 }, PAGES * 4096, start, send, poll };

MODULE_EXPORT int module_init(void) {
    at = pci_find(0x8086, ids, sizeof ids / sizeof ids[0]);
    if (at == 0 || (pci_read(at, 0x10) & 1) != 0) {
        return -ENODEV;             /* none, or ports rather than memory */
    }
    return net_card_register(&card);
}

/* The card is reset before its memory goes back: it must not write into
   pages someone else has by then. */
MODULE_EXPORT int module_exit(void) {
    net_card_register(NULL);
    if (pages != 0) {
        reg_set(RCTL, 0);
        reg_set(CTRL, reg(CTRL) | CTRL_RST);
        wait_ms(1);
        mem_pages_free(pages, PAGES);
    }
    return 0;
}
