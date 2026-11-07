#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "thread.h"

/* 32-bit programs: what syscall.c shares with compat/ia32.
 *
 * A 32-bit program - an i386 ELF, or a TEF of class 1 - runs in the
 * processor's compatibility mode, with a code segment of its own, and all
 * its memory inside the first gigabyte: the low memory vm.c gives a
 * program linked to a fixed address, where the 32-bit addresses it can
 * name are. It calls the kernel with int 0x80, i386's numbers and i386's
 * structures; compat/ia32 turns each call into the 64-bit one and runs
 * that (syscall_run), converting what is passed both ways. The kernel
 * loads the module with the first 32-bit program and drops it when the
 * last is gone. */

#define IA32_MODULE "compat/ia32"

/* Where its memory goes, all of it in [IA32_LOW, IA32_STACK). Clear of
   the bottom 128 MiB, where a small machine has the kernel's own pages. */
#define IA32_LOW    0x00200000      /* the least vm_low gives */
#define IA32_EXEC   0x08000000      /* a position-independent program */
#define IA32_INTERP 0x0F000000      /* its loader */
#define IA32_MMAP   0x10000000      /* mmap, growing up to the heap */
#define IA32_BRK    0x30000000      /* the heap */
#define IA32_HIGH   0x3C000000      /* where the heap stops */
#define IA32_STACK  0x40000000      /* the stack top, growing down */

/* A signal for a 32-bit handler: what the module needs to build i386's
   frame for it, and the registers - the ones it interrupted going in, the
   handler's coming out. */
struct ia32_signal {
    uint32_t sig, code, addr, pid, status, trapno, err;
    uint32_t handler, restorer, flags;
    uint64_t mask;                  /* blocked until now: back on its return */
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp, eip, eflags;
};

struct ia32_ops {
    /* The call in f, the int 0x80 frame (ECX in f->pad), answered. */
    uint64_t (*syscall)(struct user_regs *f);
    /* The frame built on the program's stack, and s's registers made the
       handler's; false if there is no room for it there. */
    bool (*signal)(struct ia32_signal *s);
};

void ia32_register(const struct ia32_ops *ops);

/* A 64-bit call run as syscall_entry would run it, minus what is done
   once on the way out: the module's way into every handler. */
uint64_t syscall_run(uint64_t number, uint64_t a, uint64_t b, uint64_t c,
                     uint64_t d, uint64_t e, uint64_t f);

/* The GDT entry a 32-bit program's thread pointer lives in, made to start
   at base: what set_thread_area answers, and the program loads into GS. */
unsigned tls_descriptor(uint64_t base);

/* The running thread's thread pointers: the bases of FS, which a 64-bit
   program has its TLS through, and GS, a 32-bit program's. On x86-64 they
   are the processor's FS_BASE and GS_BASE; on i386, which has no 64-bit
   programs, GS's is the GDT entry above and FS's is always 0. */
uint64_t tls_fs(void);
uint64_t tls_gs(void);
void     tls_fs_set(uint64_t base);
void     tls_gs_set(uint64_t base);
