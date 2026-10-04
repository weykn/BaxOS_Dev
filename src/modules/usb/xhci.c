/* usb/xhci: the USB host controllers every PC since about 2012 has - all
 * its USB ports, 1.1 to 3.x, behind one kind of controller. It drives every
 * one there is, polled: takes it from the firmware (and from the BIOS's
 * SMM keyboard emulation), resets it, and finds what is plugged in, through
 * hubs too. Each device is set up for the first interface of it something
 * here can drive - a boot keyboard, bulk-only storage - and offered through
 * the kernel's usb_host slot (usb.h) to the class drivers, input/usbkbd and
 * storage/usb.
 *
 * Nothing is touched until a class driver asks (start), since the firmware
 * may be reading the very disk the modules are on through this controller.
 * A controller with nothing on it is handed straight back.
 *
 * After SeaBIOS's usb-xhci.c, usb.c and usb-hub.c: rings of 16 TRBs, each
 * in the 512 bytes it is aligned to, so a completion's TRB address names
 * its ring. A controller needs one page, a device one page more. */

#include "debug.h"
#include "efi_kernel.h"
#include "mem.h"
#include "module.h"
#include "pci.h"
#include "string.h"
#include "usb.h"

#define ENODEV 19
#define EBUSY  16

#define CONTROLLERS 6
#define DEVICES     16
#define SLOTS       16              /* at most, a controller */
#define RING        16              /* TRBs a ring, the last a link */

/* ---- registers ----------------------------------------------------------- */

#define USBCMD    0x00
#define USBSTS    0x04
#define CRCR      0x18
#define DCBAAP    0x30
#define CONFIG    0x38
#define PORTSC(n) (0x400 + 0x10 * ((n) - 1))

#define CMD_RUN   1u
#define CMD_RESET 2u
#define STS_HALT  1u
#define STS_CNR   (1u << 11)

#define PORT_CCS   (1u << 0)
#define PORT_PED   (1u << 1)
#define PORT_PR    (1u << 4)
#define PORT_PLS(p) (((p) >> 5) & 0xF)
#define PORT_SPEED(p) (((p) >> 10) & 0xF)
#define PORT_KEEP  0x0E00C200u      /* power and wake bits: written back as they are */
#define PORT_CHANGES 0x00FE0000u    /* write one to clear */

#define IMAN   0x00
#define ERSTSZ 0x08
#define ERSTBA 0x10
#define ERDP   0x18

/* ---- TRBs ------------------------------------------------------------------ */

enum {
    TRB_NORMAL = 1, TRB_SETUP = 2, TRB_DATA = 3, TRB_STATUS = 4, TRB_LINK = 6,
    TRB_ENABLE_SLOT = 9, TRB_ADDRESS = 11, TRB_CONFIGURE = 12, TRB_EVALUATE = 13,
    TRB_RESET_EP = 14, TRB_SET_DEQUEUE = 16,
    TRB_TRANSFER = 32, TRB_COMMAND = 33,
};

#define TRB_CYCLE  (1u << 0)
#define TRB_TOGGLE (1u << 1)        /* on a link: the cycle turns over */
#define TRB_CHAIN  (1u << 4)
#define TRB_IOC    (1u << 5)
#define TRB_IDT    (1u << 6)
#define TRB_IN     (1u << 16)       /* a data or status stage's direction */

#define CC_SUCCESS 1
#define CC_STALL   6
#define CC_SHORT   13

struct trb {
    uint64_t param;
    uint32_t status, control;
};

struct ring {
    struct trb trb[RING];
    uint32_t next, done;            /* where the next goes; one past the last done */
    uint32_t cycle;
    uint32_t status, control;       /* of the last completion */
    uint8_t  slot, epid, queued, size;
    uint8_t  report[64];            /* an interrupt endpoint's, as it came */
};

_Static_assert(sizeof(struct ring) <= 512, "a ring in its 512 bytes");

struct controller {
    volatile uint8_t *caps, *op, *run;
    volatile uint32_t *bells;
    uint32_t pci;
    uint64_t page;                  /* input context, DCBAA, rings, buffer */
    uint64_t pad_pages;             /* scratchpad: its array, then the pages */
    unsigned pads, ports, slots, csz;
    uint32_t event, cycle;          /* the event ring's place */
    uint8_t  usb2_first, usb2_count;
};

