#include "syscall.h"

#include <stdbool.h>

#include "ata.h"
#include "console.h"
#include "debug.h"
#include "efi_kernel.h"
#include "driver.h"
#include "mem.h"
#include "net.h"
#include "proc.h"
#include "string.h"
#include "thread.h"
#include "vga.h"
#include "vm.h"

#define MSR_EFER  0xC0000080
#define MSR_STAR  0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084

#define EFER_SCE    0x001
#define RFLAGS_MASK 0x700       /* clear TF, IF and DF on entry to the kernel */

#define SECTOR_SIZE 512
#define PAGE_SIZE   4096

static uint64_t write_protect(bool on);

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
#define ELOOP  40
#define EPROTONOSUPPORT 93
#define ESOCKTNOSUPPORT 94
#define EAFNOSUPPORT    97
#define ENOTSOCK        88

/* The arguments of the call being handled, all six of them. Handlers take
   the first three, which is all but a few of them want; the rest read the
   others here. A syscall can now start a program, and that program makes
   calls of its own, which overwrite these - so a handler that wants one
   reads it before it runs anything, which every one of them does. */
static uint64_t arg[6];

/* For thread.c, which keeps each thread's while another runs. */
uint64_t *syscall_args(void) {
    return arg;
}

/* Defined further down, where the loader and the page tables are. */
static uint64_t sys_execve(uint64_t path, uint64_t argv, uint64_t envp);
static uint64_t sys_fork(uint64_t a, uint64_t b, uint64_t c);
static uint64_t sys_clone(uint64_t flags, uint64_t stack, uint64_t parent_tid);
static uint64_t sys_clone3(uint64_t args, uint64_t size, uint64_t c);
static uint64_t sys_rt_sigtimedwait(uint64_t set, uint64_t info, uint64_t timeout);
static uint64_t sys_timer_create(uint64_t clock, uint64_t event, uint64_t id);
static uint64_t sys_timer_settime(uint64_t id, uint64_t flags, uint64_t spec);
static uint64_t sys_timer_gettime(uint64_t id, uint64_t spec, uint64_t c);
static uint64_t sys_wait4(uint64_t pid, uint64_t status, uint64_t options);
static uint64_t sys_pipe(uint64_t out, uint64_t b, uint64_t c);
static uint64_t sys_pipe2(uint64_t out, uint64_t flags, uint64_t c);
static uint64_t sys_eventfd(uint64_t count, uint64_t b, uint64_t c);
static uint64_t sys_eventfd2(uint64_t count, uint64_t flags, uint64_t c);
static uint64_t sys_getrandom(uint64_t buf, uint64_t length, uint64_t flags);
static bool fits(uint64_t addr, uint64_t size);
static bool claim(uint64_t addr, uint64_t size);
static int  read_at(const struct fs_file *file, uint64_t offset, void *dest, uint64_t size);
static uint64_t fs_errno(int err);
struct handle;

/* What the loaded program turned out to be, for the auxiliary vector its
   libc reads off the stack. */
static uint64_t started_base;   /* where the loader went, 0 without one */
static uint64_t started_phdr, started_entry;
static uint64_t started_phent, started_phnum;

/* In syscall_entry.asm. */
void syscall_entry(void);
extern const char trap_stubs[];     /* one 16-byte stub per exception vector */
extern uint64_t user_cs, user_ss;   /* ring 3's selectors, for the iretq frame */
void page_fault_entry(void);
int  user_enter(uint64_t entry, uint64_t stack);
__attribute__((noreturn)) void user_exit(int code);

/* The handlers (the table is before syscall_init).
 *
 * A table of pairs, because an array with a slot per syscall number would be
 * a thousand entries long for the seventy that are answered. Walking it on
 * every call was thirty-odd comparisons deep, though, and a program off a
 * Linux system makes thousands of calls before it prints anything - so the
 * numbers below 256, which is all but a handful of them, are looked up in a
 * byte apiece instead. */
#define LOW_NUMBERS 256
static uint8_t low[LOW_NUMBERS];    /* the slot, one-based; 0 means none */

static uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (uint64_t)high << 32 | low;
}

static void wrmsr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

/* ---- what a program may point at -----------------------------------------
 *
 * A program can only see its own window, so every pointer it hands over is
 * checked against it. A page of the window it has not touched yet is not
 * mapped, but reading one maps it, exactly as the program's own access
 * would - so a bad pointer inside the window is simply zeroes, and one
 * outside it never gets that far. */

bool user_range(uint64_t addr, uint64_t size) {
    return fits(addr, size);
}

/* The program's NUL-terminated string at addr, or NULL if it runs to the end
   of the window without one. */
static const char *user_string(uint64_t addr) {
    if (!user_range(addr, 0)) {
        return NULL;
    }
    /* Somewhere in the next page or two, or it is not a string. */
    for (uint64_t p = addr; p < addr + 4096 && user_range(p, 0); p++) {
        if (*(const char *)p == '\0') {
            return (const char *)addr;
        }
    }
    return NULL;
}

/* ---- open files ----------------------------------------------------------
 *
 * A descriptor is an index into this table, 0, 1 and 2 among them: the
 * console is an open file like any other, which is what lets a program dup
 * it, close it, or put a file of its own in its place - how a shell spells
 * a redirection. Only the run of sectors and where we are in it are kept,
 * which is all fs_sector needs - not the name, so the table costs a couple
 * of dozen bytes a file. */

/* Only a file open for writing needs its name kept - a write goes back to
   the filesystem by name - and only a few are ever open at once, so the
   names live here rather than in every handle, where they would cost four
   times as much. */
#define WRITERS 4

static char writer_names[WRITERS][FS_NAME_LEN];

static struct handle handles[PROGRAM_FILES];

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

/* ---- /dev ----------------------------------------------------------------
 *
 * The handful of files every Linux program expects to be able to open. None
 * of them is on the disk and none of them needs to be: what they do is so
 * little that they are a switch in read and another in write.
 *
 * /dev/null is where a shell sends output it does not want, and a program
 * given nowhere to put something and no /dev/null to put it stops. */

enum dev {
    DEV_NULL = 1, DEV_ZERO, DEV_FULL, DEV_RANDOM, DEV_TTY,
};

static const struct {
    const char *name;
    enum dev    which;
} devices[] = {
    { "/dev/null",    DEV_NULL   },
    { "/dev/zero",    DEV_ZERO   },
    { "/dev/full",    DEV_FULL   },
    { "/dev/random",  DEV_RANDOM },
    { "/dev/urandom", DEV_RANDOM },
    { "/dev/tty",     DEV_TTY    },
    { "/dev/stdin",   DEV_TTY    },
    { "/dev/stdout",  DEV_TTY    },
    { "/dev/stderr",  DEV_TTY    },
    { "/dev/console", DEV_TTY    },
};

#define DEVICES (sizeof devices / sizeof devices[0])

/* What a path names inside /dev - "" for the folder itself - or NULL if it
   is elsewhere. From inside /dev, a name is spelled against it. */
static const char *in_dev(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    if (strcmp(name, "/dev") == 0) {
        return "";
    }
    if (name[0] == '/') {
        return name[1] == 'd' && name[2] == 'e' && name[3] == 'v' && name[4] == '/'
             ? name + 5 : NULL;
    }
    if (strcmp(fs_cwd(), "dev/") != 0) {
        return NULL;
    }
    if (strcmp(name, ".") == 0) {
        return "";
    }
    return name[0] == '.' && name[1] == '/' ? name + 2 : name;
}

/* Which of them a path names, or 0. */
static enum dev dev_named(const char *name) {
    const char *leaf = in_dev(name);

    for (unsigned i = 0; leaf != NULL && *leaf != '\0' && i < DEVICES; i++) {
        if (strcmp(leaf, devices[i].name + 5) == 0) {
            return devices[i].which;
        }
    }
    return 0;
}

static bool dev_folder(const char *name) {
    const char *leaf = in_dev(name);

    return leaf != NULL && *leaf == '\0';
}

struct handle *handle_of(uint64_t fd) {
    if (fd >= PROGRAM_FILES) {
        return NULL;
    }
    struct handle *h = &handles[fd];
    return h->used != 0 ? h : NULL;
}

/* True if fd is the console rather than a file, which is what decides
   whether a terminal's questions have an answer. */
static bool is_console(uint64_t fd) {
    struct handle *h = handle_of(fd);

    return h != NULL && h->start == CONSOLE_MARK;
}

/* What a program starts with: the console on 0, 1 and 2, nothing else open.
   A program that exits without closing its files leaves them behind, so this
   runs before each one rather than after. */
static void handles_reset(void) {
    memset(handles, 0, sizeof handles);
    if (net != NULL) {
        net->close_all();
    }
    memset(writer_names, 0, sizeof writer_names);
    for (unsigned fd = 0; fd < 3; fd++) {
        handles[fd].used = 1;
        handles[fd].start = CONSOLE_MARK;
    }
}

/* ---- pipes ---------------------------------------------------------------
 *
 * A buffer with a read end and a write end, both ordinary descriptors. It
 * grows as it is written to, since the program filling it has to finish
 * before the one draining it starts and there is no way to push back. */

#define PIPES      8
#define PIPE_FIRST 8192
#define PIPE_MAX   (1024 * 1024)

/* A pipe handle's size says which end it is. An eventfd is a pipe with no
   buffer, only a count; O_NONBLOCK and EFD_SEMAPHORE are in its offset. */
#define PIPE_READ  0
#define PIPE_WRITE 1
#define PIPE_EVENT 2
#define PIPE_FILE  3                /* a /proc/net file: the text, read once */
#define PIPE_TEXT  8192             /* the most of one */
#define EFD_SEMAPHORE 1

static struct pipe {
    char    *data;
    uint32_t size, len, read_at;
    unsigned refs;                  /* descriptors on either end */
    uint64_t count;                 /* an eventfd's */
} pipes[PIPES];

static struct pipe *pipe_of(const struct handle *h) {
    return h != NULL && h->start == PIPE_MARK && h->folder > 0 &&
           h->folder <= PIPES && pipes[h->folder - 1].refs > 0
         ? &pipes[h->folder - 1] : NULL;
}

static void pipe_drop(struct pipe *p) {
    if (p != NULL && p->refs > 0 && --p->refs == 0) {
        if (p->data != NULL) {
            mem_free(p->data);
        }
        *p = (struct pipe){ 0 };
    }
}


/* A free one, or PIPES. */
static unsigned pipe_slot(void) {
    unsigned slot = 0;

    while (slot < PIPES && pipes[slot].refs > 0) {
        slot++;
    }
    return slot;
}

/* The name of a /proc/net file the network module has, or NULL. */
static const char *proc_net_name(const char *path) {
    static const char dir[] = "/proc/net/";

    if (path == NULL || net == NULL) {
        return NULL;
    }
    for (const char *d = dir; *d != '\0'; d++, path++) {
        if (*path != *d) {
            return NULL;
        }
    }
    return net->proc(path, NULL, 0) != (size_t)-1 ? path : NULL;
}

/* One opened: what it says now, made then, and read from there on. */
static uint64_t proc_net_open(const char *name) {
    unsigned slot = pipe_slot();
    char *text;
    uint64_t fd;

    if (slot == PIPES || (text = mem_alloc(PIPE_TEXT)) == NULL) {
        return ERR(ENFILE);
    }
    size_t len = net->proc(name, text, PIPE_TEXT);

    fd = give_handle((struct handle){ .start = PIPE_MARK, .folder = slot + 1, .size = PIPE_FILE });
    if ((int64_t)fd < 0) {
        mem_free(text);
        return fd;
    }
    pipes[slot] = (struct pipe){ .data = text, .size = PIPE_TEXT, .refs = 1,
                                 .len = (uint32_t)(len < PIPE_TEXT ? len : PIPE_TEXT) };
    return fd;
}

static bool event_ready(const struct handle *h) {
    struct pipe *p = pipe_of(h);

    return p != NULL && h->size == PIPE_EVENT && p->count > 0;
}

/* Every pipe the open files hold, counted once more or once less: what the
   program being put aside still has open, so that a child closing its end
   does not take the buffer out from under it. */
static void pipes_hold(int by) {
    for (unsigned fd = 0; fd < PROGRAM_FILES; fd++) {
        struct pipe *p = handles[fd].used != 0 ? pipe_of(&handles[fd]) : NULL;

        if (handles[fd].used != 0 && handles[fd].start == SOCK_MARK && net != NULL) {
            if (by > 0) {
                net->hold((int)handles[fd].folder);
            } else {
                net->drop((int)handles[fd].folder);
            }
        }
        if (p == NULL) {
            continue;
        }
        if (by > 0) {
            p->refs++;
        } else {
            pipe_drop(p);
        }
    }
}

/* Adds to the buffer, growing it if there is room to. */
static uint64_t pipe_write(struct pipe *p, const char *from, uint64_t count) {

    if (p->len + count > p->size) {
        uint32_t want = p->size;
        void *bigger = NULL;

        while (want < p->len + count && want < PIPE_MAX) {
            want *= 2;
        }
        if (want < p->len + count) {
            count = want - p->len;  /* as much of it as will ever fit */
        }
        if (count == 0) {
            return ERR(EPIPE);
        }
        if ((bigger = mem_alloc(want)) == NULL) {
            return ERR(ENOMEM);
        }
        memcpy(bigger, p->data, p->len);
        mem_free(p->data);
        p->data = bigger;
        p->size = want;
    }
    memcpy(p->data + p->len, from, (size_t)count);
    p->len += (uint32_t)count;
    return count;
}

static uint32_t pipe_left(const struct pipe *p) {
    return p->len - p->read_at;
}

static uint64_t pipe_read(struct pipe *p, char *to, uint64_t count) {
    uint32_t left = p->len - p->read_at;

    if (count > left) {
        count = left;
    }
    memcpy(to, p->data + p->read_at, (size_t)count);
    p->read_at += (uint32_t)count;
    return count;
}

/* exit ends the thread, exit_group the process - all its threads. */

/* An eventfd's count, all of it or one of it, once it is not 0. Another
   thread is what adds to it, so the wait lets the others run. */
static uint64_t event_read(struct handle *h, struct pipe *p, uint64_t buf, uint64_t count) {
    if (count < 8) {
        return ERR(EINVAL);
    }
    while (p->count == 0) {
        if ((h->offset & O_NONBLOCK) != 0 || thread_alone()) {
            return ERR(EAGAIN);
        }
        interrupt_check();
        thread_yield();
    }
    uint64_t took = (h->offset & EFD_SEMAPHORE) != 0 ? 1 : p->count;

    p->count -= took;
    *(uint64_t *)buf = took;
    return 8;
}

static uint64_t sys_exit(uint64_t code, uint64_t b, uint64_t c) {
    (void)b;
    (void)c;
    thread_exit((int)code);
}

static uint64_t sys_exit_group(uint64_t code, uint64_t b, uint64_t c) {
    (void)b;
    (void)c;
    process_exit((int)code);
}

