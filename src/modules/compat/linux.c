/* compat/linux: the calls a program off a Linux system makes beyond the
 * core the kernel keeps - stat and its kin, what is in a folder, links, the
 * terminal, poll and select, the clock, the machine - and the ones that
 * answer "done" for what this machine has no notion of: owners,
 * permissions, priorities, extended attributes.
 *
 * Nothing loads it by hand: the kernel does, at the first call that is
 * its, and drops it once no program is running, so a machine at the
 * prompt holds none of it. What it shares with syscall.c is in linux.h. */

#include "console.h"
#include "efi_kernel.h"
#include "fs.h"
#include "linux.h"
#include "mem.h"
#include "module.h"
#include "net.h"
#include "proc.h"
#include "string.h"
#include "syscall.h"
#include "thread.h"
#include "vga.h"

#define SECTOR_SIZE 512
#define arg (syscall_args())        /* the call's six, as syscall.c keeps them */

static uint64_t sys_ok(uint64_t a, uint64_t b, uint64_t c) {
    (void)a;
    (void)b;
    (void)c;
    return 0;
}

static uint64_t sys_root(uint64_t a, uint64_t b, uint64_t c) {
    (void)a;
    (void)b;
    (void)c;
    return 0;                       /* getuid and its kin: this is root */
}

/* For the calls that hand back a structure of figures this machine does not
   keep - how much processor time has been used, and the like. Zeroes are
   what a program that has only just started would see anyway. */
static uint64_t sys_zeroed(uint64_t out, uint64_t b, uint64_t c) {
    (void)b;
    (void)c;
    if (out != 0) {
        if (!user_range(out, 144)) {    /* the largest of them, struct rusage */
            return ERR(EFAULT);
        }
        memset((void *)out, 0, 144);
    }
    return 0;
}

#define RUSAGE_CHILDREN (-1)

static uint64_t sys_getrusage(uint64_t who, uint64_t out, uint64_t c) {
    bool children = (int32_t)who == RUSAGE_CHILDREN;
    uint64_t user = children ? now_running.children_user : user_us();
    uint64_t sys = children ? now_running.children_sys : now_running.sys;
    int64_t *usage = (int64_t *)out;

    (void)c;
    if (!user_range(out, 144)) {
        return ERR(EFAULT);
    }
    memset(usage, 0, 144);
    usage[0] = (int64_t)(user / 1000000);       /* ru_utime */
    usage[1] = (int64_t)(user % 1000000);
    usage[2] = (int64_t)(sys / 1000000);        /* ru_stime */
    usage[3] = (int64_t)(sys % 1000000);
    return 0;
}

/* In clock ticks, a hundred to the second; the answer is ticks since boot. */
static uint64_t sys_times(uint64_t out, uint64_t b, uint64_t c) {
    int64_t *tms = (int64_t *)out;

    (void)b;
    (void)c;
    if (out != 0) {
        if (!user_range(out, 32)) {
            return ERR(EFAULT);
        }
        tms[0] = (int64_t)(user_us() / 10000);
        tms[1] = (int64_t)(now_running.sys / 10000);
        tms[2] = (int64_t)(now_running.children_user / 10000);
        tms[3] = (int64_t)(now_running.children_sys / 10000);
    }
    return efi_uptime_ms() / 10;
}

/* Which of a few descriptors can be read or written without waiting.
 *
 * Every file here is ready the moment it is asked about; the console is
 * ready once something has been typed, so waiting on it is waiting for a
 * key. A shell asks this before it reads, and one told the question cannot
 * be answered at all takes that for the end of its input and leaves - which
 * is what this is here to prevent.
 *
 * An fd_set is a bitmap, and every descriptor this kernel hands out is in
 * its first word, so only that word is read and written. The timeout is
 * seconds and then fractions, per_ms of them to a millisecond: a program
 * telling a lone Escape from the start of an arrow key waits a few tens of
 * them, and has to be answered that quickly. */
static uint64_t wait_ready(uint64_t nfds, uint64_t readfds, uint64_t writefds,
                           uint64_t exceptfds, uint64_t timeout, int64_t per_ms) {
    uint64_t want = 0, ready = 0, writable = 0, console_bits = 0, sock_bits = 0, event_bits = 0;
    int64_t until = 0;

    if (nfds > PROGRAM_FILES) {
        nfds = PROGRAM_FILES;
    }
    if ((readfds != 0 && !user_range(readfds, 8)) ||
        (writefds != 0 && !user_range(writefds, 8)) ||
        (exceptfds != 0 && !user_range(exceptfds, 8)) ||
        (timeout != 0 && !user_range(timeout, 16))) {
        return ERR(EFAULT);
    }
    if (readfds != 0) {
        want = *(uint64_t *)readfds;
    }
    for (uint64_t fd = 0; fd < nfds; fd++) {
        uint64_t bit = 1ull << fd;

        if (handle_of(fd) == NULL) {
            continue;
        }
        if (handle_of(fd)->start == SOCK_MARK) {
            sock_bits |= bit & (want | (writefds != 0 ? *(uint64_t *)writefds : 0));
            continue;
        }
        if (writefds != 0 && (*(uint64_t *)writefds & bit) != 0) {
            writable |= bit;        /* nothing else here has to wait to write */
        }
        if ((want & bit) == 0) {
            continue;
        }
        if (is_console(fd)) {
            console_bits |= bit;
        } else if (handle_of(fd)->start == PIPE_MARK) {
            struct pipe *p = pipe_of(handle_of(fd));

            /* A pipe with nothing left in it is at its end, which is a read
               that returns nothing rather than a wait. */
            (void)p;
            if (handle_of(fd)->size == PIPE_EVENT || handle_of(fd)->size == PIPE_PAIR ||
                handle_of(fd)->size == PIPE_READ) {
                event_bits |= bit;  /* ready once something adds to it */
                ready |= event_ready(handle_of(fd)) || readable(handle_of(fd)) ? bit : 0;
            } else {
                ready |= bit;       /* what is in it, or its end */
            }
        } else {
            ready |= bit;           /* a file is always there to be read */
        }
    }
    if (timeout != 0) {
        const int64_t *spec = (const int64_t *)timeout;

        until = (int64_t)efi_uptime_ms() + spec[0] * 1000 + spec[1] / per_ms;
    }
    uint64_t began = wait_began();

    uint64_t wanted_out = writefds != 0 ? *(uint64_t *)writefds : 0;

    while ((console_bits | sock_bits | event_bits) != 0 && ready == 0 && writable == 0) {
        if (console_bits != 0 && console_ready()) {
            ready = console_bits;
        }
        for (uint64_t fd = 0; fd < nfds; fd++) {
            if ((event_bits >> fd & 1) != 0 &&
                (event_ready(handle_of(fd)) ||
                 readable(handle_of(fd)))) {
                ready |= 1ull << fd;
            }
        }
        for (uint64_t fd = 0; fd < nfds && net != NULL; fd++) {
            uint64_t bit = 1ull << fd;
            unsigned r;

            if ((sock_bits & bit) == 0) {
                continue;
            }
            r = net->ready((int)handle_of(fd)->folder);
            if ((want & bit) != 0 && (r & (NET_IN | NET_HUP | NET_ERR)) != 0) {
                ready |= bit;
            }
            if ((wanted_out & bit) != 0 && (r & (NET_OUT | NET_ERR)) != 0) {
                writable |= bit;
            }
        }
        if (ready != 0 || writable != 0 ||
            (timeout != 0 && (int64_t)efi_uptime_ms() >= until)) {
            break;                  /* it waited as long as it was asked to */
        }
        if (interrupt_check()) {
            wait_ended(began);
            return ERR(EINTR);
        }
        thread_yield();
    }
    wait_ended(began);
    if (readfds != 0) {
        *(uint64_t *)readfds = ready;
    }
    if (writefds != 0) {
        *(uint64_t *)writefds = writable;
    }
    if (exceptfds != 0) {
        *(uint64_t *)exceptfds = 0;
    }
    uint64_t count = 0;

    for (uint64_t bits = ready | writable; bits != 0; bits >>= 1) {
        count += bits & 1;
    }
    return count;
}