/* The controller's page. */
#define INPUT   0x000               /* up to 33 contexts of 64 */
#define DCBAA   0x880
#define ERST    0x940
#define COMMANDS 0xA00
#define EVENTS  0xC00
#define BUFFER  0xE00               /* 512 bytes, for control transfers */

struct device {
    struct usb_device pub;          /* first: what class drivers are handed */
    struct controller *c;
    uint64_t page;                  /* output context, then four rings */
    uint32_t route;
    uint8_t  slot, speed, root, depth;
    uint8_t  tt_slot, tt_port;      /* the high-speed hub a slow device is behind */
    uint8_t  in_ep, out_ep;         /* endpoint context ids */
    uint8_t  hub;
};

static struct controller controllers[CONTROLLERS];
static struct device     devices[DEVICES];
static unsigned          controller_count, device_count;
static bool              started;

static uint32_t rd(volatile uint8_t *base, unsigned r) { return *(volatile uint32_t *)(base + r); }
static void wr(volatile uint8_t *base, unsigned r, uint32_t v) { *(volatile uint32_t *)(base + r) = v; }
static void wr64(volatile uint8_t *base, unsigned r, uint64_t v) {
    wr(base, r, (uint32_t)v);
    wr(base, r + 4, (uint32_t)(v >> 32));
}

static void sleep_ms(uint64_t ms) {
    for (uint64_t until = efi_uptime_ms() + ms; efi_uptime_ms() < until;) {
        __asm__ volatile("pause");
    }
}

static bool wait_bits(volatile uint8_t *base, unsigned r, uint32_t mask, uint32_t want,
                      uint64_t ms) {
    for (uint64_t until = efi_uptime_ms() + ms; efi_uptime_ms() < until;) {
        if ((rd(base, r) & mask) == want) {
            return true;
        }
    }
    return false;
}

/* ---- rings and events ------------------------------------------------------ */

static struct ring *ring_at(uint64_t page, unsigned offset) {
    return (struct ring *)(page + offset);
}

static void ring_init(struct ring *r) {
    memset(r, 0, sizeof *r);
    r->cycle = 1;
}

/* Room for n TRBs in a row: a link back to the start first if there is
   not, so that no transfer is split across it. */
static void ring_room(struct ring *r, unsigned n) {
    r->done = ~0u;                      /* nothing of this is done yet */
    if (r->next + n <= RING - 1) {
        return;
    }
    r->trb[r->next].param = (uint64_t)&r->trb[0];
    r->trb[r->next].status = 0;
    r->trb[r->next].control = TRB_LINK << 10 | TRB_TOGGLE | r->cycle;
    r->next = 0;
    r->cycle ^= 1;
}

static void ring_put(struct ring *r, uint64_t param, uint32_t status, uint32_t control) {
    struct trb *t = &r->trb[r->next++];

    t->param = param;
    t->status = status;
    __asm__ volatile("" : : : "memory");
    t->control = control | r->cycle;
}

/* Everything the controller has said since last asked: each completion is
   left on the ring it is for. */
static void events(struct controller *c) {
    struct trb *ring = (struct trb *)(c->page + EVENTS);

    for (;;) {
        struct trb *e = &ring[c->event];
        uint32_t control = e->control;

        if ((control & TRB_CYCLE) != c->cycle) {
            break;
        }
        unsigned type = control >> 10 & 0x3F;

        if (type == TRB_TRANSFER || type == TRB_COMMAND) {
            uint64_t at = e->param & ~0xFull;
            uint64_t base = at & ~511ull;

            /* Only ever one of ours: a ring in this controller's page or in
               one of its devices'. */
            bool ours = (at >> 12) == (c->page >> 12);

            for (unsigned i = 0; i < device_count && !ours; i++) {
                ours = devices[i].c == c && (at >> 12) == (devices[i].page >> 12);
            }
            if (ours && at - base < RING * sizeof(struct trb)) {
                struct ring *r = (struct ring *)base;

                r->done = (uint32_t)((at - base) / sizeof(struct trb)) + 1;
                r->status = e->status;
                r->control = control;
            }
        }
        if (++c->event == RING) {
            c->event = 0;
            c->cycle ^= 1;
        }
        wr64(c->run, 0x20 + ERDP, (uint64_t)&ring[c->event] | 8);
    }
}

