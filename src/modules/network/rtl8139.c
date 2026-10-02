/* network/rtl8139: Realtek's RTL8139, the cheap card of the early 2000s and
 * one QEMU still offers (-nic model=rtl8139).
 *
 * All through I/O ports, polled. It receives into one ring of its own
 * layout - each frame behind a four-byte header, the whole thing written
 * round and round - and sends from four slots taken in turn. It can only
 * reach the first four gigabytes. */

#include "module.h"
#include "net.h"
#include "efi_kernel.h"
#include "io.h"
#include "mem.h"
#include "pci.h"
#include "string.h"

#define ENODEV 19

#define IDR     0x00                /* the MAC address */
#define TSD     0x10                /* four transmit statuses */
#define TSAD    0x20                /* and their addresses */
#define RBSTART 0x30
#define CR      0x37
#define CAPR    0x38
#define IMR     0x3C
#define ISR     0x3E
#define TCR     0x40
#define RCR     0x44
#define CONFIG1 0x52

#define CR_RESET 0x10
#define CR_RE    0x08
#define CR_TE    0x04
#define CR_BUFE  0x01               /* nothing received left in the ring */
#define TSD_TOK  (1u << 15)

#define RING   16384                /* more than a TCP window's worth in flight */
#define PAGES  6                    /* the ring, the 16 + 1500 it may run
                                       over by, and the frame being sent */

static const uint16_t ids[] = { 0x8139 };

static uint16_t io;
static uint64_t pages;
static uint8_t *ring, *tx_buf;
static unsigned rx_at, tx_slot;

static void send(const uint8_t *frame, size_t len) {
    uint64_t until = efi_uptime_ms() + 50;

    memcpy(tx_buf, frame, len);
    if (len < 60) {
        memset(tx_buf + len, 0, 60 - len);      /* the card does not pad */
        len = 60;
    }
    outl(io + TSAD + 4 * tx_slot, (uint32_t)(uint64_t)tx_buf);
    outl(io + TSD + 4 * tx_slot, (uint32_t)len);
    while ((inl(io + TSD + 4 * tx_slot) & TSD_TOK) == 0 && efi_uptime_ms() < until) {
        __asm__ volatile("pause");
    }
    tx_slot = (tx_slot + 1) % 4;
}

static void poll(void (*input)(const uint8_t *frame, size_t len)) {
    while ((inb(io + CR) & CR_BUFE) == 0) {
        const uint8_t *at = ring + rx_at;
        uint16_t status = (uint16_t)(at[0] | at[1] << 8);
        uint16_t len = (uint16_t)(at[2] | at[3] << 8);    /* with the CRC */

        if ((status & 1) == 0 || len < 18 || len > 1522) {
            break;                  /* not a good frame: leave it be */
        }
        input(at + 4, len - 4u);
        rx_at = (rx_at + len + 4 + 3) & ~3u;
        rx_at %= RING;
        outw(io + CAPR, (uint16_t)(rx_at - 16));
    }
    outw(io + ISR, 0xFFFF);
}

static uint32_t at;                 /* the card's PCI address */
static struct net_card card;

static bool start(void) {
    if ((pages = mem_pages_below(PAGES, 1ull << 32)) == 0) {
        return false;
    }
    io = (uint16_t)(pci_read(at, 0x10) & ~3u);
    pci_write(at, 0x04, (pci_read(at, 0x04) & 0xFFFF) | 0x5);     /* ports, bus master */

    outb(io + CONFIG1, 0);          /* awake */
    outb(io + CR, CR_RESET);
    for (uint64_t until = efi_uptime_ms() + 100;
         (inb(io + CR) & CR_RESET) != 0 && efi_uptime_ms() < until;) {
    }
    for (unsigned i = 0; i < 6; i++) {
        card.mac[i] = inb(io + IDR + i);
    }
    ring = (uint8_t *)pages;
    tx_buf = (uint8_t *)pages + 5 * 4096;
    rx_at = tx_slot = 0;
    outl(io + RBSTART, (uint32_t)pages);
    outw(io + IMR, 0);              /* polled */
    /* Ours, broadcast and multicast; frames may run past the end of the
       ring rather than wrapping inside one. */
    outl(io + RCR, 0x2 | 0x4 | 0x8 | 0x80 | 1u << 11);     /* and 16 KiB of it */
    outb(io + CR, CR_RE | CR_TE);
    outl(io + TCR, 0x03000700);
    return true;
}

static struct net_card card = { "rtl8139", { 0 }, PAGES * 4096, start, send, poll };

MODULE_EXPORT int module_init(void) {
    at = pci_find(0x10EC, ids, 1);
    if (at == 0 || (pci_read(at, 0x10) & 1) == 0) {
        return -ENODEV;
    }
    return net_card_register(&card);
}

MODULE_EXPORT int module_exit(void) {
    net_card_register(NULL);
    if (pages != 0) {
        outb(io + CR, CR_RESET);
        mem_pages_free(pages, PAGES);
    }
    return 0;
}