/* select takes a timeval and pselect6 a timespec, which are the same two
   numbers; what the second of them counts is finer than this clock. */
static uint64_t sys_select(uint64_t nfds, uint64_t readfds, uint64_t writefds) {
    return wait_ready(nfds, readfds, writefds, arg[3], arg[4], 1000);
}

static uint64_t sys_pselect6(uint64_t nfds, uint64_t readfds, uint64_t writefds) {
    return wait_ready(nfds, readfds, writefds, arg[3], arg[4], 1000000);
}

static uint64_t sys_rename(uint64_t from, uint64_t to, uint64_t c) {
    const char *old_name = user_string(from);
    char kept[FS_NAME_LEN];
    const char *new_name;

    (void)c;
    if (old_name == NULL || strlen(old_name) + 1 > sizeof kept) {
        return ERR(EFAULT);
    }
    /* user_string hands back one buffer's worth of pointer at a time, so the
       first name is copied before the second is asked for. */
    memcpy(kept, old_name, strlen(old_name) + 1);
    new_name = user_string(to);
    if (new_name == NULL) {
        return ERR(EFAULT);
    }
    return fs_errno(fs_rename(kept, new_name));
}

/* How much disk there is and how much of it is spoken for. */
struct statfs {
    int64_t  type, bsize;
    uint64_t blocks, bfree, bavail, files, ffree;
    int32_t  fsid[2];
    int64_t  namelen, frsize, flags, spare[4];
};

/* The same figures whichever file is asked about: there is one filesystem,
   and every path on the machine is on it. */
static uint64_t statfs_fill(uint64_t out) {
    struct statfs *stats = (struct statfs *)out;
    struct fs_stats disk;

    if (!user_range(out, sizeof *stats)) {
        return ERR(EFAULT);
    }
    if (fs_get_stats(&disk) != 0) {
        return ERR(EIO);
    }
    memset(stats, 0, sizeof *stats);
    stats->type = FS_MAGIC;
    stats->bsize = SECTOR_SIZE;
    stats->frsize = SECTOR_SIZE;
    stats->blocks = disk.total;
    stats->bfree = disk.total - disk.used;
    stats->bavail = stats->bfree;
    /* The table grows while there is disk to grow into, so free space is
       as good a count of the files still to come as any. */
    stats->files = disk.files + stats->bfree;
    stats->ffree = stats->bfree;
    stats->namelen = FS_NAME_LEN - 1;
    return 0;
}

static uint64_t sys_statfs(uint64_t path, uint64_t out, uint64_t c) {
    (void)c;
    return user_string(path) == NULL ? ERR(EFAULT) : statfs_fill(out);
}

/* Switching the machine off, or starting it again, the way Linux spells it:
   two magic numbers so that it cannot happen by accident. */
#define REBOOT_MAGIC1 0xFEE1DEADu
#define REBOOT_RESTART 0x01234567u
#define REBOOT_POWER_OFF 0x4321FEDCu

static uint64_t sys_reboot(uint64_t magic1, uint64_t magic2, uint64_t command) {
    (void)magic2;
    if ((uint32_t)magic1 != REBOOT_MAGIC1) {
        return ERR(EINVAL);
    }
    if ((uint32_t)command == REBOOT_RESTART) {
        efi_restart();
    } else if ((uint32_t)command == REBOOT_POWER_OFF) {
        efi_power_off();
    } else {
        return ERR(EINVAL);
    }
    return ERR(EIO);                /* the firmware would not do it */
}

/* The console answers what a terminal is asked: its settings, which it
   keeps, and how wide it is. A file is not a terminal and says so, which is
   what tells a libc reading a script from one apart from a person typing.
   Nothing here has process groups, so the questions about those go
   unanswered - and a shell that asks takes that for "no job control here"
   and carries on without it. */
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403      /* once what is queued has been printed */
#define TCSETSF     0x5404      /* and what was typed thrown away */
#define TCGETS2     0x802C542Au /* the same four, carrying the line speeds */
#define TCSETS2     0x402C542Bu
#define TCSETSW2    0x402C542Cu
#define TCSETSF2    0x402C542Du
#define TIOCGWINSZ  0x5413
#define TIOCGPGRP   0x540F      /* which group of programs the keyboard is for */
#define TIOCSPGRP   0x5410
#define FIONREAD    0x541B
#define FIONBIO     0x5421

struct winsize {
    uint16_t rows, columns, pixel_w, pixel_h;
};