/* Waits for everything on r to be done: its completion code, or -1. */
static int wait_ring(struct controller *c, struct ring *r, uint64_t ms) {
    for (uint64_t until = efi_uptime_ms() + ms; efi_uptime_ms() < until;) {
        events(c);
        if (r->done == r->next) {
            return r->status >> 24;
        }
    }
    return -1;
}

static int command(struct controller *c, uint64_t param, uint32_t control) {
    struct ring *r = ring_at(c->page, COMMANDS);

    ring_room(r, 1);
    ring_put(r, param, 0, control);
    c->bells[0] = 0;
    return wait_ring(c, r, 1000);
}

/* ---- contexts ---------------------------------------------------------------- */

/* Context i of the input context: 0 says what changes, 1 is the slot,
   2 on endpoints by id - 1 + 1. */
static volatile uint32_t *input(struct controller *c, unsigned i) {
    return (volatile uint32_t *)(c->page + INPUT + i * c->csz);
}

static void input_clear(struct controller *c) {
    memset((void *)(c->page + INPUT), 0, 33 * c->csz);
}

/* The slot context everything about where d is goes in. */
static void slot_context(struct device *d, unsigned entries) {
    volatile uint32_t *s = input(d->c, 1);

    s[0] = d->route | (uint32_t)d->speed << 20 | (d->hub ? 1u << 26 : 0) | entries << 27;
    s[1] = (uint32_t)d->root << 16 | (uint32_t)d->hub << 24;
    s[2] = d->tt_slot | (uint32_t)d->tt_port << 8;
}

static void endpoint_context(struct device *d, unsigned epid, unsigned type,
                             unsigned size, unsigned interval, struct ring *r) {
    volatile uint32_t *e = input(d->c, epid + 1);

    e[0] = interval << 16;
    e[1] = 3u << 1 | type << 3 | size << 16;
    e[2] = (uint32_t)(uint64_t)r | 1;
    e[3] = (uint32_t)((uint64_t)r >> 32);
    e[4] = type == 4 ? 8 : type == 7 ? size | size << 16 : size;
    r->slot = d->slot;
    r->epid = (uint8_t)epid;
}

/* ---- transfers ----------------------------------------------------------------- */

static struct ring *ep_ring(struct device *d, unsigned epid) {
    unsigned at = epid == 1 ? 0x800 : epid == d->in_ep ? 0xA00 : 0xC00;

    return ring_at(d->page, at);
}

/* A stalled endpoint is reset and its ring put back where it is. */
static void unstall(struct device *d, struct ring *r) {
    struct controller *c = d->c;

    command(c, 0, TRB_RESET_EP << 10 | (uint32_t)r->epid << 16 | (uint32_t)d->slot << 24);
    command(c, (uint64_t)&r->trb[r->next] | r->cycle,
            TRB_SET_DEQUEUE << 10 | (uint32_t)r->epid << 16 | (uint32_t)d->slot << 24);
    r->done = r->next;
}

static int control(struct device *d, uint8_t type, uint8_t request, uint16_t value,
                   uint16_t index, void *data, uint16_t length) {
    struct controller *c = d->c;
    struct ring *r = ep_ring(d, 1);
    uint8_t *buffer = (uint8_t *)(c->page + BUFFER);
    bool in = type & USB_IN;
    uint64_t setup = type | (uint32_t)request << 8 | (uint64_t)value << 16 |
                     (uint64_t)index << 32 | (uint64_t)length << 48;

    if (length > 512) {
        return -1;
    }
    if (!in && length > 0) {
        memcpy(buffer, data, length);
    }
    ring_room(r, 3);
    ring_put(r, setup, 8, TRB_SETUP << 10 | TRB_IDT |
             (length == 0 ? 0 : in ? 3u << 16 : 2u << 16));
    if (length > 0) {
        ring_put(r, (uint64_t)buffer, length, TRB_DATA << 10 | (in ? TRB_IN : 0));
    }
    ring_put(r, 0, 0, TRB_STATUS << 10 | TRB_IOC | (in && length > 0 ? 0 : TRB_IN));
    c->bells[d->slot] = 1;

    int cc = wait_ring(c, r, 1000);

    if (cc == CC_STALL) {
        unstall(d, r);
    }
    if (cc != CC_SUCCESS) {
        return -1;
    }
    if (in && length > 0) {
        memcpy(data, buffer, length);
    }
    return 0;
}

