#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What tuxpac needs of a C library, straight on Linux's syscalls: it is
 * built static and alone, with nothing on the disk to link against. Calls
 * answer as the kernel does, a negated errno on failure. */

#define O_RDONLY    0
#define O_WRONLY    1
#define O_CREAT     0100
#define O_TRUNC     01000
#define O_DIRECTORY 0200000

#define ENOENT  2
#define EEXIST  17
#define EAGAIN  11
#define EINPROGRESS 115

#define S_IFMT  0170000
#define S_IFDIR 0040000
#define S_IFLNK 0120000

struct stat {
    uint64_t dev, ino, nlink;
    uint32_t mode, uid, gid, pad;
    uint64_t rdev;
    int64_t  size, blksize, blocks;
    uint64_t times[6];
    int64_t  unused[3];
};

struct pollfd {
    int     fd;
    int16_t events, revents;
};

#define POLLIN  1
#define POLLOUT 4

long sys_open(const char *path, int flags, int mode);
long sys_read(int fd, void *buf, size_t n);
long sys_write(int fd, const void *buf, size_t n);
long sys_pread(int fd, void *buf, size_t n, uint64_t at);
long sys_close(int fd);
long sys_stat(const char *path, struct stat *st);
long sys_lstat(const char *path, struct stat *st);
long sys_fstat(int fd, struct stat *st);
long sys_mkdir(const char *path);
long sys_rmdir(const char *path);
long sys_unlink(const char *path);
long sys_rename(const char *from, const char *to);
long sys_symlink(const char *target, const char *path);
long sys_getdents(int fd, void *buf, size_t n);
long sys_poll(struct pollfd *p, unsigned n, int ms);
long sys_socket(int domain, int type, int proto);
long sys_connect(int s, const void *addr, unsigned len);
long sys_sendto(int s, const void *buf, size_t n, const void *addr, unsigned len);
long sys_recv(int s, void *buf, size_t n);

/* Runs path with argv, as a child, and answers its exit code - or a
   negated errno if it could not be started. */
int run(const char *path, const char *const *argv);
/* The same with extra NAME=value pairs in front of the environment, NULL-ended. */
int run_env(const char *path, const char *const *argv, const char *const *extra);

void *xalloc(size_t bytes);         /* zeroed; tells the user if there is none */
void  xfree(void *p);

void  *memcpy(void *d, const void *s, size_t n);
void  *memset(void *d, int c, size_t n);
void  *memmove(void *d, const void *s, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
char  *strcpy(char *d, const char *s);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *str_word(char **text);       /* the next word, NUL-ended; *text moves past it */

static inline bool memcmp_n(const char *a, const char *b, size_t n) {
    while (n > 0 && *a == *b) {
        a++;
        b++;
        n--;
    }
    return n == 0;
}

/* %s, %u, %x and %0Nu, as the kernel's printf. print writes to standard
   output, fail to standard error and makes the exit code 1. */
void print(const char *fmt, ...);
void fail(const char *fmt, ...);
void format(char *out, const char *fmt, ...);
const char *errstr(long err);       /* for a negated errno */

extern int status;                  /* the exit code */