static uint64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t out) {
    struct handle *h = handle_of(fd);

    if (h != NULL && h->start == SOCK_MARK && net != NULL && request != FIONBIO) {
        return net->ioctl(h, request, out);
    }

    if (h != NULL && (h->start == SOCK_MARK || h->start == PIPE_MARK) && request == FIONBIO) {
        if (!user_range(out, 4)) {
            return ERR(EFAULT);
        }
        h->offset = *(int32_t *)out != 0 ? h->offset | O_NONBLOCK : h->offset & ~O_NONBLOCK;
        return 0;
    }
    if (!is_console(fd)) {
        return ERR(ENOTTY);
    }
    switch (request) {
    case TCGETS:
    case TCGETS2: {
        size_t size = request == TCGETS ? TERMIOS_OLD : TERMIOS_NEW;

        if (!user_range(out, size)) {
            return ERR(EFAULT);
        }
        console_get((void *)out, size);
        return 0;
    }
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
    case TCSETS2:
    case TCSETSW2:
    case TCSETSF2: {
        size_t size = request < TCGETS2 ? TERMIOS_OLD : TERMIOS_NEW;

        if (!user_range(out, size)) {
            return ERR(EFAULT);
        }
        console_set((const void *)out, size);
        return 0;
    }
    case TIOCGWINSZ: {
        struct winsize *size = (struct winsize *)out;

        if (!user_range(out, sizeof *size)) {
            return ERR(EFAULT);
        }
        size->rows = (uint16_t)vga_height();
        size->columns = (uint16_t)vga_width();
        size->pixel_w = (uint16_t)vga_pixel_width();
        size->pixel_h = (uint16_t)vga_pixel_height();
        return 0;
    }
    case TIOCGPGRP:
        /* There is one program, so the keyboard is always its: a shell that
           is told so keeps its job control rather than complaining its way
           out of it. */
        if (!user_range(out, 4)) {
            return ERR(EFAULT);
        }
        *(uint32_t *)out = 1;
        return 0;
    case TIOCSPGRP:
        return 0;               /* handing it to the one group it is already */
    case FIONREAD:
        /* Nothing is ever waiting: a key is read when it is asked for. */
        if (!user_range(out, 4)) {
            return ERR(EFAULT);
        }
        *(uint32_t *)out = 0;
        return 0;
    }
    return ERR(ENOTTY);
}

/* Whether a file is there. Nothing here has permissions, so being there is
   the whole of the answer. */
static uint64_t sys_access(uint64_t path, uint64_t mode, uint64_t c) {
    struct fs_file file;
    const char *name = user_string(path);

    (void)mode;
    (void)c;
    if (name == NULL) {
        return ERR(EFAULT);
    }
    if (proc_command(name) != NULL || proc_folder(name) || proc_net_name(name) != NULL ||
        dev_named(name) != 0 || dev_folder(name)) {
        return 0;
    }
    if (fs_stat(name, &file) == 0) {
        return 0;
    }
    return fs_folder_at(name, &(unsigned){ 0 }) == 0 ? 0 : ERR(ENOENT);
}

static uint64_t sys_faccessat(uint64_t dirfd, uint64_t path, uint64_t mode) {
    char joined[FS_NAME_LEN];
    const char *name = at_path(dirfd, user_string(path), joined, sizeof joined);
    struct fs_file file;

    (void)mode;
    if (name == NULL) {
        return ERR(EFAULT);
    }
    if (proc_command(name) != NULL || proc_folder(name) || proc_net_name(name) != NULL ||
        dev_named(name) != 0 || dev_folder(name) || fs_stat(name, &file) == 0) {
        return 0;
    }
    return fs_folder_at(name, &(unsigned){ 0 }) == 0 ? 0 : ERR(ENOENT);
}

/* One processor, and a program may ask which ones it may run on. */
static uint64_t sys_sched_getaffinity(uint64_t pid, uint64_t size, uint64_t mask) {
    (void)pid;
    if (size < 8 || !user_range(mask, 8)) {
        return ERR(EINVAL);
    }
    memset((void *)mask, 0, size < 128 ? size : 128);
    *(uint8_t *)mask = 1;
    return 8;
}

/* Which user a program is, was, and may go back to being - three copies of
   the same answer, since everything here is root. */
static uint64_t sys_getresuid(uint64_t real, uint64_t effective, uint64_t saved) {
    uint64_t of[3] = { real, effective, saved };

    for (unsigned i = 0; i < 3; i++) {
        if (!user_range(of[i], 4)) {
            return ERR(EFAULT);
        }
        *(uint32_t *)of[i] = 0;
    }
    return 0;
}

/* poll, which is select spelled differently: everything but the keyboard
   is ready at once, and that is waited for as long as it is asked. */
struct pollfd {
    int32_t  fd;
    int16_t  events, revents;
};

#define POLLIN  0x001
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010

static uint64_t poll_once(struct pollfd *p, uint64_t count, bool *sockets) {
    uint64_t ready = 0;

    for (uint64_t i = 0; i < count; i++) {
        struct handle *h = handle_of((uint64_t)p[i].fd);

        p[i].revents = 0;
        if (h == NULL) {
            continue;
        }
        /* Reading the console waits for a key, so it is only ready once one
           has been typed, and a socket once a packet has come; everything
           else is there the moment it is asked about. */
        if (h->start == SOCK_MARK) {
            unsigned r = net != NULL ? net->ready((int)h->folder) : NET_ERR;

            *sockets = true;
            p[i].revents = (int16_t)(((p[i].events & POLLIN) && (r & NET_IN) ? POLLIN : 0) |
                                     ((p[i].events & POLLOUT) && (r & NET_OUT) ? POLLOUT : 0) |
                                     (r & NET_ERR ? POLLERR : 0) | (r & NET_HUP ? POLLHUP : 0));
            ready += p[i].revents != 0;
            continue;
        }
        if ((p[i].events & POLLIN) != 0 &&
            (h->start != PIPE_MARK ||
             (h->size != PIPE_EVENT && h->size != PIPE_PAIR && h->size != PIPE_READ) ||
             event_ready(h) || readable(h)) &&
            (!is_console((uint64_t)p[i].fd) || console_ready())) {
            p[i].revents |= POLLIN;
        }
        if ((p[i].events & POLLOUT) != 0) {
            p[i].revents |= POLLOUT;
        }
        ready += p[i].revents != 0;
    }
    return ready;
}

/* Until something is ready, or wait_ms has gone by; a negative wait is for
   ever. */
static uint64_t poll_until(uint64_t fds, uint64_t count, int64_t wait_ms) {
    struct pollfd *p = (struct pollfd *)fds;
    int64_t until = (int64_t)efi_uptime_ms() + wait_ms;
    uint64_t ready;
    bool sockets = false;

    if (!user_range(fds, count * sizeof *p)) {
        return ERR(EFAULT);
    }
    uint64_t began = wait_began();

    while ((ready = poll_once(p, count, &sockets)) == 0 &&
           (wait_ms < 0 || (int64_t)efi_uptime_ms() < until)) {
        if (interrupt_check()) {
            wait_ended(began);
            return ERR(EINTR);
        }
        thread_yield();
    }
    wait_ended(began);
    return ready;
}

static uint64_t sys_poll(uint64_t fds, uint64_t count, uint64_t timeout) {
    return poll_until(fds, count, (int32_t)timeout);
}

/* ppoll's timeout is a timespec, and no timespec is for ever. */
static uint64_t sys_ppoll(uint64_t fds, uint64_t count, uint64_t timeout) {
    const int64_t *spec = (const int64_t *)timeout;

    if (timeout != 0 && !user_range(timeout, 16)) {
        return ERR(EFAULT);
    }
    return poll_until(fds, count, timeout == 0 ? -1 : spec[0] * 1000 + spec[1] / 1000000);
}

/* Capabilities: root has them all. A program that drops some is told it
   did. */
#define CAP_V1 0x19980330u
#define CAP_V2 0x20071026u
#define CAP_V3 0x20080522u

