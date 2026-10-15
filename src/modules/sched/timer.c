/* sched/timer: the local APIC's timer, every 10 ms, interrupting programs -
 * never the kernel, which keeps interrupts off. The kernel's tick
 * (syscall.c) does with it what a syscall would: Ctrl-C is looked for, a
 * signal is acted on, and the processor goes round, so that a program
 * that never makes a syscall can still be stopped, and the others still
 * run beside it.
 *
 * Only once the firmware is gone: until then the timer may be its own. The
 * I/O APIC's lines are masked as it starts, since nothing here takes a
 * device's interrupt and the firmware may have left some wired. */

#include "efi_kernel.h"
#include "mem.h"
#include "module.h"
#include "syscall.h"

#define ENODEV 19

#define MSR_APIC_BASE 0x1B
#define APIC_ENABLED  (1u << 11)
#define APIC_X2       (1u << 10)

#define EOI     0x0B0
#define SVR     0x0F0
#define LVT_CMCI 0x2F0
#define TIMER   0x320
#define THERMAL 0x330
#define PERF    0x340
#define LINT0   0x350
#define ERROR   0x370
#define INITIAL 0x380
#define CURRENT 0x390
#define DIVIDE  0x3E0

#define MASKED   (1u << 16)
#define PERIODIC (1u << 17)
#define VECTOR   0xF0
#define SPURIOUS 0xFF
#define PERIOD_US 10000

#define IOAPIC 0xFEC00000ull        /* where every PC has its first */

static volatile uint32_t *mmio;
static bool x2;

static uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;

    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (uint64_t)hi << 32 | lo;
}

static void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static uint32_t rd(unsigned r) {
    return x2 ? (uint32_t)rdmsr(0x800 + (r >> 4)) : mmio[r / 4];
}

static void wr(unsigned r, uint32_t v) {
    if (x2) {
        wrmsr(0x800 + (r >> 4), v);
    } else {
        mmio[r / 4] = v;
    }
}

static void ack(void) {
    wr(EOI, 0);
}

/* Every line of the I/O APIC, if it is where it should be and says so. */
static void ioapic_mask(void) {
    volatile uint32_t *io = (volatile uint32_t *)IOAPIC;

    mem_map_io(IOAPIC);
    io[0] = 1;
    uint32_t version = io[4];
    unsigned last = version >> 16 & 0xFF;

    if ((version & 0xFF) < 0x10 || (version & 0xFF) > 0x2F || last < 15 || last > 119) {
        return;
    }
    for (unsigned i = 0; i <= last; i++) {
        io[0] = 0x10 + 2 * i;
        io[4] |= MASKED;
    }
}

MODULE_EXPORT int module_init(void) {
    uint64_t base = rdmsr(MSR_APIC_BASE);

    if (!efi_gone() || (base & APIC_ENABLED) == 0) {
        return -ENODEV;
    }
    x2 = (base & APIC_X2) != 0;
    if (!x2) {
        mem_map_io(base & ~0xFFFull);
        mmio = (volatile uint32_t *)(base & 0xFFFFF000ull);
    }
    ioapic_mask();
    wr(LINT0, MASKED);
    wr(ERROR, MASKED);
    wr(THERMAL, MASKED);
    wr(PERF, MASKED);
    if ((rd(0x30) >> 16 & 0xFF) >= 6) {
        wr(LVT_CMCI, MASKED);       /* only where there is one */
    }
    wr(SVR, rd(SVR) | 0x100 | SPURIOUS);

    /* Its rate, against the processor's counter. */
    wr(DIVIDE, 3);                  /* by 16 */
    wr(TIMER, MASKED | VECTOR);
    wr(INITIAL, 0xFFFFFFFFu);
    uint64_t until = efi_uptime_us() + PERIOD_US;

    while (efi_uptime_us() < until) {
    }
    uint32_t count = 0xFFFFFFFFu - rd(CURRENT);

    wr(INITIAL, 0);
    if (count == 0 || !tick_register(VECTOR, ack)) {
        return -ENODEV;
    }
    wr(TIMER, PERIODIC | VECTOR);
    wr(INITIAL, count);
    return 0;
}

MODULE_EXPORT int module_exit(void) {
    wr(TIMER, MASKED | VECTOR);
    wr(INITIAL, 0);
    tick_register(0, NULL);
    return 0;
}
