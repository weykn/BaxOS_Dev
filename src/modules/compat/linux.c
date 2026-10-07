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
#include "input.h"
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
/* sched_getparam(pid, param): every thread here runs at priority 0. */
static uint64_t sys_sched_getparam(uint64_t pid, uint64_t param, uint64_t c) {
    (void)pid;
    (void)c;
    if (!user_range(param, 4)) {
        return ERR(EFAULT);
    }
    *(int32_t *)param = 0;
    return 0;
}

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
        if (handle_of(fd)->start == SOCK_MARK || handle_of(fd)->start == MOD_MARK) {
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
        } else if (handle_of(fd)->start == DEV_MARK && handle_of(fd)->folder >= DEV_EVENT0) {
            event_bits |= bit;
            ready |= input_ready(handle_of(fd)->folder - DEV_EVENT0) ? bit : 0;
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

    /* With nothing to watch, a timeout is a sleep - which is how top, and
       many an older program, waits between one look and the next. */
    while (((console_bits | sock_bits | event_bits) != 0 || timeout != 0) &&
           ready == 0 && writable == 0) {
        if (console_bits != 0 && console_ready()) {
            ready = console_bits;
        }
        for (uint64_t fd = 0; fd < nfds; fd++) {
            if ((event_bits >> fd & 1) != 0 &&
                (handle_of(fd)->start == DEV_MARK ? input_ready(handle_of(fd)->folder - DEV_EVENT0) :
                 event_ready(handle_of(fd)) || readable(handle_of(fd)))) {
                ready |= 1ull << fd;
            }
        }
        for (uint64_t fd = 0; fd < nfds; fd++) {
            uint64_t bit = 1ull << fd;
            unsigned r;

            if ((sock_bits & bit) == 0) {
                continue;
            }
            r = handle_of(fd)->start == SOCK_MARK ? sock_ready(handle_of(fd))
              : ops_of(handle_of(fd)) != NULL ? ops_of(handle_of(fd))->ready(handle_of(fd)) : NET_ERR;
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
/* ---- mounts -----------------------------------------------------------------
 *
 * There is one filesystem, but Linux has /proc, /sys and /dev as filesystems
 * of their own, mounted there - and a program finds its way about by that: a
 * udev library will not walk /sys until it knows it is sysfs and not the
 * disk. So each answers as its own mount, with its own type. */

enum { MOUNT_ROOT = 1, MOUNT_PROC, MOUNT_SYS, MOUNT_DEV };

static unsigned mount_named(const char *name) {
    static const struct { const char *at; unsigned id; } mounts[] = {
        { "proc", MOUNT_PROC }, { "sys", MOUNT_SYS }, { "dev", MOUNT_DEV },
    };
    const char *rel = name;

    if (name == NULL) {
        return MOUNT_ROOT;
    }
    if (*name != '/') {
        rel = fs_cwd();             /* a relative name is in the working folder */
    }
    while (*rel == '/') {
        rel++;
    }
    for (unsigned i = 0; i < sizeof mounts / sizeof mounts[0]; i++) {
        size_t n = strlen(mounts[i].at);

        if (memcmp(rel, mounts[i].at, n) == 0 && (rel[n] == '\0' || rel[n] == '/')) {
            return mounts[i].id;
        }
    }
    return MOUNT_ROOT;
}

/* The mount a descriptor is on: its file's or folder's name, where it has one. */
static unsigned mount_of_fd(uint64_t fd) {
    struct handle *h = handle_of(fd);
    struct fs_file entry;

    if (h == NULL) {
        return MOUNT_ROOT;
    }
    switch (h->start) {
    case PROCDIR_MARK:
        return h->folder != 0 ? MOUNT_DEV : MOUNT_PROC;
    case PROC_MARK:
        return MOUNT_PROC;
    case DEV_MARK:
    case CONSOLE_MARK:
    case MOD_MARK:
        return MOUNT_DEV;
    case WRITE_MARK:
        return mount_named(write_name(h));
    default:
        if (h->start <= FOLDER_MARK && h->folder != 0 && fs_file(h->folder - 1, &entry) == 0) {
            char whole[FS_NAME_LEN + 1] = "/";      /* the table's names are from the root */

            strcpy(whole + 1, entry.name);
            return mount_named(whole);
        }
        return MOUNT_ROOT;
    }
}

#define PROC_SUPER_MAGIC 0x9FA0
#define SYSFS_MAGIC      0x62656572
#define TMPFS_MAGIC      0x01021994

static uint64_t statfs_fill(uint64_t out, unsigned mount);

static uint64_t statfs_fill(uint64_t out, unsigned mount) {
    struct statfs *stats = (struct statfs *)out;
    struct fs_stats disk;

    if (!user_range(out, sizeof *stats)) {
        return ERR(EFAULT);
    }
    if (fs_get_stats(&disk) != 0) {
        return ERR(EIO);
    }
    memset(stats, 0, sizeof *stats);
    stats->type = mount == MOUNT_PROC ? PROC_SUPER_MAGIC : mount == MOUNT_SYS ? SYSFS_MAGIC :
                  mount == MOUNT_DEV ? TMPFS_MAGIC : FS_MAGIC;
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
    return user_string(path) == NULL ? ERR(EFAULT) : statfs_fill(out, mount_named(user_string(path)));
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

/* ---- the screen and the input devices ------------------------------------
 *
 * What a Linux graphics program asks before it draws: /dev/fb0 for the
 * screen's size and layout (it then mmaps it), /dev/input/event* for what
 * the keys and the mouse can send, and the terminal to stop drawing over it
 * (KDSETMODE) and to stop taking the keys (KDSKBMODE). */

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOGETCMAP         0x4604
#define FBIOPUTCMAP         0x4605
#define FBIOPAN_DISPLAY     0x4606
#define FBIOGET_CON2FBMAP   0x460F
#define FBIOBLANK           0x4611
#define FBIO_WAITFORVSYNC   0x40044620u

struct fb_bitfield {
    uint32_t offset, length, msb_right;
};

struct fb_var {
    uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    uint32_t bits_per_pixel, grayscale;
    struct fb_bitfield red, green, blue, transp;
    uint32_t nonstd, activate, height, width, accel_flags, pixclock;
    uint32_t left_margin, right_margin, upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len, sync, vmode, rotate, colorspace, reserved[4];
};

struct fb_fix {
    char     id[16];
    uint64_t smem_start;
    uint32_t smem_len, type, type_aux, visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;
    uint64_t mmio_start;
    uint32_t mmio_len, accel;
    uint16_t capabilities, reserved[2];
};

_Static_assert(sizeof(struct fb_var) == 160, "fb_var_screeninfo is Linux's");
_Static_assert(sizeof(struct fb_fix) == 80, "fb_fix_screeninfo is Linux's");

static void fb_var_fill(struct fb_var *v) {
    struct vga_screen s;

    vga_screen(&s);
    memset(v, 0, sizeof *v);
    v->xres = v->xres_virtual = s.width;
    v->yres = v->yres_virtual = s.height;
    v->bits_per_pixel = 32;
    v->red = (struct fb_bitfield){ s.red_first ? 0 : 16, 8, 0 };
    v->green = (struct fb_bitfield){ 8, 8, 0 };
    v->blue = (struct fb_bitfield){ s.red_first ? 16 : 0, 8, 0 };
    v->height = v->width = 0xFFFFFFFF;          /* millimetres: not known */
    v->pixclock = (uint32_t)(1000000000000ull / ((uint64_t)s.width * s.height * 60 + 1));
}

static uint64_t fb_ioctl(uint64_t request, uint64_t out) {
    switch (request) {
    case FBIOGET_VSCREENINFO:
        if (!user_range(out, sizeof(struct fb_var))) {
            return ERR(EFAULT);
        }
        fb_var_fill((struct fb_var *)out);
        return 0;
    case FBIOPUT_VSCREENINFO: {
        /* The firmware's mode is the only one there is: asked for it, the
           answer is yes; asked for another, no. */
        struct fb_var now, *want = (struct fb_var *)out;

        if (!user_range(out, sizeof now)) {
            return ERR(EFAULT);
        }
        fb_var_fill(&now);
        if (want->xres != now.xres || want->yres != now.yres ||
            (want->bits_per_pixel != 0 && want->bits_per_pixel != 32)) {
            return ERR(EINVAL);
        }
        *want = now;
        return 0;
    }
    case FBIOGET_FSCREENINFO: {
        struct fb_fix *f = (struct fb_fix *)out;
        struct vga_screen s;

        if (!user_range(out, sizeof *f)) {
            return ERR(EFAULT);
        }
        vga_screen(&s);
        memset(f, 0, sizeof *f);
        memcpy(f->id, "EFI VGA", 8);
        f->smem_start = s.base;
        f->smem_len = s.pitch * s.height;
        f->visual = 2;                              /* FB_VISUAL_TRUECOLOR */
        f->line_length = s.pitch;
        return 0;
    }
    case FBIOGET_CON2FBMAP:
        if (!user_range(out, 8)) {
            return ERR(EFAULT);
        }
        ((uint32_t *)out)[1] = 0;
        return 0;
    case FBIOGETCMAP:
    case FBIOPUTCMAP:
    case FBIOPAN_DISPLAY:
    case FBIOBLANK:
    case FBIO_WAITFORVSYNC:
        return 0;
    }
    return ERR(ENOTTY);
}

#define IOC_NR(r)   ((r) & 0xFF)
#define IOC_TYPE(r) ((r) >> 8 & 0xFF)
#define IOC_SIZE(r) ((r) >> 16 & 0x3FFF)
#define IOC_DIR(r)  ((r) >> 30 & 3)

#define EVIOCGVERSION 0x01
#define EVIOCGID      0x02
#define EVIOCGREP     0x03
#define EVIOCGNAME    0x06
#define EVIOCGPHYS    0x07
#define EVIOCGUNIQ    0x08
#define EVIOCGPROP    0x09
#define EVIOCGKEY     0x18
#define EVIOCGBIT     0x20          /* + the event type, to 0x3F */
#define EVIOCGABS     0x40          /* + the axis, to 0x7F */
#define EVIOCGRAB     0x90
#define EVIOCSCLOCKID 0xA0

#define KEY_LAST_KBD 0x7F           /* KEY_ESC .. KEY_COMPOSE: what a PC keyboard has */

/* What a variable-length answer comes to: as much as was room for. */
static uint64_t ev_give(uint64_t out, uint64_t room, const void *what, uint64_t size) {
    if (!user_range(out, room)) {
        return ERR(EFAULT);
    }
    memset((void *)out, 0, room);
    memcpy((void *)out, what, size < room ? size : room);
    return size < room ? size : room;
}

static void bit_set(uint8_t *bits, unsigned n) {
    bits[n >> 3] |= (uint8_t)(1 << (n & 7));
}

static uint64_t ev_ioctl(unsigned device, uint64_t request, uint64_t out) {
    bool mouse = device == INPUT_MOUSE;
    unsigned nr = IOC_NR(request);
    uint64_t room = IOC_SIZE(request);
    uint8_t bits[INPUT_KEY_BYTES] = { 0 };

    if (IOC_TYPE(request) != 'E') {
        return ERR(ENOTTY);
    }
    if (IOC_DIR(request) == 1) {    /* the ones that set something */
        if (nr == EVIOCSCLOCKID && user_range(out, 4)) {
            input_clock(device, *(int32_t *)out != 0);
        }
        return nr == EVIOCGRAB || nr == EVIOCSCLOCKID || nr == EVIOCGREP ? 0 : ERR(EINVAL);
    }
    switch (nr) {
    case EVIOCGVERSION: {
        int32_t version = 0x010001;

        return (int64_t)ev_give(out, 4, &version, 4) < 0 ? ERR(EFAULT) : 0;
    }
    case EVIOCGID: {
        uint16_t id[4] = { 0x11, 0x01, mouse ? 0x02 : 0x01, 0xAB41 };  /* BUS_I8042 */

        return (int64_t)ev_give(out, 8, id, 8) < 0 ? ERR(EFAULT) : 0;
    }
    case EVIOCGREP: {
        uint32_t rep[2] = { 500, 33 };

        return mouse ? ERR(EINVAL) : (int64_t)ev_give(out, 8, rep, 8) < 0 ? ERR(EFAULT) : 0;
    }
    case EVIOCGNAME: {
        const char *name = mouse ? "PS/2 Generic Mouse" : "AT Translated Set 2 keyboard";

        return ev_give(out, room, name, strlen(name) + 1);
    }
    case EVIOCGPHYS: {
        const char *phys = mouse ? "isa0060/serio1/input0" : "isa0060/serio0/input0";

        return ev_give(out, room, phys, strlen(phys) + 1);
    }
    case EVIOCGUNIQ:
        return ERR(ENOENT);
    case EVIOCGKEY:
        return ev_give(out, room, input_keys(device), INPUT_KEY_BYTES);
    case EVIOCGPROP:
        return ev_give(out, room, bits, 0);
    }
    if (nr >= EVIOCGBIT && nr < EVIOCGABS) {
        switch (nr - EVIOCGBIT) {
        case 0:
            bit_set(bits, EV_SYN);
            bit_set(bits, EV_KEY);
            bit_set(bits, mouse ? EV_REL : EV_REP);
            return ev_give(out, room, bits, 4);
        case EV_KEY:
            if (mouse) {
                bit_set(bits, BTN_LEFT);
                bit_set(bits, BTN_RIGHT);
                bit_set(bits, BTN_MIDDLE);
            } else {
                for (unsigned k = 1; k <= KEY_LAST_KBD; k++) {
                    bit_set(bits, k);
                }
            }
            return ev_give(out, room, bits, INPUT_KEY_BYTES);
        case EV_REL:
            if (mouse) {
                bit_set(bits, REL_X);
                bit_set(bits, REL_Y);
            }
            return ev_give(out, room, bits, 2);
        default:
            return ev_give(out, room, bits, 0);
        }
    }
    if (nr >= EVIOCGABS && nr < EVIOCGABS + 0x40) {
        return ERR(EINVAL);         /* nothing absolute here */
    }
    /* What else there is to read - LEDs, sounds, switches, multitouch
       slots, effects - this hardware has none of: all clear. */
    return IOC_DIR(request) == 2 ? ev_give(out, room, bits, 0) : ERR(EINVAL);
}

#define KDGKBTYPE   0x4B33
#define KDGETLED    0x4B31
#define KDSETLED    0x4B32
#define KDSETMODE   0x4B3A
#define KDGETMODE   0x4B3B
#define KDGKBMODE   0x4B44
#define KDSKBMODE   0x4B45
#define KDSKBMUTE   0x4B51
#define VT_OPENQRY  0x5600
#define VT_GETMODE  0x5601
#define VT_SETMODE  0x5602
#define VT_GETSTATE 0x5603
#define VT_RELDISP  0x5605
#define VT_ACTIVATE 0x5606
#define VT_WAITACTIVE 0x5607
#define TIOCSCTTY   0x540E
#define TIOCNOTTY   0x5422
#define K_XLATE     1
#define K_UNICODE   3
#define K_OFF       4

static unsigned kb_mode = K_XLATE;

/* The terminal's side of it: there is one console, and it is VT 1. */
static uint64_t vt_ioctl(uint64_t request, uint64_t out) {
    switch (request) {
    case KDSETMODE:
        graphics_take();
        vga_lend(out == 1);         /* KD_GRAPHICS */
        return 0;
    case KDGETMODE:
    case KDGKBMODE:
    case VT_OPENQRY:
        if (!user_range(out, 4)) {
            return ERR(EFAULT);
        }
        *(int32_t *)out = request == KDGETMODE ? vga_lent() :
                          request == KDGKBMODE ? (int32_t)kb_mode : 1;
        return 0;
    case KDSKBMODE:
        graphics_take();
        kb_mode = (unsigned)out;
        console_keys(out == K_XLATE || out == K_UNICODE);
        return 0;
    case KDGKBTYPE:
    case KDGETLED:
        if (!user_range(out, 1)) {
            return ERR(EFAULT);
        }
        *(uint8_t *)out = request == KDGKBTYPE ? 2 : 0;     /* KB_101 */
        return 0;
    case VT_GETMODE:
        return user_range(out, 8) ? (memset((void *)out, 0, 8), 0) : ERR(EFAULT);
    case VT_GETSTATE: {
        if (!user_range(out, 6)) {
            return ERR(EFAULT);
        }
        uint16_t *st = (uint16_t *)out;

        st[0] = 1;                  /* VT 1 is the one on screen */
        st[1] = 0;
        st[2] = 1 << 1;             /* and the only one in use */
        return 0;
    }
    case KDSKBMUTE:                 /* X's way of K_OFF: the keys are its own */
        graphics_take();
        console_keys(out == 0);
        return 0;
    case KDSETLED:
    case VT_SETMODE:
    case VT_RELDISP:
    case VT_ACTIVATE:
    case VT_WAITACTIVE:
    case TIOCSCTTY:
    case TIOCNOTTY:
        return 0;
    }
    return ERR(ENOTTY);
}

static uint64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t out) {
    struct handle *h = handle_of(fd);

    if (h != NULL && h->start == DEV_MARK && h->folder == DEV_FB) {
        return fb_ioctl(request, out);
    }
    if (h != NULL && h->start == MOD_MARK) {
        if (request == FIONBIO && user_range(out, 4)) {
            h->offset = *(int32_t *)out != 0 ? h->offset | O_NONBLOCK : h->offset & ~O_NONBLOCK;
            return 0;
        }
        return ops_of(h) != NULL ? ops_of(h)->ioctl(h, request, out) : ERR(EIO);
    }
    if (h != NULL && h->start == DEV_MARK && h->folder >= DEV_EVENT0) {
        if (request == FIONBIO) {
            if (!user_range(out, 4)) {
                return ERR(EFAULT);
            }
            h->offset = *(int32_t *)out != 0 ? h->offset | O_NONBLOCK : h->offset & ~O_NONBLOCK;
            return 0;
        }
        return ev_ioctl(h->folder - DEV_EVENT0, request, out);
    }

    if (h != NULL && h->start == SOCK_MARK && request != FIONBIO) {
        return (h->offset & SOCK_UNIX) == 0 && net != NULL ? net->ioctl(h, request, out) : ERR(ENOTTY);
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
        /* Which group the keyboard is for: a shell hands it to each job it
           runs in front, and takes it back after. */
        if (!user_range(out, 4)) {
            return ERR(EFAULT);
        }
        *(int32_t *)out = tty_foreground();
        return 0;
    case TIOCSPGRP:
        if (!user_range(out, 4)) {
            return ERR(EFAULT);
        }
        tty_set_foreground(*(const int32_t *)out);
        return 0;
    case FIONREAD:
        /* Nothing is ever waiting: a key is read when it is asked for. */
        if (!user_range(out, 4)) {
            return ERR(EFAULT);
        }
        *(uint32_t *)out = 0;
        return 0;
    }
    return vt_ioctl(request, out);
}

/* Whether a file is there. Nothing here has permissions, so being there is
   the whole of the answer. */
/* A module's /dev file, by name: whether it is one, and what it is. */
static bool mod_named(const char *name, uint32_t *mode, uint32_t *rdev) {
    const char *leaf;
    const struct file_ops *ops = ops_named(name, &leaf);
    uint32_t m = 0, r = 0;

    if (ops == NULL || !ops->named(leaf, &m, &r)) {
        return false;
    }
    if (mode != NULL) {
        *mode = m;
        *rdev = r;
    }
    return true;
}

static uint64_t sys_access(uint64_t path, uint64_t mode, uint64_t c) {
    struct fs_file file;
    const char *name = user_string(path);

    (void)mode;
    (void)c;
    if (name == NULL) {
        return ERR(EFAULT);
    }
    if (proc_command(name) != NULL || proc_folder(name) || proc_net_name(name) != NULL ||
        dev_named(name) != 0 || dev_folder(name) || proc_fd_folder(name) || mod_named(name, NULL, NULL)) {
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
    char target[FS_NAME_LEN + 16];
    struct fs_file file;

    (void)mode;
    if (name == NULL) {
        return ERR(EFAULT);
    }
    if (proc_fd_target(name, target, sizeof target) != NULL) {
        name = target;
    }
    if (proc_command(name) != NULL || proc_folder(name) || proc_net_name(name) != NULL ||
        dev_named(name) != 0 || dev_folder(name) || proc_fd_folder(name) || mod_named(name, NULL, NULL) ||
        fs_stat(name, &file) == 0) {
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
        if (h->start == SOCK_MARK || h->start == MOD_MARK) {
            unsigned r = h->start == SOCK_MARK ? sock_ready(h)
                       : ops_of(h) != NULL ? ops_of(h)->ready(h) : NET_ERR;

            *sockets = true;
            p[i].revents = (int16_t)(((p[i].events & POLLIN) && (r & NET_IN) ? POLLIN : 0) |
                                     ((p[i].events & POLLOUT) && (r & NET_OUT) ? POLLOUT : 0) |
                                     (r & NET_ERR ? POLLERR : 0) | (r & NET_HUP ? POLLHUP : 0));
            ready += p[i].revents != 0;
            continue;
        }
        if ((p[i].events & POLLIN) != 0 &&
            (h->start != DEV_MARK || h->folder < DEV_EVENT0 || input_ready(h->folder - DEV_EVENT0)) &&
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

/* ---- epoll ----------------------------------------------------------------
 *
 * poll with the list kept in the kernel: epoll_ctl adds a descriptor once,
 * and every epoll_wait looks at all of them - an X server's main loop. The
 * list lives in a buffer descriptor of its own (buffer_open), which is what
 * the epoll descriptor is, and goes with it. Readiness is poll's; every
 * entry is level-triggered, which an edge-triggered user only sees as
 * being told twice. */

#define EPOLL_LIST    4096
#define EPOLL_ENTRIES ((EPOLL_LIST - 8) / 16)
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3
#define EPOLLONESHOT  (1u << 30)
#define EPOLL_EVENTS  0x201F        /* IN, PRI, OUT, ERR, HUP, RDHUP */

struct epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

struct epoll_set {
    uint32_t count, pad;
    struct { int32_t fd; uint32_t events; uint64_t data; } entry[EPOLL_ENTRIES];
};

_Static_assert(sizeof(struct epoll_set) <= EPOLL_LIST, "an epoll set fits its buffer");

static uint64_t sys_epoll_create1(uint64_t flags, uint64_t b, uint64_t c) {
    uint64_t fd = buffer_open(EPOLL_LIST);

    (void)b;
    (void)c;
    if ((int64_t)fd >= 0 && (flags & O_CLOEXEC) != 0) {
        fd_cloexec(fd, true);
    }
    return fd;
}

static struct epoll_set *epoll_of(uint64_t fd) {
    return pipe_data(handle_of(fd));
}

static uint64_t sys_epoll_ctl(uint64_t epfd, uint64_t op, uint64_t fd) {
    struct epoll_set *set = epoll_of(epfd);
    uint64_t at = arg[3];
    const struct epoll_event *e = (const struct epoll_event *)at;
    unsigned i = 0;

    if (set == NULL || handle_of(fd) == NULL) {
        return ERR(EBADF);
    }
    if (op != EPOLL_CTL_DEL && !user_range(at, sizeof *e)) {
        return ERR(EFAULT);
    }
    while (i < set->count && set->entry[i].fd != (int32_t)fd) {
        i++;
    }
    switch (op) {
    case EPOLL_CTL_ADD:
        if (i < set->count) {
            return ERR(EEXIST);
        }
        if (set->count == EPOLL_ENTRIES) {
            return ERR(ENOSPC);
        }
        set->count++;
        __attribute__((fallthrough));   /* the new one is filled in like a change */
    case EPOLL_CTL_MOD:
        if (i == set->count) {
            return ERR(ENOENT);
        }
        set->entry[i].fd = (int32_t)fd;
        set->entry[i].events = e->events;
        set->entry[i].data = e->data;
        return 0;
    case EPOLL_CTL_DEL:
        if (i == set->count) {
            return ERR(ENOENT);
        }
        set->entry[i] = set->entry[--set->count];
        return 0;
    }
    return ERR(EINVAL);
}

/* The entries ready now, up to max of them, into out; how many. */
static uint64_t epoll_once(struct epoll_set *set, struct epoll_event *out, uint64_t max) {
    uint64_t found = 0;
    bool sockets = false;

    for (unsigned i = 0; i < set->count && found < max; i++) {
        struct pollfd p = { .fd = set->entry[i].fd,
                            .events = (int16_t)(set->entry[i].events & EPOLL_EVENTS) };

        if (p.events == 0 || poll_once(&p, 1, &sockets) == 0) {
            continue;
        }
        out[found].events = (uint32_t)(uint16_t)p.revents;
        out[found].data = set->entry[i].data;
        found++;
        if ((set->entry[i].events & EPOLLONESHOT) != 0) {
            set->entry[i].events = 0;   /* until epoll_ctl arms it again */
        }
    }
    return found;
}

static uint64_t epoll_until(uint64_t epfd, uint64_t events, uint64_t max, int64_t wait_ms) {
    struct epoll_set *set = epoll_of(epfd);
    int64_t until = (int64_t)efi_uptime_ms() + wait_ms;
    uint64_t found;

    if (set == NULL) {
        return ERR(EBADF);
    }
    if ((int64_t)max <= 0 || !user_range(events, max * sizeof(struct epoll_event))) {
        return ERR((int64_t)max <= 0 ? EINVAL : EFAULT);
    }
    uint64_t began = wait_began();

    while ((found = epoll_once(set, (struct epoll_event *)events, max)) == 0 &&
           (wait_ms < 0 || (int64_t)efi_uptime_ms() < until)) {
        if (interrupt_check()) {
            wait_ended(began);
            return ERR(EINTR);
        }
        thread_yield();
    }
    wait_ended(began);
    return found;
}

static uint64_t sys_epoll_wait(uint64_t epfd, uint64_t events, uint64_t max) {
    return epoll_until(epfd, events, max, (int32_t)arg[3]);
}

/* epoll_pwait2's timeout is a timespec, NULL for ever. */
static uint64_t sys_epoll_pwait2(uint64_t epfd, uint64_t events, uint64_t max) {
    const int64_t *spec = (const int64_t *)arg[3];

    if (arg[3] != 0 && !user_range(arg[3], 16)) {
        return ERR(EFAULT);
    }
    return epoll_until(epfd, events, max, arg[3] == 0 ? -1 : spec[0] * 1000 + spec[1] / 1000000);
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
    /* No seccomp here, said as a kernel built without it says it: Firefox
       and Chromium then run their processes unsandboxed rather than wait for
       a sandbox broker that never comes. */
    if (option == 21 || option == 22) {     /* PR_GET_SECCOMP, PR_SET_SECCOMP */
        return ERR(EINVAL);
    }
    if (option == 7) {                      /* PR_GET_KEEPCAPS */
        return process_creds()->keepcaps;
    }
    if (option == 8) {                      /* PR_SET_KEEPCAPS */
        process_creds()->keepcaps = value != 0;
    }
    return 0;
}

/* Resource limits: the stack is the window's top end, and nothing else is
   limited. new_limit is refused, since nothing here would honour it. */
#define RLIMIT_NOFILE 7

static uint64_t sys_prlimit64(uint64_t pid, uint64_t resource, uint64_t new_limit) {
    uint64_t *old = (uint64_t *)arg[3];

    (void)pid;
    (void)new_limit;                /* asked to change: told it did. Firefox
                                       raises its stack's, and stops if it cannot */
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

#define LINK_MAX_COPY (1024 * 1024)

static char linked[2][64];

/* A second name for a file. The filesystem has one name a file, so the
   second is a copy - which is all a program wants of one it makes to put a
   lock file in place without a race: the name there, holding the same. */
static uint64_t link_copy(const char *from, const char *to) {
    struct fs_file file, there;
    char *data = NULL;
    int err;

    if (from == NULL || to == NULL) {
        return ERR(EFAULT);
    }
    if (fs_stat(from, &file) != 0) {
        return ERR(ENOENT);
    }
    if (fs_lstat(to, &there) == 0) {
        return ERR(EEXIST);
    }
    if (file.size > LINK_MAX_COPY || (file.size & FS_LINK) != 0) {
        return ERR(EPERM);
    }
    if (file.size > 0) {
        if ((data = mem_alloc(file.size + SECTOR_SIZE)) == NULL) {
            return ERR(ENOMEM);
        }
        if (fs_read_many(file.start, 0, (file.size + SECTOR_SIZE - 1) / SECTOR_SIZE, data) < 0) {
            mem_free(data);
            return ERR(EIO);
        }
    }
    err = fs_write(to, data, file.size);
    mem_free(data);
    if (err >= 0 && strlen(from) < sizeof linked[0] && strlen(to) < sizeof linked[1]) {
        strcpy(linked[0], from);
        strcpy(linked[1], to);
    }
    return err < 0 ? fs_errno(err) : 0;
}

/* Two links while both names are there, for the newest pair: a lock taken
   the way shadow's useradd takes one checks the count came out at 2. */
static unsigned links_of(const char *name) {
    struct fs_file file;

    if (name == NULL || linked[0][0] == '\0' ||
        (strcmp(name, linked[0]) != 0 && strcmp(name, linked[1]) != 0)) {
        return 1;
    }
    return fs_lstat(linked[0], &file) == 0 && fs_lstat(linked[1], &file) == 0 ? 2 : 1;
}

/* user_string answers in one buffer, so each name is copied out of it
   before the next is asked for. */
static uint64_t sys_linkat(uint64_t olddir, uint64_t oldpath, uint64_t newdir) {
    char a[FS_NAME_LEN + 16], b[FS_NAME_LEN], fd[32];
    const char *from = at_path(olddir, user_string(oldpath), a, sizeof a);

    /* The descriptor itself (AT_EMPTY_PATH), or /proc/self/fd/N for it: how
       an O_TMPFILE file is given its name - here, a copy under that name. */
    if (from != NULL && from[0] == '\0') {
        ksprintf(fd, "/proc/self/fd/%u", (unsigned)olddir);
        from = fd;
    }
    if (from != NULL && proc_fd_target(from, a, sizeof a) != NULL) {
        from = a;
        /* An O_TMPFILE file's name is the kernel's own, which nothing else
           knows: it is given the new one, however big it is. */
        const char *leaf = NULL;
        struct fs_file there;

        for (const char *c = a; *c != '\0'; c++) {
            leaf = *c == '/' ? c : leaf;
        }
        const char *to = at_path(newdir, user_string(arg[3]), b, sizeof b);

        if (to != NULL && leaf != NULL && memcmp(leaf, "/.tmp", 5) == 0) {
            return fs_lstat(to, &there) == 0 ? ERR(EEXIST) : fs_errno(fs_rename(a, to));
        }
    }
    if (from != NULL && from != a) {
        strcpy(a, from);
        from = a;
    }
    return link_copy(from, at_path(newdir, user_string(arg[3]), b, sizeof b));
}

static uint64_t sys_link(uint64_t from, uint64_t to, uint64_t c) {
    uint64_t kept = arg[3], result;

    (void)c;
    arg[3] = to;
    result = sys_linkat((uint64_t)AT_FDCWD, from, (uint64_t)AT_FDCWD);
    arg[3] = kept;
    return result;
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
    return handle_of(fd) == NULL ? ERR(EBADF) : statfs_fill(out, mount_of_fd(fd));
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
    char target[FS_NAME_LEN + 16];

    if (proc_fd_target(name, target, sizeof target) != NULL) {
        size_t n = strlen(target);

        n = n < size ? n : size;
        memcpy((void *)buf, target, n);
        return n;                   /* a link's text: no NUL */
    }
    got = fs_readlink(name, (char *)buf, size);
    if (got == FS_ENOENT && (fs_folder_at(name, &(unsigned){ 0 }) == 0 || dev_named(name) != 0 ||
                             dev_folder(name) || proc_folder(name) || proc_command(name) != NULL)) {
        return ERR(EINVAL);         /* there, just not a link: realpath walks on */
    }
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

/* Linux's number for a device: what tells a program one from another. */
static uint16_t rdev_of(unsigned which) {
    for (unsigned i = 0; i < DEVICES; i++) {
        if (devices[i].which == which) {
            return devices[i].rdev;
        }
    }
    return 0;
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
              h->start == WRITE_MARK ? file_ino(write_name(h), true) :
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
    } else if (h->start == MOD_MARK) {
        uint32_t mode = S_IFCHR | 0620, rdev = 0;

        if (ops_of(h) != NULL) {
            ops_of(h)->stat(h, &mode, &rdev);
        }
        st->mode = mode;
        st->rdev = rdev;
        st->size = 0;
        st->blocks = 0;
        st->ino = DEV_INO + 0x100 + h->folder;
    } else if (h->start == DEV_MARK) {
        st->mode = S_IFCHR | 0666;
        st->size = 0;
        st->blocks = 0;
        st->ino = DEV_INO + h->folder;
        st->rdev = rdev_of(h->folder);
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

static uint64_t statx_core(uint64_t dirfd, uint64_t path, uint64_t flags);

/* statx, saying which mount too (STATX_MNT_ID), as a udev library asks. */
#define STATX_MNT_ID 0x1000

/* chmod: a folder keeps the mode it is given, as a home folder made with
   mode 0 and then opened up needs. A file's stays as it is. */
static uint64_t chmod_name(const char *name, uint64_t mode) {
    unsigned index;

    if (name == NULL) {
        return ERR(EFAULT);
    }
    if (fs_folder_at(name, &index) == 0 && index != 0) {
        fs_set_mode(name, (unsigned)(S_IFDIR | (mode & 07777)));
    }
    return 0;
}

static uint64_t sys_chmod_(uint64_t path, uint64_t mode, uint64_t c) {
    (void)c;
    return chmod_name(user_string(path), mode);
}

static uint64_t sys_fchmod_(uint64_t fd, uint64_t mode, uint64_t c) {
    char name[32], target[FS_NAME_LEN + 16];

    (void)c;
    ksprintf(name, "/proc/self/fd/%u", (unsigned)fd);
    return proc_fd_target(name, target, sizeof target) != NULL ? chmod_name(target, mode) : 0;
}

static uint64_t sys_fchmodat_(uint64_t dirfd, uint64_t path, uint64_t mode) {
    char joined[FS_NAME_LEN];

    return chmod_name(at_path(dirfd, user_string(path), joined, sizeof joined), mode);
}

/* There is one owner on disk, root - except under /home, where everything
   belongs to whoever asks, so a user's own files look like their own. */
static bool owned_by_caller(const char *name, const char *joined) {
    if (name == NULL) {
        return false;
    }
    if (name == joined && name[0] != '/') {
        return memcmp(name, "home/", 5) == 0;   /* relative to a folder's descriptor */
    }
    return name[0] == '/' ? memcmp(name, "/home/", 6) == 0
                          : memcmp(fs_cwd(), "home/", 5) == 0;
}

static uint64_t sys_statx(uint64_t dirfd, uint64_t path, uint64_t flags) {
    uint64_t result = statx_core(dirfd, path, flags);
    struct statx *out = (struct statx *)arg[4];
    const char *given = user_string(path);

    if (result == 0) {
        char joined[FS_NAME_LEN];
        const char *name = given != NULL && given[0] != '\0'
                         ? at_path(dirfd, given, joined, sizeof joined) : NULL;

        out->mask |= STATX_MNT_ID;
        out->rest[0] = name == NULL ? mount_of_fd(dirfd) : mount_named(name);
        if (out->nlink == 1) {
            out->nlink = links_of(name);
        }
        if (owned_by_caller(name, joined)) {
            out->uid = process_creds()->uid;
            out->gid = process_creds()->gid;
        }
    }
    return result;
}

static uint64_t statx_core(uint64_t dirfd, uint64_t path, uint64_t flags) {
    struct statx *out = (struct statx *)arg[4];
    char joined[FS_NAME_LEN];
    const char *given = user_string(path);
    const char *name = at_path(dirfd, given, joined, sizeof joined);
    struct fs_file file;
    bool folder = false;

    char target[FS_NAME_LEN + 16];

    if (name == NULL || !user_range(arg[4], sizeof *out)) {
        return ERR(EINVAL);
    }
    if (proc_fd_target(name, target, sizeof target) != NULL) {
        name = target;              /* /proc/self/fd/N: what it names */
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
        out->rdev_major = (uint32_t)(st.rdev >> 8);
        out->rdev_minor = (uint32_t)(st.rdev & 0xFF);
        out->dev_minor = 1;
        return 0;
    }
    const struct proc_cmd *cmd = proc_command(name);
    enum dev which = dev_named(name);
    uint32_t mmode, mrdev;

    if (mod_named(name, &mmode, &mrdev)) {
        memset(out, 0, sizeof *out);
        out->mask = STATX_BASIC;
        out->blksize = SECTOR_SIZE;
        out->nlink = 1;
        out->mode = (uint16_t)mmode;
        out->ino = DEV_INO + 0x100 + (mrdev & 0xFF);
        out->rdev_major = mrdev >> 8;
        out->rdev_minor = mrdev & 0xFF;
        out->dev_minor = 1;
        return 0;
    }
    uint32_t puid, pgid;

    if (proc_pid_folder(name, &puid, &pgid) != 0) {
        memset(out, 0, sizeof *out);
        out->mask = STATX_BASIC;
        out->blksize = SECTOR_SIZE;
        out->nlink = 1;
        out->mode = S_IFDIR | 0555;
        out->ino = PROC_INO + 0x10000 + (unsigned)proc_pid_folder(name, &puid, &pgid);
        out->uid = puid;
        out->gid = pgid;
        out->dev_minor = 1;
        return 0;
    }
    if (cmd != NULL || proc_folder(name) || which != 0 || dev_folder(name) || proc_fd_folder(name) ||
        proc_task_links(name) != 0) {
        memset(out, 0, sizeof *out);
        out->mask = STATX_BASIC;
        out->blksize = SECTOR_SIZE;
        out->nlink = proc_task_links(name) != 0 ? proc_task_links(name) : 1;
        out->mode = (uint16_t)(which != 0 ? S_IFCHR | 0666 :
                               cmd != NULL ? S_IFREG | 0755 : S_IFDIR | 0755);
        out->ino = which != 0 ? DEV_INO + which : PROC_INO;
        out->size = cmd != NULL ? proc_read(cmd, 0, NULL, 0) : 0;
        out->rdev_major = rdev_of(which) >> 8;
        out->rdev_minor = rdev_of(which) & 0xFF;
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

static uint64_t newfstatat_core(uint64_t dirfd, uint64_t path, uint64_t out);

static uint64_t sys_newfstatat(uint64_t dirfd, uint64_t path, uint64_t out) {
    uint64_t result = newfstatat_core(dirfd, path, out);
    char joined[FS_NAME_LEN];
    const char *name = at_path(dirfd, user_string(path), joined, sizeof joined);

    if (result == 0 && owned_by_caller(name, joined)) {
        ((struct stat *)out)->uid = process_creds()->uid;
        ((struct stat *)out)->gid = process_creds()->gid;
    }
    if (result == 0 && ((struct stat *)out)->nlink == 1) {
        ((struct stat *)out)->nlink = links_of(name);
    }
    return result;
}

static uint64_t newfstatat_core(uint64_t dirfd, uint64_t path, uint64_t out) {
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
    char target[FS_NAME_LEN + 16];

    if (proc_fd_target(name, target, sizeof target) != NULL) {
        name = target;
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
    uint32_t puid, pgid;
    int pid = proc_pid_folder(name, &puid, &pgid);

    if (pid != 0) {
        fill_stat((struct stat *)out, 0, true, PROC_INO + 0x10000 + (unsigned)pid);
        ((struct stat *)out)->uid = puid;
        ((struct stat *)out)->gid = pgid;
        return 0;
    }
    if (proc_net_name(name) != NULL) {
        fill_stat((struct stat *)out, 0, false, PROC_INO + 2);
        ((struct stat *)out)->mode = S_IFREG | 0444;
        return 0;
    }
    uint32_t mmode, mrdev;

    if (mod_named(name, &mmode, &mrdev)) {
        fill_stat((struct stat *)out, 0, false, DEV_INO + 0x100 + (mrdev & 0xFF));
        ((struct stat *)out)->mode = mmode;
        ((struct stat *)out)->rdev = mrdev;
        return 0;
    }
    if (dev_named(name) != 0) {
        fill_stat((struct stat *)out, 0, false, DEV_INO + dev_named(name));
        ((struct stat *)out)->mode = S_IFCHR | 0666;
        ((struct stat *)out)->rdev = rdev_of(dev_named(name));
        return 0;
    }
    if (dev_folder(name) || proc_fd_folder(name) || proc_task_links(name) != 0) {
        fill_stat((struct stat *)out, 0, true, PROC_INO);
        if (proc_task_links(name) != 0) {
            ((struct stat *)out)->nlink = proc_task_links(name);
        }
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

/* /proc itself: self, the machine's files, and a folder for each process -
   what ps and top walk. */
static uint64_t pid_dents(struct handle *h, uint64_t buf, uint64_t count) {
    static const char *const fixed[] = {
        "self", "cpuinfo", "meminfo", "stat", "uptime", "loadavg", "mounts",
    };
    const unsigned nfixed = sizeof fixed / sizeof fixed[0];
    uint64_t used = 0;

    for (;; h->offset++) {
        char number[12];
        int pid = h->offset < nfixed ? 0 : proc_pid_at(h->offset - nfixed);

        if (h->offset >= nfixed && pid == 0) {
            break;
        }
        if (pid != 0) {
            ksprintf(number, "%u", (unsigned)pid);
        }
        const char *name = pid != 0 ? number : fixed[h->offset];
        size_t length = strlen(name);
        uint64_t reclen = (sizeof(struct dirent64) + length + 1 + 7) & ~7ull;

        if (used + reclen > count) {
            break;                  /* the rest waits for the next call */
        }
        struct dirent64 *out = (struct dirent64 *)(buf + used);

        out->ino = PROC_INO + 0x10000 + (pid != 0 ? (unsigned)pid : 0x8000 + h->offset);
        out->off = (int64_t)(h->offset + 1);
        out->reclen = (uint16_t)reclen;
        out->type = pid != 0 ? DT_DIR : h->offset == 0 ? DT_LNK : DT_REG;
        memcpy(out->name, name, length + 1);
        used += reclen;
    }
    return used;
}

/* /proc/self/fd: a link per descriptor open, named by its number. */
static uint64_t fd_dents(struct handle *h, uint64_t buf, uint64_t count) {
    uint64_t used = 0;

    for (; h->offset < PROGRAM_FILES; h->offset++) {
        char name[12];

        if (handle_of(h->offset) == NULL) {
            continue;
        }
        ksprintf(name, "%u", (unsigned)h->offset);
        size_t length = strlen(name);
        uint64_t reclen = (sizeof(struct dirent64) + length + 1 + 7) & ~7ull;

        if (used + reclen > count) {
            break;
        }
        struct dirent64 *out = (struct dirent64 *)(buf + used);

        out->ino = PROC_INO + 0x1000 + h->offset;
        out->off = (int64_t)(h->offset + 1);
        out->reclen = (uint16_t)reclen;
        out->type = DT_LNK;
        memcpy(out->name, name, length + 1);
        used += reclen;
    }
    return used;
}

/* /dev, from its table: names as the folder shows them, without "/dev/" -
   or /dev/input, with folder 2, and without "/dev/input/". /dev lists that
   folder as one more entry past the table. */
static uint64_t dev_dents(struct handle *h, uint64_t buf, uint64_t count) {
    static const char input[] = "input/";
    uint64_t used = 0;

    if (h->folder == 3) {
        return 0;                   /* /dev/pts: its terminals are ipc/pty's, by number */
    }
    for (; h->offset <= DEVICES; h->offset++) {
        const char *name = h->offset == DEVICES ? "input" :
                           devices[h->offset].name + sizeof "/dev/" - 1;
        bool inside = memcmp(name, input, sizeof input - 1) == 0;

        if ((h->folder == 2) != inside || (h->folder == 2 && h->offset == DEVICES)) {
            continue;
        }
        name += inside ? sizeof input - 1 : 0;
        size_t length = strlen(name);
        uint64_t reclen = (sizeof(struct dirent64) + length + 1 + 7) & ~7ull;

        if (used + reclen > count) {
            break;                  /* the rest waits for the next call */
        }
        struct dirent64 *out = (struct dirent64 *)(buf + used);

        out->ino = DEV_INO + h->offset + 1;
        out->off = (int64_t)(h->offset + 1);
        out->reclen = (uint16_t)reclen;
        out->type = h->offset == DEVICES ? DT_DIR : DT_CHR;
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
        if (h->folder == PROC_FD_FOLDER) {
            return fd_dents(h, buf, count);
        }
        if (h->folder == PROC_PIDS_FOLDER) {
            return pid_dents(h, buf, count);
        }
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

/* The system's name is Tuxlet, unless /etc/tuxlet/uname's first line says
   otherwise - "Linux" for a program that will not run anywhere else. */
static uint64_t sys_uname(uint64_t out, uint64_t b, uint64_t c) {
    static const char *const fields[] = { "Tuxlet", "tuxlet", "1", "1", "x86_64", "" };
    char *field = (char *)out;
    struct fs_file file;
    const char *name;

    (void)b;
    (void)c;
    if (!user_range(out, 6 * 65)) {
        return ERR(EFAULT);
    }
    for (unsigned i = 0; i < 6; i++, field += 65) {
        memset(field, 0, 65);
        memcpy(field, fields[i], strlen(fields[i]));
    }
    if (fs_stat("/etc/tuxlet/uname", &file) == 0 && file.size > 0 &&
        (name = fs_sector(file.start, 0)) != NULL) {
        field = (char *)out;
        memset(field, 0, 65);
        for (unsigned i = 0; i < file.size && i < 64 && name[i] != '\n'; i++) {
            field[i] = name[i];
        }
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

/* fallocate's plain kind: the file at least offset + len long, as on
   Linux - Chromium sizes the memory it shares this way. Space is never short
   here, so nothing need be set aside. */
static uint64_t sys_fallocate(uint64_t fd, uint64_t mode, uint64_t offset) {
    struct handle *h = handle_of(fd);
    uint64_t want = offset + syscall_args()[3];

    if (h == NULL) {
        return ERR(EBADF);
    }
    if ((mode & 0x01) == 0 && want > h->size) {    /* not FALLOC_FL_KEEP_SIZE */
        return fd_truncate(fd, want);
    }
    return 0;
}

/* ---- who a process is ---------------------------------------------------
 *
 * Users and groups, kept by the kernel per process (struct creds) and
 * changed as Linux allows: root to anything, anyone else only among its own
 * real, effective and saved ids. Nothing on the disk is refused anyone -
 * this is one person's machine - but a program sees who it runs as, and the
 * session starts as a user, not root. */

static bool cred_ok(uint32_t want, uint32_t a, uint32_t b, uint32_t c) {
    return process_creds()->euid == 0 || process_creds()->capable ||
           want == a || want == b || want == c;
}

static uint64_t sys_getresuid_(uint64_t r, uint64_t e, uint64_t s) {
    struct creds *c = process_creds();

    if (!user_range(r, 4) || !user_range(e, 4) || !user_range(s, 4)) {
        return ERR(EFAULT);
    }
    *(uint32_t *)r = c->uid;
    *(uint32_t *)e = c->euid;
    *(uint32_t *)s = c->suid;
    return 0;
}

static uint64_t sys_getresgid_(uint64_t r, uint64_t e, uint64_t s) {
    struct creds *c = process_creds();

    if (!user_range(r, 4) || !user_range(e, 4) || !user_range(s, 4)) {
        return ERR(EFAULT);
    }
    *(uint32_t *)r = c->gid;
    *(uint32_t *)e = c->egid;
    *(uint32_t *)s = c->sgid;
    return 0;
}

static uint64_t sys_getgroups_(uint64_t size, uint64_t list, uint64_t x) {
    struct creds *c = process_creds();

    (void)x;
    if (size == 0) {
        return c->ngroups;
    }
    if (size < c->ngroups) {
        return ERR(EINVAL);
    }
    if (!user_range(list, c->ngroups * 4)) {
        return ERR(EFAULT);
    }
    memcpy((void *)list, c->groups, c->ngroups * 4);
    return c->ngroups;
}

static uint64_t sys_setgroups_(uint64_t size, uint64_t list, uint64_t x) {
    struct creds *c = process_creds();

    (void)x;
    if (c->euid != 0 && !c->capable) {
        return ERR(EPERM);
    }
    if (size > 16 || !user_range(list, size * 4)) {
        return ERR(size > 16 ? EINVAL : EFAULT);
    }
    memcpy(c->groups, (const void *)list, size * 4);
    c->ngroups = (uint32_t)size;
    return 0;
}

static uint64_t sys_setresuid_(uint64_t r, uint64_t e, uint64_t s) {
    struct creds *c = process_creds();
    uint32_t nr = (uint32_t)r, ne = (uint32_t)e, ns = (uint32_t)s;

    if ((nr != ~0u && !cred_ok(nr, c->uid, c->euid, c->suid)) ||
        (ne != ~0u && !cred_ok(ne, c->uid, c->euid, c->suid)) ||
        (ns != ~0u && !cred_ok(ns, c->uid, c->euid, c->suid))) {
        return ERR(EPERM);
    }
    c->capable |= c->keepcaps && (c->euid == 0 || c->capable);
    c->uid = nr != ~0u ? nr : c->uid;
    c->euid = ne != ~0u ? ne : c->euid;
    c->suid = ns != ~0u ? ns : c->suid;
    return 0;
}

static uint64_t sys_setresgid_(uint64_t r, uint64_t e, uint64_t s) {
    struct creds *c = process_creds();
    uint32_t nr = (uint32_t)r, ne = (uint32_t)e, ns = (uint32_t)s;

    if ((nr != ~0u && !cred_ok(nr, c->gid, c->egid, c->sgid)) ||
        (ne != ~0u && !cred_ok(ne, c->gid, c->egid, c->sgid)) ||
        (ns != ~0u && !cred_ok(ns, c->gid, c->egid, c->sgid))) {
        return ERR(EPERM);
    }
    c->gid = nr != ~0u ? nr : c->gid;
    c->egid = ne != ~0u ? ne : c->egid;
    c->sgid = ns != ~0u ? ns : c->sgid;
    return 0;
}

static uint64_t sys_setuid_(uint64_t u, uint64_t b, uint64_t x) {
    struct creds *c = process_creds();

    (void)b;
    (void)x;
    if (c->euid == 0) {
        c->uid = c->euid = c->suid = (uint32_t)u;  /* root gives itself up for good */
        return 0;
    }
    return sys_setresuid_(~0ull, u, ~0ull);
}

static uint64_t sys_setgid_(uint64_t g, uint64_t b, uint64_t x) {
    struct creds *c = process_creds();

    (void)b;
    (void)x;
    if (c->euid == 0) {
        c->gid = c->egid = c->sgid = (uint32_t)g;
        return 0;
    }
    return sys_setresgid_(~0ull, g, ~0ull);
}

static uint64_t sys_setreuid_(uint64_t r, uint64_t e, uint64_t x) {
    (void)x;
    return sys_setresuid_(r, e, ~0ull);
}

static uint64_t sys_setregid_(uint64_t r, uint64_t e, uint64_t x) {
    (void)x;
    return sys_setresgid_(r, e, ~0ull);
}

/* setfsuid/setfsgid: the old one back, which is the effective one here. */
static uint64_t sys_setfsuid_(uint64_t u, uint64_t b, uint64_t x) {
    (void)u; (void)b; (void)x;
    return process_creds()->euid;
}

static uint64_t sys_setfsgid_(uint64_t g, uint64_t b, uint64_t x) {
    (void)g; (void)b; (void)x;
    return process_creds()->egid;
}

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
    213, 291, 233, 232, 281, 441,       /* epoll_create, _create1, _ctl, _wait, _pwait, _pwait2 */
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
    113, 114, 122, 123,                 /* setreuid, setregid, setfsuid, setfsgid */
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
    sys_chmod_,
    sys_fchmod_,
    sys_fchmodat_,
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
    sys_getresuid_,
    sys_getresgid_,
    sys_select,
    sys_pselect6,
    sys_renameat,
    sys_renameat,
    sys_linkat,
    sys_symlinkat,
    sys_link,
    sys_symlink,
    sys_mknodat,
    sys_truncate,
    sys_poll,
    sys_ppoll,
    sys_epoll_create1, sys_epoll_create1, sys_epoll_ctl, sys_epoll_wait, sys_epoll_wait,
    sys_epoll_pwait2,
    sys_fstatfs,
    sys_getrlimit,
    sys_ok,
    sys_ok,
    sys_fallocate,
    sys_ok,
    sys_zeroed,
    sys_getgroups_,
    sys_root,
    sys_ok,
    sys_sched_getparam,
    sys_root,
    sys_ok,
    sys_capget,
    sys_ok,
    sys_prctl,
    sys_setuid_,
    sys_setgid_,
    sys_setresuid_,
    sys_setresgid_,
    sys_setgroups_,
    sys_setreuid_, sys_setregid_, sys_setfsuid_, sys_setfsgid_,
};

static syscall_fn find(uint64_t number) {
    if ((number >= SYS_SETXATTR && number <= SYS_FREMOVEXATTR) ||
        number == 303 || number == 304) {
        /* All twelve xattr calls, and name_to_handle_at and open_by_handle_at:
           a filesystem without them, as Linux says it - which is what has a
           udev library fall back to finding its way by path. */
        return sys_no_xattr;
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
