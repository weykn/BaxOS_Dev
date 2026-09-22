#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What a utility has instead of a C library.
 *
 * These are the kernel's syscalls - Linux's numbers, Linux's arguments -
 * reached the way any program reaches them, plus the few string and printing
 * helpers the utilities share. Everything is inline: a raw utility is one
 * object file, and there is nothing to link against. */

#define SYS_read        0
#define SYS_write       1
#define SYS_open        2
#define SYS_close       3
#define SYS_fstat       5
#define SYS_rename      82
#define SYS_mkdir       83
#define SYS_unlink      87
#define SYS_getdents64  217
#define SYS_getcwd      79
#define SYS_newfstatat  262
#define SYS_statfs      137
#define SYS_reboot      169
#define SYS_lseek       8
#define SYS_chdir       80
#define SYS_exit        60
#define SYS_spawn       1000    /* this machine's own: run a program, and wait */
#define SYS_uname       63
#define SYS_sysinfo     99
#define SYS_ioctl       16

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0x40
#define O_TRUNC  0x200

#define AT_FDCWD (-100)

#define STDOUT 1
#define STDERR 2

static inline long syscall3(long number, long a, long b, long c) {
    long result;

    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return result;
}

static inline long syscall4(long number, long a, long b, long c, long d) {
    long result;
    register long r10 __asm__("r10") = d;

    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory");
    return result;
}

static inline long sys_read(int fd, void *buffer, long count) {
    return syscall3(SYS_read, fd, (long)buffer, count);
}

static inline long sys_write(int fd, const void *text, long length) {
    return syscall3(SYS_write, fd, (long)text, length);
}

static inline long sys_open(const char *path, long flags) {
    return syscall3(SYS_open, (long)path, flags, 0644);
}

static inline long sys_close(int fd) {
    return syscall3(SYS_close, fd, 0, 0);
}

static inline long sys_unlink(const char *path) {
    return syscall3(SYS_unlink, (long)path, 0, 0);
}

static inline long sys_mkdir(const char *path) {
    return syscall3(SYS_mkdir, (long)path, 0755, 0);
}

static inline long sys_rename(const char *from, const char *to) {
    return syscall3(SYS_rename, (long)from, (long)to, 0);
}

/* How big the disk is, and how much of it is left. */
struct statfs {
    int64_t  type, bsize;
    uint64_t blocks, bfree, bavail, files, ffree;
    int32_t  fsid[2];
    int64_t  namelen, frsize, flags, spare[4];
};

static inline long sys_statfs(const char *path, struct statfs *out) {
    return syscall3(SYS_statfs, (long)path, (long)out, 0);
}

#define REBOOT_MAGIC1    0xFEE1DEAD
#define REBOOT_MAGIC2    0x28121969
#define REBOOT_RESTART   0x01234567
#define REBOOT_POWER_OFF 0x4321FEDC

static inline long sys_reboot(long command) {
    return syscall4(SYS_reboot, (long)(unsigned)REBOOT_MAGIC1,
                    (long)(unsigned)REBOOT_MAGIC2, command, 0);
}

static inline long sys_getcwd(char *buffer, long size) {
    return syscall3(SYS_getcwd, (long)buffer, size, 0);
}

static inline long sys_chdir(const char *path) {
    return syscall3(SYS_chdir, (long)path, 0, 0);
}

/* Runs path with argv - a NULL-terminated list, argv[0] the name it is
   called by - and comes back with what it exited with. There is no fork
   here: the kernel puts this program's memory aside, runs that one in it,
   and gives it back. With out set, what the program printed is taken into
   it rather than shown, which is what $(...) is built on. */
static inline long sys_spawn(const char *path, const char *const *argv,
                             char *out, long out_size) {
    return syscall4(SYS_spawn, (long)path, (long)argv, (long)out, out_size);
}

static inline long sys_getdents64(int fd, void *buffer, long count) {
    return syscall3(SYS_getdents64, fd, (long)buffer, count);
}

/* Linux's dirent, as getdents64 writes them one after another. */
struct dirent64 {
    uint64_t ino;
    int64_t  off;
    uint16_t reclen;
    uint8_t  type;
    char     name[];
};

#define DT_DIR 4

/* The fields of struct stat this needs, at Linux's offsets. */
struct stat {
    uint64_t dev, ino, nlink;
    uint32_t mode, uid, gid, pad;
    uint64_t rdev, size;
    int64_t  blksize, blocks;
    int64_t  times[6];
    int64_t  unused[3];
};

#define S_IFMT  0170000
#define S_IFDIR 0040000

static inline long sys_fstat(int fd, struct stat *out) {
    return syscall3(SYS_fstat, fd, (long)out, 0);
}

static inline long sys_stat(const char *path, struct stat *out) {
    return syscall4(SYS_newfstatat, AT_FDCWD, (long)path, (long)out, 0);
}

/* What the machine calls itself: six fixed-width fields, one after another. */
struct utsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65], domain[65];
};

static inline long sys_uname(struct utsname *out) {
    return syscall3(SYS_uname, (long)out, 0, 0);
}