static int host_control(const struct usb_device *u, uint8_t type, uint8_t request,
                        uint16_t value, uint16_t index, void *data, uint16_t length) {
    return length > 256 ? -1 : control((struct device *)u, type, request, value, index,
                                       data, length);
}

/* Split at 64 KiB boundaries, which a TRB may not cross. */
static int host_bulk(const struct usb_device *u, uint8_t address, void *data, uint32_t length) {
    struct device *d = (struct device *)u;
    unsigned epid = (address & 0xF) * 2 + ((address & USB_IN) ? 1 : 0);
    struct ring *r = ep_ring(d, epid);
    uint64_t at = (uint64_t)data, end = at + length;
    unsigned n = 0;

    for (uint64_t p = at; p < end; p = (p | 0xFFFF) + 1) {
        n++;
    }
    if (n == 0 || n > 8) {
        return -1;
    }
    ring_room(r, n);
    for (uint64_t p = at; p < end;) {
        uint64_t stop = (p | 0xFFFF) + 1 < end ? (p | 0xFFFF) + 1 : end;

        ring_put(r, p, (uint32_t)(stop - p),
                 TRB_NORMAL << 10 | (stop == end ? TRB_IOC : TRB_CHAIN));
        p = stop;
    }
    d->c->bells[d->slot] = epid;

    int cc = wait_ring(d->c, r, 5000);

    if (cc == CC_SUCCESS || cc == CC_SHORT) {
        return (int)(length - (r->status & 0xFFFFFF));
    }
    if (cc == CC_STALL) {
        unstall(d, r);
        control(d, 0x02, 1 /* CLEAR_FEATURE */, 0 /* ENDPOINT_HALT */, address, NULL, 0);
    }
    return -1;
}

/* One transfer kept queued on the interrupt endpoint; each time it is
   found done, what came is handed over and another queued. */
static int host_report(const struct usb_device *u, void *data, unsigned length) {
    struct device *d = (struct device *)u;
    struct ring *r = ep_ring(d, d->in_ep);
    int got = 0;

    if (d->in_ep == 0) {
        return -1;
    }
    if (r->queued) {
        events(d->c);
        if (r->done != r->next) {
            return 0;
        }
        unsigned cc = r->status >> 24;

        r->queued = 0;
        if (cc == CC_SUCCESS || cc == CC_SHORT) {
            got = r->size - (int)(r->status & 0xFFFFFF);
            got = got < (int)length ? got : (int)length;
            memcpy(data, r->report, (size_t)got);
        } else if (cc == CC_STALL) {
            unstall(d, r);
        } else {
            return -1;
        }
    }
    r->size = (uint8_t)(u->in_size < sizeof r->report ? u->in_size : sizeof r->report);
    ring_room(r, 1);
    ring_put(r, (uint64_t)r->report, r->size, TRB_NORMAL << 10 | TRB_IOC);
    r->queued = 1;
    d->c->bells[d->slot] = d->in_ep;
    return got;
}

/* ---- finding devices ------------------------------------------------------------ */

enum { FULL = 1, LOW = 2, HIGH = 3, SUPER = 4 };

static void enumerate(struct controller *c, struct device *parent, uint8_t port,
                      uint8_t speed);

/* The xHCI interval for an interrupt endpoint's bInterval: frames for a
   slow device, a power of two of microframes for a fast one. */
static unsigned interval(uint8_t speed, uint8_t b) {
    unsigned i = 3;

    if (speed == HIGH || speed >= SUPER) {
        return b > 0 ? b - 1u : 0;
    }
    while (b > 1 && i < 10) {
        b >>= 1;
        i++;
    }
    return i;
}

/* What of the configuration in buf this host drives: the interface, and
   the endpoints of it. */
