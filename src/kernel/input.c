#include "input.h"

#include <stddef.h>

#include "console.h"
#include "efi_kernel.h"
#include "linux.h"
#include "mem.h"
#include "string.h"

#define EVENTS 128                  /* waiting at once; past that the oldest go */

/* struct input_event, as a 64-bit Linux lays it out. */
struct event {
    uint64_t sec, usec;
    uint16_t type, code;
    int32_t  value;
};

static struct queue {
    struct event *events;           /* NULL until something opens it */
    unsigned head, tail;
    bool     monotonic;             /* times from boot, not from 1970 */
    uint8_t  down[INPUT_KEY_BYTES];
} queues[INPUTS];

void input_report(unsigned device, unsigned type, unsigned code, int value) {
    struct queue *q = &queues[device];

    if (type == EV_KEY && code < INPUT_KEY_BYTES * 8) {
        uint8_t bit = (uint8_t)(1 << (code & 7));

        if (value != 0 && (q->down[code >> 3] & bit) != 0) {
            value = 2;
        }
        q->down[code >> 3] = value != 0 ? q->down[code >> 3] | bit : q->down[code >> 3] & ~bit;
    }
    if (q->events == NULL) {
        return;
    }
    uint64_t us = efi_uptime_us();  /* from boot; made 1970's as it is read */

    if (q->tail - q->head == EVENTS) {
        q->head++;
    }
    q->events[q->tail++ % EVENTS] = (struct event){
        .sec = us / 1000000, .usec = us % 1000000,
        .type = (uint16_t)type, .code = (uint16_t)code, .value = value };
}

bool input_open(unsigned device) {
    struct queue *q = &queues[device];

    if (q->events == NULL && (q->events = mem_alloc(EVENTS * sizeof *q->events)) == NULL) {
        return false;
    }
    return true;
}

bool input_ready(unsigned device) {
    (void)console_ready();          /* the drivers read the hardware */
    return queues[device].head != queues[device].tail;
}

uint64_t input_read(unsigned device, void *buf, uint64_t count) {
    struct queue *q = &queues[device];
    uint64_t n = 0;

    if (!input_ready(device)) {
        return 0;
    }
    uint64_t shift = q->monotonic ? 0 : realtime_us() - efi_uptime_us();

    while (q->head != q->tail && count - n >= sizeof(struct event)) {
        struct event e = q->events[q->head++ % EVENTS];
        uint64_t us = e.sec * 1000000 + e.usec + shift;

        e.sec = us / 1000000;
        e.usec = us % 1000000;
        memcpy((char *)buf + n, &e, sizeof e);
        n += sizeof e;
    }
    return n;
}

void input_clock(unsigned device, bool monotonic) {
    queues[device].monotonic = monotonic;
}

const uint8_t *input_keys(unsigned device) {
    return queues[device].down;
}

void input_release(void) {
    for (unsigned i = 0; i < INPUTS; i++) {
        mem_free(queues[i].events);
        queues[i].events = NULL;
        queues[i].head = queues[i].tail = 0;
        queues[i].monotonic = false;
    }
}