/* How long the machine has been up, and what its RAM comes to. */
struct sysinfo {
    int64_t  uptime;
    uint64_t loads[3];
    uint64_t totalram, freeram, sharedram, bufferram;
    uint64_t totalswap, freeswap;
    uint16_t procs, pad;
    uint64_t totalhigh, freehigh;
    uint32_t unit;
    char     spare[4];
};

static inline long sys_sysinfo(struct sysinfo *out) {
    return syscall3(SYS_sysinfo, (long)out, 0, 0);
}

/* The terminal's settings, as tcgetattr and tcsetattr pass them. Turning
   ICANON and ECHO off is what makes a single keypress arrive on its own,
   rather than a whole line once Enter is pressed. */
struct termios {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t  line, cc[19];
    uint32_t ispeed, ospeed;
};

#define TCGETS 0x5401
#define TCSETS 0x5402
#define ICANON 0x0002
#define ECHO   0x0008

static inline long sys_termios_get(int fd, struct termios *out) {
    return syscall3(SYS_ioctl, fd, TCGETS, (long)out);
}

static inline long sys_termios_set(int fd, const struct termios *in) {
    return syscall3(SYS_ioctl, fd, TCSETS, (long)in);
}

/* How big the screen is, in characters and in pixels. */
struct winsize {
    uint16_t rows, columns, pixel_w, pixel_h;
};

#define TIOCGWINSZ 0x5413

static inline long sys_winsize(struct winsize *out) {
    return syscall3(SYS_ioctl, STDOUT, TIOCGWINSZ, (long)out);
}

/* ---- the little that stands in for a C library -------------------------- */

static inline size_t ulen(const char *s) {
    size_t n = 0;

    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static inline void ucopy(char *to, const char *from, size_t n) {
    for (size_t i = 0; i < n; i++) {
        to[i] = from[i];
    }
}

static inline bool usame(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static inline void put(const char *text) {
    sys_write(STDOUT, text, (long)ulen(text));
}

/* Colours, as every terminal has taken them for fifty years; the console
   understands the same few. */
#define DIM    "\033[90m"
#define ACCENT "\033[96m"
#define BRIGHT "\033[97m"
#define GREEN  "\033[92m"
#define RED    "\033[91m"
#define PLAIN  "\033[0m"

static inline void put_error(const char *who, const char *what, const char *why) {
    sys_write(STDERR, RED, (long)ulen(RED));
    sys_write(STDERR, who, (long)ulen(who));
    sys_write(STDERR, ": ", 2);
    if (what != NULL) {
        sys_write(STDERR, what, (long)ulen(what));
        sys_write(STDERR, ": ", 2);
    }
    sys_write(STDERR, why, (long)ulen(why));
    sys_write(STDERR, PLAIN "\n", (long)ulen(PLAIN) + 1);
}

/* Spaces, to line a column up. One write rather than one a space: a syscall
   costs far more than the character it carries, and a column of them was a
   fifth of everything `ls` did. */
static inline void put_spaces(size_t count) {
    static const char spaces[] = "                                ";

    while (count > 0) {
        size_t n = count < sizeof spaces - 1 ? count : sizeof spaces - 1;

        sys_write(STDOUT, spaces, (long)n);
        count -= n;
    }
}

/* An unsigned number, right-aligned in width columns when width is given,
   and written in one go. */
static inline void put_number(uint64_t value, unsigned width) {
    char text[24];
    unsigned n = sizeof text;

    do {
        text[--n] = (char)('0' + value % 10);
        value /= 10;
    } while (value > 0);

    if (width > sizeof text - n) {
        put_spaces(width - (sizeof text - n));
    }
    sys_write(STDOUT, text + n, (long)(sizeof text - n));
}

/* The first c in s, or NULL. */
static inline char *ufind(const char *s, char c) {
    for (; *s != '\0'; s++) {
        if (*s == c) {
            return (char *)s;
        }
    }
    return NULL;
}

/* The last one. */
static inline char *ufind_last(const char *s, char c) {
    char *last = NULL;

    for (; *s != '\0'; s++) {
        if (*s == c) {
            last = (char *)s;
        }
    }
    return last;
}

/* Copies text onto the end of out, which holds max bytes in all. */
static inline void uappend(char *out, size_t max, const char *text) {
    size_t n = ulen(out);

    while (*text != '\0' && n + 1 < max) {
        out[n++] = *text++;
    }
    out[n] = '\0';
}

/* Joins a folder and a name into out, as "folder/name". */
static inline void join(char *out, size_t max, const char *folder, const char *name) {
    size_t n = 0, length = ulen(folder);

    if (!usame(folder, ".") && !usame(folder, "")) {
        for (size_t i = 0; i < length && n + 1 < max; i++) {
            out[n++] = folder[i];
        }
        if (n > 0 && out[n - 1] != '/' && n + 1 < max) {
            out[n++] = '/';
        }
    }
    for (size_t i = 0; name[i] != '\0' && n + 1 < max; i++) {
        out[n++] = name[i];
    }
    out[n] = '\0';
}
