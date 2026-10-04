/* network/rtl8169: Realtek's gigabit family as PCs have it on the board -
 * the RTL8168/8111 at 1 Gb/s and the RTL8125 at 2.5 - the most common
 * wired port on desktop boards for fifteen years.
 *
 * After Linux's r8169 driver, the smallest part of it: reset the MAC, two
 * rings of descriptors in one page and receive buffers behind them, polled,
 * no interrupts. The PHY is left as the firmware brought it up, which
 * negotiates on its own; Linux's per-revision tuning and PHY firmware are
 * not applied, so a revision that needs them may not get a link.
 *
 * Not tested on hardware: no emulator has the chip. */

#include "efi_kernel.h"
#include "mem.h"
#include "module.h"
#include "net.h"
#include "pci.h"
#include "string.h"

#define ENODEV 19

#define MAC0        0x00
#define MAR0        0x08            /* the multicast filter */
#define TX_RING     0x20
#define CHIP_CMD    0x37
#define TX_POLL     0x38            /* the 8168's, a byte */
#define INTR_MASK   0x3C            /* the 8168's, 16 bits */
#define TX_CONFIG   0x40
#define RX_CONFIG   0x44
#define CFG9346     0x50
#define OCPDR       0xB0
#define RX_MAX_SIZE 0xDA
#define RX_RING     0xE4
#define MISC        0xF0
#define MASK_8125   0x38            /* the 8125's interrupt mask, 32 bits */
#define POLL_8125   0x90
#define MAC0_BKP    0x19E0          /* where the 8125 keeps its own address */

#define CMD_RESET   0x10
#define CMD_RX      0x08
#define CMD_TX      0x04
#define RXDV_GATE   (1u << 19)
#define ACCEPT      0x0E            /* broadcast, multicast, ours */

#define OWN   (1u << 31)
#define END   (1u << 30)
#define FIRST (1u << 29)
#define LAST  (1u << 28)

#define RX_COUNT 8
#define TX_COUNT 4
#define BUF      2048
#define PAGES    (1 + (RX_COUNT + 1) * BUF / 4096)   /* rings, buffers, one to send */

struct desc {
    uint32_t opts1, opts2;
    uint64_t addr;
};

static const uint16_t ids[] = { 0x8125, 0x8168, 0x8161 };

static uint32_t at;
static volatile uint8_t *regs;
static bool is_8125;
static uint64_t pages;
static volatile struct desc *rx, *tx;
static uint8_t *rx_buf, *tx_buf;
static unsigned rx_next, tx_next;
static struct net_card card;

static uint8_t r8(unsigned r) { return regs[r]; }
static uint32_t r32(unsigned r) { return *(volatile uint32_t *)(regs + r); }
static void w8(unsigned r, uint8_t v) { regs[r] = v; }
static void w16(unsigned r, uint16_t v) { *(volatile uint16_t *)(regs + r) = v; }
static void w32(unsigned r, uint32_t v) { *(volatile uint32_t *)(regs + r) = v; }

/* The 8125's MAC registers behind its OCP window. */
static void ocp_modify(uint32_t reg, uint16_t clear, uint16_t set) {
    w32(OCPDR, reg << 15);
    uint16_t v = (uint16_t)((r32(OCPDR) & ~clear) | set);

    w32(OCPDR, 0x80000000u | reg << 15 | v);
}

static void send(const uint8_t *frame, size_t len) {
    volatile struct desc *d = &tx[tx_next];
    uint64_t until = efi_uptime_ms() + 50;

    if (len > BUF) {
        return;
    }
    memcpy(tx_buf, frame, len);
    d->addr = (uint64_t)tx_buf;
    d->opts2 = 0;
    __asm__ volatile("" : : : "memory");
    d->opts1 = OWN | FIRST | LAST | (tx_next == TX_COUNT - 1 ? END : 0) | (uint32_t)len;
    if (is_8125) {
        w16(POLL_8125, 1);
    } else {
        w8(TX_POLL, 0x40);
    }
    while ((d->opts1 & OWN) && efi_uptime_ms() < until) {
        __asm__ volatile("pause");
    }
    tx_next = (tx_next + 1) % TX_COUNT;
}