static uint64_t write_to(struct handle *h, uint64_t text, uint64_t length) {
    if (h == NULL) {
        return ERR(EBADF);
    }
    if (h->start == WRITE_MARK) {
        /* Into the file where the descriptor is, which is its end when the
           program has only been writing and anywhere in it once the program
           has moved about - what a program keeping a scratch file of its own
           does, ed's buffer among them. */
        if (fs_write_at(writer_names[h->writer - 1], h->offset,
                        (const void *)text, length) < 0) {
            return ERR(EIO);
        }
        h->offset += (uint32_t)length;
        if (h->offset > h->size) {
            h->size = h->offset;
        }
        return length;
    }
    if (h->start == PIPE_MARK) {
        struct pipe *p = pipe_of(h);

        if (p != NULL && h->size == PIPE_EVENT) {
            if (length < 8) {
                return ERR(EINVAL);
            }
            p->count += *(const uint64_t *)text;
            return 8;
        }
        return p == NULL || h->size != PIPE_WRITE ? ERR(EBADF)
                                                  : pipe_write(p, (const char *)text, length);
    }
    if (h->start == SOCK_MARK) {
        return net != NULL ? net->write(h, text, length) : ERR(EBADF);
    }
    if (h->start == DEV_MARK) {
        /* Written to nowhere, and gone. /dev/full is the one that cannot
           take it, which is what it is for. */
        return h->folder == DEV_FULL ? ERR(ENOSPC) : length;
    }
    if (h->start != CONSOLE_MARK) {
        return ERR(EBADF);          /* a file open for reading */
    }
    for (uint64_t i = 0; i < length; i++) {
        vga_putc(((const char *)text)[i]);
    }
    return length;
}

static uint64_t sys_write(uint64_t fd, uint64_t text, uint64_t length) {
    /* Only from the program's own memory, or it could print the kernel's. */
    if (!user_range(text, length)) {
        return ERR(EFAULT);
    }
    return write_to(handle_of(fd), text, length);
}

/* ---- time spent -----------------------------------------------------------
 *
 * One program runs at a time, and one that forks waits for the child to
 * finish, so a program's time is the time since it started less the time
 * its children ran and the time it spent waiting - for a key, or asleep. Of
 * what is left, the time inside its syscalls and page faults is system time
 * and the rest user time. All of it is in microseconds. */

struct times {
    uint64_t started;           /* when the program began */
    uint64_t idle;              /* how long it waited */
    uint64_t sys;               /* how long the kernel worked for it */
    uint64_t children_wall;     /* how long its finished children ran */
    uint64_t children_user, children_sys;   /* and what of it was which */
};

static struct times now_running;    /* the program running now */

static uint64_t self_us(void) {
    uint64_t took = efi_uptime_us() - now_running.started;
    uint64_t not = now_running.children_wall + now_running.idle;

    return took > not ? took - not : 0;
}

static uint64_t user_us(void) {
    uint64_t self = self_us();

    return self > now_running.sys ? self - now_running.sys : 0;
}

/* Marks the start of a wait, and then its end, which counts it idle. */
uint64_t wait_began(void) {
    return efi_uptime_us();
}

void wait_ended(uint64_t began) {
    now_running.idle += efi_uptime_us() - began;
}

/* Ctrl-C, while a program waits on the network: there are no signals to
   deliver, so it ends the program as SIGINT's default would. Only a wait
   that could last for ever checks, so a program reading keys is left its
   Ctrl-C. */
void interrupt_check(void) {
    if (console_interrupted()) {
        vga_puts("^C\n");
        process_exit(130);
    }
}

/* The same around the kernel working for a program: what it took is system
   time, less what went on waiting or on children inside it - a fork runs a
   whole child inside the call. */
struct kernel_mark {
    uint64_t at, idle, children_wall;
};

static struct kernel_mark kernel_began(void) {
    return (struct kernel_mark){ efi_uptime_us(), now_running.idle,
                                 now_running.children_wall };
}

static void kernel_ended(struct kernel_mark m) {
    uint64_t took = efi_uptime_us() - m.at;
    uint64_t not = (now_running.idle - m.idle) +
                   (now_running.children_wall - m.children_wall);

    now_running.sys += took > not ? took - not : 0;
}

/* The time of day in milliseconds. The firmware's clock counts whole
   seconds, so it is read once and the uptime counted on from there. */
static uint64_t realtime_us(void) {
    static uint64_t offset;

    if (offset == 0) {
        offset = efi_epoch() * 1000000 - efi_uptime_us();
    }
    return offset + efi_uptime_us();
}

