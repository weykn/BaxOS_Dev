/* ipc/pty: pseudo-terminals - /dev/ptmx and /dev/pts/N.
 *
 * A terminal emulator (xterm) opens /dev/ptmx and gets the master end; the
 * shell it starts gets /dev/pts/N, the slave, as its terminal. What the
 * master writes is typed at the slave, and what the slave writes is what the
 * master reads to draw. In between is a terminal's line discipline, as
 * Linux's: lines gathered and edited before a read sees them, typing echoed,
 * Ctrl-C a signal to the terminal's foreground group, \n written as \r\n.
 *
 * Each costs its two queues while it is open, and nothing once it is closed. */

#include <stdbool.h>
#include <stddef.h>

#include "linux.h"
#include "mem.h"
#include "module.h"
#include "net.h"
#include "string.h"
#include "syscall.h"
#include "thread.h"

#define EBUSY 16

#define PTYS   8
#define QUEUE  4096                 /* bytes waiting each way */
#define LINE   256                  /* a line being edited */
#define SLAVE  0x40000000u          /* in a descriptor's offset: the slave end */

#define SIGINT   2
#define SIGQUIT  3
#define SIGHUP   1
#define SIGWINCH 28

/* struct termios2, as Linux has it on the wire. */
struct termios {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t  line, cc[19];
    uint32_t ispeed, ospeed;
};

#define ICRNL  0x100
#define IXON   0x400
#define OPOST  0x1
#define ONLCR  0x4
#define ISIG   0x1
#define ICANON 0x2
#define ECHO   0x8
#define ECHOE  0x10
#define ECHOK  0x20
#define ECHOCTL 0x200
#define ECHOKE 0x800
#define IEXTEN 0x8000

enum { VINTR, VQUIT, VERASE, VKILL, VEOF, VTIME, VMIN, VSWTC, VSTART, VSTOP, VSUSP,
       VEOL, VREPRINT, VDISCARD, VWERASE, VLNEXT, VEOL2 };

struct winsize {
    uint16_t rows, cols, xpixel, ypixel;
};

static struct pty {
    unsigned masters, slaves;       /* descriptors on each end */
    bool     opened;                /* the slave has been, so its going is a hang-up */
    bool     locked;                /* until unlockpt */
    bool     eof;                   /* Ctrl-D on an empty line: the next read is 0 */
    int      pgrp, sid;             /* its foreground group and its session */
    struct termios t;
    struct winsize size;
    unsigned in_len, out_len, line_len;
    char     in[QUEUE];             /* for the slave to read */
    char     out[QUEUE];            /* for the master to read */
    char     line[LINE];            /* gathered, not yet a line */
} *ptys[PTYS];

static unsigned slot;               /* ours, in the kernel's file_ops */

static struct pty *pty_of(const struct handle *h) {
    return h->folder >= 1 && h->folder <= PTYS ? ptys[h->folder - 1] : NULL;
}

static bool is_slave(const struct handle *h) {
    return (h->offset & SLAVE) != 0;
}

/* ---- the line discipline ------------------------------------------------ */

static void out_put(struct pty *p, char c) {
    if (p->out_len < QUEUE) {
        p->out[p->out_len++] = c;
    }
}

/* What the slave writes, or what is echoed, on its way to the master. */
static void output(struct pty *p, char c) {
    if ((p->t.oflag & OPOST) != 0 && (p->t.oflag & ONLCR) != 0 && c == '\n') {
        out_put(p, '\r');
    }
    out_put(p, c);
}

static void echo(struct pty *p, char c) {
    if ((p->t.lflag & ECHO) == 0) {
        return;
    }
    if ((p->t.lflag & ECHOCTL) != 0 && (uint8_t)c < 0x20 && c != '\n' && c != '\t') {
        output(p, '^');
        output(p, (char)(c + '@'));
        return;
    }
    output(p, c);
}

static void erase_one(struct pty *p) {
    if (p->line_len == 0) {
        return;
    }
    p->line_len--;
    if ((p->t.lflag & ECHO) != 0 && (p->t.lflag & ECHOE) != 0) {
        output(p, '\b');
        output(p, ' ');
        output(p, '\b');
    }
}

/* The gathered line, or what there is of it, handed to the slave. */
static void line_done(struct pty *p) {
    unsigned n = p->line_len < QUEUE - p->in_len ? p->line_len : QUEUE - p->in_len;

    memcpy(p->in + p->in_len, p->line, n);
    p->in_len += n;
    p->line_len = 0;
}