static bool choose(struct device *d, const uint8_t *buf, unsigned length,
                   uint8_t *in_attr, uint16_t *out_size, uint8_t *in_interval) {
    bool taken = false;

    for (unsigned at = 0; at + 2 <= length && buf[at] >= 2; at += buf[at]) {
        const uint8_t *x = buf + at;

        if (x[1] == 4 && at + 9 <= length) {        /* an interface */
            if (taken) {
                break;
            }
            bool keyboard = x[5] == USB_CLASS_HID && x[6] == 1 && x[7] == 1;
            bool storage = x[5] == USB_CLASS_STORAGE && x[7] == 0x50;
            bool hub = x[5] == 9;

            if (keyboard || storage || hub) {
                taken = true;
                d->pub.interface = x[2];
                d->pub.class = x[5];
                d->pub.subclass = x[6];
                d->pub.protocol = x[7];
            }
        } else if (x[1] == 5 && taken && at + 7 <= length) {    /* its endpoints */
            uint16_t size = (uint16_t)(x[4] | x[5] << 8) & 0x7FF;

            if (x[2] & USB_IN) {
                d->pub.in = x[2];
                d->pub.in_size = size;
                *in_attr = x[3] & 3;
                *in_interval = x[6];
            } else if ((x[3] & 3) == 2) {
                d->pub.out = x[2];
                *out_size = size;
            }
        }
    }
    return taken;
}

/* Sets up the endpoints chosen, and the configuration they are in. */
static bool configure(struct device *d, const uint8_t *config, unsigned length) {
    struct controller *c = d->c;
    uint8_t attr = 0, period = 0;
    uint16_t out_size = 0;

    if (!choose(d, config, length, &attr, &out_size, &period)) {
        return false;
    }
    if (control(d, 0x00, 9 /* SET_CONFIGURATION */, config[5], 0, NULL, 0) < 0) {
        return false;
    }
    if (d->pub.class == 9) {
        d->pub.in = 0;                  /* a hub's changes are asked for, not waited on */
        return true;
    }
    input_clear(c);
    unsigned last = 1;

    if (d->pub.in != 0) {
        d->in_ep = (uint8_t)((d->pub.in & 0xF) * 2 + 1);
        ring_init(ep_ring(d, d->in_ep));
        endpoint_context(d, d->in_ep, attr == 3 ? 7 : 6, d->pub.in_size,
                         attr == 3 ? interval(d->speed, period) : 0, ep_ring(d, d->in_ep));
        input(c, 0)[1] |= 1u << d->in_ep;
        last = d->in_ep;
    }
    if (d->pub.out != 0) {
        d->out_ep = (uint8_t)((d->pub.out & 0xF) * 2);
        ring_init(ep_ring(d, d->out_ep));
        endpoint_context(d, d->out_ep, 2, out_size, 0, ep_ring(d, d->out_ep));
        input(c, 0)[1] |= 1u << d->out_ep;
        last = d->out_ep > last ? d->out_ep : last;
    }
    input(c, 0)[1] |= 1;
    slot_context(d, last);
    return command(c, c->page + INPUT, TRB_CONFIGURE << 10 | (uint32_t)d->slot << 24) ==
           CC_SUCCESS;
}

/* ---- hubs -------------------------------------------------------------------------- */

#define HUB_PORT 0x23                   /* class request to one of its ports */

static int port_status(struct device *hub, unsigned port, uint32_t *status) {
    return control(hub, 0xA3, 0 /* GET_STATUS */, 0, (uint16_t)port, status, 4);
}

static void hub_setup(struct device *hub) {
    uint8_t desc[12] = { 0 };
    bool super = hub->speed >= SUPER;

    if (control(hub, 0xA0, 6, super ? 0x2A00 : 0x2900, 0, desc, sizeof desc) < 0) {
        return;
    }
    unsigned ports = desc[2] < 15 ? desc[2] : 15;

    /* Its port count goes in its slot, which makes it a hub to the
       controller. */
    hub->hub = (uint8_t)ports;
    input_clear(hub->c);
    input(hub->c, 0)[1] = 1;
    slot_context(hub, 1);
    command(hub->c, hub->c->page + INPUT, TRB_CONFIGURE << 10 | (uint32_t)hub->slot << 24);
    if (super) {
        control(hub, 0x20, 12 /* SET_HUB_DEPTH */, hub->depth, 0, NULL, 0);
    }
    for (unsigned p = 1; p <= ports; p++) {
        control(hub, HUB_PORT, 3 /* SET_FEATURE */, 8 /* POWER */, (uint16_t)p, NULL, 0);
    }
    sleep_ms(desc[5] * 2u + 100);

    for (unsigned p = 1; p <= ports; p++) {
        uint32_t st;

        if (port_status(hub, p, &st) < 0 || !(st & 1)) {
            continue;                   /* nothing connected */
        }
        control(hub, HUB_PORT, 3, 4 /* RESET */, (uint16_t)p, NULL, 0);
        for (uint64_t until = efi_uptime_ms() + 500; efi_uptime_ms() < until;) {
            sleep_ms(10);
            if (port_status(hub, p, &st) < 0 || !(st & (1u << 4))) {
                break;
            }
        }
        control(hub, HUB_PORT, 1 /* CLEAR_FEATURE */, 20 /* C_RESET */, (uint16_t)p, NULL, 0);
        control(hub, HUB_PORT, 1, 16 /* C_CONNECTION */, (uint16_t)p, NULL, 0);
        if (!(st & 1) || (!super && !(st & 2))) {
            continue;
        }
        sleep_ms(10);
        enumerate(hub->c, hub, (uint8_t)p,
                  super ? SUPER : (st & (1u << 9)) ? LOW : (st & (1u << 10)) ? HIGH : FULL);
    }
}