static uint64_t realtime_ms(void) {
    return realtime_us() / 1000;
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
        if (handles[fd].start == SOCK_MARK) {
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
        } else if (handles[fd].start == PIPE_MARK) {
            struct pipe *p = pipe_of(&handles[fd]);

            /* A pipe with nothing left in it is at its end, which is a read
               that returns nothing rather than a wait. */
            (void)p;
            if (handles[fd].size == PIPE_EVENT) {
                event_bits |= bit;  /* ready once something adds to it */
                ready |= event_ready(&handles[fd]) ? bit : 0;
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
            if ((event_bits >> fd & 1) != 0 && event_ready(&handles[fd])) {
                ready |= 1ull << fd;
            }
        }
        for (uint64_t fd = 0; fd < nfds && net != NULL; fd++) {
            uint64_t bit = 1ull << fd;
            unsigned r;

            if ((sock_bits & bit) == 0) {
                continue;
            }
            r = net->ready((int)handles[fd].folder);
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
        if (sock_bits != 0) {
            interrupt_check();
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

/* Reads from where the descriptor is, in the run of sectors starting at
   start, and moves it on. Stops at the end of the file. */
static uint64_t read_run(struct handle *h, uint32_t start, uint64_t buf, uint64_t count) {
    uint64_t left = h->offset < h->size ? h->size - h->offset : 0;

    if (count > left) {
        count = left;
    }
    for (uint64_t done = 0; done < count;) {
        /* Whole sectors in one call, straight into the program's buffer: a
           read costs the call, not the bytes. */
        uint64_t whole = h->offset % SECTOR_SIZE == 0 ? (count - done) / SECTOR_SIZE : 0;

        if (whole > 0) {
            if (fs_read_many(start, h->offset / SECTOR_SIZE, (unsigned)whole,
                             (char *)buf + done) < 0) {
                return ERR(EIO);
            }
            h->offset += (uint32_t)(whole * SECTOR_SIZE);
            done += whole * SECTOR_SIZE;
            continue;
        }
        const char *sector = fs_sector(start, h->offset / SECTOR_SIZE);

        if (sector == NULL) {
            return ERR(EIO);
        }
        uint64_t chunk = SECTOR_SIZE - h->offset % SECTOR_SIZE;

        if (chunk > count - done) {
            chunk = count - done;
        }
        memcpy((char *)buf + done, sector + h->offset % SECTOR_SIZE, (size_t)chunk);
        h->offset += (uint32_t)chunk;
        done += chunk;
    }
    return count;
}

static uint64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count) {
    struct handle *h = handle_of(fd);

    if (!user_range(buf, count)) {
        return ERR(EFAULT);
    }
    if (h == NULL) {
        return ERR(EBADF);
    }
    if (h->start == CONSOLE_MARK) {
        uint64_t began = wait_began();      /* waiting on whoever is typing */
        uint64_t got = console_read((char *)buf, count);

        wait_ended(began);
        return got;
    }
    if (h->start == WRITE_MARK) {
        /* Open for writing, and being read: where its sectors are has to be
           asked for, since a write may have moved the whole file. */
        struct fs_file file;

        if (fs_stat(writer_names[h->writer - 1], &file) < 0) {
            return ERR(EIO);
        }
        h->size = file.size;
        return read_run(h, file.start, buf, count);
    }
    if (h->start == PROC_MARK) {
        uint64_t got = proc_read(proc_at(h->folder - 1), h->offset, (char *)buf, count);

        h->offset += (uint32_t)got;
        return got;
    }
    if (h->start == PIPE_MARK) {
        struct pipe *p = pipe_of(h);

        if (p != NULL && h->size == PIPE_EVENT) {
            return event_read(h, p, buf, count);
        }
        /* The end of what is there is the end of the input: whatever filled
           it has already finished by the time anything reads. */
        return p == NULL || (h->size != PIPE_READ && h->size != PIPE_FILE)
             ? ERR(EBADF) : pipe_read(p, (char *)buf, count);
    }
    if (h->start == SOCK_MARK) {
        return net != NULL ? net->read(h, buf, count) : ERR(EBADF);
    }
    if (h->start == DEV_MARK) {
        if (h->folder == DEV_NULL) {
            return 0;               /* nothing in it, ever */
        }
        if (h->folder == DEV_RANDOM) {
            return sys_getrandom(buf, count, 0);
        }
        memset((void *)buf, 0, (size_t)count);
        return count;
    }
    if (h->start >= FIRST_MARK) {
        return ERR(EBADF);          /* a folder, or a file being written */
    }

    return read_run(h, h->start, buf, count);
}

/* The lowest free descriptor, holding what was found - which is the one
   Linux hands out too, and what a program closing 1 and opening a file
   counts on. */
uint64_t give_handle(struct handle h) {
    for (unsigned i = 0; i < PROGRAM_FILES; i++) {
        if (handles[i].used == 0) {
            h.used = 1;
            handles[i] = h;
            return i;
        }
    }
    return ERR(EMFILE);
}

/* Opens a file, or the folder of that name if there is no such file - which
   is what getdents64 needs a descriptor for. "." is a folder like any other
   here, since the filesystem resolves it. */
static uint64_t open_name(const char *name, uint64_t flags) {
    struct fs_file file;
    unsigned folder;
    enum dev which;

    if (name == NULL) {
        return ERR(EINVAL);
    }
    /* Asked not to go through a link at the end, and there is one there. */
    if ((flags & O_NOFOLLOW) != 0 && fs_lstat(name, &file) == 0 &&
        (file.size & FS_LINK) != 0) {
        return ERR(ELOOP);
    }
    if ((which = dev_named(name)) != 0) {
        if (which == DEV_TTY) {
            return give_handle((struct handle){ .start = CONSOLE_MARK });
        }
        return give_handle((struct handle){ .start = DEV_MARK, .folder = which });
    }
    if ((flags & O_ACCMODE) != O_RDONLY) {
        /* Open for writing. The name is kept, because that is what a write
           goes back to; what is in the file is kept too, unless the program
           asked for it to be thrown away. */
        struct handle h = { .start = WRITE_MARK };
        char scratch[FS_NAME_LEN];
        unsigned slot = 0;
        size_t length;
        bool empty;

        while (slot < WRITERS && writer_names[slot][0] != '\0') {
            slot++;
        }
        if (slot == WRITERS) {
            return ERR(EMFILE);
        }
        if ((flags & O_TMPFILE) == O_TMPFILE) {
            /* A file with no name of its own, in the folder named: a
               program's scratch buffer. It gets a name all the same - there
               is nowhere else to put it - one per slot, so however many
               times a program does this it costs the same handful of
               entries. */
            if (fs_folder_at(name, &(unsigned){ 0 }) != 0) {
                return ERR(ENOENT);
            }
            ksprintf(scratch, "%s/.tmp%u", name, slot);
            name = scratch;
            empty = true;
        } else {
            empty = (flags & O_TRUNC) != 0;
            if (fs_stat(name, &file) != 0) {
                if ((flags & O_CREAT) == 0) {
                    return ERR(ENOENT);
                }
                empty = true;
            }
        }
        length = strlen(name);
        if (length + 1 > FS_NAME_LEN) {
            return ERR(EINVAL);
        }
        if (empty && fs_write(name, NULL, 0) < 0) {
            return ERR(EACCES);
        }
        if (fs_stat(name, &file) != 0) {
            return ERR(EACCES);
        }
        memcpy(writer_names[slot], name, length + 1);
        h.writer = slot + 1;
        h.size = file.size;
        h.offset = (flags & O_APPEND) != 0 ? file.size : 0;
        return give_handle(h);
    }
    if (proc_net_name(name) != NULL) {
        return proc_net_open(proc_net_name(name));
    }
    for (unsigned i = 0; proc_at(i) != NULL; i++) {
        if (proc_command(name) == proc_at(i)) {
            return give_handle((struct handle){
                .start = PROC_MARK, .folder = i + 1,
                .size = (uint32_t)proc_read(proc_at(i), 0, NULL, 0) });
        }
    }
    if (proc_folder(name) || dev_folder(name)) {
        return give_handle((struct handle){ .start = PROCDIR_MARK, .folder = dev_folder(name) });
    }
    if (fs_folder_at(name, &folder) == 0) {
        return give_handle((struct handle){ .start = FOLDER_MARK, .folder = folder });
    }
    if (fs_stat(name, &file) == 0) {
        return give_handle((struct handle){ .start = file.start, .size = file.size });
    }
    return ERR(ENOENT);
}

static uint64_t sys_open(uint64_t path, uint64_t flags, uint64_t mode) {
    (void)mode;
    return open_name(user_string(path), flags);
}

/* A relative path, spelled out from a folder a program already has open.
 *
 * Every call with "at" in its name takes one of those, and a program walking
 * a tree uses nothing else: it opens a folder, reads it, and then asks about
 * what it found relative to that descriptor rather than by a path from the
 * root. `find` does exactly this, and ignoring the descriptor made it look
 * for everything in the working directory.
 *
 * AT_FDCWD, and an absolute path whatever the descriptor says, are the
 * working directory's business and go through untouched. */
#define AT_FDCWD (-100)

/* In the flags of an *at call that can be about a link or what it names:
   the link, please. */
#define AT_SYMLINK_NOFOLLOW 0x100

static const char *at_path(uint64_t dirfd, const char *name, char *out, size_t max) {
    struct handle *h;
    struct fs_file folder;
    size_t n;

    if (name == NULL || name[0] == '/' || (int32_t)dirfd == AT_FDCWD) {
        return name;
    }
    h = handle_of(dirfd);
    if (h == NULL || h->start != FOLDER_MARK) {
        return name;                /* not a folder: nothing to be relative to */
    }
    if (h->folder == 0) {
        folder.name[0] = '\0';      /* the root */
    } else if (fs_file(h->folder - 1, &folder) != 0) {
        return name;
    }
    /* The table spells a folder without a leading slash and with a trailing
       one, which is the one place a path is put together here. */
    n = strlen(folder.name);
    if (n + strlen(name) + 2 > max) {
        return NULL;
    }
    out[0] = '/';
    memcpy(out + 1, folder.name, n);
    strcpy(out + 1 + n, name);
    return out;
}

static uint64_t sys_openat(uint64_t dirfd, uint64_t path, uint64_t flags) {
    char joined[FS_NAME_LEN];

    return open_name(at_path(dirfd, user_string(path), joined, sizeof joined), flags);
}

static uint64_t sys_close(uint64_t fd, uint64_t b, uint64_t c) {
    struct handle *h = handle_of(fd);

    (void)b;
    (void)c;
    if (h == NULL) {
        return ERR(EBADF);
    }
    h->used = 0;
    if (h->start == PIPE_MARK) {
        pipe_drop(pipe_of(h));
        return 0;
    }
    if (h->start == SOCK_MARK) {
        if (net != NULL) {
            net->drop((int)h->folder);
        }
        return 0;
    }
    /* The name a file is written back to goes only with the last descriptor
       holding it: a copy made with dup keeps the file open. */
    if (h->writer > 0) {
        for (unsigned i = 0; i < PROGRAM_FILES; i++) {
            if (handles[i].used != 0 && handles[i].writer == h->writer) {
                return 0;
            }
        }
        writer_names[h->writer - 1][0] = '\0';
    }
    return 0;
}

/* A second descriptor for the same open file. Everything a handle holds is
   copied, where Linux would share it - the two then move through a file
   independently, which is the one thing a program doing this rarely
   notices, since it is redirecting rather than reading. */
static uint64_t dup_to(uint64_t fd, uint64_t to) {
    struct handle *h = handle_of(fd);

    if (h == NULL || to >= PROGRAM_FILES) {
        return ERR(EBADF);
    }
    if (to != fd) {
        struct pipe *p = pipe_of(h);

        if (handles[to].used != 0) {
            sys_close(to, 0, 0);
        }
        handles[to] = *h;
        if (p != NULL) {
            p->refs++;
        }
        if (h->start == SOCK_MARK && net != NULL) {
            net->hold((int)h->folder);
        }
    }
    return to;
}

static uint64_t sys_dup(uint64_t fd, uint64_t b, uint64_t c) {
    struct handle *h = handle_of(fd);
    struct pipe *p = pipe_of(h);
    uint64_t made;

    (void)b;
    (void)c;
    if (h == NULL) {
        return ERR(EBADF);
    }
    made = give_handle(*h);
    if ((int64_t)made >= 0 && p != NULL) {
        p->refs++;
    }
    if ((int64_t)made >= 0 && h->start == SOCK_MARK && net != NULL) {
        net->hold((int)h->folder);
    }
    return made;
}

static uint64_t sys_dup2(uint64_t fd, uint64_t to, uint64_t c) {
    (void)c;
    return dup_to(fd, to);
}

static uint64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence) {
    struct handle *h = handle_of(fd);

    if (h == NULL || whence > 2) {
        return ERR(h == NULL ? EBADF : EINVAL);
    }
    /* SEEK_SET, SEEK_CUR, SEEK_END, and the offset is signed. */
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)h->offset : (int64_t)h->size;
    int64_t where = base + (int64_t)offset;

    /* Past the end is allowed, as on Linux: a read there finds nothing, and
       a write there leaves zeroes in between - which is how an assembler
       lays out the sections of what it writes. */
    if (where < 0 || where > (int64_t)UINT32_MAX) {
        return ERR(EINVAL);
    }
    h->offset = (uint32_t)where;
    return (uint64_t)where;
}

/* Emptying a file, which is the only length a program ever truncates one to
   here: the file is rewritten from its start, as it would be by an open for
   writing. */
static uint64_t sys_ftruncate(uint64_t fd, uint64_t length, uint64_t c) {
    struct handle *h = handle_of(fd);

    (void)c;
    if (h == NULL || h->start != WRITE_MARK) {
        return ERR(EBADF);
    }
    if (length != 0) {
        return ERR(EINVAL);
    }
    if (fs_write(writer_names[h->writer - 1], NULL, 0) < 0) {
        return ERR(EIO);
    }
    h->offset = 0;
    return 0;
}

/* Linux's getcwd returns the length it wrote, the NUL included. */
static uint64_t sys_getcwd(uint64_t buf, uint64_t size, uint64_t c) {
    const char *cwd = fs_cwd();
    size_t n = strlen(cwd);
    char *out = (char *)buf;

    (void)c;
    /* The working directory is stored with a trailing slash and no leading
       one; a path wants the opposite, and the root is just "/". */
    if (n > 0) {
        n--;
    }
    if (!user_range(buf, size)) {
        return ERR(EFAULT);
    }
    if (size < n + 2) {
        return ERR(ERANGE);
    }
    out[0] = '/';
    memcpy(out + 1, cwd, n);
    out[n + 1] = '\0';
    return n + 2;
}

/* A program with a working directory of its own - a shell's cd - moves the
   one the machine has, there being only the one program. */
static uint64_t sys_chdir(uint64_t path, uint64_t b, uint64_t c) {
    const char *name = user_string(path);

    (void)b;
    (void)c;
    if (name == NULL) {
        return ERR(EFAULT);
    }
    return fs_errno(fs_chdir(name));
}

/* A folder that is already open, named by its entry in the table. A program
   that walks a tree keeps its way back open rather than by name, which is
   what `df` does before it will report anything. */
static uint64_t sys_fchdir(uint64_t fd, uint64_t b, uint64_t c) {
    struct handle *h = handle_of(fd);
    const char *name;

    (void)b;
    (void)c;
    if (h == NULL || h->start != FOLDER_MARK) {
        return ERR(EBADF);
    }
    if (h->folder == 0) {
        return fs_chdir("/") == 0 ? 0 : ERR(ENOENT);
    }
    struct fs_file entry;
    char path[FS_NAME_LEN + 1];

    if (fs_file(h->folder - 1, &entry) != 0) {
        return ERR(EBADF);
    }
    /* The table spells a folder with no leading slash, and a name without one
       is resolved from where we are now - which is not where the descriptor
       is. `mkdir -p a/b` saves the working directory, descends into a, and
       comes back through here; given a relative name it came back to a/<the
       directory it started in>, and nothing after that was where it looked
       for it. */
    path[0] = '/';
    strcpy(path + 1, entry.name);
    name = path;
    return fs_chdir(name) == 0 ? 0 : ERR(ENOENT);
}

/* ---- what a libc asks for ------------------------------------------------
 *
 * These are the calls a program makes before it does anything of its own:
 * where its heap is, where its thread-local data lives, whether its output
 * is a terminal. Each is answered honestly for a machine with one process,
 * one screen and memory that is already readable, writable and executable. */

#define ARCH_SET_FS 0x1002
#define MSR_FS_BASE 0xC0000100

static uint64_t program_break;   /* the heap's end */
static uint64_t program_map;     /* where the next mmap goes */

/* Where the heap starts, and where mmap starts handing memory out: in the
   program's own region where it has one, and otherwise sharing the fixed
   window with the program, the heap from the top of it and mmap from the
   far end growing down. */
static void program_memory_start(void) {
    program_break = vm_base() + USER_BRK;
    program_map = vm_base() + USER_MMAP;
}

static uint64_t sys_brk(uint64_t addr, uint64_t b, uint64_t c) {
    (void)b;
    (void)c;
    /* Linux answers a request it cannot meet with the break unchanged. */
    if (addr >= program_break && fits(addr, 0) &&
        addr < program_break + 0x10000000) {
        program_break = addr;
    }
    return program_break;
}

/* Memory, and sometimes a file in it.
 *
 * A loader maps a library by mmapping the file: once loosely, to see how much
 * room the whole of it needs, and then a segment at a time at fixed addresses
 * inside that room. There is no paging store behind any of this, so a mapping
 * is pages of memory with the file read into them - a private copy, which is
 * exactly what MAP_PRIVATE promises anyway. */

#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

/* ---- mappings a page fault fills in ---------------------------------------
 *
 * A loader maps a library whole and then uses a fraction of it: `uname` runs
 * on a few hundred kilobytes of a two-megabyte C library. Reading all of it,
 * into pages cleared first and bought one by one, was most of what starting a
 * program cost.
 *
 * So a file mapping is a promise rather than a copy. mmap writes down where
 * the memory is and what belongs there, and nothing is read until the program
 * touches it - then a chunk at a time, since a read costs its round trip to
 * the firmware and not its bytes. Memory the program never looks at is never
 * bought, never cleared and never read.
 *
 * The promises belong to the program, so they are put aside and brought back
 * with everything else of its when it starts another. */

#define MAPPINGS   16
#define FILL_PAGES 16       /* pages read around the one that faulted */

static struct mapping {
    uint64_t at, end;       /* the memory it covers; at == end when free */
    uint64_t offset;        /* where in the file `at` is */
    uint32_t start, size;   /* the file: its first sector, and its length */
} mappings[MAPPINGS];

/* Forgets whatever was promised for addr .. addr + size, which is what a
   mapping laid over an older one means. A hole in the middle of one leaves
   the part before it, since that is the only part a loader ever goes back
   to. */
static void map_trim(uint64_t at, uint64_t size) {
    uint64_t end = at + size;

    for (unsigned i = 0; i < MAPPINGS; i++) {
        struct mapping *m = &mappings[i];

        if (m->at == m->end || end <= m->at || at >= m->end) {
            continue;
        }
        if (at <= m->at && end >= m->end) {
            m->at = m->end = 0;
        } else if (at <= m->at) {
            m->offset += end - m->at;
            m->at = end;
        } else {
            m->end = at;
        }
    }
}

static bool map_record(uint64_t at, uint64_t size, uint32_t start, uint32_t bytes,
                       uint64_t offset) {
    map_trim(at, size);
    for (unsigned i = 0; i < MAPPINGS; i++) {
        if (mappings[i].at == mappings[i].end) {
            mappings[i] = (struct mapping){ .at = at, .end = at + size,
                                            .offset = offset,
                                            .start = start, .size = bytes };
            return true;
        }
    }
    return false;               /* no room to promise: read it now instead */
}

/* Makes good on the promises covering the page that faulted: a chunk of
   pages around it, with every file that has something to say about them read
   into it.
 *
 * A promise need not start or end on a page boundary - an ELF segment rarely
 * does - so the page that faulted is matched against the bytes a promise
 * covers rather than against whole pages, and a page two promises share gets
 * both of their stretches. */
static bool map_fill(uint64_t addr) {
    uint64_t page = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t chunk = FILL_PAGES * PAGE_SIZE;
    uint64_t first = 0, last = 0;
    bool found = false;

    /* The chunk to fill: the one this page falls in, counting from the start
       of the first promise that covers the page. */
    for (unsigned i = 0; i < MAPPINGS && !found; i++) {
        struct mapping *m = &mappings[i];
        uint64_t base;

        if (m->at == m->end || m->at >= page + PAGE_SIZE || m->end <= page) {
            continue;
        }
        base = m->at & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t top = (m->end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

        first = base + (page - base) / chunk * chunk;
        last = first + chunk;
        if (last > top) {
            last = top;
        }
        found = true;
    }
    if (!found) {
        return false;
    }
    /* A page already there has already been filled, and what the program has
       since written to it is its own: this fault is a write to a page a fork
       write-protected, and reading over it would undo the program's work. */
    if (vm_mapped(page)) {
        return false;
    }
    while (first < page && vm_mapped(first)) {
        first += PAGE_SIZE;
    }
    while (last > page + PAGE_SIZE && vm_mapped(last - PAGE_SIZE)) {
        last -= PAGE_SIZE;
    }
    if (!vm_reserve(first, last - first)) {
        return false;
    }
    /* Whatever the pages are owed. Anything no promise covers, and anything
       past the end of a file, is the zeroes the pages arrived as. */
    for (unsigned i = 0; i < MAPPINGS; i++) {
        struct mapping *m = &mappings[i];
        struct fs_file file;
        uint64_t from, into, count;

        if (m->at == m->end || m->at >= last || m->end <= first) {
            continue;
        }
        into = m->at > first ? m->at : first;
        count = (m->end < last ? m->end : last) - into;
        from = m->offset + (into - m->at);
        file = (struct fs_file){ .start = m->start, .size = m->size };
        if (from >= file.size) {
            continue;
        }
        if (count > file.size - from) {
            count = file.size - from;
        }
        if (read_at(&file, from, (void *)into, count) < 0) {
            return false;
        }
    }
    return true;
}

static uint64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot) {
    uint64_t flags = arg[3], fd = arg[4], offset = arg[5];
    uint64_t size = (length + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t at;

    (void)prot;                     /* every page here is already read-write-execute */
    if (size == 0) {
        return ERR(EINVAL);
    }
    if ((flags & MAP_FIXED) != 0) {
        at = addr & ~(uint64_t)(PAGE_SIZE - 1);
    } else if (vm_base() != 0) {
        at = program_map;
        program_map += size + PAGE_SIZE;     /* a gap, so a fixed map nearby is safe */
    } else {
        /* The old window: handed out from the far end, growing down. */
        if (size > program_map - program_break) {
            return ERR(ENOMEM);
        }
        program_map -= size;
        at = program_map;
    }
    if (!fits(at, size)) {
        return ERR(ENOMEM);
    }
    if ((flags & MAP_ANONYMOUS) != 0) {
        /* New memory is empty memory, and a page of this region arrives empty
           - so somewhere of the program's choosing needs nothing done to it
           at all until it is touched. A loader reserves a library's whole
           span this way and then maps pieces over it; buying the span was
           buying two megabytes to throw most of it away.

           A fixed address is different: it is somewhere that may already hold
           something, and the .bss of a library is exactly that - the tail of
           a segment already read in, which has to read as zeroes. */
        map_trim(at, size);
        if ((flags & MAP_FIXED) != 0) {
            if (!claim(at, size)) {
                return ERR(ENOMEM);
            }
            memset((void *)at, 0, size);
        }
        return at;
    }

    struct handle *h = handle_of(fd);
    uint64_t count = 0;

    if (h == NULL || h->start >= CONSOLE_MARK) {
        return ERR(EBADF);
    }
    if (map_record(at, size, h->start, h->size, offset)) {
        return at;                  /* read when, and if, it is touched */
    }
    if (!claim(at, size)) {
        return ERR(ENOMEM);
    }
    if (offset < h->size) {
        count = h->size - offset;
        if (count > size) {
            count = size;
        }
        struct fs_file file = { .start = h->start, .size = h->size };

        if (read_at(&file, offset, (void *)at, count) < 0) {
            return ERR(EIO);
        }
    }
    /* Past the end of the file the mapping reads as zeroes. */
    memset((char *)at + count, 0, size - count);
    return at;
}

static uint64_t sys_munmap(uint64_t addr, uint64_t length, uint64_t c) {
    (void)c;
    map_trim(addr, length);
    if (vm_holds(addr, length)) {
        vm_release(addr, length);   /* the pages go back to the firmware */
    }
    return 0;
}

static uint64_t sys_ok(uint64_t a, uint64_t b, uint64_t c) {
    (void)a;
    (void)b;
    (void)c;
    return 0;                       /* munmap, mprotect, set_tid_address */
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

/* What the filesystem said, as a program's libc expects to hear it. Getting
   this wrong is not cosmetic: `mkdir -p a/b` creates the parent only when it
   is told the parent is missing, and anything else it reports and stops. */
static uint64_t fs_errno(int err) {
    switch (err) {
    case 0:            return 0;
    case FS_ENOENT:    return ERR(ENOENT);
    case FS_EEXIST:    return ERR(EEXIST);
    case FS_ENOSPC:    return ERR(ENOSPC);
    case FS_ENOTEMPTY: return ERR(ENOTEMPTY);
    case FS_EINVAL:    return ERR(EINVAL);
    case FS_EIO:       return ERR(EIO);
    case FS_ELOOP:     return ERR(ELOOP);
    default:           return ERR(EACCES);
    }
}

static uint64_t sys_unlink(uint64_t path, uint64_t b, uint64_t c) {
    const char *name = user_string(path);

    (void)b;
    (void)c;
    if (name == NULL) {
        return ERR(EFAULT);
    }
    return fs_errno(fs_remove(name));
}

/* The same, from a folder the program already has open - which is what a
   libc actually calls. coreutils' rm is unlinkat and nothing else. */
static uint64_t sys_unlinkat(uint64_t dirfd, uint64_t path, uint64_t flags) {
    char joined[FS_NAME_LEN];
    const char *name = at_path(dirfd, user_string(path), joined, sizeof joined);

    (void)flags;                    /* AT_REMOVEDIR: a folder goes the same way */
    if (name == NULL) {
        return ERR(EFAULT);
    }
    return fs_errno(fs_remove(name));
}

static uint64_t sys_mkdir(uint64_t path, uint64_t mode, uint64_t c) {
    const char *name = user_string(path);

    (void)mode;
    (void)c;
    if (name == NULL) {
        return ERR(EFAULT);
    }
    return fs_errno(fs_mkdir(name));
}

static uint64_t sys_mkdirat(uint64_t dirfd, uint64_t path, uint64_t mode) {
    char joined[FS_NAME_LEN];
    const char *name = at_path(dirfd, user_string(path), joined, sizeof joined);

    (void)mode;
    if (name == NULL) {
        return ERR(EFAULT);
    }
    return fs_errno(fs_mkdir(name));
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

static uint64_t sys_getpid(uint64_t a, uint64_t b, uint64_t c) {
    (void)a;
    (void)b;
    (void)c;
    return 1;                       /* there is only ever the one */
}

/* Only ARCH_SET_FS, which is how a libc points at its thread-local data.
   Even a program with one thread has to have it set, or the first access to
   a thread variable reads address zero; each thread keeps its own. */
static uint64_t sys_arch_prctl(uint64_t code, uint64_t addr, uint64_t c) {
    (void)c;
    if (code != ARCH_SET_FS || !user_range(addr, 0)) {
        return ERR(EINVAL);
    }
    wrmsr(MSR_FS_BASE, addr);
    return 0;
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
/* What a TCGETS hands over, and what the one carrying the line speeds does. */
#define TERMIOS_OLD 36
#define TERMIOS_NEW 44
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

/* A read from a given place that leaves the file where it was. */
static uint64_t sys_pread64(uint64_t fd, uint64_t buf, uint64_t count) {
    struct handle *h = handle_of(fd);
    uint64_t offset = arg[3];
    uint32_t was;
    uint64_t got;

    if (h == NULL || (h->start >= FIRST_MARK && h->start != WRITE_MARK)) {
        return ERR(EBADF);          /* not a file on the disk */
    }
    was = h->offset;
    if (offset > h->size) {
        return 0;
    }
    h->offset = (uint32_t)offset;
    got = sys_read(fd, buf, count);
    h->offset = was;
    return got;
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

/* Futexes: a thread waits on a word until another wakes it, running the
   others meanwhile (thread.c). One that is alone has nobody to wake it, so
   a wait for ever is answered with "the value changed" - which sends it
   back to look at its lock again rather than into a wait that never ends. */
#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_REQUEUE        3
#define FUTEX_CMP_REQUEUE    4
#define FUTEX_WAKE_OP        5
#define FUTEX_WAIT_BITSET    9
#define FUTEX_WAKE_BITSET    10
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_CMD            0x7F
#define ETIMEDOUT            110

static uint64_t realtime_ms(void);
static uint64_t timespec_ms(uint64_t spec);

static uint64_t futex_wait(uint64_t address, uint64_t op, uint32_t value, uint64_t timeout) {
    uint64_t until = 0;

    if (timeout != 0) {
        if (!user_range(timeout, 16)) {
            return ERR(EFAULT);
        }
        uint64_t at = timespec_ms(timeout), now = efi_uptime_ms();

        if ((op & FUTEX_CMD) == FUTEX_WAIT_BITSET) {    /* a time, not a wait */
            uint64_t clock = (op & FUTEX_CLOCK_REALTIME) ? realtime_ms() : now;

            at = at > clock ? at - clock : 0;
        }
        until = now + at + 1;
    }
    if (*(volatile uint32_t *)address != value) {
        return ERR(EAGAIN);
    }
    if (until == 0 && thread_alone()) {
        return ERR(EAGAIN);
    }
    thread_sleep_on(address);
    while (!thread_woken()) {
        if (until != 0 && efi_uptime_ms() >= until) {
            thread_sleep_on(0);
            return ERR(ETIMEDOUT);
        }
        interrupt_check();
        thread_yield();
    }
    return 0;
}

/* FUTEX_WAKE_OP: changes the second word, wakes at the first, and at the
   second too if what was there passes a test. */
static uint64_t futex_wake_op(uint64_t address, uint32_t n, uint64_t address2, uint32_t n2,
                              uint32_t code) {
    uint32_t *word = (uint32_t *)address2, old;
    int32_t oparg = (int32_t)(code << 8) >> 20, cmparg = (int32_t)(code << 20) >> 20;
    unsigned op = code >> 28, cmp = code >> 24 & 15;
    bool pass;

    if (!user_range(address2, 4)) {
        return ERR(EFAULT);
    }
    if (op & 8) {
        oparg = 1 << (oparg & 31);
    }
    old = *word;
    switch (op & 7) {
    case 0: *word = (uint32_t)oparg; break;
    case 1: *word = old + (uint32_t)oparg; break;
    case 2: *word = old | (uint32_t)oparg; break;
    case 3: *word = old & ~(uint32_t)oparg; break;
    case 4: *word = old ^ (uint32_t)oparg; break;
    default: return ERR(ENOSYS);
    }
    switch (cmp) {
    case 0: pass = (int32_t)old == cmparg; break;
    case 1: pass = (int32_t)old != cmparg; break;
    case 2: pass = (int32_t)old < cmparg; break;
    case 3: pass = (int32_t)old <= cmparg; break;
    case 4: pass = (int32_t)old > cmparg; break;
    case 5: pass = (int32_t)old >= cmparg; break;
    default: return ERR(ENOSYS);
    }
    int woke = thread_wake(address, (int)n, 0, 0);

    return (uint64_t)(woke + (pass ? thread_wake(address2, (int)n2, 0, 0) : 0));
}

static uint64_t sys_futex(uint64_t address, uint64_t op, uint64_t value) {
    uint64_t timeout = arg[3], address2 = arg[4], value3 = arg[5];

    if (!user_range(address, 4)) {
        return ERR(EFAULT);
    }
    switch (op & FUTEX_CMD) {
    case FUTEX_WAIT:
    case FUTEX_WAIT_BITSET:
        return futex_wait(address, op, (uint32_t)value, timeout);
    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET:
        return (uint64_t)thread_wake(address, (int)value, 0, 0);
    case FUTEX_CMP_REQUEUE:
        if (*(volatile uint32_t *)address != (uint32_t)value3) {
            return ERR(EAGAIN);
        }
        /* fall through */
    case FUTEX_REQUEUE:
        return (uint64_t)thread_wake(address, (int)value, address2, (int)timeout);
    case FUTEX_WAKE_OP:
        return futex_wake_op(address, (uint32_t)value, address2, (uint32_t)timeout,
                             (uint32_t)value3);
    }
    return ERR(ENOSYS);
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

static uint64_t sys_root(uint64_t a, uint64_t b, uint64_t c) {
    (void)a;
    (void)b;
    (void)c;
    return 0;                       /* getuid and its kin: this is root */
}

/* There is one process, so it is its own group and its own session, and
   putting it in either changes nothing. A shell asks before it decides
   whether it can run jobs; one that is refused outright gives up and exits,
   so these answer rather than being left out. */
static uint64_t sys_getpgrp(uint64_t a, uint64_t b, uint64_t c) {
    (void)a;
    (void)b;
    (void)c;
    return 1;
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

/* The file mode a program's own files would be trimmed by. Nothing here has
   permissions, so this is the usual answer and nothing more. */
static uint64_t sys_umask(uint64_t mask, uint64_t b, uint64_t c) {
    (void)mask;
    (void)b;
    (void)c;
    return 022;
}

#define S_IFIFO 0010000
#define S_IFCHR 0020000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IFLNK 0120000
#define S_IFSOCK 0140000

/* Descriptors have no flags worth keeping here: a program setting
   close-on-exec is told it worked, and one asking gets nothing back. The
   one command that does something is F_DUPFD, which a libc uses to move a
   descriptor out of the way of the ones a program is about to redirect -
   and which is dup by another name. The lowest free descriptor is handed
   back rather than the one asked for, there being few enough of them. */
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_DUPFD_CLOEXEC 1030

static uint64_t sys_fcntl(uint64_t fd, uint64_t command, uint64_t c) {
    struct handle *h = handle_of(fd);

    (void)c;
    if (h == NULL) {
        return ERR(EBADF);
    }
    switch (command) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        return sys_dup(fd, 0, 0);
    case F_GETFL:
        /* A file open for writing can be read back too, so that is what it
           says: a libc that asked for "w+" and is told write-only gives up
           on the file it has just been handed. */
        if (h->start == SOCK_MARK || h->start == PIPE_MARK) {
            return (h->start == SOCK_MARK || h->size == PIPE_EVENT ? O_RDWR :
                    h->size == PIPE_WRITE ? O_WRONLY : O_RDONLY) | (h->offset & O_NONBLOCK);
        }
        return h->start == CONSOLE_MARK || h->start == WRITE_MARK ? O_RDWR : O_RDONLY;
    case F_SETFL:
        if (h->start == SOCK_MARK || h->start == PIPE_MARK) {
            h->offset = (h->offset & ~O_NONBLOCK) | (arg[2] & O_NONBLOCK);
        }
        return 0;
    case F_GETFD:
    case F_SETFD:
        return 0;
    }
    return ERR(EINVAL);
}

struct iovec {
    uint64_t base;
    uint64_t length;
};

static uint64_t sys_readv(uint64_t fd, uint64_t vectors, uint64_t count);

static uint64_t sys_writev(uint64_t fd, uint64_t vectors, uint64_t count) {
    uint64_t written = 0;

    if (!user_range(vectors, count * sizeof(struct iovec))) {
        return ERR(EFAULT);
    }
    for (uint64_t i = 0; i < count; i++) {
        const struct iovec *v = (const struct iovec *)vectors + i;
        uint64_t n = sys_write(fd, v->base, v->length);

        if (n == (uint64_t)-1) {
            return written > 0 ? written : n;
        }
        written += n;
    }
    return written;
}

/* Something that varies, for the stack guard a libc sets up before main.
   There is no entropy source on the machine, so this is the clock stirred
   about - enough to keep the guard from being the same value every boot, and
   no more than that. */
/* The same as read, into several buffers in turn. A short read on one of
   them ends the whole call, as Linux's does. */
static uint64_t sys_readv(uint64_t fd, uint64_t vectors, uint64_t count) {
    const struct iovec *v = (const struct iovec *)vectors;
    uint64_t total = 0;

    if (!user_range(vectors, count * sizeof *v)) {
        return ERR(EFAULT);
    }
    for (uint64_t i = 0; i < count; i++) {
        uint64_t got;

        if (v[i].length == 0) {
            continue;
        }
        got = sys_read(fd, (uint64_t)v[i].base, v[i].length);
        if ((int64_t)got < 0) {
            return total > 0 ? total : got;
        }
        total += got;
        if (got < v[i].length) {
            break;
        }
    }
    return total;
}

/* A write that does not move the descriptor, which is where it is written.
   Only a file open for writing has anywhere to put one. */
static uint64_t sys_pwrite64(uint64_t fd, uint64_t text, uint64_t count) {
    struct handle *h = handle_of(fd);
    uint64_t offset = arg[3];

    if (!user_range(text, count)) {
        return ERR(EFAULT);
    }
    if (h == NULL || h->start != WRITE_MARK) {
        return ERR(EBADF);
    }
    if (fs_write_at(writer_names[h->writer - 1], (uint32_t)offset,
                    (const void *)text, count) < 0) {
        return ERR(EIO);
    }
    if (offset + count > h->size) {
        h->size = (uint32_t)(offset + count);
    }
    return count;
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
            (h->start != PIPE_MARK || h->size != PIPE_EVENT || event_ready(h)) &&
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
        if (sockets) {
            interrupt_check();
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

static uint64_t sys_getrandom(uint64_t buf, uint64_t length, uint64_t flags) {
    uint64_t state = efi_seconds() * 6364136223846793005ull + 1442695040888963407ull;

    (void)flags;
    if (!user_range(buf, length)) {
        return ERR(EFAULT);
    }
    for (uint64_t i = 0; i < length; i++) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        ((uint8_t *)buf)[i] = (uint8_t)(state >> 33);
    }
    return length;
}

/* A thread that ends holding a lock is not looked for, so the list of its
   locks is not kept. */
static uint64_t sys_set_robust_list(uint64_t head, uint64_t length, uint64_t c) {
    (void)head;
    (void)length;
    (void)c;
    return 0;
}

static uint64_t sys_set_tid_address(uint64_t a, uint64_t b, uint64_t c) {
    (void)a;
    (void)b;
    (void)c;
    return (uint64_t)thread_id();
}

static uint64_t sys_gettid(uint64_t a, uint64_t b, uint64_t c) {
    (void)a;
    (void)b;
    (void)c;
    return (uint64_t)thread_id();
}

static uint64_t sys_sched_yield(uint64_t a, uint64_t b, uint64_t c) {
    (void)a;
    (void)b;
    (void)c;
    thread_yield();
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

/* A hard link, or a device node: there are neither here, and a program told
   so goes on to copy rather than stopping. */
static uint64_t sys_no_links(uint64_t a, uint64_t b, uint64_t c) {
    (void)a;
    (void)b;
    (void)c;
    return ERR(EPERM);
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
static uint32_t folder_ino(unsigned one_based) {
    return one_based + 1;
}

/* Whether a table entry is a folder: the filesystem spells one with a slash
   on the end, and gives it no first sector - so the entry alone cannot be
   told from an empty file without looking at the name. */
static bool is_folder_entry(const struct fs_file *file) {
    size_t n = strlen(file->name);

    return n > 0 && file->name[n - 1] == '/';
}

static void fill_stat(struct stat *out, uint64_t size, bool folder, uint32_t start) {
    memset(out, 0, sizeof *out);
    out->dev = 1;
    out->ino = start != 0 ? start : 1;
    out->nlink = 1;
    out->mode = (folder ? S_IFDIR | 0755 : S_IFREG | 0644);
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
              h->start == PROCDIR_MARK ? PROC_INO : h->start);
    if (h->start == CONSOLE_MARK) {
        /* Not a file at all: a program told this is a regular file reads it
           as one, all at once and to its end. */
        st->mode = S_IFCHR | 0620;
        st->size = 0;
        st->blocks = 0;
        st->rdev = 0x0500 | fd;     /* a terminal, as Linux numbers them */
    } else if (h->start == PIPE_MARK) {
        struct pipe *p = pipe_of(h);

        st->mode = h->size == PIPE_FILE ? S_IFREG | 0444 : S_IFIFO | 0600;
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
        out->ino = file.start;
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
    out->mode = (uint16_t)(folder ? S_IFDIR | 0755 : S_IFREG | 0644);
    out->ino = file.start != 0 ? file.start : 1;
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
        fill_stat((struct stat *)out, file.size & ~FS_LINK, false, file.start);
        ((struct stat *)out)->mode = S_IFLNK | 0777;
        return 0;
    }
    int err = fs_stat(name, &file);

    if (err == 0 && !is_folder_entry(&file)) {
        fill_stat((struct stat *)out, file.size, false, file.start);
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

/* /proc, which holds one entry per built-in command and nothing else. */
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
        out->ino = is_folder ? folder_ino((unsigned)index + 1) : entry.start;
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

    while (efi_uptime_ms() < until_ms) {
        thread_yield();
        __asm__ volatile("pause");
    }
    wait_ended(began);
}

static uint64_t timespec_ms(uint64_t spec) {
    const int64_t *t = (const int64_t *)spec;

    return (uint64_t)t[0] * 1000 + (uint64_t)(t[1] + 999999) / 1000000;
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
    return 0;
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
    return 0;
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

/* ---- descriptors and exceptions ------------------------------------------
 *
 * A program's page faults are how its memory gets mapped, and any other
 * exception it raises ends it, instead of rebooting the machine.
 *
 * syscall and sysret do not take selectors: they take whatever sits at four
 * consecutive slots the STAR register points at. The firmware's descriptors
 * have to stay exactly where they are, because firmware code still runs and
 * still uses them, so ours are copied in behind them and STAR is pointed at
 * the copy. The TSS is here only for RSP0, the stack an exception from ring
 * 3 switches to, which syscall_entry.asm keeps pointed at the syscall
 * stack. */

#define TSS_SIZE       104
#define TSS_IOMAP      102      /* holds the I/O permission bitmap's offset */
#define EXCEPTIONS     32
#define VEC_PAGE_FAULT 14
#define GDT_BYTES      256      /* room for the firmware's table and ours:
                                   a firmware's own runs to a few dozen
                                   bytes, and one that does not fit is done
                                   without rather than made room for */

struct idt_gate {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct desc_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static uint8_t        gdt[GDT_BYTES] __attribute__((aligned(16)));

/* syscall_entry.asm keeps the kernel stack pointer in this, at RSP0, so it
   is reached by name from there and cannot be static. */
uint8_t tss[TSS_SIZE] __attribute__((aligned(16), used));
static uint16_t       kernel_cs;        /* where our own code segment landed */

/* Four in this order, because that is the order sysret expects them. */
static const uint64_t descriptors[4] = {
    0x00AF9A000000FFFF,     /* kernel code, 64-bit: what syscall switches to */
    0x00CF92000000FFFF,     /* kernel data */
    0x00CFF2000000FFFF,     /* user data, ring 3 */
    0x00AFFA000000FFFF,     /* user code, 64-bit */
};

static void gdt_init(void) {
    struct desc_ptr had, ours;
    unsigned used;

    __asm__ volatile("sgdt %0" : "=m"(had));
    used = (unsigned)had.limit + 1;
    if (used > GDT_BYTES - sizeof descriptors - 16) {
        used = 0;                       /* implausibly large; do without it */
    } else {
        memcpy(gdt, (const void *)had.base, used);
    }
    used = (used + 7) & ~7u;            /* our block has to be aligned */

    kernel_cs = (uint16_t)used;
    memcpy(gdt + used, descriptors, sizeof descriptors);

    /* The TSS descriptor is twice the width of the others, and carries the
       address of the TSS split across it. */
    uint64_t base = (uint64_t)tss;
    uint64_t *slot = (uint64_t *)(gdt + used + sizeof descriptors);
    slot[0] = (TSS_SIZE - 1) | (base & 0xFFFFFF) << 16 |
              (uint64_t)0x89 << 40 | (base >> 24 & 0xFF) << 56;
    slot[1] = base >> 32;

    ours.limit = (uint16_t)(used + sizeof descriptors + 16 - 1);
    ours.base = (uint64_t)gdt;
    __asm__ volatile("lgdt %0" : : "m"(ours));
    /* Every selector the firmware was using still means what it did, because
       its descriptors were copied in at the very same offsets - so there is
       nothing to reload. */
    __asm__ volatile("ltr %w0" : : "r"((uint16_t)(used + sizeof descriptors)));

    /* Ring 3's two selectors, with the bits that say ring 3 already on: what
       the frame an IRETQ returns through has to carry. */
    user_ss = (uint64_t)(kernel_cs + 16) | 3;
    user_cs = (uint64_t)(kernel_cs + 24) | 3;
}

/* The firmware's interrupt table is kept and only the first thirty-two
   entries - the CPU's own exceptions - are pointed at our handlers.
 *
 * Installing a table of our own instead would take the hardware interrupts
 * with it, and the firmware's timer is what drives its keyboard: the driver
 * polls on a timer event and leaves what it finds in a queue for
 * ReadKeyStroke. A kernel that owns the interrupt table here is a kernel
 * nobody can type at. */
static void traps_init(void) {
    struct desc_ptr idtr;
    struct idt_gate *idt;

    memset(tss, 0, TSS_SIZE);
    *(uint16_t *)(tss + TSS_IOMAP) = TSS_SIZE;  /* no bitmap: no ports for ring 3 */

    __asm__ volatile("sidt %0" : "=m"(idtr));
    if (idtr.limit < EXCEPTIONS * sizeof(struct idt_gate) - 1) {
        return;
    }
    idt = (struct idt_gate *)idtr.base;

    uint64_t cr0 = write_protect(false);
    for (unsigned i = 0; i < EXCEPTIONS; i++) {
        uint64_t handler = i == VEC_PAGE_FAULT ? (uint64_t)page_fault_entry
                                               : (uint64_t)trap_stubs + i * 16;
        idt[i] = (struct idt_gate){
            .offset_low  = (uint16_t)handler,
            .selector    = kernel_cs,
            .type        = 0x8E,        /* present 64-bit interrupt gate */
            .offset_mid  = (uint16_t)(handler >> 16),
            .offset_high = (uint32_t)(handler >> 32),
        };
    }
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

/* A real binary's memcpy is written in SSE, which faults unless the operating
   system says it is prepared to have those registers used. Nothing here ever
   has to save them: one program runs at a time and nothing interrupts it
   into another. */
static void sse_init(void) {
    uint64_t cr0, cr4;

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 = (cr0 & ~(uint64_t)(1 << 2)) | (1 << 1);   /* not emulated, monitored */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 9) | (1 << 10);        /* OSFXSR, OSXMMEXCPT */
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
    __asm__ volatile("fninit");
}

/* fsync and its kin: every write is on the disk already, and this makes
   the drive keep it. */
static uint64_t sys_sync(uint64_t a, uint64_t b, uint64_t c) {
    (void)a;
    (void)b;
    (void)c;
    ata_sync();
    return 0;
}

/* The calls answered, number and handler side by side. The calls a libc
   makes for the plainest commands are among them: rm is unlinkat, mkdir is
   mkdirat, mv is renameat2. */
static const uint16_t numbers[] = {
    SYS_READ,
    SYS_WRITE,
    SYS_OPEN,
    SYS_CLOSE,
    SYS_FSTAT,
    SYS_LSEEK,
    SYS_MMAP,
    SYS_MPROTECT,
    SYS_MUNMAP,
    SYS_BRK,
    SYS_IOCTL,
    SYS_WRITEV,
    SYS_GETPID,
    SYS_UNAME,
    SYS_SYSINFO,
    SYS_SCHED_YIELD,
    SYS_MADVISE,
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
    SYS_FSYNC,
    SYS_FDATASYNC,
    SYS_SYNC,
    SYS_FADVISE64,
    SYS_CLOCK_NANOSLEEP,
    SYS_RT_SIGSUSPEND,
    SYS_KILL,
    SYS_TGKILL,
    SYS_GETTID,
    SYS_GETCWD,
    SYS_ARCH_PRCTL,
    SYS_GETDENTS64,
    SYS_SET_TID_ADDRESS,
    SYS_GETRANDOM,
    SYS_SET_ROBUST_LIST,
    SYS_PRLIMIT64,
    SYS_READLINKAT,
    SYS_CLOCK_GETTIME,
    SYS_CLOCK_GETRES,
    SYS_PREAD64,
    SYS_UNLINK,
    SYS_RMDIR,
    SYS_MKDIR,
    SYS_RENAME,
    SYS_STATFS,
    SYS_REBOOT,
    SYS_ACCESS,
    SYS_FACCESSAT,
    SYS_FACCESSAT2,
    SYS_STATX,
    SYS_FCNTL,
    SYS_FUTEX,
    SYS_SCHED_GETAFFINITY,
    SYS_GETUID,
    SYS_GETGID,
    SYS_GETEUID,
    SYS_GETEGID,
    SYS_RT_SIGACTION,
    SYS_RT_SIGPROCMASK,
    SYS_NANOSLEEP,
    SYS_EXIT,
    SYS_EXIT_GROUP,
    SYS_OPENAT,
    SYS_NEWFSTATAT,
    SYS_STAT,
    SYS_LSTAT,
    SYS_CHDIR,
    SYS_FCHDIR,
    SYS_DUP,
    SYS_DUP2,
    SYS_DUP3,
    SYS_FTRUNCATE,
    SYS_UMASK,
    SYS_GETTIMEOFDAY,
    SYS_TIME,
    SYS_GETPPID,
    SYS_GETPGRP,
    SYS_GETPGID,
    SYS_SETPGID,
    SYS_SETSID,
    SYS_READLINK,
    SYS_SIGALTSTACK,
    SYS_GETRESUID,
    SYS_GETRESGID,
    SYS_SELECT,
    SYS_PSELECT6,
    SYS_FORK,
    SYS_VFORK,
    SYS_CLONE,
    SYS_CLONE3,
    SYS_RT_SIGTIMEDWAIT,
    SYS_TIMER_CREATE,
    SYS_TIMER_SETTIME,
    SYS_TIMER_GETTIME,
    SYS_TIMER_GETOVERRUN,
    SYS_TIMER_DELETE,
    SYS_EXECVE,
    SYS_WAIT4,
    SYS_PIPE,
    SYS_PIPE2,
    SYS_EVENTFD,
    SYS_EVENTFD2,
    SYS_UNLINKAT,
    SYS_MKDIRAT,
    SYS_RENAMEAT,
    SYS_RENAMEAT2,
    SYS_LINKAT,
    SYS_SYMLINKAT,
    SYS_LINK,
    SYS_SYMLINK,
    SYS_MKNODAT,
    SYS_TRUNCATE,
    SYS_READV,
    SYS_PWRITE64,
    SYS_POLL,
    SYS_PPOLL,
    SYS_FSTATFS,
    SYS_GETRLIMIT,
    SYS_SETRLIMIT,
    SYS_FLOCK,
    SYS_FALLOCATE,
    SYS_MSYNC,
    SYS_SYNCFS,
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
    sys_read,
    sys_write,
    sys_open,
    sys_close,
    sys_fstat,
    sys_lseek,
    sys_mmap,
    sys_ok,
    sys_munmap,
    sys_brk,
    sys_ioctl,
    sys_writev,
    sys_getpid,
    sys_uname,
    sys_sysinfo,
    sys_sched_yield,
    sys_ok,
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
    sys_sync,
    sys_sync,
    sys_sync,
    sys_ok,
    sys_clock_nanosleep,
    sys_ok,
    sys_ok,
    sys_ok,
    sys_gettid,
    sys_getcwd,
    sys_arch_prctl,
    sys_getdents64,
    sys_set_tid_address,
    sys_getrandom,
    sys_set_robust_list,
    sys_prlimit64,
    sys_readlinkat,
    sys_clock_gettime,
    sys_clock_getres,
    sys_pread64,
    sys_unlink,
    sys_unlink,
    sys_mkdir,
    sys_rename,
    sys_statfs,
    sys_reboot,
    sys_access,
    sys_faccessat,
    sys_faccessat,
    sys_statx,
    sys_fcntl,
    sys_futex,
    sys_sched_getaffinity,
    sys_root,
    sys_root,
    sys_root,
    sys_root,
    sys_ok,
    sys_ok,
    sys_nanosleep,
    sys_exit,
    sys_exit_group,
    sys_openat,
    sys_newfstatat,
    sys_stat,
    sys_lstat,
    sys_chdir,
    sys_fchdir,
    sys_dup,
    sys_dup2,
    sys_dup2,
    sys_ftruncate,
    sys_umask,
    sys_gettimeofday,
    sys_time,
    sys_root,
    sys_getpgrp,
    sys_getpgrp,
    sys_ok,
    sys_getpgrp,
    sys_readlink,
    sys_ok,
    sys_getresuid,
    sys_getresuid,
    sys_select,
    sys_pselect6,
    sys_fork,
    sys_fork,
    sys_clone,
    sys_clone3,
    sys_rt_sigtimedwait,
    sys_timer_create,
    sys_timer_settime,
    sys_timer_gettime,
    sys_ok,
    sys_ok,
    sys_execve,
    sys_wait4,
    sys_pipe,
    sys_pipe2,
    sys_eventfd,
    sys_eventfd2,
    sys_unlinkat,
    sys_mkdirat,
    sys_renameat,
    sys_renameat,
    sys_no_links,
    sys_symlinkat,
    sys_no_links,
    sys_symlink,
    sys_no_links,
    sys_truncate,
    sys_readv,
    sys_pwrite64,
    sys_poll,
    sys_ppoll,
    sys_fstatfs,
    sys_getrlimit,
    sys_ok,
    sys_ok,
    sys_ok,
    sys_ok,
    sys_sync,
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

#define SYSCALLS (sizeof numbers / sizeof numbers[0])

void syscall_init(void) {
    gdt_init();
    sse_init();
    traps_init();
    vm_start();                     /* the region a program runs in */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
    /* syscall takes its code segment from one half and sysret counts on from
       the other: the four descriptors gdt_init laid down, in their order. */
    wrmsr(MSR_STAR, (uint64_t)(kernel_cs + 8) << 48 | (uint64_t)kernel_cs << 32);
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_FMASK, RFLAGS_MASK);

    for (unsigned i = 0; i < SYSCALLS; i++) {
        if (numbers[i] < LOW_NUMBERS) {
            low[numbers[i]] = (uint8_t)(i + 1);
        }
    }
}

/* The handler for a number, or NULL. */
static syscall_fn handler_for(uint64_t number) {
    if (number < LOW_NUMBERS) {
        return low[number] != 0 ? handlers[low[number] - 1] : NULL;
    }
    for (unsigned i = 0; i < SYSCALLS; i++) {
        if (numbers[i] == number) {
            return handlers[i];
        }
    }
    return NULL;
}

/* Called by syscall_entry. Every call is recorded on the way through,
   including the numbers this kernel has no handler for. */
uint64_t syscall_dispatch(uint64_t a, uint64_t b, uint64_t c,
                          uint64_t d, uint64_t e, uint64_t f, uint64_t number) {
    void *entry = tracer != NULL ? tracer->begin((uint32_t)number, a, b, c) : NULL;
    uint64_t result = ERR(ENOSYS);
    arg[0] = a;
    arg[1] = b;
    arg[2] = c;
    arg[3] = d;
    arg[4] = e;
    arg[5] = f;

    syscall_fn handler = handler_for(number);

    if (handler == NULL && net != NULL) {
        handler = net->syscall(number);     /* the socket calls are the module's */
    }

    if (handler != NULL) {
        struct kernel_mark mark = kernel_began();

        result = handler(a, b, c);
        kernel_ended(mark);
    }

    /* SYS_EXIT does not come back, and neither does a program killed
       mid-call, so those entries stay unfinished - which is worth seeing. */
    if (entry != NULL && tracer != NULL) {
        tracer->end(entry, result);
    }
    return result;
}

/* ---- program memory ------------------------------------------------------
 *
 * Every page a program has comes from vm.c: its own region, hung off a slot
 * of the firmware's top-level page table, bought a page at a time as it is
 * touched and handed back the moment the program ends. An idle machine holds
 * none of it.
 *
 * A program linked to run at a fixed address - 0x400000, where a plain `ld`
 * puts things - gets low memory as well, for its own image; the rest of it,
 * the loader, the heap and the stack, is in its region like anyone's. */

/* The firmware write-protects its own page tables and its own descriptors -
   CR0.WP, which makes even ring 0 respect a read-only page - so editing one
   means turning that off for as long as the edit takes. */
static uint64_t write_protect(bool on) {
    uint64_t cr0;

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" : : "r"(on ? cr0 | 0x10000 : cr0 & ~0x10000ull) : "memory");
    return cr0;
}

/* The tables that are the machine's rather than any program's: what low
   memory has cost, which stays until the machine is switched off. The tables
   describing a region come and go with the program they describe, so they
   are counted as the program's - which is what keeps what the machine costs
   the same figure whoever asks and whatever is running. */
size_t program_tables(void) {
    return vm_fixed_tables();
}

size_t program_memory(void) {
    return vm_memory() + vm_tables();
}

/* Called by a trap stub with the vector that fired, just before the program
   is ended. Naming it is the difference between "something went wrong" and
   knowing a program used an instruction this machine never turned on. */
void trap_report(unsigned vector, const uint64_t *frame) {
    static const char *const named[] = {
        [0] = "divide by zero", [1] = "debug", [3] = "breakpoint",
        [4] = "overflow", [5] = "bound range", [6] = "illegal instruction",
        [7] = "no maths unit", [8] = "double fault", [10] = "bad task switch",
        [11] = "segment not there", [12] = "bad stack", [13] = "protection fault",
        [16] = "maths error", [17] = "misaligned", [19] = "SSE error",
    };
    const char *name = vector < sizeof named / sizeof named[0] && named[vector] != NULL
                     ? named[vector] : "exception";

    /* Past the RDI the stub kept, the processor's own frame: an error code
       for the vectors that have one, then where it was. */
    bool coded = vector == 8 || (vector >= 10 && vector <= 14) || vector == 17 ||
                 vector == 21 || vector == 29 || vector == 30;

    const uint64_t *cpu = frame + (coded ? 2 : 1);

    dbg("trap %u (%s) at %x code %x: killed\n", (uint64_t)vector, name,
        cpu[0], coded ? frame[1] : 0);
}

/* Called by page_fault_entry with the address that faulted, and where from. */
void page_fault(uint64_t addr, uint64_t rip) {
    struct kernel_mark mark = kernel_began();

    /* Something promised to a mapping comes first: the page is not merely
       empty, it has a stretch of a file that belongs in it. */
    if (map_fill(addr) || vm_fault(addr)) {
        kernel_ended(mark);
        return;
    }
    dbg("fault at %x from %x: killed\n", addr, rip);
    process_exit(PROGRAM_KILLED);
}

/* ---- loading -----------------------------------------------------------
 *
 * ELF64 as a loader sees it: the file header names the entry point and the
 * program header table, and each PT_LOAD entry in that table is a stretch of
 * the file to copy to a fixed address, zero-padded to memsz. Everything else
 * an ELF carries - sections, symbols, relocations, permission flags - is for
 * linkers and debuggers, and a linked executable needs none of it here: the
 * memory these programs live in is already read-write-execute. */

#define ELF_CLASS64 2       /* ident[4] */
#define ELF_LITTLE  1       /* ident[5] */
#define ET_EXEC     2
#define ET_DYN      3       /* position-independent: it runs wherever it is put */
#define EM_X86_64   0x3E
#define PT_LOAD     1
#define PT_INTERP   3       /* the path of the loader it wants */

struct elf_header {
    uint8_t  ident[16];     /* magic, class, endianness, version */
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;         /* the program header table, as a file offset */
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;     /* stride of the table, not necessarily our size */
    uint16_t phnum;
} __attribute__((packed));

struct elf_program {
    uint32_t type;
    uint32_t flags;         /* ignored: everything here is RWX anyway */
    uint64_t offset;        /* where it is in the file ... */
    uint64_t vaddr;         /* ... and where it goes in memory */
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;         /* anything past filesz reads as zero (.bss) */
} __attribute__((packed));  /* p_align follows, and we do not need it */

/* Copies size bytes from offset in the file to dest, going through the
   filesystem's one sector buffer. Returns 0, FS_EIO, or PROGRAM_EINVAL if
   the range runs off the end of the file. */
static int read_at(const struct fs_file *file, uint64_t offset, void *dest, uint64_t size) {
    char *out = dest;

    if (offset > file->size || size > (uint64_t)file->size - offset) {
        return PROGRAM_EINVAL;
    }
    while (size > 0) {
        /* Once we are on a sector boundary with whole sectors left to read,
           they go straight to their destination in one call rather than one
           at a time through the filesystem's buffer - the difference between
           a thousand round trips to the firmware and one. */
        if (offset % SECTOR_SIZE == 0 && size >= SECTOR_SIZE) {
            unsigned count = (unsigned)(size / SECTOR_SIZE);

            if (fs_read_many(file->start, (unsigned)(offset / SECTOR_SIZE), count, out) < 0) {
                return FS_EIO;
            }
            uint64_t moved = (uint64_t)count * SECTOR_SIZE;
            out += moved;
            offset += moved;
            size -= moved;
            continue;
        }
        const char *sector = fs_sector(file->start, (unsigned)(offset / SECTOR_SIZE));
        if (sector == NULL) {
            return FS_EIO;
        }
        uint64_t chunk = SECTOR_SIZE - offset % SECTOR_SIZE;
        if (chunk > size) {
            chunk = size;
        }
        memcpy(out, sector + offset % SECTOR_SIZE, (size_t)chunk);
        out += chunk;
        offset += chunk;
        size -= chunk;
    }
    return 0;
}

/* True if addr .. addr + size is room a program may load into or start at:
   its own region, or the low memory given one linked to a fixed address. */
static bool fits(uint64_t addr, uint64_t size) {
    return vm_holds(addr, size);
}

/* Makes the pages under addr .. addr + size real. */
static bool claim(uint64_t addr, uint64_t size) {
    return vm_reserve(addr, size);
}

/* Maps the pages under vaddr .. vaddr + size and fills them from offset in
   the file. The pages start out zeroed, so whatever part of them the file
   does not cover is already the .bss it should be. */
static int load_segment(const struct fs_file *file, uint64_t offset, uint64_t vaddr,
                        uint64_t file_size, uint64_t mem_size) {
    /* A promise rather than a copy, the same as a mapping a program asks for
       itself: a loader's own half-megabyte and a program's whole image are
       read only where they are used. The window has no such machinery, and
       a segment is small enough there not to want it. */
    if (vm_holds(vaddr, mem_size) && file_size > 0 &&
        map_record(vaddr, file_size, file->start, file->size, offset)) {
        return 0;
    }
    if (!claim(vaddr, mem_size)) {
        dbg("  segment: no memory for %x\n", vaddr);
        return FS_ENOSPC;
    }
    return file_size > 0 ? read_at(file, offset, (void *)vaddr, file_size) : 0;
}

/* Where the loadable segments of one linked to a fixed address begin and
   end: what it needs of low memory. False if it has none. */
static bool elf_span(const struct fs_file *file, const struct elf_header *header,
                     uint64_t *low, uint64_t *high) {
    struct elf_program program;

    *low = (uint64_t)-1;
    *high = 0;
    for (unsigned i = 0; i < header->phnum; i++) {
        if (read_at(file, header->phoff + (uint64_t)i * header->phentsize,
                    &program, sizeof program) != 0) {
            return false;
        }
        if (program.type == PT_LOAD && program.memsz != 0) {
            if (program.vaddr < *low) {
                *low = program.vaddr;
            }
            if (program.vaddr + program.memsz > *high) {
                *high = program.vaddr + program.memsz;
            }
        }
    }
    return *high > *low;
}

/* Loads one ELF - the program, or the loader it asks for - shifted by bias,
   which is 0 for something linked to run at a fixed address. Reports where
   its program headers ended up, and the path of the loader it wants. */
static int load_elf_at(const struct fs_file *file, const struct elf_header *header,
                       uint64_t bias, uint64_t *entry, uint64_t *phdr,
                       char *interp, size_t interp_max) {
    if (header->ident[4] != ELF_CLASS64 || header->ident[5] != ELF_LITTLE ||
        (header->type != ET_EXEC && header->type != ET_DYN) ||
        header->machine != EM_X86_64 ||
        header->phentsize < sizeof(struct elf_program) ||
        !fits(bias + header->entry, 0)) {
        return PROGRAM_EINVAL;
    }
    if (phdr != NULL) {
        *phdr = 0;
    }

    for (unsigned i = 0; i < header->phnum; i++) {
        struct elf_program program;
        int err = read_at(file, header->phoff + (uint64_t)i * header->phentsize,
                          &program, sizeof program);

        if (err < 0) {
            return err;
        }
        if (program.type == PT_INTERP && interp != NULL) {
            /* The path of the loader this program was linked against, as a
               string inside the file. */
            if (program.filesz == 0 || program.filesz > interp_max) {
                return PROGRAM_EINVAL;
            }
            err = read_at(file, program.offset, interp, program.filesz);
            if (err < 0) {
                return err;
            }
            interp[program.filesz - 1] = '\0';
            continue;
        }
        if (program.type != PT_LOAD || program.memsz == 0) {
            continue;
        }
        if (program.filesz > program.memsz || !fits(bias + program.vaddr, program.memsz)) {
            return PROGRAM_EINVAL;
        }
        dbg("  load %x..%x from %x\n", bias + program.vaddr,
            bias + program.vaddr + program.memsz, program.offset);
        err = load_segment(file, program.offset, bias + program.vaddr,
                           program.filesz, program.memsz);
        if (err < 0) {
            return err;
        }
        /* The program headers are part of the first segment: where they
           landed is what the auxiliary vector has to say. */
        if (phdr != NULL && *phdr == 0 && header->phoff >= program.offset &&
            header->phoff < program.offset + program.filesz) {
            *phdr = bias + program.vaddr + (header->phoff - program.offset);
        }
    }
    *entry = bias + header->entry;
    return 0;
}

/* What program_load needs to hold while it works: three hundred bytes of
   headers and paths, borrowed rather than put on the kernel stack, since a
   program may start a program which starts a program. */
struct load_work {
    struct elf_header header, loader_header;
    struct fs_file    loader;
    char              interp[FS_NAME_LEN];
};

int program_load(const struct fs_file *file, uint64_t *entry) {
    struct load_work *w = NULL;
    int err;

    if ((w = mem_alloc(sizeof *w)) == NULL) {
        return FS_ENOSPC;
    }

#define header (w->header)
#define interp (w->interp)

    interp[0] = '\0';
    dbg("load: %s size %u\n", file->name, (uint64_t)file->size);
    memset(mappings, 0, sizeof mappings);   /* nothing of the last one is owed */
    vm_reset();
    started_base = 0;
    started_phdr = started_phent = started_phnum = 0;

    /* Everything on the disk is linked: an ELF, or not a program at all. */
    if (file->size < sizeof header || read_at(file, 0, &header, sizeof header) != 0 ||
        header.ident[0] != 0x7F || header.ident[1] != 'E' ||
        header.ident[2] != 'L' || header.ident[3] != 'F') {
        err = PROGRAM_EINVAL;
        goto done;
    }
    uint64_t bias = header.type == ET_DYN ? vm_base() + USER_EXEC : 0;
    uint64_t low, high;

    /* Something linked to run at a fixed address goes there, in low memory;
       anything position-independent goes in its region. */
    if (vm_base() == 0) {
        err = PROGRAM_EINVAL;
        goto done;
    }
    if (header.type == ET_EXEC &&
        (!elf_span(file, &header, &low, &high) || !vm_low(low, high))) {
        dbg("load: no low memory for %x..%x\n", low, high);
        err = FS_ENOSPC;
        goto done;
    }
    err = load_elf_at(file, &header, bias, entry, &started_phdr,
                      interp, FS_NAME_LEN);
    if (err < 0) {
        goto done;
    }
    started_phent = header.phentsize;
    started_phnum = header.phnum;
    started_entry = *entry;

    if (interp[0] != '\0') {
        /* It is dynamically linked: its loader runs first, and does the
           rest of the work itself through these same syscalls. */
        dbg("load: interpreter %s\n", interp);
        if (fs_stat(interp, &w->loader) < 0 ||
            read_at(&w->loader, 0, &w->loader_header,
                    sizeof w->loader_header) < 0) {
            err = PROGRAM_ENOINTERP;
            goto done;
        }
        started_base = vm_base() + USER_INTERP;
        err = load_elf_at(&w->loader, &w->loader_header, started_base, entry,
                          NULL, NULL, 0);
        if (err < 0) {
            goto done;
        }
    }
    if (err == 0) {
        program_memory_start();
    }
done:
    mem_free(w);
    return err;
}
#undef header
#undef interp

/* ---- the stack a program starts on ---------------------------------------
 *
 * A libc does not start at main. It starts at _start, which expects the
 * stack to hold, in order: how many arguments there are, the arguments
 * themselves, the environment, and then a list of pairs describing the
 * program and the machine - the auxiliary vector. It reads its own program
 * headers out of that list to set up thread-local storage, and takes the
 * sixteen bytes at AT_RANDOM for its stack guard. Without all of it, a real
 * binary crashes before it reaches its first instruction of its own. */

enum {
    AT_NULL = 0, AT_PHDR = 3, AT_PHENT = 4, AT_PHNUM = 5, AT_PAGESZ = 6,
    AT_BASE = 7, AT_FLAGS = 8, AT_ENTRY = 9, AT_UID = 11, AT_EUID = 12,
    AT_GID = 13, AT_EGID = 14, AT_HWCAP = 16, AT_CLKTCK = 17,
    AT_SECURE = 23, AT_RANDOM = 25, AT_EXECFN = 31,
};

/* Where build_stack keeps its working-out. Borrowed from the firmware rather
   than put on the kernel stack: it is a page and a half of it, and a program
   may start a program which starts a program - the kernel's own stack would
   have to be sized for the deepest that can ever go. */
struct stack_work {
    struct { uint64_t type, value; } aux[20];
    uint64_t strings[EXEC_ENV + EXEC_ARGS];
};

/* argc, the arguments, the environment and the auxiliary vector, laid out
   where the program will start. */
static uint64_t build_stack(unsigned argc, const char *const *argv,
                            unsigned envc, const char *const *envv) {
    struct stack_work *work = NULL;
    uint64_t top = vm_base() + USER_STACK;
    uint64_t random_at, rsp;
    uint64_t *out;
    unsigned n = 0, count = 0;

    /* Only checked, not bought: a stack page arrives when it is first
       touched, like every other page of a region - most programs use a few
       of the sixty-four, and buying them all up front was a quarter of a
       megabyte each program had whether it wanted it or not. */
    if (!vm_holds(top - USER_STACK_BYTES, USER_STACK_BYTES)) {
        return 0;
    }
    if ((work = mem_alloc(sizeof *work)) == NULL) {
        return 0;
    }

    uint64_t *const strings = work->strings;

#define aux (work->aux)

    if (argc > EXEC_ARGS) {
        argc = EXEC_ARGS;           /* more than a command line can hold */
    }
    if (envc > EXEC_ENV) {
        envc = EXEC_ENV;
    }

    /* The strings go at the very top, and the vector is built below them. */
    for (unsigned i = 0; i < argc + envc; i++) {
        const char *text = i < argc ? argv[i] : envv[i - argc];
        size_t length = strlen(text) + 1;

        top -= length;
        memcpy((void *)top, text, length);
        strings[i] = top;
    }
    top = (top - 16) & ~15ull;
    random_at = top;
    for (unsigned i = 0; i < 16; i++) {
        ((uint8_t *)random_at)[i] = (uint8_t)(efi_seconds() * 37 + i * 101 + 17);
    }

    aux[count++] = (typeof(aux[0])){ AT_PHDR, started_phdr };
    aux[count++] = (typeof(aux[0])){ AT_PHENT, started_phent };
    aux[count++] = (typeof(aux[0])){ AT_PHNUM, started_phnum };
    aux[count++] = (typeof(aux[0])){ AT_PAGESZ, PAGE_SIZE };
    aux[count++] = (typeof(aux[0])){ AT_BASE, started_base };
    aux[count++] = (typeof(aux[0])){ AT_FLAGS, 0 };
    aux[count++] = (typeof(aux[0])){ AT_ENTRY, started_entry };
    aux[count++] = (typeof(aux[0])){ AT_UID, 0 };
    aux[count++] = (typeof(aux[0])){ AT_EUID, 0 };
    aux[count++] = (typeof(aux[0])){ AT_GID, 0 };
    aux[count++] = (typeof(aux[0])){ AT_EGID, 0 };
    aux[count++] = (typeof(aux[0])){ AT_HWCAP, 0 };
    aux[count++] = (typeof(aux[0])){ AT_CLKTCK, 100 };
    aux[count++] = (typeof(aux[0])){ AT_SECURE, 0 };
    aux[count++] = (typeof(aux[0])){ AT_RANDOM, random_at };
    aux[count++] = (typeof(aux[0])){ AT_EXECFN, strings[0] };
    aux[count++] = (typeof(aux[0])){ AT_NULL, 0 };

    /* argc, the arguments, their terminator, the environment and its
       terminator, then the pairs - and all of it has to leave the stack
       sixteen-byte aligned. */
    rsp = (top - (1 + argc + 1 + envc + 1 + count * 2) * 8) & ~15ull;
    out = (uint64_t *)rsp;

    out[n++] = argc;
    for (unsigned i = 0; i < argc; i++) {
        out[n++] = strings[i];
    }
    out[n++] = 0;
    for (unsigned i = 0; i < envc; i++) {
        out[n++] = strings[argc + i];
    }
    out[n++] = 0;
    for (unsigned i = 0; i < count; i++) {
        out[n++] = aux[i].type;
        out[n++] = aux[i].value;
    }
    mem_free(work);
    return rsp;
}
#undef aux

static unsigned entered;            /* programs running, one inside another */

bool program_running(void) {
    return entered > 0;
}

int program_run(uint64_t entry, unsigned argc, const char *const *argv,
                unsigned envc, const char *const *envv, bool fresh) {
    uint64_t rsp = build_stack(argc, argv, envc, envv);
    char was[TRACE_NAME] = "";
    const char *before;

    /* A program started from nothing gets a clean terminal and nothing open
       but the console. One that a fork ran execve on keeps both: the files
       are what the shell set up between the two, which is what a redirection
       is made of. */
    if (fresh) {
        handles_reset();
        console_reset();
        now_running = (struct times){ .started = efi_uptime_us() };
    }

    dbg("run: entry %x rsp %x phdr %x base %x\n", entry, rsp, started_phdr,
        started_base);
    if (rsp == 0) {
        return PROGRAM_KILLED;
    }
    /* Its calls are recorded under its own name from here to its last, which
       is what gives it a file of its own in /log. */
    before = tracer != NULL ? tracer->program(argc > 0 ? argv[0] : "program") : NULL;
    if (before != NULL) {
        strcpy(was, before);        /* the slot may be reused while it runs */
    }

    entered++;
    thread_enter();
    int code = user_enter(entry, rsp);
    thread_leave();
    entered--;

    /* Its name goes back to whatever started it, but what it did stays in
       the ring to be written out when the machine is next idle: a program
       that has just ended is the moment someone is waiting for the prompt,
       and a disk write here is a quarter of a second of that wait. */
    if (tracer != NULL) {
        tracer->program(was[0] != '\0' ? was : NULL);
    }

    /* Every page it was lent goes back now rather than at the next program:
       an idle machine should be holding nothing on its behalf. */
    vm_reset();
    if (fresh && net != NULL) {
        net->close_all();           /* and every socket it left open */
    }
    return code;
}

/* ---- one program starting another ----------------------------------------
 *
 * A shell runs a command by forking and then, in the child, replacing itself
 * with the program. Programs are not scheduled side by side here - only a
 * program's own threads are (thread.c) - so the two halves of that cannot
 * run at once, but they do not have to. What
 * happens instead is that fork runs its child there and then, to the end, and
 * only gives the parent its answer once the child is finished.
 *
 * What makes that safe is that the child, until it calls execve, is running
 * in its parent's own memory: every page of it is write-protected at the
 * fork, and the first write to one copies what was under it aside (vm.c). The
 * child's scribbles are undone page by page when it finishes, and the parent
 * carries on as if it had only ever been waiting. A shell between fork and
 * execve touches a handful of pages, so that costs a handful of pages.
 *
 * execve is where the child stops being its parent: it takes a region of its
 * own, the parent's is left exactly as the fork found it, and the program
 * that is loaded there runs until it exits. Its exit code goes back to fork,
 * which hands it to the next wait4.
 *
 * What a pipeline needs is the one thing this cannot do - two programs at
 * once - so a pipe is a buffer rather than a channel: the first program fills
 * it and finishes, and the second reads it. `a | b` works, and so does the
 * command substitution a shell does constantly; what does not is a pipeline
 * whose first half never ends. */

#define NEST_DEPTH 4        /* programs inside programs, at most */
#define STACK_MARGIN 2048   /* kernel stack a fork will not go below */

/* start.asm's, for measuring what is left of it. */
extern char stack_bottom[];
#define EXEC_LINE  4096     /* arguments and environment handed over, in bytes */
#define EXEC_ARGS  64
#define EXEC_ENV   64
#define PROC_OUT   16384    /* the most of a kernel command's output that
                               can be sent somewhere other than the screen */

/* Everything about the program that is running which the program it starts
   would otherwise overwrite: what it had open, where its heap had got to, and
   the terminal as it left it. Its memory is not in here - the child runs in a
   region of its own, and vm.c puts back whatever it changed of its parent's
   before that.

   Borrowed from the firmware rather than kept on the kernel stack: a program
   may start a program which starts a program, and at a kilobyte a time the
   kernel's stack would have to be sized for the deepest that can ever go. */
struct saved {
    struct handle handles[PROGRAM_FILES];
    char      writers[WRITERS][FS_NAME_LEN];
    char      settings[TERMIOS_NEW];
    char      cwd[FS_NAME_LEN + 1];
    uint64_t  brk, map;
    uint64_t  fs_base;      /* where its libc keeps its thread's own data */
    struct times times;     /* its time, while a child runs */
    struct mapping maps[MAPPINGS];  /* what its mmaps still owe it */
};

static unsigned nest;               /* how deep we are in that */

static uint64_t sys_pipe2(uint64_t out, uint64_t flags, uint64_t c) {
    uint32_t *fds = (uint32_t *)out;
    unsigned slot;
    uint64_t read_fd, write_fd;
    void *data = NULL;

    (void)flags;
    (void)c;
    if (!user_range(out, 8)) {
        return ERR(EFAULT);
    }
    if ((slot = pipe_slot()) == PIPES || (data = mem_alloc(PIPE_FIRST)) == NULL) {
        return ERR(ENFILE);
    }
    pipes[slot] = (struct pipe){ .data = data, .size = PIPE_FIRST, .refs = 2 };

    struct handle h = { .start = PIPE_MARK, .folder = slot + 1 };

    read_fd = give_handle(h);
    h.size = 1;                     /* the writing end */
    write_fd = give_handle(h);
    if ((int64_t)read_fd < 0 || (int64_t)write_fd < 0) {
        if ((int64_t)read_fd >= 0) {
            handles[read_fd].used = 0;
        }
        mem_free(data);
        pipes[slot] = (struct pipe){ 0 };
        return ERR(EMFILE);
    }
    fds[0] = (uint32_t)read_fd;
    fds[1] = (uint32_t)write_fd;
    return 0;
}

static uint64_t sys_pipe(uint64_t out, uint64_t b, uint64_t c) {
    return sys_pipe2(out, 0, b + c - b - c);
}

/* A count one thread adds to and another waits on - what a program wakes
   its own poll with. */
static uint64_t sys_eventfd2(uint64_t count, uint64_t flags, uint64_t c) {
    unsigned slot;
    uint64_t fd;

    (void)c;
    if ((slot = pipe_slot()) == PIPES) {
        return ERR(ENFILE);
    }
    fd = give_handle((struct handle){ .start = PIPE_MARK, .folder = slot + 1, .size = PIPE_EVENT,
                                      .offset = (uint32_t)flags & (O_NONBLOCK | EFD_SEMAPHORE) });
    if ((int64_t)fd >= 0) {
        pipes[slot] = (struct pipe){ .refs = 1, .count = (uint32_t)count };
    }
    return fd;
}

static uint64_t sys_eventfd(uint64_t count, uint64_t b, uint64_t c) {
    (void)b;
    return sys_eventfd2(count, 0, c);
}

/* ---- what a child leaves behind ----------------------------------------- */

#define CHILDREN 8

static struct child {
    int pid, status;
    bool waited;
    uint64_t user, sys;             /* its CPU time, children in, for wait4 */
} children[CHILDREN];

static int last_pid = 1;            /* the shell itself is 1 */

static void child_done(int pid, int code, uint64_t user, uint64_t sys) {
    static unsigned next;

    /* Linux's wait status: the exit code in the second byte, or the signal
       that ended it in the low seven bits. */
    children[next] = (struct child){
        .pid = pid,
        .status = code == PROGRAM_KILLED ? 11 : (code & 0xFF) << 8,
        .user = user, .sys = sys,
    };
    next = (next + 1) % CHILDREN;
}

static uint64_t sys_wait4(uint64_t pid, uint64_t status, uint64_t options) {
    (void)options;
    for (unsigned i = 0; i < CHILDREN; i++) {
        struct child *ch = &children[i];

        if (ch->pid == 0 || ch->waited) {
            continue;
        }
        if ((int64_t)pid > 0 && ch->pid != (int)pid) {
            continue;
        }
        ch->waited = true;
        if (arg[3] != 0) {          /* its rusage: the times, nothing else */
            int64_t *usage = (int64_t *)arg[3];

            if (!user_range(arg[3], 144)) {
                return ERR(EFAULT);
            }
            memset(usage, 0, 144);
            usage[0] = (int64_t)(ch->user / 1000000);
            usage[1] = (int64_t)(ch->user % 1000000);
            usage[2] = (int64_t)(ch->sys / 1000000);
            usage[3] = (int64_t)(ch->sys % 1000000);
        }
        if (status != 0) {
            if (!user_range(status, 4)) {
                return ERR(EFAULT);
            }
            *(int32_t *)status = ch->status;
        }
        return (uint64_t)ch->pid;
    }
    return ERR(ECHILD);
}

/* ---- putting a program aside -------------------------------------------- */

static struct saved *context_save(void) {
    struct saved *s;
    void *block = NULL;

    if ((block = mem_alloc(sizeof *s)) == NULL) {
        return NULL;
    }
    s = block;
    memcpy(s->handles, handles, sizeof handles);
    memcpy(s->writers, writer_names, sizeof writer_names);
    console_get(s->settings, sizeof s->settings);
    s->cwd[0] = '/';
    strcpy(s->cwd + 1, fs_cwd());
    s->brk = program_break;
    s->map = program_map;
    /* Every thread-local a libc reads is at an offset from FS, and the
       program being started sets FS to its own. Putting this back is what
       lets the one underneath find its own again. */
    s->fs_base = rdmsr(MSR_FS_BASE);
    s->times = now_running;
    now_running = (struct times){ .started = efi_uptime_us() };  /* the child's */
    memcpy(s->maps, mappings, sizeof mappings);
    pipes_hold(1);                  /* it still holds its ends of them */
    return s;
}

static void context_restore(struct saved *s) {
    pipes_hold(-1);                 /* the ends the child was left holding */
    memcpy(handles, s->handles, sizeof handles);
    memcpy(writer_names, s->writers, sizeof writer_names);
    console_set(s->settings, sizeof s->settings);
    fs_chdir(s->cwd);
    program_break = s->brk;
    program_map = s->map;
    wrmsr(MSR_FS_BASE, s->fs_base);
    /* All the child ran, its own children included, is the parent's
       children's; what of it was CPU time is their CPU time. */
    struct times child = now_running;
    uint64_t wall = efi_uptime_us() - child.started;
    uint64_t user = user_us();

    now_running = s->times;
    now_running.children_wall += wall;
    now_running.children_user += user + child.children_user;
    now_running.children_sys += child.sys + child.children_sys;
    memcpy(mappings, s->maps, sizeof mappings);
    mem_free(s);
}

/* ---- fork and execve ----------------------------------------------------- */

extern struct user_regs *user_frame;
extern int user_resume(const struct user_regs *regs, uint64_t rax);

static uint64_t fork_on(uint64_t stack);

static uint64_t sys_fork(uint64_t a, uint64_t b, uint64_t c) {
    (void)a;
    (void)b;
    (void)c;
    return fork_on(0);
}

/* A fork whose child starts on stack, if that is not 0: what vfork and
   posix_spawn ask for, the child running on memory its parent set aside. */
static uint64_t fork_on(uint64_t stack) {
    struct user_regs *child = NULL;
    struct saved *state;
    unsigned was_level = vm_level();
    int pid, code;

    /* A program starting a program starting a program is a stack of syscalls
       inside each other, and all of them are on the kernel's own stack. How
       much each costs depends on what they do, so the room left is measured
       rather than guessed: a fork with less than this much stack under it is
       refused, and a shell reports that the way it reports any machine that
       cannot start a process. Guessing wrong the other way would be the
       kernel writing past the bottom of its own stack. */
    if (nest >= NEST_DEPTH || thread_stack_left(&state) < STACK_MARGIN) {
        return ERR(EAGAIN);
    }
    /* The child's registers are borrowed rather than kept on the kernel
       stack: the child runs inside this call, and everything it does is
       nested inside it. */
    if ((child = mem_alloc(sizeof *child)) == NULL) {
        return ERR(ENOMEM);
    }
    *child = *user_frame;
    if (stack != 0) {
        child->rsp = stack;
    }
    uint64_t user0 = now_running.children_user, sys0 = now_running.children_sys;

    if ((state = context_save()) == NULL) {
        mem_free(child);
        return ERR(ENOMEM);
    }
    /* From here every page the child writes is copied aside first, so that
       what it does to its parent's memory can be undone. */
    if (!vm_undo_begin()) {
        context_restore(state);
        mem_free(child);
        return ERR(ENOMEM);
    }
    pid = ++last_pid;
    nest++;
    thread_enter();
    code = user_resume(child, 0);   /* the child, from this very syscall */
    thread_leave();
    nest--;
    mem_free(child);

    /* It may have been killed inside a program of its own, which leaves that
       program's region still on the stack of them. */
    vm_unwind(was_level);
    vm_undo_end(true);
    context_restore(state);
    child_done(pid, code, now_running.children_user - user0, now_running.children_sys - sys0);
    return (uint64_t)pid;
}

/* glibc's fork is a clone, and so is anything else that starts a process -
   or a thread. A thread gets a kernel stack of its own and runs whenever the
   ones beside it wait (thread.c); the call returns to its parent first. */
#define CLONE_VM             0x00000100
#define CLONE_VFORK          0x00004000     /* a fork that shares until execve */
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000

static uint64_t start_thread(uint64_t flags, uint64_t stack, uint64_t parent_tid,
                             uint64_t child_tid, uint64_t tls) {
    int tid;

    if (((flags & CLONE_PARENT_SETTID) && !user_range(parent_tid, 4)) ||
        ((flags & CLONE_CHILD_CLEARTID) && !user_range(child_tid, 4))) {
        return ERR(EFAULT);
    }
    tid = thread_create(user_frame, stack, (flags & CLONE_SETTLS) ? tls : rdmsr(MSR_FS_BASE),
                        (flags & CLONE_CHILD_CLEARTID) ? child_tid : 0);
    if (tid < 0) {
        return ERR(-tid);
    }
    if (flags & CLONE_PARENT_SETTID) {
        *(int32_t *)parent_tid = tid;
    }
    return (uint64_t)tid;
}

static uint64_t clone_with(uint64_t flags, uint64_t stack, uint64_t parent_tid,
                           uint64_t child_tid, uint64_t tls) {
    /* vfork, and posix_spawn built on it, share memory only until the child
       starts a program - which a fork, run to the end and then undone, is
       the same as from where the parent stands. */
    if ((flags & CLONE_VFORK) && !(flags & CLONE_THREAD)) {
        return fork_on(stack);
    }
    if (flags & (CLONE_VM | CLONE_THREAD)) {
        return start_thread(flags, stack, parent_tid, child_tid, tls);
    }
    return sys_fork(0, 0, 0);
}

static uint64_t sys_clone(uint64_t flags, uint64_t stack, uint64_t parent_tid) {
    return clone_with(flags, stack, parent_tid, arg[3], arg[4]);
}

/* The same with its arguments in a structure; the stack is given as where
   it starts and how big it is. */
static uint64_t sys_clone3(uint64_t args, uint64_t size, uint64_t c) {
    const uint64_t *a = (const uint64_t *)args;

    (void)c;
    if (size < 64 || !user_range(args, 64)) {
        return ERR(EINVAL);
    }
    /* flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls */
    return clone_with(a[0], a[5] != 0 ? a[5] + a[6] : 0, a[3], a[2], a[7]);
}

/* Waiting for a signal. Nothing can send one, so the wait lasts until its
   timeout - or for ever, in a thread, which the others run beside; alone,
   the program is told it timed out rather than hanging. */
static uint64_t sys_rt_sigtimedwait(uint64_t set, uint64_t info, uint64_t timeout) {
    (void)set;
    (void)info;
    if (thread_alone()) {
        return ERR(EAGAIN);
    }
    if (timeout != 0 && !user_range(timeout, 16)) {
        return ERR(EFAULT);
    }
    uint64_t until = timeout != 0 ? efi_uptime_ms() + timespec_ms(timeout) : 0;

    while (until == 0 || efi_uptime_ms() < until) {
        thread_yield();
    }
    return ERR(EAGAIN);
}
/* ---- timers ---------------------------------------------------------------
 *
 * Nothing can interrupt a running program to say its time is up, so a timer
 * is taken and never goes off: a program that sets one to cut a slow job
 * short - vim, for a regex that is taking too long - carries on as it would
 * on a machine fast enough never to need it. */

static uint64_t sys_timer_create(uint64_t clock, uint64_t event, uint64_t id) {
    static int32_t next;

    (void)clock;
    (void)event;
    if (!user_range(id, 4)) {
        return ERR(EFAULT);
    }
    *(int32_t *)id = next++;
    return 0;
}

/* timer_settime's old value, or timer_gettime's: never running. */
static uint64_t sys_timer_settime(uint64_t id, uint64_t flags, uint64_t spec) {
    uint64_t old = arg[3];

    (void)id;
    (void)flags;
    (void)spec;
    if (old != 0) {
        if (!user_range(old, 32)) {
            return ERR(EFAULT);
        }
        memset((void *)old, 0, 32);
    }
    return 0;
}

static uint64_t sys_timer_gettime(uint64_t id, uint64_t spec, uint64_t c) {
    (void)id;
    (void)c;
    if (!user_range(spec, 32)) {
        return ERR(EFAULT);
    }
    memset((void *)spec, 0, 32);
    return 0;
}

/* Copies a program's argument or environment list out of its memory, since
   its memory is about to be put aside. They land in line as one run of
   strings, with out pointing into it. Returns how many there were. */
static unsigned copy_list(uint64_t list, char *line, size_t room, size_t *used,
                          const char **out, unsigned max) {
    unsigned n = 0;

    if (list == 0 || !user_range(list, 8)) {
        out[0] = NULL;
        return 0;
    }
    for (const uint64_t *p = (const uint64_t *)list;
         n < max - 1 && user_range((uint64_t)p, 8) && *p != 0; p++) {
        const char *word = user_string(*p);
        size_t len = word == NULL ? 0 : strlen(word) + 1;

        if (word == NULL || *used + len > room) {
            break;
        }
        out[n++] = line + *used;
        memcpy(line + *used, word, len);
        *used += len;
    }
    out[n] = NULL;
    return n;
}

/* The words after the first, joined back up: a built-in is handed the rest of
   the line rather than a list of words. */
static char *join_args(char *line, unsigned argc) {
    size_t first = strlen(line) + 1;

    if (argc < 2) {
        return line + first - 1;    /* the NUL after the only word */
    }
    /* Exactly argc - 1 words: the environment comes straight after the last
       of them, and joining up to a double NUL ran on into it - a command
       given no last argument was handed PATH=... as one. */
    for (size_t i = first, words = 1; words < argc - 1; i++) {
        if (line[i] == '\0') {
            line[i] = ' ';
            words++;
        }
    }
    return line + first;
}

/* Everything execve has to carry from the old program's memory to the new
   one's, in one block borrowed from the firmware. */
struct exec_args {
    char        line[EXEC_LINE];
    char        name[FS_NAME_LEN];
    char        script[FS_NAME_LEN];    /* the file, once "#!" has named the
                                           program that is to run it */
    char        shebang[FS_NAME_LEN * 2];
    const char *words[EXEC_ARGS];
    const char *env[EXEC_ENV];
    const char *rest[EXEC_ARGS];        /* the list "#!" builds, before it
                                           replaces the one above */
};

/* "#!" at the front of a file names the program that runs it, the way Linux
   has read it since the seventies - and optionally one argument to hand that
   program before the file itself. Reports the two, pointing into out, or
   false if the file does not begin that way. */
static bool shebang(const struct fs_file *file, char *out, size_t max,
                    const char **interp, const char **extra) {
    size_t n = 0;

    *interp = *extra = NULL;
    if (file->size < 3 || read_at(file, 0, out, file->size < max - 1 ? file->size
                                                                     : max - 1) < 0) {
        return false;
    }
    out[file->size < max - 1 ? file->size : max - 1] = '\0';
    if (out[0] != '#' || out[1] != '!') {
        return false;
    }
    for (n = 2; out[n] == ' ' || out[n] == '\t'; n++) {
    }
    *interp = out + n;
    while (out[n] != '\0' && out[n] != '\n' && out[n] != ' ' && out[n] != '\t') {
        n++;
    }
    if (out[n] == ' ' || out[n] == '\t') {
        out[n++] = '\0';
        while (out[n] == ' ' || out[n] == '\t') {
            n++;
        }
        if (out[n] != '\0' && out[n] != '\n') {
            *extra = out + n;
            while (out[n] != '\0' && out[n] != '\n') {
                n++;
            }
        }
    }
    out[n] = '\0';                  /* the end of the line, either way */
    return **interp != '\0';
}

/* Starts the program at path from the kernel itself - what the shell does -
   with a clean terminal and the machine's environment. A script that begins
   with "#!" is handed to the program it names, as execve would. Returns its
   exit code, or a negative FS_E* or PROGRAM_E* code if it could not start. */
int program_start(const char *path, unsigned argc, const char *const *argv,
                  unsigned envc, const char *const *envv) {
    struct {
        char        line[FS_NAME_LEN * 2];
        const char *words[EXEC_ARGS + 3];
    } *w = mem_alloc(sizeof *w);
    const char *interp, *extra;
    struct fs_file file;
    uint64_t entry;
    unsigned n = 0;
    int err;

    if (w == NULL) {
        return FS_ENOSPC;
    }
    if ((err = fs_stat(path, &file)) == 0 &&
        shebang(&file, w->line, sizeof w->line, &interp, &extra)) {
        w->words[n++] = interp;
        if (extra != NULL) {
            w->words[n++] = extra;
        }
        w->words[n++] = path;
        for (unsigned i = 1; i < argc && n < EXEC_ARGS; i++) {
            w->words[n++] = argv[i];
        }
        w->words[n] = NULL;
        argv = w->words;
        argc = n;
        err = fs_stat(interp, &file);
    }
    if (err == 0 && file.size == 0) {
        err = PROGRAM_EINVAL;
    }
    if (err == 0) {
        err = program_load(&file, &entry);
    }
    if (err == 0) {
        err = program_run(entry, argc, argv, envc, envv, true);
    }
    mem_free(w);
    return err;
}

static uint64_t sys_execve(uint64_t path, uint64_t argv, uint64_t envp) {
    const char *given = user_string(path);
    const struct proc_cmd *cmd;
    struct exec_args *held = NULL;
    struct fs_file file;
    size_t used = 0;
    unsigned argc, envc;
    uint64_t entry;
    int code;

    if (given == NULL || strlen(given) + 1 > FS_NAME_LEN) {
        return ERR(EINVAL);
    }
    if ((held = mem_alloc(sizeof *held)) == NULL) {
        return ERR(ENOMEM);
    }
    strcpy(held->name, given);
    argc = copy_list(argv, held->line, sizeof held->line, &used, held->words, EXEC_ARGS);
    envc = copy_list(envp, held->line, sizeof held->line, &used, held->env, EXEC_ENV);
    if (argc == 0) {
        held->words[0] = held->name;
        held->words[1] = NULL;
        argc = 1;
    }

    /* A kernel built-in is not a file: there is nothing to load, and nothing
       to give a region of its own. It simply runs, and that is the program.
       It prints rather than writing to a descriptor, so when the shell has
       sent its output somewhere else what it prints is taken and written
       there - which is what makes `mem | head` and `uptime > file` work. */
    if ((cmd = proc_command(held->name)) != NULL) {
        void *taken = NULL;

        if (!is_console(1) &&
            (taken = mem_alloc(PROC_OUT)) != NULL) {
            vga_capture(taken, PROC_OUT);
        }
        proc_run(cmd, join_args(held->line, argc));
        if (taken != NULL) {
            vga_capture_end();
            write_to(handle_of(1), (uint64_t)taken, strlen(taken));
            mem_free(taken);
        }
        mem_free(held);
        process_exit(0);
    }
    if (fs_stat(held->name, &file) != 0 || file.size == 0) {
        mem_free(held);
        return ERR(ENOENT);
    }

    const char *interp, *extra;

    if (shebang(&file, held->shebang, sizeof held->shebang, &interp, &extra)) {
        const char **rest = held->rest;
        unsigned n = 0;

        strcpy(held->script, held->name);
        rest[n++] = interp;
        if (extra != NULL) {
            rest[n++] = extra;
        }
        rest[n++] = held->script;   /* what it is being asked to run */
        for (unsigned i = 1; i < argc && n < EXEC_ARGS - 1; i++) {
            rest[n++] = held->words[i];
        }
        rest[n] = NULL;
        memcpy(held->words, rest, (n + 1) * sizeof rest[0]);
        argc = n;
        if (strlen(interp) + 1 > FS_NAME_LEN) {
            mem_free(held);
            return ERR(EINVAL);
        }
        strcpy(held->name, interp);
        if (fs_stat(held->name, &file) != 0 || file.size == 0) {
            mem_free(held);
            return ERR(ENOENT);
        }
    }
    /* A region of its own, so that whoever forked this child keeps theirs. */
    if (!vm_push()) {
        mem_free(held);
        return ERR(ENOMEM);
    }
    code = program_load(&file, &entry);
    if (code == 0) {
        /* Its files are what the fork left it - a shell sets those up between
           the fork and here, and that is what a redirection is. */
        code = program_run(entry, argc, held->words, envc, held->env, false);
    } else {
        dbg("exec: %s could not be loaded (%d)\n", held->name, (uint64_t)code);
        code = 127;
    }
    vm_pop();
    mem_free(held);
    process_exit(code);
}