/* One character typed at the slave: what the master writes. */
static void input(struct pty *p, char c) {
    const uint8_t *cc = p->t.cc;

    if (c == '\r' && (p->t.iflag & ICRNL) != 0) {
        c = '\n';
    }
    if ((p->t.lflag & ISIG) != 0 && (c == (char)cc[VINTR] || c == (char)cc[VQUIT])) {
        echo(p, c);
        p->line_len = 0;
        p->in_len = 0;
        signal_pgrp(p->pgrp, c == (char)cc[VINTR] ? SIGINT : SIGQUIT);
        return;
    }
    if ((p->t.lflag & ICANON) == 0) {
        if (p->in_len < QUEUE) {
            p->in[p->in_len++] = c;
        }
        echo(p, c);
        return;
    }
    if (c == (char)cc[VERASE] || c == '\b') {
        erase_one(p);
    } else if (c == (char)cc[VKILL]) {
        while (p->line_len > 0) {
            erase_one(p);
        }
    } else if (c == (char)cc[VWERASE]) {
        while (p->line_len > 0 && p->line[p->line_len - 1] == ' ') {
            erase_one(p);
        }
        while (p->line_len > 0 && p->line[p->line_len - 1] != ' ') {
            erase_one(p);
        }
    } else if (c == (char)cc[VEOF] && cc[VEOF] != 0) {
        p->eof = p->line_len == 0;
        line_done(p);
    } else if (c == '\n' || (cc[VEOL] != 0 && c == (char)cc[VEOL])) {
        if (p->line_len < LINE) {
            p->line[p->line_len++] = c;
        }
        echo(p, c);
        line_done(p);
    } else if (p->line_len < LINE - 1) {
        p->line[p->line_len++] = c;
        echo(p, c);
    }
}

/* ---- the descriptors ------------------------------------------------------ */

/* Waits for test(p) to hold - another program, at the other end, makes it so -
   unless the descriptor is non-blocking. 0 once it does, or the error. */
static uint64_t wait_for(struct handle *h, bool (*test)(const struct pty *p)) {
    while (!test(pty_of(h))) {
        if ((h->offset & O_NONBLOCK) != 0) {
            return ERR(EAGAIN);
        }
        if (interrupt_check()) {
            return ERR(EINTR);
        }
        thread_yield();
    }
    return 0;
}

static bool master_can_read(const struct pty *p) {
    return p->out_len > 0 || (p->opened && p->slaves == 0);
}

static bool slave_can_read(const struct pty *p) {
    return p->in_len > 0 || p->eof || p->masters == 0 ||
           ((p->t.lflag & ICANON) == 0 && p->t.cc[VMIN] == 0);
}

static bool slave_can_write(const struct pty *p) {
    return p->out_len < QUEUE || p->masters == 0;
}

static uint64_t take(char *queue, unsigned *len, uint64_t buf, uint64_t count) {
    if (count > *len) {
        count = *len;
    }
    memcpy((void *)buf, queue, count);
    *len -= (unsigned)count;
    memmove(queue, queue + count, *len);
    return count;
}

static uint64_t pty_read(struct handle *h, uint64_t buf, uint64_t count) {
    struct pty *p = pty_of(h);
    uint64_t err;

    if (p == NULL) {
        return ERR(EIO);            /* the buffer: checked by the syscall, or the kernel's own */
    }
    if (!is_slave(h)) {
        if ((err = wait_for(h, master_can_read)) != 0) {
            return err;
        }
        return p->out_len == 0 ? ERR(EIO) : take(p->out, &p->out_len, buf, count);
    }
    if ((err = wait_for(h, slave_can_read)) != 0) {
        return err;
    }
    if (p->in_len == 0) {
        p->eof = false;
        return 0;                   /* Ctrl-D, the master gone, or VMIN 0 */
    }
    if ((p->t.lflag & ICANON) != 0) {
        /* A line at a time, as a terminal hands them over. */
        unsigned end = 0;

        while (end < p->in_len && p->in[end] != '\n') {
            end++;
        }
        if (end < p->in_len && end + 1 < count) {
            count = end + 1;
        }
    }
    return take(p->in, &p->in_len, buf, count);
}

static uint64_t pty_write(struct handle *h, uint64_t buf, uint64_t count) {
    struct pty *p = pty_of(h);
    const char *text = (const char *)buf;

    if (p == NULL) {
        return ERR(EIO);            /* the buffer: checked by the syscall, or the kernel's own */
    }
    if (!is_slave(h)) {
        for (uint64_t i = 0; i < count; i++) {
            input(p, text[i]);      /* typed: whatever does not fit is lost, as on Linux */
        }
        return count;
    }
    for (uint64_t i = 0; i < count; i++) {
        uint64_t err = p->out_len + 2 > QUEUE ? wait_for(h, slave_can_write) : 0;

        if (err != 0 || p->masters == 0) {
            return i > 0 ? i : p->masters == 0 ? ERR(EIO) : err;
        }
        output(p, text[i]);
    }
    return count;
}

