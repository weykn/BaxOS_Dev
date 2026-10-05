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
#define ENOTCONN        107
#define ECONNREFUSED    111
#define EADDRINUSE      98

#define FOLDER_MARK  0xFFFFFFFFu
#define WRITE_MARK   0xFFFFFFFEu    /* a file open for writing */
#define CONSOLE_MARK 0xFFFFFFFDu    /* the screen and the keyboard */
#define PROCDIR_MARK 0xFFFFFFFCu    /* /proc, or /dev with folder 1: not on the disk */
#define PROC_MARK    0xFFFFFFFBu    /* one of the commands in it */
#define PIPE_MARK    0xFFFFFFFAu    /* one end of a pipe */
#define DEV_MARK     0xFFFFFFF9u    /* one of the made-up files in /dev */
#define MOD_MARK     0xFFFFFFF7u    /* a module's own: its files slot in size */
#define FIRST_MARK   MOD_MARK       /* below this, a start is a sector */

/* Files a module makes under /dev - ipc/pty's /dev/ptmx and /dev/pts/N. A
 * descriptor on one is MOD_MARK, with the module's slot in size; folder and
 * offset are the module's own, except that O_NONBLOCK in offset is the
 * descriptor's. The kernel reads, writes, polls, duplicates and closes it
 * through these. */
struct file_ops {
    const char *prefix;             /* the names it answers, under /dev/ */
    /* A descriptor on name - what follows /dev/ - or a negated errno. */
    uint64_t (*open)(const char *name, uint64_t flags);
    /* Whether name is one, and what stat says it is. */
    bool     (*named)(const char *name, uint32_t *mode, uint32_t *rdev);
    uint64_t (*read)(struct handle *h, uint64_t buf, uint64_t count);
    uint64_t (*write)(struct handle *h, uint64_t buf, uint64_t count);
    uint64_t (*ioctl)(struct handle *h, uint64_t request, uint64_t arg);
    unsigned (*ready)(struct handle *h);    /* NET_IN, NET_OUT, NET_HUP: as a socket's */
    void     (*hold)(struct handle *h);     /* one more descriptor on it */
    void     (*drop)(struct handle *h);     /* one fewer */
    void     (*stat)(struct handle *h, uint32_t *mode, uint32_t *rdev);
};

#define FILE_OPS 4
extern const struct file_ops *file_ops[FILE_OPS];

/* A module's files coming and going: the slot it has, one-based, or 0 if
   there is none - and with NULL, giving slot up. */
unsigned files_register(const struct file_ops *ops, unsigned slot);

/* The ops a MOD_MARK descriptor is the module's through, or NULL. */
static inline const struct file_ops *ops_of(const struct handle *h) {
    return h != NULL && h->start == MOD_MARK && h->size >= 1 && h->size <= FILE_OPS
         ? file_ops[h->size - 1] : NULL;
}

/* A module's /dev name: the ops it is under, or NULL; leaf is what follows
   "/dev/". */
const struct file_ops *ops_named(const char *path, const char **leaf);

/* Sends sig to every process in group pgid: a terminal's Ctrl-C. */
void signal_pgrp(int pgid, int sig);

/* The running process's pid, group and session. */
void process_ids(int *pid, int *pgid, int *sid);

/* Where a made-up file's number starts, clear of any real one: those are
   sector numbers, and the disk is far smaller than this. */
#define PROC_INO 0x01000000u
#define DEV_INO  (PROC_INO + 0x10000u)    /* /dev's, as its listing numbers them */

enum dev {
    DEV_NULL = 1, DEV_ZERO, DEV_FULL, DEV_RANDOM, DEV_TTY,
    DEV_FB,                         /* the screen, /dev/fb0 */
    DEV_EVENT0,                     /* the keyboard and the mouse, as evdev: */
    DEV_EVENT1,                     /* DEV_EVENT0 + INPUT_* */
};

struct device {
    const char *name;
    enum dev    which;
    uint16_t    rdev;               /* Linux's number for it, major << 8 | minor */
};

#define DEVICES 15
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
#define PIPE_BUFFER 5               /* a buffer a module keeps its own state in:
                                       an epoll set's list (compat/linux) */

/* A descriptor on a fresh buffer of bytes, zeroed, which goes with the last
   descriptor on it; or a negated errno. */
uint64_t buffer_open(uint32_t bytes);

/* The running program's time, in microseconds (syscall.c). */
struct times {
    uint64_t started;           /* when the program began */
    uint64_t idle;              /* how long it waited */
    uint64_t sys;               /* how long the kernel worked for it */
    uint64_t children_wall;     /* how long its finished children ran */
    uint64_t children_user, children_sys;   /* and what of it was which */
};

/* The running process's, through these: every process has its own. */
struct times *process_times(void);
#define now_running (*process_times())

/* What a TCGETS hands over, and what the one carrying the line speeds does. */
#define TERMIOS_OLD 36
#define TERMIOS_NEW 44

#define WRITERS 4
char (*process_writers(void))[FS_NAME_LEN];
#define writer_names (process_writers())

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
void        *pipe_data(const struct handle *h);     /* a PIPE_BUFFER's bytes, or NULL */
uint32_t     pipe_left(const struct pipe *p);
bool         readable(const struct handle *h);
bool         event_ready(const struct handle *h);
enum dev     dev_named(const char *name);
unsigned     dev_folder(const char *name);  /* 1 /dev, 2 /dev/input, 3 /dev/pts, 0 none */
const char  *proc_net_name(const char *path);
/* /proc/self/fd/N, and /proc/<own pid>/fd/N: the path descriptor N is open
   on, into out (at least FS_NAME_LEN + 16 bytes) - or NULL for any other name. */
const char  *proc_fd_target(const char *name, char *out, size_t max);
uint32_t     file_ino(const char *name, bool follow);
bool         is_fifo(const struct fs_file *file);
uint64_t     self_us(void);
uint64_t     user_us(void);
uint64_t     realtime_us(void);
uint64_t     realtime_ms(void);
uint64_t     deliverable(void);         /* the signal to be handled, or 0 */

/* A program taking the screen or the input devices for itself: they are
   given back when it ends (syscall.c). */
void graphics_take(void);

/* The terminal's foreground group: where Ctrl-C goes (TIOCGPGRP/TIOCSPGRP). */
int  tty_foreground(void);
void tty_set_foreground(int pgid);

#define LINUX_MODULE "compat/linux"

/* The module's own: the handler for a syscall number, or NULL. */
void linux_register(syscall_fn (*find)(uint64_t number));
