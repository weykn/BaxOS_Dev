#include "thread.h"
#include "ia32.h"

#include <stddef.h>

#include "mem.h"
#include "string.h"
#include "vga.h"

#define THREADS       512           /* at once, over every process: a browser has hundreds */
#define STACK_PAGES   (THREAD_STACK_BYTES / 4096)  /* a thread's kernel stack, with the
                                       thread itself at the bottom of it */

struct thread {
    uint64_t ksp;                   /* its kernel stack, while it is not running */
    uint8_t *bottom;                /* the lowest byte of that stack */
    void    *proc;                  /* its process (syscall.c's), NULL for the kernel */
    bool     leader;                /* the one whose end is the process's */
    int      tid;
    unsigned slot;                  /* its place in all[] */
    bool     dead;                  /* ended; its stack goes at the next switch */
    bool     woken;
    bool     exiting;               /* the process is ending: the leader does it */
    int      exit_code;
    uint64_t futex;                 /* where it waits, 0 if nowhere */
    uint64_t wake_us;               /* when that wait times out, 0 if never */
    bool     kicked;                /* a signal came for its process meanwhile */
    uint64_t sigmask;               /* the signals it has blocked: each thread its own */
    uint64_t restore[3];            /* rt_sigreturn's RCX and R11 to put back, if [0] */
    uint64_t clear_tid;
    /* What belongs to whoever is running, put aside while it is not. */
    uint64_t fs_base, gs_base, kernel_rsp, args[6];
    struct user_regs *frame;
    struct user_regs regs;          /* where a new one starts */
    /* Its x87 and SSE registers, while another thread has the processor: a
       program's memcpy keeps things in them across a syscall, as it may. */
    uint8_t  fpu[512] __attribute__((aligned(16)));
};

#ifdef __i386__
#define FXSAVE  "fxsave %0"
#define FXRSTOR "fxrstor %0"
#else
#define FXSAVE  "fxsave64 %0"
#define FXRSTOR "fxrstor64 %0"
#endif

static void fpu_save(struct thread *t) {
    __asm__ volatile(FXSAVE : "=m"(t->fpu));
}

static void fpu_load(const struct thread *t) {
    __asm__ volatile(FXRSTOR : : "m"(t->fpu));
}

extern uint8_t tss[];
extern struct user_regs *user_frame;
extern char stack_bottom[];
extern int user_resume(const struct user_regs *regs, uint64_t rax);
extern __attribute__((noreturn)) void user_exit(int code);
extern void thread_switch(uint64_t *save, uint64_t to);
uint64_t *syscall_args(void);

/* syscall.c's side of a process: what changes hands between two, and what
   is left of one whose last thread has ended. */
void process_switch(void *from, void *to);
void process_finished(int code);
int  process_pid(void *proc);

/* What thread_switch keeps on a stack: the registers C keeps across a
   call - RBX, RBP and R12-R15, or on i386 EBX, ESI, EDI and EBP. */
#ifdef __i386__
#define SWITCH_SAVES 4
#else
#define SWITCH_SAVES 6
#endif

static struct thread first = { .bottom = (uint8_t *)stack_bottom, .tid = 1 };
static struct thread *all[THREADS] = { &first };
static struct thread *current = &first;
static int next_tid = 2;            /* pids and tids alike: one counter, no clash */

/* The TSS's stack pointer for ring 0 - RSP0, or ESP0 on i386, beside
   which is SS0: a word of the processor's width. */
static uintptr_t *kernel_rsp(void) {
    return (uintptr_t *)(tss + 4);
}

/* Threads ended whose stacks are not given back yet. reap runs at every
   switch, and going through every thread to find none was most of a
   switch: each one's record is on a page of its own. */
static unsigned dead_count;

static void mark_dead(struct thread *t) {
    if (!t->dead) {
        t->dead = true;
        dead_count++;
    }
}

/* The most of a thread's kernel stack ever in use: each starts zeroed, so
   its lowest byte that is not still zero marks how deep it went. */
