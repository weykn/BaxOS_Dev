#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* TCR, the Tuxlet C runtime: what the core programs (tsh, tuxlet-coreutils)
 * have in place of a C library, linked into each statically - start-up, the
 * Linux syscalls they use, a memory allocator, string functions and a way to
 * print. Deliberately small: no locale, no threads, no printf. Calls answer
 * as the kernel does, a negated errno on failure. No initialised pointers in
 * data: a TEF64 file is never relocated (tef64.ld). */

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

struct stat {
    uint64_t dev, ino, nlink;
    uint32_t mode, uid, gid, pad;
    uint64_t rdev;
    int64_t  size, blksize, blocks;
    uint64_t times[6];
    int64_t  unused[3];
};

long sys(long n, long a, long b, long c, long d);
int  vfork(void);               /* clone(CLONE_VM | CLONE_VFORK) */
void tcr_restorer(void);        /* rt_sigreturn, for a signal handler */

#define read(fd, buf, n)    sys(0, fd, (long)(buf), n, 0)
#define write(fd, buf, n)   sys(1, fd, (long)(buf), n, 0)
#define open(path, flags, mode) (int)sys(2, (long)(path), flags, mode, 0)
#define close(fd)           sys(3, fd, 0, 0, 0)
#define stat(path, st)      sys(4, (long)(path), (long)(st), 0, 0)
#define execve(path, argv, envp) sys(59, (long)(path), (long)(argv), (long)(envp), 0)
#define wait4(pid, status)  sys(61, pid, (long)(status), 0, 0)
#define getcwd(buf, n)      sys(79, (long)(buf), n, 0, 0)
#define chdir(path)         sys(80, (long)(path), 0, 0, 0)
#define rename(from, to)    sys(82, (long)(from), (long)(to), 0, 0)
#define mkdir(path, mode)   sys(83, (long)(path), mode, 0, 0)
#define unlink(path)        sys(87, (long)(path), 0, 0, 0)
#define getdents64(fd, buf, n) sys(217, fd, (long)(buf), n, 0)
#define exit(code)          sys(231, code, 0, 0, 0)
#define brk(to)             sys(12, (long)(to), 0, 0, 0)

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
