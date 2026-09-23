#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "boot.h"
#include "fs.h"

/* User programs, and the syscalls they make.
 *
 * A program is an ELF64 executable, loaded where its program headers say and
 * entered at the address in its header, or a flat binary loaded at
 * PROGRAM_BASE and entered at its first byte. A dynamically linked one
 * arrives with the path of its loader, and that is loaded too and entered
 * instead.
 *
 * It runs in ring 3 in a region of its own (vm.c), which is all of memory it
 * can see: code, data and stack alike read-write-execute. Only the pages it
 * touches take up RAM, so it can spread over the whole region on a machine
 * with far less than that free. Something linked to run at a fixed address
 * gets the fixed window from PROGRAM_BASE to PROGRAM_STACK instead, since a
 * region begins half a terabyte up and such a program has to be where it was
 * linked. Reaching outside what it was given, running out of RAM, or raising
 * any other CPU exception ends it with exit code PROGRAM_KILLED.
 *
 * It makes a syscall with the syscall instruction: the number in RAX, up to
 * three arguments in RDI, RSI and RDX, and the result back in RAX. The
 * numbers and arguments are Linux's, so a program written for Linux works as
 * long as it only uses the calls below. Besides RCX and R11, which the
 * instruction itself uses, a syscall may clobber RDX, RSI, RDI and R8-R10,
 * like a C call. A program ends with SYS_EXIT; returning from it crashes the
 * machine.
 *
 * Every call that takes a pointer checks it points into the program's own
 * memory, and returns -1 rather than reading or writing the kernel's. */

/* The fixed window, for a program linked to run at a fixed address.
   PROGRAM_BASE is where a plain `ld` puts things, so no linker flags are
   needed for one. */
#define PROGRAM_BASE   0x400000
#define PROGRAM_STACK  0x600000
#define PROGRAM_BYTES  (PROGRAM_STACK - PROGRAM_BASE)

#define PROGRAM_KILLED 139      /* what a Linux shell shows for a segfault */

/* Where things go in the region vm.c hands out, as offsets into it. A
   program taken off a Linux system wants several stretches of memory at once
   - itself, its loader, the libraries that loader maps, a heap and a stack -
   and they have to be far enough apart that none of them grows into another.
   The region is half a terabyte; these are a rounding error of it. */
#define USER_EXEC   0x01000000  /* a position-independent program */
#define USER_INTERP 0x08000000  /* its loader, ld.so */
#define USER_MMAP   0x10000000  /* what mmap hands out, growing up */
#define USER_BRK    0x30000000  /* the heap, growing up */
#define USER_STACK  0x3F000000  /* the stack top, growing down */
#define USER_STACK_BYTES 0x40000

/* Handlers that can be registered at once. There is no warning when this is
   too small - the registrations past it simply do not happen, and the calls
   they were for answer ENOSYS - so it is kept well clear of the number
   syscall_init actually makes. */
#define SYSCALL_SLOTS 144

/* Linux's numbers, and Linux's arguments. Only the handful the machine can
   actually answer are here: there is one process, no devices but the screen
   and the keyboard, and files are read-only to a program. */
enum {
    SYS_READ       = 0,     /* (fd, buf, count): a line typed at fd 0, or a file */
    SYS_WRITE      = 1,     /* (fd, text, length): prints to the screen */
    SYS_OPEN       = 2,     /* (path, flags, mode): opens a file or folder */
    SYS_CLOSE      = 3,     /* (fd) */
    SYS_FSTAT      = 5,     /* (fd, struct stat *) */
    SYS_LSEEK      = 8,     /* (fd, offset, whence): moves around an open file */
    SYS_MMAP       = 9,     /* (addr, length, prot, flags, fd, offset) */
    SYS_MPROTECT   = 10,    /* (addr, length, prot): everything is already RWX */
    SYS_MUNMAP     = 11,    /* (addr, length) */
    SYS_BRK        = 12,    /* (addr): moves or reports the heap's end */
    SYS_IOCTL      = 16,    /* (fd, request, argument) */
    SYS_WRITEV     = 20,    /* (fd, iovec *, count) */
    SYS_GETPID     = 39,    /* (): there is only ever one */
    SYS_SYSINFO    = 99,    /* (struct sysinfo *): uptime, and the RAM */