/* ---- a device -------------------------------------------------------------------------- */

static void enumerate(struct controller *c, struct device *parent, uint8_t port, uint8_t speed) {
    static const uint16_t first_size[] = { 8, 8, 8, 64, 512, 512, 512 };
    struct device *d;
    int slot;

    if (device_count == DEVICES || speed == 0 || speed > 6) {
        return;
    }
    d = &devices[device_count];
    memset(d, 0, sizeof *d);
    d->c = c;
    d->speed = speed;
    if (parent == NULL) {
        d->root = port;
    } else {
        if (parent->depth >= 5) {
            return;
        }
        d->root = parent->root;
        d->depth = (uint8_t)(parent->depth + 1);
        d->route = parent->route | (uint32_t)(port < 15 ? port : 15) << (4 * parent->depth);
        d->tt_slot = parent->tt_slot;
        d->tt_port = parent->tt_port;
        if (parent->speed == HIGH && speed <= LOW) {
            d->tt_slot = parent->slot;
            d->tt_port = port;
        }
    }
    if (command(c, 0, TRB_ENABLE_SLOT << 10) != CC_SUCCESS) {
        return;
    }
    slot = (int)(ring_at(c->page, COMMANDS)->control >> 24);
    if (slot <= 0 || (unsigned)slot > c->slots ||
        (d->page = mem_pages_below(1, 1ull << 32)) == 0) {
        return;
    }
    memset((void *)d->page, 0, 4096);
    d->slot = (uint8_t)slot;
    ((volatile uint64_t *)(c->page + DCBAA))[slot] = d->page;
    device_count++;

    /* Its address, with endpoint 0 at the size every device of its speed
       takes, then the size it says it has. */
    struct ring *r0 = ep_ring(d, 1);

    ring_init(r0);
    input_clear(c);
    input(c, 0)[1] = 3;
    slot_context(d, 1);
    endpoint_context(d, 1, 4, first_size[speed], 0, r0);
    if (command(c, c->page + INPUT, TRB_ADDRESS << 10 | (uint32_t)slot << 24) != CC_SUCCESS) {
        device_count--;
        mem_pages_free(d->page, 1);
        return;
    }
    sleep_ms(2);

    uint8_t desc[18];

    if (control(d, 0x80, 6, 0x0100, 0, desc, 8) < 0) {
        dbg("xhci: slot %u: no descriptor\n", (uint64_t)slot);
        return;
    }
    uint16_t size = speed >= SUPER ? (uint16_t)(1u << desc[7]) : desc[7];

    if (size != first_size[speed] && size >= 8) {
        input_clear(c);
        input(c, 0)[1] = 2;
        input(c, 2)[1] = (uint32_t)size << 16;
        command(c, c->page + INPUT, TRB_EVALUATE << 10 | (uint32_t)slot << 24);
    }

    /* The configuration, into a page borrowed while it is looked at. */
    uint64_t page = mem_pages_below(1, 1ull << 32);
    uint8_t head[9];

    if (page == 0 || control(d, 0x80, 6, 0x0200, 0, head, 9) < 0) {
        mem_pages_free(page, page != 0 ? 1 : 0);
        return;
    }
    unsigned total = head[2] | head[3] << 8;

    total = total < 512 ? total : 512;
    if (control(d, 0x80, 6, 0x0200, 0, (void *)page, (uint16_t)total) == 0 &&
        configure(d, (const uint8_t *)page, total)) {
        dbg("xhci: slot %u: class %x, speed %u\n", (uint64_t)slot,
            (uint64_t)d->pub.class, (uint64_t)speed);
        mem_pages_free(page, 1);
        if (d->pub.class == 9) {
            hub_setup(d);
        }
        return;
    }
    mem_pages_free(page, 1);
    d->pub.class = 0;                   /* nothing here drives it */
}