static uint64_t sys_capget(uint64_t header, uint64_t data, uint64_t c) {
    uint32_t *h = (uint32_t *)header;

    (void)c;
    if (!user_range(header, 8)) {
        return ERR(EFAULT);
    }
    if (h[0] != CAP_V1 && h[0] != CAP_V2 && h[0] != CAP_V3) {
        h[0] = CAP_V3;
        return data == 0 ? 0 : ERR(EINVAL);
    }
    if (data != 0) {
        unsigned sets = h[0] == CAP_V1 ? 1 : 2;
        uint32_t *d = (uint32_t *)data;

        if (!user_range(data, sets * 12)) {
            return ERR(EFAULT);
        }
        for (unsigned i = 0; i < sets; i++) {
            d[3 * i] = d[3 * i + 1] = 0xFFFFFFFF;   /* effective, permitted */
            d[3 * i + 2] = 0;                       /* inheritable */
        }
    }
    return 0;
}

#define PR_CAPBSET_READ 23
#define CAP_LAST 40

static uint64_t sys_prctl(uint64_t option, uint64_t value, uint64_t c) {
    (void)c;
    if (option == PR_CAPBSET_READ) {
        return value <= CAP_LAST ? 1 : ERR(EINVAL);
    }
    return 0;
}

/* Resource limits: the stack is the window's top end, and nothing else is
   limited. new_limit is refused, since nothing here would honour it. */
#define RLIMIT_NOFILE 7

static uint64_t sys_prlimit64(uint64_t pid, uint64_t resource, uint64_t new_limit) {
    uint64_t *old = (uint64_t *)arg[3];

    (void)pid;
    if (new_limit != 0) {
        return ERR(EINVAL);
    }
    if (old != NULL) {
        if (!user_range(arg[3], 16)) {
            return ERR(EFAULT);
        }
        /* How many files may be open matters: a libc moving a descriptor out
           of the way puts it just under this, and one told it may have
           millions would ask for a descriptor this kernel has no room for. */
        old[0] = resource == RLIMIT_NOFILE ? PROGRAM_FILES
               : USER_STACK_BYTES;
        old[1] = (uint64_t)-1;      /* RLIM64_INFINITY */
    }
    return 0;
}

/* Renaming from two folders a program already has open. renameat2's flags -
   refusing to replace, swapping the two - are not offered: a shell's mv asks
   for them, finds they are not there, and falls back to asking plainly. */
static uint64_t sys_renameat(uint64_t olddir, uint64_t oldpath, uint64_t newdir) {
    char joined[FS_NAME_LEN], kept[FS_NAME_LEN];
    const char *old_name = at_path(olddir, user_string(oldpath), joined, sizeof joined);
    const char *new_name;

    if (old_name == NULL || strlen(old_name) + 1 > sizeof kept) {
        return ERR(EFAULT);
    }
    memcpy(kept, old_name, strlen(old_name) + 1);
    new_name = at_path(newdir, user_string(arg[3]), joined, sizeof joined);
    if (new_name == NULL) {
        return ERR(EFAULT);
    }
    return fs_errno(fs_rename(kept, new_name));
}

/* Extended attributes, which this filesystem does not have: what Linux
   answers for one without them, and ls -l takes in its stride. */
static uint64_t sys_no_xattr(uint64_t a, uint64_t b, uint64_t c) {
    (void)a;
    (void)b;
    (void)c;
    return ERR(EOPNOTSUPP);
}

/* A hard link, or a device node: there are neither here, and a program told
   so goes on to copy rather than stopping. */
static uint64_t sys_no_links(uint64_t a, uint64_t b, uint64_t c) {
    (void)a;
    (void)b;
    (void)c;
    return ERR(EPERM);
}

/* A FIFO, though, is an empty file that says so in its mode: opened, it is
   a pipe, the same one for everyone opening it. */
static uint64_t sys_mknodat(uint64_t dirfd, uint64_t path, uint64_t mode) {
    char joined[FS_NAME_LEN];
    const char *name = at_path(dirfd, user_string(path), joined, sizeof joined);
    struct fs_file file;
    int err;

    if (name == NULL) {
        return ERR(EFAULT);
    }
    if ((mode & S_IFMT) != S_IFIFO) {
        return ERR(EPERM);
    }
    if (fs_stat(name, &file) == 0) {
        return ERR(EEXIST);
    }
    err = fs_write(name, NULL, 0);
    if (err == 0) {
        err = fs_set_mode(name, (unsigned)(S_IFIFO | (mode & 0755)));
    }
    return fs_errno(err);
}

/* Emptying a file by name, which is the only length anything truncates one
   to here. */
static uint64_t sys_truncate(uint64_t path, uint64_t length, uint64_t c) {
    const char *name = user_string(path);

    (void)c;
    if (name == NULL) {
        return ERR(EFAULT);
    }
    if (length != 0) {
        return ERR(EINVAL);
    }
    return fs_write(name, NULL, 0) < 0 ? ERR(EIO) : 0;
}

/* Nothing here has a /proc to read a link out of. */
/* The older pair, which name the resource and the place to put it rather
   than a process as well. */
static uint64_t sys_getrlimit(uint64_t resource, uint64_t out, uint64_t c) {
    uint64_t kept = arg[3];
    uint64_t result;

    (void)c;
    arg[3] = out;
    result = sys_prlimit64(0, resource, 0);
    arg[3] = kept;
    return result;
}

static uint64_t sys_fstatfs(uint64_t fd, uint64_t out, uint64_t c) {
    (void)c;
    return handle_of(fd) == NULL ? ERR(EBADF) : statfs_fill(out);
}

/* ---- symbolic links ------------------------------------------------------
 *
 * A link is a file holding a path, and the filesystem follows it wherever a
 * path is resolved - /bin is one, to usr/bin. These are the calls that make
 * one and read one back, and neither follows the link it is given. */

static uint64_t sys_symlinkat(uint64_t target, uint64_t dirfd, uint64_t path) {
    char kept[FS_LINK_LEN], joined[FS_NAME_LEN];
    const char *to = user_string(target);
    const char *name;

    if (to == NULL || strlen(to) + 1 > sizeof kept) {
        return ERR(to == NULL ? EFAULT : EINVAL);
    }
    strcpy(kept, to);
    name = at_path(dirfd, user_string(path), joined, sizeof joined);
    if (name == NULL) {
        return ERR(EFAULT);
    }
    return fs_errno(fs_symlink(kept, name));
}

static uint64_t sys_symlink(uint64_t target, uint64_t path, uint64_t c) {
    (void)c;
    return sys_symlinkat(target, (uint64_t)AT_FDCWD, path);
}

