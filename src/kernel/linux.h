#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "fs.h"
#include "syscall.h"

/* What syscall.c shares with compat/linux, the module that answers the
   calls a program off a Linux system makes beyond the core: stat and its
   kin, folders, the terminal, poll and select, the clock, and the calls
   that answer "done" for what this machine does not have. The kernel loads
   it at the first such call and drops it once no program is running. */

/* Linux answers a failed call with the negated error number, and a libc
   tells the two apart by the result being a small negative. -1 on its own
   means EPERM, which is rarely what went wrong. */
#define ERR(e) ((uint64_t)-(int64_t)(e))

#define ENOENT  2
#define EBADF   9
#define ENOMEM 12
#define EIO     5
#define EFAULT 14
#define EACCES 13
#define EEXIST 17
#define EAGAIN 11
#define EINTR  4
#define EMFILE 24
#define ENOTTY 25
#define EINVAL 22
#define ENOSYS 38
#define ENOEXEC 8
#define ENOTDIR 20
#define ERANGE  34
#define ECHILD 10
#define ENOSPC 28
#define EPERM   1
#define ENOTEMPTY 39
#define ENFILE 23
#define EPIPE  32
#define ESPIPE 29

#define S_IFMT  0170000             /* a file's type, in its mode */
#define S_IFIFO 0010000
#define S_IFCHR 0020000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IFLNK 0120000
#define S_IFSOCK 0140000
#define ELOOP  40
#define EPROTONOSUPPORT 93
#define ESOCKTNOSUPPORT 94
#define EAFNOSUPPORT    97
#define EOPNOTSUPP      95
#define ENOTSOCK        88

#define FOLDER_MARK  0xFFFFFFFFu
#define WRITE_MARK   0xFFFFFFFEu    /* a file open for writing */
#define CONSOLE_MARK 0xFFFFFFFDu    /* the screen and the keyboard */
#define PROCDIR_MARK 0xFFFFFFFCu    /* /proc, or /dev with folder 1: not on the disk */
#define PROC_MARK    0xFFFFFFFBu    /* one of the commands in it */
#define PIPE_MARK    0xFFFFFFFAu    /* one end of a pipe */
#define DEV_MARK     0xFFFFFFF9u    /* one of the made-up files in /dev */
#define FIRST_MARK   SOCK_MARK      /* below this, a start is a sector */

/* Where a made-up file's number starts, clear of any real one: those are
   sector numbers, and the disk is far smaller than this. */
#define PROC_INO 0x01000000u
#define DEV_INO  (PROC_INO + 0x10000u)    /* /dev's, as its listing numbers them */

enum dev {
    DEV_NULL = 1, DEV_ZERO, DEV_FULL, DEV_RANDOM, DEV_TTY,
};

struct device {
    const char *name;
    enum dev    which;
};

#define DEVICES 10
extern const struct device devices[DEVICES];

/* A pipe handle's size says which end it is. An eventfd is a pipe with no
   buffer, only a count; O_NONBLOCK and EFD_SEMAPHORE are in its offset. */
#define PIPE_READ  0
#define PIPE_WRITE 1
#define PIPE_EVENT 2
#define PIPE_FILE  3                /* a /proc/net file: the text, read once */
#define PIPE_PAIR  4                /* an AF_UNIX socketpair end: it reads its
                                       own pipe and writes the other's, whose
                                       slot is in offset from bit 16 */

/* The running program's time, in microseconds (syscall.c). */
struct times {
    uint64_t started;           /* when the program began */
    uint64_t idle;              /* how long it waited */
    uint64_t sys;               /* how long the kernel worked for it */
    uint64_t children_wall;     /* how long its finished children ran */
    uint64_t children_user, children_sys;   /* and what of it was which */
};

extern struct times now_running;

/* What a TCGETS hands over, and what the one carrying the line speeds does. */
#define TERMIOS_OLD 36
#define TERMIOS_NEW 44

#define WRITERS 4
extern char writer_names[WRITERS][FS_NAME_LEN];

/* The working directory, for a relative path in the *at calls. */
#define AT_FDCWD (-100)

/* In the flags of an *at call that can be about a link or what it names:
   the link, please. */
#define AT_SYMLINK_NOFOLLOW 0x100

/* A folder's number, from its table entry counting from one: the root,
   which has none, is 1. */
static inline uint32_t folder_ino(unsigned one_based) {
    return one_based + 1;
}

/* A timespec's time, in milliseconds, rounded up. */
static inline uint64_t timespec_ms(uint64_t spec) {
    const int64_t *t = (const int64_t *)spec;

    return (uint64_t)t[0] * 1000 + (uint64_t)(t[1] + 999999) / 1000000;
}

struct pipe;

const char  *user_string(uint64_t addr);
const char  *at_path(uint64_t dirfd, const char *name, char *out, size_t max);
uint64_t     fs_errno(int err);
bool         is_console(uint64_t fd);
struct pipe *pipe_of(const struct handle *h);
uint32_t     pipe_left(const struct pipe *p);
bool         readable(const struct handle *h);
bool         event_ready(const struct handle *h);
enum dev     dev_named(const char *name);
bool         dev_folder(const char *name);
const char  *proc_net_name(const char *path);
uint32_t     file_ino(const char *name, bool follow);
bool         is_fifo(const struct fs_file *file);
uint64_t     self_us(void);
uint64_t     user_us(void);
uint64_t     realtime_us(void);
uint64_t     realtime_ms(void);
uint64_t     deliverable(void);         /* the signal to be handled, or 0 */

#define LINUX_MODULE "compat/linux"

/* The module's own: the handler for a syscall number, or NULL. */
void linux_register(syscall_fn (*find)(uint64_t number));