/* ---- a controller ---------------------------------------------------------------------- */

/* The BIOS's SMM code may be driving it for a USB keyboard on port 0x60;
   it is asked to stop, and its interrupts off. */
static void handoff(struct controller *c) {
    uint32_t hcc = rd(c->caps, 0x10);
    unsigned at = (hcc >> 16) << 2;

    while (at != 0) {
        uint32_t cap = rd(c->caps, at);

        if ((cap & 0xFF) == 1) {
            wr(c->caps, at, cap | 1u << 24);
            wait_bits(c->caps, at, 1u << 16, 0, 1000);
            wr(c->caps, at + 4, (rd(c->caps, at + 4) & ~0xE011u) | 0xE0000000u);
        }
        unsigned next = (cap >> 8 & 0xFF) << 2;

        at = next != 0 ? at + next : 0;
    }
}

/* The supported-protocol capabilities say which ports are USB 2's: those
   need a reset to be enabled, which USB 3's do by themselves. */
static void protocols(struct controller *c) {
    uint32_t hcc = rd(c->caps, 0x10);
    unsigned at = (hcc >> 16) << 2;

    while (at != 0) {
        uint32_t cap = rd(c->caps, at);

        if ((cap & 0xFF) == 2 && (cap >> 24) == 2) {
            uint32_t ports = rd(c->caps, at + 8);

            c->usb2_first = (uint8_t)ports;
            c->usb2_count = (uint8_t)(ports >> 8);
        }
        unsigned next = (cap >> 8 & 0xFF) << 2;

        at = next != 0 ? at + next : 0;
    }
}

static bool usb2_port(struct controller *c, unsigned p) {
    return p >= c->usb2_first && p < (unsigned)c->usb2_first + c->usb2_count;
}

static void root_ports(struct controller *c) {
    for (unsigned p = 1; p <= c->ports; p++) {
        uint32_t sc = rd(c->op, PORTSC(p));

        if (!(sc & PORT_CCS)) {
            continue;
        }
        if (usb2_port(c, p)) {
            wr(c->op, PORTSC(p), (sc & PORT_KEEP) | PORT_PR);
        }
        if (!wait_bits(c->op, PORTSC(p), PORT_PED, PORT_PED, 500)) {
            continue;
        }
        sc = rd(c->op, PORTSC(p));
        wr(c->op, PORTSC(p), (sc & PORT_KEEP) | PORT_CHANGES);
        sleep_ms(10);
        enumerate(c, NULL, (uint8_t)p, (uint8_t)PORT_SPEED(sc));
    }
}

static void stop_controller(struct controller *c) {
    wr(c->op, USBCMD, rd(c->op, USBCMD) & ~CMD_RUN);
    wait_bits(c->op, USBSTS, STS_HALT, STS_HALT, 100);
}