static unsigned pty_ready(struct handle *h) {
    struct pty *p = pty_of(h);

    if (p == NULL) {
        return NET_ERR;
    }
    if (!is_slave(h)) {
        return (master_can_read(p) ? NET_IN : 0) | NET_OUT |
               (p->opened && p->slaves == 0 ? NET_HUP : 0);
    }
    return (p->in_len > 0 || p->eof || p->masters == 0 ? NET_IN : 0) |
           (p->out_len < QUEUE ? NET_OUT : 0) | (p->masters == 0 ? NET_HUP : 0);
}

static void pty_hold(struct handle *h) {
    struct pty *p = pty_of(h);

    if (p != NULL) {
        *(is_slave(h) ? &p->slaves : &p->masters) += 1;
    }
}

static void pty_drop(struct handle *h) {
    struct pty *p = pty_of(h);
    unsigned *n;

    if (p == NULL) {
        return;
    }
    n = is_slave(h) ? &p->slaves : &p->masters;
    if (*n > 0 && --*n == 0 && !is_slave(h)) {
        signal_pgrp(p->pgrp, SIGHUP);   /* the window has gone */
    }
    if (p->masters == 0 && p->slaves == 0) {
        mem_free(p);
        ptys[h->folder - 1] = NULL;
    }
}

static void pty_stat(struct handle *h, uint32_t *mode, uint32_t *rdev) {
    *mode = 0020000 | 0620;          /* S_IFCHR */
    *rdev = is_slave(h) ? 0x8800 | (h->folder - 1) : 0x0502;  /* pts/N: 136, N; ptmx: 5, 2 */
}

/* ---- ioctl ---------------------------------------------------------------- */

#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TCSBRK      0x5409
#define TCXONC      0x540A
#define TCFLSH      0x540B
#define TIOCSCTTY   0x540E
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define TIOCOUTQ    0x5411
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define FIONREAD    0x541B
#define TIOCNOTTY   0x5422
#define TIOCGSID    0x5429
#define TCGETS2     0x802C542Au
#define TCSETS2     0x402C542Bu
#define TCSETSW2    0x402C542Cu
#define TCSETSF2    0x402C542Du
#define TIOCGPTN    0x80045430u
#define TIOCSPTLCK  0x40045431u
#define TIOCGPTLCK  0x80045439u
#define TIOCGPTPEER 0x5441

static uint64_t slave_open(unsigned n, uint64_t flags);

static uint64_t put_int(uint64_t out, int32_t value) {
    if (!user_range(out, 4)) {
        return ERR(EFAULT);
    }
    *(int32_t *)out = value;
    return 0;
}

static uint64_t pty_ioctl(struct handle *h, uint64_t request, uint64_t arg) {
    struct pty *p = pty_of(h);
    int pid, pgid, sid;

    if (p == NULL) {
        return ERR(EIO);
    }
    switch (request) {
    case TCGETS:
    case TCGETS2: {
        size_t size = request == TCGETS ? 36 : sizeof p->t;

        if (!user_range(arg, size)) {
            return ERR(EFAULT);
        }
        memcpy((void *)arg, &p->t, size);
        return 0;
    }
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
    case TCSETS2:
    case TCSETSW2:
    case TCSETSF2: {
        size_t size = request < TCGETS2 ? 36 : sizeof p->t;

        if (!user_range(arg, size)) {
            return ERR(EFAULT);
        }
        memcpy(&p->t, (const void *)arg, size);
        if (request == TCSETSF || request == TCSETSF2) {
            p->in_len = p->line_len = 0;
        }
        if ((p->t.lflag & ICANON) == 0 && p->line_len > 0) {
            line_done(p);           /* raw now: what was gathered is there to read */
        }
        return 0;
    }
    case TIOCGWINSZ:
        if (!user_range(arg, sizeof p->size)) {
            return ERR(EFAULT);
        }
        memcpy((void *)arg, &p->size, sizeof p->size);
        return 0;
    case TIOCSWINSZ:
        if (!user_range(arg, sizeof p->size)) {
            return ERR(EFAULT);
        }
        memcpy(&p->size, (const void *)arg, sizeof p->size);
        signal_pgrp(p->pgrp, SIGWINCH);
        return 0;
    case TIOCSCTTY:
        process_ids(&pid, &pgid, &sid);
        p->sid = sid;
        p->pgrp = pgid;
        return 0;
    case TIOCGPGRP:
        return put_int(arg, p->pgrp);
    case TIOCSPGRP:
        if (!user_range(arg, 4)) {
            return ERR(EFAULT);
        }
        p->pgrp = *(const int32_t *)arg;
        return 0;
    case TIOCGSID:
        return put_int(arg, p->sid);
    case FIONREAD:
        return put_int(arg, (int32_t)(is_slave(h) ? p->in_len : p->out_len));
    case TIOCOUTQ:
        return put_int(arg, 0);
    case TCFLSH:
        if (arg == 0 || arg == 2) {
            p->in_len = p->line_len = 0;
        }
        return 0;
    case TCSBRK:
    case TCXONC:
    case TIOCNOTTY:
        return 0;
    case TIOCGPTN:
        return is_slave(h) ? ERR(ENOTTY) : put_int(arg, (int32_t)h->folder - 1);
    case TIOCSPTLCK:
        if (is_slave(h) || !user_range(arg, 4)) {
            return ERR(is_slave(h) ? ENOTTY : EFAULT);
        }
        p->locked = *(const int32_t *)arg != 0;
        return 0;
    case TIOCGPTLCK:
        return is_slave(h) ? ERR(ENOTTY) : put_int(arg, p->locked);
    case TIOCGPTPEER:
        return is_slave(h) ? ERR(ENOTTY) : slave_open(h->folder - 1, arg);
    }
    return ERR(ENOTTY);
}

