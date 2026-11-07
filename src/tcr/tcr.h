#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* TCR, the Tuxlet C runtime: what the core programs (tsh, tuxlet-coreutils)
 * have in place of a C library, linked into each statically - start-up, the
 * Linux syscalls they use, a memory allocator, string functions and a way to
 * print. Deliberately small: no locale, no threads, no printf. Calls answer
 * as the kernel does, a negated errno on failure. No initialised pointers in
 * data: a TEF file is never relocated (tef.ld). Built -m32, it is the same
 * for a 32-bit program: i386's numbers, int 0x80 and stat64. */

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0100
#define O_TRUNC  01000

#define ENOENT  2
#define EINTR   4
#define ENOEXEC 8
#define EEXIST  17

#define S_IFMT  0170000
#define S_IFDIR 0040000

#ifdef __i386__
struct stat {                   /* stat64, i386's */
    uint64_t dev;
    uint32_t pad0, ino32, mode, nlink, uid, gid;
    uint64_t rdev;
    uint32_t pad3;
    int64_t  size;
    uint32_t blksize;
    uint64_t blocks;
    uint32_t times[6];
    uint64_t ino;
} __attribute__((packed));

enum { NR_READ = 3, NR_WRITE = 4, NR_OPEN = 5, NR_CLOSE = 6, NR_STAT = 195, NR_EXECVE = 11,
       NR_WAIT4 = 114, NR_GETCWD = 183, NR_CHDIR = 12, NR_RENAME = 38, NR_MKDIR = 39,
       NR_RMDIR = 40, NR_UNLINK = 10, NR_GETDENTS64 = 220, NR_EXIT = 252, NR_BRK = 45,
       NR_SIGACTION = 174 };
#else
struct stat {
    uint64_t dev, ino, nlink;
    uint32_t mode, uid, gid, pad;
    uint64_t rdev;
    int64_t  size, blksize, blocks;
    uint64_t times[6];
    int64_t  unused[3];
};

enum { NR_READ = 0, NR_WRITE = 1, NR_OPEN = 2, NR_CLOSE = 3, NR_STAT = 4, NR_EXECVE = 59,
       NR_WAIT4 = 61, NR_GETCWD = 79, NR_CHDIR = 80, NR_RENAME = 82, NR_MKDIR = 83,
       NR_RMDIR = 84, NR_UNLINK = 87, NR_GETDENTS64 = 217, NR_EXIT = 231, NR_BRK = 12,
       NR_SIGACTION = 13 };
#endif

long sys(long n, long a, long b, long c, long d);
int  vfork(void);               /* clone(CLONE_VM | CLONE_VFORK) */
void tcr_restorer(void);        /* sigreturn, for a signal handler */

#define read(fd, buf, n)    sys(NR_READ, fd, (long)(buf), n, 0)
#define write(fd, buf, n)   sys(NR_WRITE, fd, (long)(buf), n, 0)
#define open(path, flags, mode) (int)sys(NR_OPEN, (long)(path), flags, mode, 0)
#define close(fd)           sys(NR_CLOSE, fd, 0, 0, 0)
#define stat(path, st)      sys(NR_STAT, (long)(path), (long)(st), 0, 0)
#define execve(path, argv, envp) sys(NR_EXECVE, (long)(path), (long)(argv), (long)(envp), 0)
#define wait4(pid, status)  sys(NR_WAIT4, pid, (long)(status), 0, 0)
#define getcwd(buf, n)      sys(NR_GETCWD, (long)(buf), n, 0, 0)
#define chdir(path)         sys(NR_CHDIR, (long)(path), 0, 0, 0)
#define rename(from, to)    sys(NR_RENAME, (long)(from), (long)(to), 0, 0)
#define mkdir(path, mode)   sys(NR_MKDIR, (long)(path), mode, 0, 0)
#define rmdir(path)         sys(NR_RMDIR, (long)(path), 0, 0, 0)
#define unlink(path)        sys(NR_UNLINK, (long)(path), 0, 0, 0)
#define getdents64(fd, buf, n) sys(NR_GETDENTS64, fd, (long)(buf), n, 0)
#define exit(code)          sys(NR_EXIT, code, 0, 0, 0)
#define brk(to)             sys(NR_BRK, (long)(to), 0, 0, 0)
#define rt_sigaction(sig, act) sys(NR_SIGACTION, sig, (long)(act), 0, 8)

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
char  *strchr(const char *s, int c);
void  *memcpy(void *to, const void *from, size_t n);
void  *memset(void *to, int c, size_t n);

/* Memory from the heap (brk), NULL when there is none. What is freed is
   kept for the next malloc that fits; the heap never shrinks. */
void *malloc(size_t n);
void  free(void *p);
void *realloc(void *p, size_t n);

/* Writes the strings to fd, up to a NULL: out(2, name, ": not found\n", NULL). */
void out(int fd, ...);

/* A line to standard output. */
int puts(const char *s);

/* n in decimal, into buf (21 bytes is room for any), which it returns. */
char *utoa(unsigned long n, char *buf);

/* The last part of a path. */
const char *base_of(const char *path);

/* Whether path is a folder. */
bool is_folder(const char *path);

/* to, or to/<base of from> if to is a folder, in buf. */
const char *dest_of(const char *from, const char *to, char *buf, size_t max);

int main(int argc, char **argv, char **envp);