static bool start_controller(struct controller *c) {
    uint32_t params1 = rd(c->caps, 0x04), params2 = rd(c->caps, 0x08);

    c->op = c->caps + (rd(c->caps, 0) & 0xFF);
    c->run = c->caps + (rd(c->caps, 0x18) & ~0x1Fu);
    c->bells = (volatile uint32_t *)(c->caps + (rd(c->caps, 0x14) & ~0x3u));
    c->ports = params1 >> 24;
    c->slots = (params1 & 0xFF) < SLOTS ? (params1 & 0xFF) : SLOTS;
    c->csz = (rd(c->caps, 0x10) & 4) ? 64 : 32;
    c->pads = (params2 >> 21 & 0x1F) << 5 | params2 >> 27;
    if ((rd(c->op, 0x08) & 1) == 0) {
        return false;                   /* no 4 KiB pages */
    }
    handoff(c);
    protocols(c);
    stop_controller(c);
    wr(c->op, USBCMD, CMD_RESET);
    if (!wait_bits(c->op, USBCMD, CMD_RESET, 0, 1000) ||
        !wait_bits(c->op, USBSTS, STS_CNR, 0, 1000)) {
        return false;
    }
    if ((c->page = mem_pages_below(1, 1ull << 32)) == 0) {
        return false;
    }
    memset((void *)c->page, 0, 4096);
    if (c->pads != 0) {
        if ((c->pad_pages = mem_pages_below(1 + c->pads, 1ull << 32)) == 0) {
            return false;
        }
        memset((void *)c->pad_pages, 0, (1 + (size_t)c->pads) * 4096);
        for (unsigned i = 0; i < c->pads; i++) {
            ((uint64_t *)c->pad_pages)[i] = c->pad_pages + (1 + (uint64_t)i) * 4096;
        }
        ((uint64_t *)(c->page + DCBAA))[0] = c->pad_pages;
    }
    ring_init(ring_at(c->page, COMMANDS));
    c->event = 0;
    c->cycle = 1;

    wr(c->op, CONFIG, c->slots);
    wr64(c->op, DCBAAP, c->page + DCBAA);
    wr64(c->op, CRCR, (c->page + COMMANDS) | 1);
    ((volatile uint64_t *)(c->page + ERST))[0] = c->page + EVENTS;
    ((volatile uint64_t *)(c->page + ERST))[1] = RING;
    wr(c->run, 0x20 + IMAN, 0);
    wr(c->run, 0x20 + ERSTSZ, 1);
    wr64(c->run, 0x20 + ERDP, c->page + EVENTS);
    wr64(c->run, 0x20 + ERSTBA, c->page + ERST);
    wr(c->op, USBCMD, CMD_RUN);
    return wait_bits(c->op, USBSTS, STS_HALT, 0, 100);
}

static void free_controller(struct controller *c) {
    if (c->page != 0) {
        mem_pages_free(c->page, 1);
    }
    if (c->pad_pages != 0) {
        mem_pages_free(c->pad_pages, 1 + c->pads);
    }
    c->page = c->pad_pages = 0;
}

/* ---- the slot ---------------------------------------------------------------------------- */

static bool host_start(void) {
    if (started) {
        return true;
    }
    started = true;
    bool up[CONTROLLERS];

    for (unsigned i = 0; i < controller_count; i++) {
        efi_pci(controllers[i].pci, true);
        up[i] = start_controller(&controllers[i]);
    }
    /* Long enough for what is plugged in to say so - a USB 3 link takes
       its time to train after a reset - once for all of them. */
    sleep_ms(300);
    for (unsigned i = 0; i < controller_count; i++) {
        struct controller *c = &controllers[i];
        unsigned before = device_count;

        if (up[i]) {
            root_ports(c);
            dbg("xhci: %x: %u ports, %u devices\n", (uint64_t)c->pci, (uint64_t)c->ports,
                (uint64_t)(device_count - before));
            if (device_count > before) {
                continue;
            }
        }
        /* Nothing on it: back to the firmware, as it was. */
        stop_controller(c);
        free_controller(c);
        efi_pci(c->pci, false);
    }
    return true;
}

static const struct usb_device *host_device(unsigned i) {
    unsigned n = 0;

    for (unsigned k = 0; k < device_count; k++) {
        if (devices[k].pub.class != 0 && devices[k].pub.class != 9 && n++ == i) {
            return &devices[k].pub;
        }
    }
    return NULL;
}

static const struct usb_host host = { host_start, host_device, host_control, host_bulk,
                                      host_report };

MODULE_EXPORT int module_init(void) {
    for (uint32_t at = pci_next_class(0x0C, 0x03, 0); at != 0 && controller_count < CONTROLLERS;
         at = pci_next_class(0x0C, 0x03, at)) {
        uint64_t bar;

        if ((pci_read(at, 0x08) >> 8 & 0xFF) != 0x30 || (bar = pci_memory(at, 0)) == 0) {
            continue;                   /* UHCI, OHCI or EHCI: older, not driven */
        }
        controllers[controller_count].pci = at;
        controllers[controller_count++].caps = (volatile uint8_t *)bar;
    }
    if (controller_count == 0) {
        return -ENODEV;
    }
    usb_host_register(&host);
    return 0;
}

/* Once started, its devices are the class drivers' and nobody else can
   give them back. */
MODULE_EXPORT int module_exit(void) {
    if (started) {
        return -EBUSY;
    }
    usb_host_register(NULL);
    return 0;
}