static uint32_t deepest;

static uint32_t depth(const struct thread *t) {
    const uint8_t *at = (const uint8_t *)(t + 1) + 8, *top = (const uint8_t *)t + THREAD_STACK_BYTES;

    while (at < top && *at == 0) {
        at++;
    }
    return (uint32_t)(top - at);
}

uint32_t thread_stack_peak(void) {
    uint32_t most = deepest;

    for (unsigned i = 1; i < THREADS; i++) {
        if (all[i] != NULL && depth(all[i]) > most) {
            most = depth(all[i]);
        }
    }
    return most;
}

/* Gives back the stacks of the threads that have ended, other than this. */
static void reap(void) {
    for (unsigned i = 1; dead_count > 0 && i < THREADS; i++) {
        if (all[i] != NULL && all[i]->dead && all[i] != current) {
            deepest = depth(all[i]) > deepest ? depth(all[i]) : deepest;
            mem_pages_free((uint64_t)(uintptr_t)all[i], STACK_PAGES);   /* it is at the base */
            all[i] = NULL;
            dead_count--;
        }
    }
}

/* The word just above a thread's record, which its stack reaches only by
   running out: checked at every switch, so that an overflow stops the
   machine saying so rather than quietly wrecking the record. Two pages of
   stack, where the deepest seen - a browser, an office suite, tar, modman -
   is about four kilobytes. */
#define CANARY 0x6b636174536b7854ull

static void switch_to(struct thread *next) {
    struct thread *prev = current;

    if (prev != &first && *(const uint64_t *)(prev + 1) != CANARY) {
        kprintf("\nkernel stack overflow: thread %u\n", (unsigned)prev->tid);
        for (;;) {
            __asm__ volatile("cli; hlt");
        }
    }

    prev->fs_base = tls_fs();
    prev->gs_base = tls_gs();
    prev->kernel_rsp = *kernel_rsp();
    prev->frame = user_frame;
    memcpy(prev->args, syscall_args(), sizeof prev->args);
    if (prev->proc != next->proc) {
        process_switch(prev->proc, next->proc);
    }
    current = next;
    tls_fs_set(next->fs_base);
    tls_gs_set(next->gs_base);      /* and what a 32-bit thread loading GS gets */
    *kernel_rsp() = (uintptr_t)next->kernel_rsp;
    user_frame = next->frame;
    memcpy(syscall_args(), next->args, sizeof next->args);
    fpu_save(prev);
    thread_switch(&prev->ksp, next->ksp);
    fpu_load(current);              /* back: this thread's own again */
    reap();
}

/* The next thread after this one, of any process - or, with mine, of this
   one only - or NULL. Every thread waiting calls thread_yield in its wait, so
   going round all of them is what lets every process get on. */
uint64_t efi_uptime_us(void);

#define WAITING 1                   /* thread_wait's: no futex is at an odd address */

/* Whether t waits at a futex that nothing has woken, with no time-out due
   and no signal come: switching to it would only have it look and pass the
   processor on, and a browser has a hundred of those. */
static bool asleep(const struct thread *t, uint64_t now) {
    return t->futex != 0 && !t->woken && !t->kicked && !t->exiting &&
           (t->wake_us == 0 || now < t->wake_us);
}

/* The next thread to run after this one, round the table: of this process
   only for `mine`, and asleep or not for `any` - which asks whether there is
   another at all, one that could yet do what this one waits for. */
static struct thread *next_one(bool mine, bool any) {
    unsigned at = current->slot;
    uint64_t now = any ? 0 : efi_uptime_us();

    for (unsigned n = 1; n < THREADS; n++) {
        struct thread *t = all[(at + n) % THREADS];

        if (t != NULL && !t->dead && (!mine || t->proc == current->proc) &&
            (any || !asleep(t, now))) {
            return t;
        }
    }
    return NULL;
}