static void poll(void (*input)(const uint8_t *frame, size_t len)) {
    for (;;) {
        volatile struct desc *d = &rx[rx_next];
        uint32_t opts = d->opts1;

        if (opts & OWN) {
            return;
        }
        uint32_t len = opts & 0x3FFF;   /* with the CRC */

        if ((opts & (FIRST | LAST)) == (FIRST | LAST) && !(opts & (1u << 21)) && len > 4) {
            input(rx_buf + rx_next * BUF, len - 4);
        }
        d->opts1 = OWN | (rx_next == RX_COUNT - 1 ? END : 0) | BUF;
        rx_next = (rx_next + 1) % RX_COUNT;
    }
}

static bool valid(const uint8_t *mac) {
    return (mac[0] & 1) == 0 && (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) != 0;
}

static bool start(void) {
    uint64_t bar = pci_memory(at, 2);

    if (bar == 0 || (pages = mem_pages_below(PAGES, 1ull << 32)) == 0) {
        return false;
    }
    efi_pci(at, true);                  /* a firmware network driver may be running it */
    regs = (volatile uint8_t *)bar;

    w8(CHIP_CMD, CMD_RESET);
    for (uint64_t until = efi_uptime_ms() + 100;
         (r8(CHIP_CMD) & CMD_RESET) && efi_uptime_ms() < until;) {
    }
    for (unsigned i = 0; i < 6; i++) {
        card.mac[i] = r8(MAC0 + i);
    }
    if (is_8125 && !valid(card.mac)) {
        for (unsigned i = 0; i < 6; i++) {
            card.mac[i] = r8(MAC0_BKP + i);
        }
    }

    memset((void *)pages, 0, PAGES * 4096);
    rx = (volatile struct desc *)pages;
    tx = (volatile struct desc *)(pages + 256);
    rx_buf = (uint8_t *)pages + 4096;
    tx_buf = rx_buf + RX_COUNT * BUF;
    rx_next = tx_next = 0;
    for (unsigned i = 0; i < RX_COUNT; i++) {
        rx[i].addr = (uint64_t)(rx_buf + i * BUF);
        rx[i].opts1 = OWN | (i == RX_COUNT - 1 ? END : 0) | BUF;
    }

    w8(CFG9346, 0xC0);                  /* configuration writable */
    if (is_8125) {
        w32(MASK_8125, 0);
        ocp_modify(0xEB58, 1, 0);       /* the old descriptor format */
    } else {
        w16(INTR_MASK, 0);
    }
    w32(MISC, r32(MISC) & ~RXDV_GATE);  /* or nothing is received */
    w16(RX_MAX_SIZE, BUF);
    w32(TX_RING + 4, (uint32_t)((uint64_t)tx >> 32));
    w32(TX_RING, (uint32_t)(uint64_t)tx);
    w32(RX_RING + 4, (uint32_t)((uint64_t)rx >> 32));
    w32(RX_RING, (uint32_t)(uint64_t)rx);
    w8(CFG9346, 0);
    w8(CHIP_CMD, CMD_TX | CMD_RX);
    w32(RX_CONFIG, (is_8125 ? 8u << 27 : 1u << 15) | 7u << 8 | ACCEPT);
    w32(TX_CONFIG, 3u << 24 | 7u << 8 | 1u << 7);
    w32(MAR0, 0xFFFFFFFF);
    w32(MAR0 + 4, 0xFFFFFFFF);
    return true;
}

static struct net_card card = { "rtl8169", { 0 }, PAGES * 4096, start, send, poll };

MODULE_EXPORT int module_init(void) {
    at = pci_find(0x10EC, ids, sizeof ids / sizeof ids[0]);
    if (at == 0) {
        return -ENODEV;
    }
    is_8125 = pci_read(at, 0) >> 16 == 0x8125;
    return net_card_register(&card);
}

/* Reset before its memory goes back: it must not write into pages someone
   else has by then. */
MODULE_EXPORT int module_exit(void) {
    net_card_register(NULL);
    if (pages != 0) {
        w8(CHIP_CMD, CMD_RESET);
        mem_pages_free(pages, PAGES);
    }
    return 0;
}