/* Linux's readlink writes no NUL, and returns how many bytes it did write. */
static uint64_t sys_readlinkat(uint64_t dirfd, uint64_t path, uint64_t buf) {
    char joined[FS_NAME_LEN];
    const char *name = at_path(dirfd, user_string(path), joined, sizeof joined);
    uint64_t size = arg[3];
    int got;

    if (name == NULL || !user_range(buf, size)) {
        return ERR(EFAULT);
    }
    got = fs_readlink(name, (char *)buf, size);
    return got < 0 ? fs_errno(got) : (uint64_t)got;
}

static uint64_t sys_readlink(uint64_t path, uint64_t buf, uint64_t size) {
    uint64_t kept = arg[3], result;

    arg[3] = size;
    result = sys_readlinkat((uint64_t)AT_FDCWD, path, buf);
    arg[3] = kept;
    return result;
}

/* ---- describing files ---------------------------------------------------- */


struct stat {
    uint64_t dev, ino, nlink;
    uint32_t mode, uid, gid, pad;
    uint64_t rdev, size;
    int64_t  blksize, blocks;
    int64_t  times[6];
    int64_t  unused[3];
};

_Static_assert(sizeof(struct stat) == 144, "struct stat is what Linux's is");

/* A file's number, which has to differ from every other file's: a loader
   decides whether it has already loaded a library by comparing the device
   and inode of the file against the ones it holds, and would take two
   different libraries for the same one if they shared a number. The first
   sector serves, since no two files start in the same place. */
/* The number a folder is known by, from its entry in the table, counting
   from one. Nought is the root, which has no entry of its own - so every
   folder's number is one past its entry's, leaving 1 for the root. A file
   goes by its first sector instead, and those start far higher than the
   table has entries, so the two can never collide.

   Getting this wrong is not a small thing: a program walking a tree takes
   two folders with one number for a loop, and stops. */


/* Whether a table entry is a folder: the filesystem spells one with a slash
   on the end, and gives it no first sector - so the entry alone cannot be
   told from an empty file without looking at the name. */
static bool is_folder_entry(const struct fs_file *file) {
    size_t n = strlen(file->name);

    return n > 0 && file->name[n - 1] == '/';
}

/* A folder's permission bits, by its number: 0755 unless it was given
   others. */
static unsigned folder_mode(uint64_t ino) {
    struct fs_file entry;

    return ino > 1 && ino < PROC_INO && fs_file(ino - 2, &entry) == 0 &&
           (entry.start & FS_MODE) != 0 ? entry.start & 07777 : 0755;
}

static void fill_stat(struct stat *out, uint64_t size, bool folder, uint32_t start) {
    memset(out, 0, sizeof *out);
    out->dev = 1;
    out->ino = start != 0 ? start : 1;
    out->nlink = 1;
    out->mode = (folder ? S_IFDIR | folder_mode(out->ino) : S_IFREG | 0644);
    out->size = size;
    out->blksize = SECTOR_SIZE;
    out->blocks = (int64_t)((size + 511) / 512);
}

/* What a descriptor is, for whoever is asking: fstat, and a statx of an
   empty path, which is what a libc's fstat has become. */
static uint64_t stat_of_handle(uint64_t fd, struct stat *st) {
    struct handle *h = handle_of(fd);

    if (h == NULL) {
        return ERR(EBADF);
    }
    /* A folder is numbered by its table entry, one-based - the same number
       getdents64 and newfstatat give it, so that a program walking a tree
       can tell one folder from another. */
    fill_stat(st, h->size, h->start == FOLDER_MARK || h->start == PROCDIR_MARK,
              h->start == FOLDER_MARK ? folder_ino(h->folder) :
              h->start == PROC_MARK ? PROC_INO + h->folder :
              h->start == PROCDIR_MARK ? PROC_INO :
              h->start == WRITE_MARK ? file_ino(writer_names[h->writer - 1], true) :
              h->start < FIRST_MARK && h->folder != 0 ? folder_ino(h->folder) : h->start);
    if (h->start == CONSOLE_MARK) {
        /* Not a file at all: a program told this is a regular file reads it
           as one, all at once and to its end. */
        st->mode = S_IFCHR | 0620;
        st->size = 0;
        st->blocks = 0;
        st->rdev = 0x0500 | fd;     /* a terminal, as Linux numbers them */
    } else if (h->start == PIPE_MARK) {
        struct pipe *p = pipe_of(h);

        st->mode = h->size == PIPE_FILE ? S_IFREG | 0444 :
                   h->size == PIPE_PAIR ? S_IFSOCK | 0777 : S_IFIFO | 0600;
        st->size = p == NULL ? 0 : pipe_left(p);
        st->blocks = 0;
    } else if (h->start == SOCK_MARK) {
        st->mode = S_IFSOCK | 0777;
        st->size = 0;
        st->blocks = 0;
    } else if (h->start == DEV_MARK) {
        st->mode = S_IFCHR | 0666;
        st->size = 0;
        st->blocks = 0;
        st->rdev = 0x0103;
    }
    return 0;
}

static uint64_t sys_fstat(uint64_t fd, uint64_t out, uint64_t c) {
    (void)c;
    if (!user_range(out, sizeof(struct stat))) {
        return ERR(EFAULT);
    }
    return stat_of_handle(fd, (struct stat *)out);
}

/* The newer stat, which takes the same answers in a different shape. */
struct statx_timestamp {
    int64_t  seconds;
    uint32_t nanoseconds, pad;
};

struct statx {
    uint32_t mask, blksize;
    uint64_t attributes;
    uint32_t nlink, uid, gid;
    uint16_t mode, pad;
    uint64_t ino, size, blocks, attributes_mask;
    struct statx_timestamp atime, btime, ctime, mtime;
    uint32_t rdev_major, rdev_minor, dev_major, dev_minor;
    uint64_t rest[14];
};

#define STATX_BASIC 0x7ff