    /* The rest of what a program off a Linux system asks for before it does
       anything of its own. A machine with one process, no timestamps, no
       owners and no permissions can answer all of these truthfully by saying
       that there was nothing to do - and a program told ENOSYS instead
       stops, which is how `touch` failed to touch a file. */
    SYS_SCHED_YIELD = 24,   /* (): there is nothing else to yield to */
    SYS_MADVISE    = 28,    /* (addr, length, advice): taken as read */
    SYS_GETRUSAGE  = 98,    /* (who, struct rusage *) */
    SYS_TIMES      = 100,   /* (struct tms *) */
    SYS_UTIMENSAT  = 280,   /* (dirfd, path, times, flags) */
    SYS_UTIMES     = 235,
    SYS_FUTIMESAT  = 261,
    SYS_CHMOD      = 90,    /* nothing here has permissions to change */
    SYS_FCHMOD     = 91,
    SYS_FCHMODAT   = 268,
    SYS_CHOWN      = 92,    /* ...nor an owner */
    SYS_FCHOWN     = 93,
    SYS_LCHOWN     = 94,
    SYS_FCHOWNAT   = 260,
    SYS_FSYNC      = 74,    /* a write is on the disk before it returns */
    SYS_FDATASYNC  = 75,
    SYS_SYNC       = 162,
    SYS_FADVISE64  = 221,
    SYS_CLOCK_NANOSLEEP = 230,
    SYS_RT_SIGSUSPEND = 130,
    SYS_KILL       = 62,    /* the only process there is, is the caller */
    SYS_TGKILL     = 234,
    SYS_GETTID     = 186,   /* which is the process, there being one thread */
    SYS_RSEQ       = 334,
    SYS_UNAME      = 63,    /* (struct utsname *) */
    SYS_GETCWD     = 79,    /* (buf, size): the working directory, with its NUL */
    SYS_ARCH_PRCTL = 158,   /* (code, addr): where a libc puts its thread data */
    SYS_PREAD64    = 17,    /* (fd, buf, count, offset): a read that does not move */
    SYS_ACCESS     = 21,    /* (path, mode): whether a file is there */
    SYS_RT_SIGACTION   = 13,
    SYS_RT_SIGPROCMASK = 14,
    SYS_NANOSLEEP  = 35,
    SYS_DUP        = 32,
    SYS_DUP2       = 33,
    SYS_FCNTL      = 72,
    SYS_GETUID     = 102,   /* everything here runs as root */
    SYS_GETGID     = 104,
    SYS_GETEUID    = 107,
    SYS_GETEGID    = 108,
    SYS_FUTEX      = 202,   /* nothing waits: there is one thread */
    SYS_SCHED_GETAFFINITY = 204,
    SYS_STATX      = 332,
    SYS_FACCESSAT  = 269,
    SYS_FACCESSAT2 = 439,
    SYS_RENAME     = 82,    /* (from, to): moving a file is renaming it */
    SYS_MKDIR      = 83,    /* (path, mode) */
    SYS_RMDIR      = 84,    /* (path) */
    SYS_UNLINK     = 87,    /* (path): deletes a file */
    /* The same again, relative to a folder a program already has open. These
       are what a libc actually calls: coreutils' rm is unlinkat, its mkdir is
       mkdirat, its mv is renameat2. Left out, they answered "function not
       implemented" for the plainest commands there are. */
    SYS_UNLINKAT   = 263,   /* (dirfd, path, flags) */
    SYS_MKDIRAT    = 258,   /* (dirfd, path, mode) */
    SYS_RENAMEAT   = 264,   /* (olddirfd, old, newdirfd, new) */
    SYS_RENAMEAT2  = 316,   /* the same, with flags */
    SYS_LINKAT     = 265,   /* nothing here is a link */
    SYS_SYMLINKAT  = 266,
    SYS_SYMLINK    = 88,
    SYS_LINK       = 86,
    SYS_MKNODAT    = 259,
    SYS_TRUNCATE   = 76,    /* (path, length) */
    SYS_READV      = 19,    /* (fd, iovec *, count) */
    SYS_PWRITE64   = 18,    /* (fd, buf, count, offset) */
    SYS_POLL       = 7,     /* (pollfd *, count, timeout) */
    SYS_PPOLL      = 271,
    SYS_FSTATFS    = 138,   /* (fd, struct statfs *) */
    SYS_GETRLIMIT  = 97,
    SYS_SETRLIMIT  = 160,
    SYS_FLOCK      = 73,    /* one program: nothing to lock against */
    SYS_FALLOCATE  = 285,   /* a file here grows as it is written */
    SYS_MSYNC      = 26,
    SYS_SYNCFS     = 306,
    SYS_GETCPU     = 309,
    SYS_GETGROUPS  = 115,
    SYS_SCHED_GETSCHEDULER = 145,
    SYS_SCHED_SETSCHEDULER = 144,
    SYS_SCHED_GETPARAM     = 143,
    SYS_GETPRIORITY = 140,
    SYS_SETPRIORITY = 141,
    SYS_FTRUNCATE  = 77,    /* (fd, length) */
    SYS_STATFS     = 137,   /* (path, struct statfs *): how big the disk is */
    SYS_REBOOT     = 169,   /* (magic, magic, command, arg) */
    SYS_GETDENTS64 = 217,   /* (fd, buf, count): what is in an open folder */
    SYS_SET_TID_ADDRESS = 218,
    SYS_SET_ROBUST_LIST = 273,
    SYS_PRLIMIT64  = 302,
    SYS_GETRANDOM  = 318,
    SYS_READLINKAT = 267,
    SYS_CLOCK_GETTIME   = 228,  /* (clock, struct timespec *) */
    SYS_EXIT       = 60,    /* (code): ends the program */
    SYS_EXIT_GROUP = 231,   /* (code): what C's exit() compiles to */
    SYS_OPENAT     = 257,   /* (dirfd, path, flags, mode) */
    SYS_NEWFSTATAT = 262,   /* (dirfd, path, struct stat *, flags) */
    SYS_STAT       = 4,     /* (path, struct stat *) */
    SYS_LSTAT      = 6,     /* (path, struct stat *): nothing is a link */
    SYS_CHDIR      = 80,    /* (path): a program moving the working folder */
    SYS_FCHDIR     = 81,    /* (fd): the same, by a folder already open */
    SYS_DUP3       = 292,
    SYS_UMASK      = 95,
    SYS_GETTIMEOFDAY = 96,  /* (struct timeval *, struct timezone *) */
    SYS_TIME       = 201,   /* (time_t *) */
    SYS_SETPGID    = 109,
    SYS_GETPPID    = 110,
    SYS_GETPGRP    = 111,
    SYS_SETSID     = 112,
    SYS_GETPGID    = 121,
    SYS_READLINK   = 89,    /* (path, buf, size): nothing here is a link */
    SYS_SIGALTSTACK = 131,
    SYS_GETRESUID  = 118,   /* (uid_t *, uid_t *, uid_t *) */
    SYS_GETRESGID  = 120,
    SYS_SELECT     = 23,    /* (nfds, read, write, except, timeval *) */
    SYS_PSELECT6   = 270,   /* the same, with a timespec and a signal mask */