/* The thread that ends proc's life, or NULL. */
static struct thread *leader_of(void *proc) {
    for (unsigned i = 0; i < THREADS; i++) {
        if (all[i] != NULL && !all[i]->dead && all[i]->leader && all[i]->proc == proc) {
            return all[i];
        }
    }
    return NULL;
}

/* Where a thread that has ended goes: one awake if there is one, else any -
   there is always one, the kernel's own. */
static struct thread *next_after_end(void) {
    struct thread *t = next_one(false, false);

    return t != NULL ? t : next_one(false, true);
}

void timers_tick(void);            /* syscall.c: the timers that are due, sent */
void process_yielded(uint64_t us); /* syscall.c: time others had, not the caller's */

void thread_yield(void) {
    timers_tick();
    struct thread *next = next_one(false, false);

    if (next != NULL) {
        uint64_t began = efi_uptime_us();

        switch_to(next);
        process_yielded(efi_uptime_us() - began);
    }
    if (current->futex == WAITING) {
        current->futex = 0;         /* thread_wait's sleep is one round long */
    }
    if (current->exiting) {
        current->exiting = false;
        process_exit(current->exit_code);
    }
}

uint64_t *thread_restore(void) {
    return current->restore;
}

uint64_t *thread_sigmask(void) {
    return &current->sigmask;
}

bool thread_alone(void) {
    return next_one(true, true) == NULL;
}

bool thread_only(void) {
    return next_one(false, true) == NULL;
}

int thread_id(void) {
    return current->leader && current->proc != NULL ? process_pid(current->proc) : current->tid;
}

int thread_new_id(void) {
    return next_tid++;
}

/* Where a new thread's kernel stack first returns to. */
static void thread_start(void) {
    struct thread *t = current;

    fpu_load(t);
    reap();
    user_resume(&t->regs, 0);       /* back here once it calls exit */
    if (t->clear_tid != 0) {
        *(volatile int32_t *)t->clear_tid = 0;
        thread_wake(t->clear_tid, 1, 0, 0);
    }
    mark_dead(t);
    switch_to(next_after_end());
    for (;;) {
    }
}

/* Where a forked process's first thread starts, and what it does once the
   process has ended. */
static void process_entry(void) {
    struct thread *t = current;

    fpu_load(t);
    reap();
    process_finished(user_resume(&t->regs, 0));
    mark_dead(t);
    switch_to(next_after_end());
    for (;;) {
    }
}

static struct thread *thread_new(void *proc, bool leader, const struct user_regs *regs,
                                 uint64_t rsp, uint64_t fs, uint64_t sigmask, void (*entry)(void)) {
    unsigned slot = 1;
    uint64_t base;

    reap();
    while (slot < THREADS && all[slot] != NULL) {
        slot++;
    }
    if (slot == THREADS || (base = mem_pages(STACK_PAGES)) == 0) {
        return NULL;
    }
    struct thread *t = (struct thread *)base;
    uintptr_t *sp;

    memset(t, 0, THREAD_STACK_BYTES);
    *(uint64_t *)(t + 1) = CANARY;

    *t = (struct thread){
        .bottom = (uint8_t *)(t + 1), .proc = proc, .leader = leader, .tid = next_tid++,
        .slot = slot, .sigmask = sigmask, .fs_base = fs, .gs_base = tls_gs(),
        /* Its syscalls land at the top of its own stack. */
        .kernel_rsp = base + STACK_PAGES * 4096 - 64,
        .regs = *regs,
    };
    t->regs.rsp = rsp;
    fpu_save(t);                    /* it starts with its creator's, as on Linux */
    sp = (uintptr_t *)(base + STACK_PAGES * 4096 - 64);
    *--sp = 0;                      /* the entry's return address: none */
    *--sp = (uintptr_t)entry;       /* where thread_switch's ret goes */
    sp -= SWITCH_SAVES;             /* the registers it pops */
    memset(sp, 0, SWITCH_SAVES * sizeof *sp);
    t->ksp = (uint64_t)(uintptr_t)sp;
    all[slot] = t;
    return t;
}

