#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Threads: what clone(CLONE_THREAD) makes, each with a kernel stack of its
 * own, sharing its process's memory and descriptors - and the first thread
 * of every process, which is what a fork makes.
 *
 * Nothing interrupts the kernel, so another thread runs when this one waits
 * in it: every wait (futex, poll, select, a socket, sleeping, a key, a pipe,
 * a child) calls thread_yield, which passes the processor round every thread
 * there is, of every process. A thread spinning in its own code without a
 * syscall keeps it - unless sched/timer is loaded, whose tick yields for it.
 *
 * A process's first thread is its leader; its exit ends the rest. */

/* The registers of a program at a syscall, as syscall_entry left them on the
   kernel stack: fork starts a child from them, and a thread starts from a
   copy with its own stack. */
struct user_regs {
    uint64_t rax, pad, r15, r14, r13, r12, rbp, rbx;
    uint64_t r10, r9, r8, rdx, rsi, rdi;
    uint64_t rflags, rip, rsp;
};

/* A new thread of this process, starting from regs on stack rsp with FS at
   fs, which runs from the next wait on. clear_tid, if not 0, is zeroed and
   woken when it ends. Returns its id, or a negative errno. */
int thread_create(const struct user_regs *regs, uint64_t rsp, uint64_t fs, uint64_t clear_tid);

/* Lets every other thread run - of this process and of every other one -
   if there are any. */
void thread_yield(void);

/* Lets the others run, sleeping until thread_stir, until_us (0: none) or
   a short while - for a wait loop, which looks again either way. */
void thread_wait(uint64_t until_us);

/* Something a wait may be for has happened: every thread_wait looks. */
void thread_stir(void);

/* The running thread's blocked signals, as rt_sigprocmask has them. */
uint64_t *thread_sigmask(void);

/* Registers rt_sigreturn puts back that a syscall's return would not -
   RCX and R11, when [0] is set - kept with the thread until it does. */
uint64_t *thread_restore(void);

/* Whether this is the process's only thread, and whether it is the only
   thread there is at all - nothing else that could run while it waits. */
bool thread_alone(void);
bool thread_only(void);

/* This thread's id: the process's for its leader. */
int thread_id(void);

/* A fresh id, for a process: pids and tids are one series, as on Linux. */
int thread_new_id(void);

/* The most of a thread's kernel stack any has used, in bytes. */
uint32_t thread_stack_peak(void);

/* Threads alive in proc, and what each one's kernel stack costs. */
unsigned thread_count(void *proc);
size_t   thread_stacks(void);        /* every thread's but the kernel's own, in bytes */
#define THREAD_STACK_BYTES (2 * 4096)

/* A process, as the threads know it: syscall.c's, and opaque here. The
   running thread's, NULL for the kernel's own. */
void *thread_process(void);

/* The first thread of a forked process: it starts from regs, with FS at fs
   and those signals blocked, the next time anything waits. False if there is
   no room for it. */
bool thread_spawn(void *proc, const struct user_regs *regs, uint64_t fs, uint64_t sigmask);

/* This thread running a process the kernel starts, and back to whatever it
   was running before - its other threads ended. */
void *thread_adopt(void *proc);
void thread_disown(void *was);

/* Every other thread of this process ends: what execve does. */
void thread_end_others(void);

/* Ends this thread (exit), or the whole process (exit_group, a fault,
   Ctrl-C). */
__attribute__((noreturn)) void thread_exit(int code);
__attribute__((noreturn)) void process_exit(int code);

/* Futexes. thread_sleep_on marks this thread waiting at addr; thread_woken
   says whether a wake has reached it since. thread_wake wakes up to n
   waiting at addr, moving up to move more of them on to addr2, and returns
   how many it woke. */
void thread_sleep_on(uint64_t addr, uint64_t until_us);
/* A signal for proc: its threads asleep at a futex get to look. */
void thread_kick(void *proc);
bool thread_woken(void);
int  thread_wake(uint64_t addr, int n, uint64_t addr2, int move);