/* ---- the names ------------------------------------------------------------ */

/* "pts/N"'s N, or PTYS. */
static unsigned number(const char *name) {
    unsigned n = 0;

    if (memcmp(name, "pts/", 4) != 0 || name[4] == '\0') {
        return PTYS;
    }
    for (name += 4; *name >= '0' && *name <= '9'; name++) {
        n = n * 10 + (unsigned)(*name - '0');
    }
    return *name == '\0' && n < PTYS ? n : PTYS;
}

static uint64_t slave_open(unsigned n, uint64_t flags) {
    struct pty *p = n < PTYS ? ptys[n] : NULL;
    int pid, pgid, sid;
    uint64_t fd;

    if (p == NULL || p->locked) {
        return ERR(p == NULL ? ENOENT : EIO);
    }
    fd = give_handle((struct handle){ .start = MOD_MARK, .size = slot, .folder = n + 1,
                                      .offset = SLAVE | ((uint32_t)flags & O_NONBLOCK) });
    if ((int64_t)fd >= 0) {
        p->slaves++;
        p->opened = true;
        if (p->sid == 0) {          /* the first to open it has it as its terminal */
            process_ids(&pid, &pgid, &sid);
            p->sid = sid;
            p->pgrp = pgid;
        }
    }
    return fd;
}

static uint64_t pty_open(const char *name, uint64_t flags) {
    struct pty *p;
    unsigned n = 0;
    uint64_t fd;

    if (strcmp(name, "ptmx") != 0) {
        return slave_open(number(name), flags);
    }
    while (n < PTYS && ptys[n] != NULL) {
        n++;
    }
    if (n == PTYS || (p = mem_alloc(sizeof *p)) == NULL) {
        return ERR(ENOSPC);
    }
    memset(p, 0, sizeof *p);
    /* Linux's defaults: a cooked line, echoed, Ctrl-C a signal. */
    p->t = (struct termios){
        .iflag = ICRNL | IXON, .oflag = OPOST | ONLCR, .cflag = 0x4BF,
        .lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN,
        .cc = { 3, 0x1C, 0x7F, 0x15, 4, 0, 1, 0, 0x11, 0x13, 0x1A, 0, 0x12, 0x0F, 0x17, 0x16, 0 },
        .ispeed = 38400, .ospeed = 38400,
    };
    p->size = (struct winsize){ 24, 80, 0, 0 };
    p->locked = true;
    p->masters = 1;
    ptys[n] = p;
    fd = give_handle((struct handle){ .start = MOD_MARK, .size = slot, .folder = n + 1,
                                      .offset = (uint32_t)flags & O_NONBLOCK });
    if ((int64_t)fd < 0) {
        mem_free(p);
        ptys[n] = NULL;
    }
    return fd;
}

static bool pty_named(const char *name, uint32_t *mode, uint32_t *rdev) {
    unsigned n = number(name);

    if (strcmp(name, "ptmx") == 0) {
        *mode = 0020000 | 0666;
        *rdev = 0x0502;
        return true;
    }
    if (n == PTYS || ptys[n] == NULL) {
        return false;
    }
    *mode = 0020000 | 0620;
    *rdev = 0x8800 | n;
    return true;
}

static const struct file_ops ops = {
    .prefix = "pt", .open = pty_open, .named = pty_named, .read = pty_read, .write = pty_write,
    .ioctl = pty_ioctl, .ready = pty_ready, .hold = pty_hold, .drop = pty_drop, .stat = pty_stat,
};

MODULE_EXPORT int module_init(void) {
    slot = files_register(&ops, 0);
    return slot != 0 ? 0 : -EBUSY;
}

/* A terminal still open is a program's: it stays. */
MODULE_EXPORT int module_exit(void) {
    for (unsigned i = 0; i < PTYS; i++) {
        if (ptys[i] != NULL) {
            return -EBUSY;
        }
    }
    files_register(NULL, slot);
    return 0;
}