int thread_create(const struct user_regs *regs, uint64_t rsp, uint64_t fs, uint64_t clear_tid) {
    struct thread *t = thread_new(current->proc, false, regs, rsp, fs, current->sigmask,
                                  thread_start);

    if (t == NULL) {
        return -11;                 /* EAGAIN */
    }
    t->clear_tid = clear_tid;
    return t->tid;
}

bool thread_spawn(void *proc, const struct user_regs *regs, uint64_t fs, uint64_t sigmask) {
    return thread_new(proc, true, regs, regs->rsp, fs, sigmask, process_entry) != NULL;
}

/* Every thread of the process but this one ends where it waits. */
void thread_end_others(void) {
    for (unsigned i = 1; i < THREADS; i++) {
        if (all[i] != NULL && all[i] != current && all[i]->proc == current->proc) {
            mark_dead(all[i]);
        }
    }
    reap();
}

void *thread_adopt(void *proc) {
    void *was = current->proc;

    current->proc = proc;
    current->leader = true;
    return was;
}

void thread_disown(void *was) {
    thread_end_others();
    current->leader = false;
    current->proc = was;
}

unsigned thread_count(void *proc) {
    unsigned n = 0;

    for (unsigned i = 0; i < THREADS; i++) {
        n += all[i] != NULL && !all[i]->dead && all[i]->proc == proc;
    }
    return n;
}

size_t thread_stacks(void) {
    size_t n = 0;

    for (unsigned i = 0; i < THREADS; i++) {
        n += all[i] != NULL && all[i] != &first;
    }
    return n * THREAD_STACK_BYTES;
}

void *thread_process(void) {
    return current->proc;
}

void process_exit(int code) {
    struct thread *leader = leader_of(current->proc);

    if (leader != NULL && current != leader) {
        /* The leader is waiting somewhere, in the kernel: it ends the
           process from there, on its own stack. */
        leader->exiting = true;
        leader->exit_code = code;
        mark_dead(current);
        switch_to(leader);
    }
    thread_end_others();
    user_exit(code);
}

void thread_exit(int code) {
    if (current->leader) {
        while (!thread_alone()) {   /* the process lasts as long as they do */
            thread_yield();
        }
    }
    user_exit(code);                /* a thread's ends in thread_start */
}

/* A wait for anything at all - a pipe, a socket, a child, a key - sleeps
   rather than has every waiter switched into round after round only to look
   and pass the processor on, each one a change of address space: what a
   browser's dozen processes cost while it had work to do. Whatever might
   end a wait calls thread_stir, which wakes every one of them to look again;
   one it missed looks anyway once WAIT_US is up. */
#define WAIT_US 10000

void thread_wait(uint64_t until_us) {
    uint64_t latest = efi_uptime_us() + WAIT_US;

    thread_sleep_on(WAITING, until_us != 0 && until_us < latest ? until_us : latest);
    thread_yield();
}

void thread_stir(void) {
    for (unsigned i = 0; i < THREADS; i++) {
        if (all[i] != NULL && all[i]->futex == WAITING) {
            all[i]->futex = 0;
        }
    }
}

void thread_sleep_on(uint64_t addr, uint64_t until_us) {
    current->futex = addr;
    current->wake_us = until_us;
    current->woken = current->kicked = false;
}

void thread_kick(void *proc) {
    for (unsigned i = 0; i < THREADS; i++) {
        if (all[i] != NULL && all[i]->proc == proc) {
            all[i]->kicked = true;
        }
    }
}

bool thread_woken(void) {
    return current->woken;
}

int thread_wake(uint64_t addr, int n, uint64_t addr2, int move) {
    int woke = 0;

    for (unsigned i = 0; i < THREADS; i++) {
        struct thread *t = all[i];

        if (t == NULL || t->dead || t->proc != current->proc || t->futex != addr || addr == 0) {
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