static uint64_t sys_statx(uint64_t dirfd, uint64_t path, uint64_t flags) {
    struct statx *out = (struct statx *)arg[4];
    char joined[FS_NAME_LEN];
    const char *given = user_string(path);
    const char *name = at_path(dirfd, given, joined, sizeof joined);
    struct fs_file file;
    bool folder = false;

    if (name == NULL || !user_range(arg[4], sizeof *out)) {
        return ERR(EINVAL);
    }
    /* An empty path is the descriptor itself - which is what a libc's fstat
       has become, so this is the common case rather than a corner of one. */
    if (given != NULL && given[0] == '\0') {
        struct stat st;
        uint64_t err = stat_of_handle(dirfd, &st);

        if ((int64_t)err < 0) {
            return err;
        }
        memset(out, 0, sizeof *out);
        out->mask = STATX_BASIC;
        out->blksize = (uint32_t)st.blksize;
        out->nlink = 1;
        out->mode = st.mode;
        out->ino = st.ino;
        out->size = st.size;
        out->blocks = (uint64_t)st.blocks;
        out->rdev_minor = (uint32_t)(st.rdev & 0xFF);
        out->dev_minor = 1;
        return 0;
    }
    const struct proc_cmd *cmd = proc_command(name);
    enum dev which = dev_named(name);

    if (cmd != NULL || proc_folder(name) || which != 0 || dev_folder(name)) {
        memset(out, 0, sizeof *out);
        out->mask = STATX_BASIC;
        out->blksize = SECTOR_SIZE;
        out->nlink = 1;
        out->mode = (uint16_t)(which != 0 ? S_IFCHR | 0666 :
                               cmd != NULL ? S_IFREG | 0755 : S_IFDIR | 0755);
        out->ino = PROC_INO;
        out->size = cmd != NULL ? proc_read(cmd, 0, NULL, 0) : 0;
        out->dev_minor = 1;
        return 0;
    }
    if ((flags & AT_SYMLINK_NOFOLLOW) != 0 && fs_lstat(name, &file) == 0 &&
        (file.size & FS_LINK) != 0) {
        memset(out, 0, sizeof *out);
        out->mask = STATX_BASIC;
        out->blksize = SECTOR_SIZE;
        out->nlink = 1;
        out->mode = S_IFLNK | 0777;
        out->ino = file_ino(name, false);
        out->size = file.size & ~FS_LINK;
        out->blocks = 1;
        out->dev_minor = 1;
        return 0;
    }
    int err = fs_stat(name, &file);

    if (err != 0 || is_folder_entry(&file)) {
        unsigned index = 0;

        if (err == FS_ELOOP) {
            return ERR(ELOOP);
        }
        if (fs_folder_at(name, &index) != 0) {
            return ERR(ENOENT);
        }
        file = (struct fs_file){ .start = folder_ino(index) };
        folder = true;
    }
    memset(out, 0, sizeof *out);
    out->mask = STATX_BASIC;
    out->blksize = SECTOR_SIZE;
    out->nlink = 1;
    out->mode = (uint16_t)(folder ? S_IFDIR | folder_mode(file.start) :
                           is_fifo(&file) ? file.start : S_IFREG | 0644);
    out->ino = folder ? file.start : file_ino(name, true);
    out->size = folder ? 0 : file.size;
    out->blocks = (file.size + 511) / 512;
    out->dev_minor = 1;
    return 0;
}

static uint64_t sys_newfstatat(uint64_t dirfd, uint64_t path, uint64_t out) {
    struct fs_file file;
    char joined[FS_NAME_LEN];
    const char *name = at_path(dirfd, user_string(path), joined, sizeof joined);

    if (name == NULL || !user_range(out, sizeof(struct stat))) {
        return ERR(EFAULT);
    }
    /* An empty name with AT_EMPTY_PATH is the descriptor itself. */
    if (name[0] == '\0') {
        return sys_fstat(dirfd, out, 0);
    }
    const struct proc_cmd *cmd = proc_command(name);

    if (cmd != NULL) {
        fill_stat((struct stat *)out, proc_read(cmd, 0, NULL, 0), false,
                  PROC_INO + 1);
        ((struct stat *)out)->mode = S_IFREG | 0755;    /* it is run, not read */
        return 0;
    }
    if (proc_folder(name)) {
        fill_stat((struct stat *)out, 0, true, PROC_INO);
        return 0;
    }
    if (proc_net_name(name) != NULL) {
        fill_stat((struct stat *)out, 0, false, PROC_INO + 2);
        ((struct stat *)out)->mode = S_IFREG | 0444;
        return 0;
    }
    if (dev_named(name) != 0) {
        fill_stat((struct stat *)out, 0, false, PROC_INO);
        ((struct stat *)out)->mode = S_IFCHR | 0666;
        ((struct stat *)out)->rdev = 0x0103;     /* what Linux calls /dev/null */
        return 0;
    }
    if (dev_folder(name)) {
        fill_stat((struct stat *)out, 0, true, PROC_INO);
        return 0;
    }
    if ((arg[3] & AT_SYMLINK_NOFOLLOW) != 0 && fs_lstat(name, &file) == 0 &&
        (file.size & FS_LINK) != 0) {
        fill_stat((struct stat *)out, file.size & ~FS_LINK, false, file_ino(name, false));
        ((struct stat *)out)->mode = S_IFLNK | 0777;
        return 0;
    }
    int err = fs_stat(name, &file);

    if (err == 0 && !is_folder_entry(&file)) {
        fill_stat((struct stat *)out, file.size, false, file_ino(name, true));
        if (is_fifo(&file)) {
            ((struct stat *)out)->mode = file.start & 0177777;
        }
        return 0;
    }
    if (err == FS_ELOOP) {
        return ERR(ELOOP);
    }
    /* Not a file: a folder, which the filesystem knows by its own spelling -
       and which is how the root, having no entry of its own, is one. */
    unsigned folder = 0;

    if (fs_folder_at(name, &folder) != 0) {
        /* Nothing of that name. -1 on its own is EPERM, and a program told
           that reports the file as one it is not allowed to read rather than
           one that is not there. */
        return ERR(ENOENT);
    }
    /* Its table entry, one-based, which is the number getdents64 gives it
       too: a program walking a tree compares the two, and one that cannot
       tell two folders apart takes the second for a loop and stops. */
    fill_stat((struct stat *)out, 0, true, folder_ino(folder));
    return 0;
}

/* The older pair, which name a file rather than a descriptor and a name, and
   differ only in whether a link at the end is followed. */
static uint64_t stat_flags(uint64_t path, uint64_t out, uint64_t flags) {
    uint64_t kept = arg[3], result;

    arg[3] = flags;
    result = sys_newfstatat((uint64_t)AT_FDCWD, path, out);
    arg[3] = kept;
    return result;
}

static uint64_t sys_stat(uint64_t path, uint64_t out, uint64_t c) {
    (void)c;
    return stat_flags(path, out, 0);
}

static uint64_t sys_lstat(uint64_t path, uint64_t out, uint64_t c) {
    (void)c;
    return stat_flags(path, out, AT_SYMLINK_NOFOLLOW);
}

/* ---- what is in a folder -------------------------------------------------
 *
 * The table holds whole paths, so a folder is read by walking every entry
 * and keeping the ones that lie directly inside it. The descriptor remembers
 * how far the walk got, which is what makes repeated calls pick up where the
 * last left off, as Linux's do. */

#define DT_CHR 2
#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10

struct dirent64 {
    uint64_t ino;
    int64_t  off;
    uint16_t reclen;
    uint8_t  type;
    char     name[];
};

/* /ctl, which holds one entry per built-in command and nothing else. */
static uint64_t proc_dents(struct handle *h, uint64_t buf, uint64_t count) {
    uint64_t used = 0;

    for (const struct proc_cmd *cmd; (cmd = proc_at(h->offset)) != NULL; h->offset++) {
        size_t length = strlen(cmd->name);
        uint64_t reclen = (sizeof(struct dirent64) + length + 1 + 7) & ~7ull;

        if (used + reclen > count) {
            break;                  /* the rest waits for the next call */
        }
        struct dirent64 *out = (struct dirent64 *)(buf + used);

        out->ino = PROC_INO + h->offset + 1;
        out->off = (int64_t)(h->offset + 1);
        out->reclen = (uint16_t)reclen;
        out->type = DT_REG;
        memcpy(out->name, cmd->name, length + 1);
        used += reclen;
    }
    return used;
}