    /* Starting a program, the way a Linux shell starts one. There is no
       scheduler here, so fork runs its child to the end before it answers -
       see syscall.c, which is where that is made to work. */
    SYS_FORK       = 57,
    SYS_VFORK      = 58,
    SYS_CLONE      = 56,    /* (flags, stack, ...): glibc's fork is one */
    SYS_EXECVE     = 59,    /* (path, argv, envp) */
    SYS_WAIT4      = 61,    /* (pid, status *, options, rusage *) */
    SYS_PIPE       = 22,    /* (int fds[2]) */
    SYS_PIPE2      = 293,   /* (int fds[2], flags) */
};

/* Open flags, as Linux numbers them. */
#define O_ACCMODE 0x03
#define O_RDONLY  0x00
#define O_WRONLY  0x01
#define O_RDWR    0x02
#define O_CREAT   0x40
#define O_TRUNC   0x200
#define O_APPEND  0x400
/* A file with no name, made in the folder the path names: what a program
   that wants a scratch buffer of its own asks for. */
#define O_TMPFILE 0x410000

/* Descriptors a program may have at once, 0, 1 and 2 - the console -
   counted in. */
#define PROGRAM_FILES 16

/* Arguments and environment variables a program can be given, the name it
   was called by counted in. The lists are copied out of the program starting
   another and again onto the new program's stack, so they are what anybody
   types with room to spare rather than as many as could possibly fit. */
#define EXEC_ARGS 64
#define EXEC_ENV  64

#define PROGRAM_EINVAL (-7) /* not an executable this kernel can run */
#define PROGRAM_ENOINTERP (-8)  /* it wants a loader that is not on the disk */

typedef uint64_t (*syscall_fn)(uint64_t a, uint64_t b, uint64_t c);

/* Enables the syscall instruction and registers the built-in syscalls. */
void syscall_init(void);

/* Makes fn the handler for syscall number. The handlers are a short table
   rather than a slot per number, so that a call as high as SYS_EXIT costs
   nothing to leave room for; a byte per number below 256 finds the slot
   without walking it. Returns 0, or -1 if the table is full or the number
   is past what one holds. Unregistered numbers return -1 to the program. */
int syscall_register(uint64_t number, syscall_fn fn);

/* Loads a program file into memory by its program headers and stores its
   entry point in *entry. Returns 0, FS_EIO, FS_ENOSPC if RAM runs out, or
   PROGRAM_EINVAL. */
int program_load(const struct fs_file *file, uint64_t *entry);

/* Runs a loaded program until it exits, and returns its exit code. envc of 0
   gives it the machine's own environment, which is what the first program
   gets; fresh says to hand it a clean terminal and nothing open but the
   console, which is what everything but an execve wants. */
int program_run(uint64_t entry, unsigned argc, const char *const *argv,
                unsigned envc, const char *const *envv, bool fresh);

/* What the window's page tables cost, and what a running program has
   borrowed for itself, for the `mem` command. */
size_t program_tables(void);
size_t program_memory(void);
