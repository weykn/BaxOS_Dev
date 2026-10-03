#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Threads: what clone(CLONE_THREAD) makes, each with a kernel stack of its
 * own, sharing its process's memory and descriptors.
 *
 * Nothing interrupts a running thread - there is no timer - so another runs
 * only when this one waits in the kernel: every wait (futex, poll, select, a
 * socket, sleeping, a key) calls thread_yield, which passes the processor
 * round the process's threads. A thread spinning in its own code without a
 * syscall keeps it.
 *
 * A process is a level: programs run inside programs (a fork runs its child
 * inside the call), and only the threads of the innermost one run. The
 * thread that entered a level is its leader; its exit ends the rest. */

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

/* Lets the process's other threads run, if it has any. */
void thread_yield(void);

/* The running thread's blocked signals, as rt_sigprocmask has them. */
uint64_t *thread_sigmask(void);

/* Whether this is the process's only thread. */
bool thread_alone(void);

/* This thread's id: the process's for its leader. */
int thread_id(void);

/* How much kernel stack is left below here. */
uint64_t thread_stack_left(const void *here);

/* Around running a new process on this thread, before it starts and once it
   has ended. */
void thread_enter(void);
void thread_leave(void);

/* Ends this thread (exit), or the whole process (exit_group, a fault,
   Ctrl-C). */
__attribute__((noreturn)) void thread_exit(int code);
__attribute__((noreturn)) void process_exit(int code);

/* Futexes. thread_sleep_on marks this thread waiting at addr; thread_woken
   says whether a wake has reached it since. thread_wake wakes up to n
   waiting at addr, moving up to move more of them on to addr2, and returns
   how many it woke. */
void thread_sleep_on(uint64_t addr);
bool thread_woken(void);
int  thread_wake(uint64_t addr, int n, uint64_t addr2, int move);