/* /dev, from its table: names as the folder shows them, without "/dev/". */
static uint64_t dev_dents(struct handle *h, uint64_t buf, uint64_t count) {
    uint64_t used = 0;

    for (; h->offset < DEVICES; h->offset++) {
        const char *name = devices[h->offset].name + sizeof "/dev/" - 1;
        size_t length = strlen(name);
        uint64_t reclen = (sizeof(struct dirent64) + length + 1 + 7) & ~7ull;

        if (used + reclen > count) {
            break;                  /* the rest waits for the next call */
        }
        struct dirent64 *out = (struct dirent64 *)(buf + used);

        out->ino = DEV_INO + h->offset + 1;
        out->off = (int64_t)(h->offset + 1);
        out->reclen = (uint16_t)reclen;
        out->type = DT_CHR;
        memcpy(out->name, name, length + 1);
        used += reclen;
    }
    return used;
}

static uint64_t sys_getdents64(uint64_t fd, uint64_t buf, uint64_t count) {
    struct handle *h = handle_of(fd);
    struct fs_file folder, entry;
    uint64_t used = 0;

    if (h == NULL || !user_range(buf, count)) {
        return ERR(h == NULL ? EBADF : EFAULT);
    }
    if (h->start == PROCDIR_MARK) {
        return h->folder != 0 ? dev_dents(h, buf, count) : proc_dents(h, buf, count);
    }
    if (h->start != FOLDER_MARK) {
        return ERR(ENOTDIR);
    }
    if (h->folder == 0) {
        folder.name[0] = '\0';      /* the root, which has no entry */
    } else if (fs_file(h->folder - 1, &folder) != 0) {
        return ERR(EBADF);
    }
    for (;;) {
        size_t next = h->offset, index;

        if (fs_list(folder.name, &next, &entry, &index) != 0) {
            break;
        }
        const char *leaf = fs_inside(folder.name, entry.name);
        size_t length = strlen(leaf);
        bool is_folder = leaf[length - 1] == '/';
        uint64_t reclen = (sizeof(struct dirent64) + length + 1 + 7) & ~7ull;

        if (used + reclen > count) {
            break;                  /* the rest waits for the next call */
        }
        struct dirent64 *out = (struct dirent64 *)(buf + used);
        out->ino = folder_ino((unsigned)index + 1);    /* a file's as a folder's */
        out->off = (int64_t)next;
        out->reclen = (uint16_t)reclen;
        out->type = is_folder ? DT_DIR : (entry.size & FS_LINK) != 0 ? DT_LNK : DT_REG;
        memcpy(out->name, leaf, length);
        out->name[is_folder ? length - 1 : length] = '\0';

        used += reclen;
        h->offset = (uint32_t)next;
    }
    return used;
}

/* ---- the machine, and the time ------------------------------------------- */

/* What Linux tells a program about the machine as a whole. Only the figures
   this machine actually has are filled in - how long it has been up, and how
   much RAM there is and is left - which is what anything asking wants. */
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

_Static_assert(sizeof(struct sysinfo) == 112, "struct sysinfo is what Linux's is");

static uint64_t sys_sysinfo(uint64_t out, uint64_t b, uint64_t c) {
    struct sysinfo *info = (struct sysinfo *)out;
    struct mem_stats m;

    (void)b;
    (void)c;
    if (!user_range(out, sizeof *info)) {
        return ERR(EFAULT);
    }
    mem_get_stats(&m);
    memset(info, 0, sizeof *info);
    info->uptime = (int64_t)(efi_uptime_ms() / 1000);
    info->totalram = (uint64_t)m.total_kib * 1024;
    info->freeram = (uint64_t)m.free_kib * 1024;
    info->procs = 1;
    info->unit = 1;
    return 0;
}

static uint64_t sys_uname(uint64_t out, uint64_t b, uint64_t c) {
    static const char *const fields[] = { "Tuxlet", "tuxlet", "1", "1", "x86_64", "" };
    char *field = (char *)out;

    (void)b;
    (void)c;
    if (!user_range(out, 6 * 65)) {
        return ERR(EFAULT);
    }
    for (unsigned i = 0; i < 6; i++, field += 65) {
        memset(field, 0, 65);
        memcpy(field, fields[i], strlen(fields[i]));
    }
    return 0;
}

/* CLOCK_MONOTONIC and its kin count from when the machine started; the rest
   count from 1970. Seconds is all the firmware's clock offers either way,
   except for the monotonic ones, where the loader's own millisecond count
   can do better. */

static uint64_t sys_clock_gettime(uint64_t clock, uint64_t out, uint64_t c) {
    int64_t *spec = (int64_t *)out;

    (void)c;
    if (!user_range(out, 16)) {
        return ERR(EFAULT);
    }
    /* The realtime ones - REALTIME, its coarse twin, TAI - count from 1970;
       the CPU-time ones, what the program has used; the rest - MONOTONIC,
       BOOTTIME and their variants - from boot. */
    uint64_t us = clock == 0 || clock == 5 || clock == 11 ? realtime_us()
                : clock == 2 || clock == 3 ? self_us()
                : efi_uptime_us();

    spec[0] = (int64_t)(us / 1000000);
    spec[1] = (int64_t)(us % 1000000) * 1000;
    return 0;
}

/* Every clock counts in microseconds. The timespec may be NULL. */
static uint64_t sys_clock_getres(uint64_t clock, uint64_t out, uint64_t c) {
    (void)clock;
    (void)c;
    if (out == 0) {
        return 0;
    }
    if (!user_range(out, 16)) {
        return ERR(EFAULT);
    }
    ((int64_t *)out)[0] = 0;
    ((int64_t *)out)[1] = 1000;
    return 0;
}

/* The time of day, in seconds since 1970 - the firmware's clock, which is
   all there is. gettimeofday takes a timezone as well, and is handed the
   one everything here keeps: none at all. */
static uint64_t sys_gettimeofday(uint64_t tv, uint64_t tz, uint64_t c) {
    int64_t *out = (int64_t *)tv;

    (void)tz;
    (void)c;
    if (tv == 0) {
        return 0;
    }
    if (!user_range(tv, 16)) {
        return ERR(EFAULT);
    }
    uint64_t us = realtime_us();

    out[0] = (int64_t)(us / 1000000);
    out[1] = (int64_t)(us % 1000000);
    return 0;
}

/* Waits until the clock reaches until_ms. There is nothing else to run, so
   the waiting is the whole of it. */
