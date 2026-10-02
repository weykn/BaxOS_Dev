#include "thread.h"

#include <stddef.h>

#include "mem.h"
#include "string.h"

#define MSR_FS_BASE   0xC0000100
#define THREADS       16            /* at once, over every process */
#define LEVELS        16            /* programs inside programs */
#define STACK_PAGES   4             /* a thread's kernel stack, with the
                                       thread itself at the bottom of it */

struct thread {
    uint64_t ksp;                   /* its kernel stack, while it is not running */
    uint8_t *bottom;                /* the lowest byte of that stack */
    unsigned level;                 /* the process: how deep it is */
    int      tid;
    bool     dead;                  /* ended; its stack goes at the next switch */
    bool     woken;
    bool     exiting;               /* the process is ending: the leader does it */
    int      exit_code;
    uint64_t futex;                 /* where it waits, 0 if nowhere */
    uint64_t clear_tid;
    /* What belongs to whoever is running, put aside while it is not. */
    uint64_t fs_base, kernel_rsp, args[6];
    struct user_regs *frame;
    struct user_regs regs;          /* where a new one starts */
};

extern uint8_t tss[];
extern struct user_regs *user_frame;
extern char stack_bottom[];
extern int user_resume(const struct user_regs *regs, uint64_t rax);
extern __attribute__((noreturn)) void user_exit(int code);
extern void thread_switch(uint64_t *save, uint64_t to);
uint64_t *syscall_args(void);

static struct thread first = { .bottom = (uint8_t *)stack_bottom, .tid = 1 };
static struct thread *all[THREADS] = { &first };
static struct thread *current = &first;
static struct thread *leaders[LEVELS] = { &first };
static unsigned level;
static int next_tid = 1000;

static uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;

    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (uint64_t)high << 32 | low;
}

static void wrmsr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

/* The TSS's RSP0: where a syscall or an exception from ring 3 lands. */
static uint64_t *kernel_rsp(void) {
    return (uint64_t *)(tss + 4);
}

/* Gives back the stacks of the threads that have ended, other than this. */
static void reap(void) {
    for (unsigned i = 1; i < THREADS; i++) {
        if (all[i] != NULL && all[i]->dead && all[i] != current) {
            mem_pages_free((uint64_t)all[i], STACK_PAGES);   /* it is at the base */
            all[i] = NULL;
        }
    }
}

static void switch_to(struct thread *next) {
    struct thread *prev = current;

    prev->fs_base = rdmsr(MSR_FS_BASE);
    prev->kernel_rsp = *kernel_rsp();
    prev->frame = user_frame;
    memcpy(prev->args, syscall_args(), sizeof prev->args);
    current = next;
    wrmsr(MSR_FS_BASE, next->fs_base);
    *kernel_rsp() = next->kernel_rsp;
    user_frame = next->frame;
    memcpy(syscall_args(), next->args, sizeof next->args);
    thread_switch(&prev->ksp, next->ksp);
    reap();
}

/* The next of the process's threads after this one, or NULL. */
static struct thread *next_one(void) {
    unsigned at = 0;

    while (all[at] != current) {
        at++;
    }
    for (unsigned n = 1; n < THREADS; n++) {
        struct thread *t = all[(at + n) % THREADS];

        if (t != NULL && !t->dead && t->level == level) {
            return t;
        }
    }
    return NULL;
}

void thread_yield(void) {
    struct thread *next = next_one();

    if (next != NULL) {
        switch_to(next);
    }
    if (current->exiting) {
        current->exiting = false;
        process_exit(current->exit_code);
    }
}

bool thread_alone(void) {
    return next_one() == NULL;
}

int thread_id(void) {
    return current == leaders[level] ? 1 : current->tid;
}

uint64_t thread_stack_left(const void *here) {
    return (uint64_t)here - (uint64_t)current->bottom;
}

/* Where a new thread's kernel stack first returns to. */
static void thread_start(void) {
    struct thread *t = current;

    reap();
    user_resume(&t->regs, 0);       /* back here once it calls exit */
    if (t->clear_tid != 0) {
        *(volatile int32_t *)t->clear_tid = 0;
        thread_wake(t->clear_tid, 1, 0, 0);
    }
    t->dead = true;
    switch_to(next_one() != NULL ? next_one() : leaders[level]);
    for (;;) {
    }
}

int thread_create(const struct user_regs *regs, uint64_t rsp, uint64_t fs, uint64_t clear_tid) {
    unsigned slot = 1;
    uint64_t base;

    reap();
    while (slot < THREADS && all[slot] != NULL) {
        slot++;
    }
    if (slot == THREADS || (base = mem_pages(STACK_PAGES)) == 0) {
        return -11;                 /* EAGAIN */
    }
    struct thread *t = (struct thread *)base;
    uint64_t *sp = (uint64_t *)(base + STACK_PAGES * 4096);

    *t = (struct thread){
        .bottom = (uint8_t *)(t + 1), .level = level, .tid = next_tid++,
        .clear_tid = clear_tid, .fs_base = fs, .kernel_rsp = *kernel_rsp(),
        .regs = *regs,
    };
    t->regs.rsp = rsp;
    *--sp = 0;                      /* thread_start's return address: none */
    *--sp = (uint64_t)thread_start; /* where thread_switch's ret goes */
    sp -= 6;                        /* the six registers it pops */
    memset(sp, 0, 6 * 8);
    t->ksp = (uint64_t)sp;
    all[slot] = t;
    return t->tid;
}

/* Every thread of the process but this one ends where it waits. */
static void end_others(void) {
    for (unsigned i = 1; i < THREADS; i++) {
        if (all[i] != NULL && all[i] != current && all[i]->level == level) {
            all[i]->dead = true;
        }
    }
    reap();
}

void thread_enter(void) {
    leaders[++level] = current;
    current->level = level;
}

void thread_leave(void) {
    end_others();
    current->level = --level;
}

void process_exit(int code) {
    struct thread *leader = leaders[level];

    if (current != leader) {
        /* The leader is waiting somewhere, in the kernel: it ends the
           process from there, on its own stack. */
        leader->exiting = true;
        leader->exit_code = code;
        current->dead = true;
        switch_to(leader);
    }
    end_others();
    user_exit(code);
}

void thread_exit(int code) {
    if (current == leaders[level]) {
        while (!thread_alone()) {   /* the process lasts as long as they do */
            thread_yield();
        }
    }
    user_exit(code);                /* a thread's ends in thread_start */
}

void thread_sleep_on(uint64_t addr) {
    current->futex = addr;
    current->woken = false;
}

bool thread_woken(void) {
    return current->woken;
}

int thread_wake(uint64_t addr, int n, uint64_t addr2, int move) {
    int woke = 0;

    for (unsigned i = 0; i < THREADS; i++) {
        struct thread *t = all[i];

        if (t == NULL || t->dead || t->level != level || t->futex != addr || addr == 0) {
            continue;
        }
        if (woke < n) {
            t->futex = 0;
            t->woken = true;
            woke++;
        } else if (move > 0) {
            t->futex = addr2;
            move--;
        }
    }
    return woke;
}