static void sleep_until(uint64_t until_ms) {
    uint64_t began = wait_began();

    while (efi_uptime_ms() < until_ms && !interrupt_check()) {
        thread_yield();
        __asm__ volatile("pause");
    }
    wait_ended(began);
}


static uint64_t sys_nanosleep(uint64_t req, uint64_t rem, uint64_t c) {
    (void)c;
    if (!user_range(req, 16) || (rem != 0 && !user_range(rem, 16))) {
        return ERR(EFAULT);
    }
    sleep_until(efi_uptime_ms() + timespec_ms(req));
    if (rem != 0) {
        memset((void *)rem, 0, 16);
    }
    return deliverable() != 0 ? ERR(EINTR) : 0;    /* cut short by a signal */
}

#define TIMER_ABSTIME 1

/* The same, on a named clock, and with TIMER_ABSTIME until a time on it
   rather than for a while. */
static uint64_t sys_clock_nanosleep(uint64_t clock, uint64_t flags, uint64_t req) {
    uint64_t rem = arg[3];
    uint64_t want;

    if (!user_range(req, 16) || (rem != 0 && !user_range(rem, 16))) {
        return ERR(EFAULT);
    }
    want = timespec_ms(req);
    if (flags & TIMER_ABSTIME) {
        uint64_t now = clock == 0 || clock == 5 || clock == 11 ? realtime_ms()
                     : efi_uptime_ms();

        want = want > now ? want - now : 0;
    }
    sleep_until(efi_uptime_ms() + want);
    if (rem != 0) {
        memset((void *)rem, 0, 16);
    }
    return deliverable() != 0 ? ERR(EINTR) : 0;    /* cut short by a signal */
}

static uint64_t sys_time(uint64_t out, uint64_t b, uint64_t c) {
    int64_t now = (int64_t)(realtime_ms() / 1000);

    (void)b;
    (void)c;
    if (out != 0) {
        if (!user_range(out, 8)) {
            return ERR(EFAULT);
        }
        *(int64_t *)out = now;
    }
    return (uint64_t)now;
}


/* ---- the module ----------------------------------------------------------- */

static const uint16_t numbers[] = {
    SYS_FSTAT,
    SYS_IOCTL,
    SYS_UNAME,
    SYS_SYSINFO,
    SYS_GETRUSAGE,
    SYS_TIMES,
    SYS_UTIMENSAT,
    SYS_UTIMES,
    SYS_FUTIMESAT,
    SYS_CHMOD,
    SYS_FCHMOD,
    SYS_FCHMODAT,
    SYS_CHOWN,
    SYS_FCHOWN,
    SYS_LCHOWN,
    SYS_FCHOWNAT,
    SYS_FADVISE64,
    SYS_CLOCK_NANOSLEEP,
    SYS_GETDENTS64,
    SYS_PRLIMIT64,
    SYS_READLINKAT,
    SYS_CLOCK_GETTIME,
    SYS_CLOCK_GETRES,
    SYS_RENAME,
    SYS_STATFS,
    SYS_REBOOT,
    SYS_ACCESS,
    SYS_FACCESSAT,
    SYS_FACCESSAT2,
    SYS_STATX,
    SYS_SCHED_GETAFFINITY,
    SYS_NANOSLEEP,
    SYS_NEWFSTATAT,
    SYS_STAT,
    SYS_LSTAT,
    SYS_GETTIMEOFDAY,
    SYS_TIME,
    SYS_READLINK,
    SYS_GETRESUID,
    SYS_GETRESGID,
    SYS_SELECT,
    SYS_PSELECT6,
    SYS_RENAMEAT,
    SYS_RENAMEAT2,
    SYS_LINKAT,
    SYS_SYMLINKAT,
    SYS_LINK,
    SYS_SYMLINK,
    SYS_MKNODAT,
    SYS_TRUNCATE,
    SYS_POLL,
    SYS_PPOLL,
    SYS_FSTATFS,
    SYS_GETRLIMIT,
    SYS_SETRLIMIT,
    SYS_FLOCK,
    SYS_FALLOCATE,
    SYS_MSYNC,
    SYS_GETCPU,
    SYS_GETGROUPS,
    SYS_SCHED_GETSCHEDULER,
    SYS_SCHED_SETSCHEDULER,
    SYS_SCHED_GETPARAM,
    SYS_GETPRIORITY,
    SYS_SETPRIORITY,
    SYS_CAPGET,
    SYS_CAPSET,
    SYS_PRCTL,
    SYS_SETUID,
    SYS_SETGID,
    SYS_SETRESUID,
    SYS_SETRESGID,
    SYS_SETGROUPS,
};

static const syscall_fn handlers[] = {
    sys_fstat,
    sys_ioctl,
    sys_uname,
    sys_sysinfo,
    sys_getrusage,
    sys_times,
    sys_ok,
    sys_ok,
    sys_ok,
    sys_ok,
    sys_ok,
    sys_ok,
    sys_ok,
    sys_ok,
    sys_ok,
    sys_ok,
    sys_ok,
    sys_clock_nanosleep,
    sys_getdents64,
    sys_prlimit64,
    sys_readlinkat,
    sys_clock_gettime,
    sys_clock_getres,
    sys_rename,
    sys_statfs,
    sys_reboot,
    sys_access,
    sys_faccessat,
    sys_faccessat,
    sys_statx,
    sys_sched_getaffinity,
    sys_nanosleep,
    sys_newfstatat,
    sys_stat,
    sys_lstat,
    sys_gettimeofday,
    sys_time,
    sys_readlink,
    sys_getresuid,
    sys_getresuid,
    sys_select,
    sys_pselect6,
    sys_renameat,
    sys_renameat,
    sys_no_links,
    sys_symlinkat,
    sys_no_links,
    sys_symlink,
    sys_mknodat,
    sys_truncate,
    sys_poll,
    sys_ppoll,
    sys_fstatfs,
    sys_getrlimit,
    sys_ok,
    sys_ok,
    sys_ok,
    sys_ok,
    sys_zeroed,
    sys_root,
    sys_root,
    sys_ok,
    sys_zeroed,
    sys_root,
    sys_ok,
    sys_capget,
    sys_ok,
    sys_prctl,
    sys_ok,
    sys_ok,
    sys_ok,
    sys_ok,
    sys_ok,
};

static syscall_fn find(uint64_t number) {
    if (number >= SYS_SETXATTR && number <= SYS_FREMOVEXATTR) {
        return sys_no_xattr;        /* all twelve of them, one answer */
    }
    for (unsigned i = 0; i < sizeof numbers / sizeof numbers[0]; i++) {
        if (numbers[i] == number) {
            return handlers[i];
        }
    }
    return NULL;
}

MODULE_EXPORT int module_init(void) {
    linux_register(find);
    return 0;
}

MODULE_EXPORT int module_exit(void) {
    linux_register(NULL);
    return 0;
}
