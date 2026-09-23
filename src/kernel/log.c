#include "log.h"

#include <stdbool.h>
#include <stddef.h>

#include "fs.h"
#include "string.h"
#include "syscall.h"
#include "vga.h"

#define LOG_DIR      "/log"
#define LOG_MAX      32768      /* a program's file, before it starts over */
#define LOG_CHUNK    512        /* text held back before one write to disk */
#define LOG_PROGRAMS 6          /* names alive at once: a program, and any
                                   that started it */

/* The ring. `total` counts every call ever made, so the entry a slot holds is
   the (total - LOG_SIZE + i)'th; once total passes LOG_SIZE the oldest slot is
   whichever total is about to overwrite. `written` is how much of it has gone
   to disk already. */
static struct log_entry entries[LOG_SIZE];
static unsigned total, written, dropped;
static int      enabled = 1;
static bool     flushing;   /* the writes a flush makes are not logged */

/* Who is running. A name is kept for as long as any entry names it, which is
   until the next flush, so a program that starts another is still spelled
   out when its own calls are written. */
static char     who_names[LOG_PROGRAMS][LOG_NAME];
static unsigned who_now;    /* one-based; 0 is the kernel's own */

#define NAME_COUNT (sizeof names / sizeof names[0])

/* What one of a call's arguments means, so that a line of hex reads as
   something better: a descriptor shows as stdout rather than 0x1, a length
   in decimal, a path as the pointer it is. */
enum log_kind {
    LOG_NONE = 0,   /* the call does not take one here */
    LOG_HEX,        /* an address */
    LOG_INT,        /* a count, a length, a code */
    LOG_FD,         /* a descriptor: stdin, stdout, stderr or a number */
    LOG_PATH,       /* a pointer to a path */
    LOG_OPEN,       /* open flags */
    LOG_WHENCE,     /* what an lseek offset is measured from */
};

/* Every call this kernel answers, with what its arguments mean. */
static const struct {
    uint16_t      number;
    const char   *name;
    enum log_kind kinds[3];
} names[] = {
    { SYS_READ,           "read",          { LOG_FD, LOG_HEX, LOG_INT } },
    { SYS_WRITE,          "write",         { LOG_FD, LOG_HEX, LOG_INT } },
    { SYS_OPEN,           "open",          { LOG_PATH, LOG_OPEN, LOG_INT } },
    { SYS_CLOSE,          "close",         { LOG_FD, LOG_NONE, LOG_NONE } },
    { SYS_FSTAT,          "fstat",         { LOG_FD, LOG_HEX, LOG_NONE } },
    { SYS_LSEEK,          "lseek",         { LOG_FD, LOG_INT, LOG_WHENCE } },
    { SYS_MMAP,           "mmap",          { LOG_HEX, LOG_INT, LOG_INT } },
    { SYS_MPROTECT,       "mprotect",      { LOG_HEX, LOG_INT, LOG_INT } },
    { SYS_MUNMAP,         "munmap",        { LOG_HEX, LOG_INT, LOG_NONE } },
    { SYS_BRK,            "brk",           { LOG_HEX, LOG_NONE, LOG_NONE } },
    { SYS_IOCTL,          "ioctl",         { LOG_FD, LOG_HEX, LOG_HEX } },
    { SYS_WRITEV,         "writev",        { LOG_FD, LOG_HEX, LOG_INT } },
    { SYS_GETPID,         "getpid",        { LOG_NONE, LOG_NONE, LOG_NONE } },
    { SYS_UNAME,          "uname",         { LOG_HEX, LOG_NONE, LOG_NONE } },
    { SYS_GETCWD,         "getcwd",        { LOG_HEX, LOG_INT, LOG_NONE } },
    { SYS_ARCH_PRCTL,     "arch_prctl",    { LOG_HEX, LOG_HEX, LOG_NONE } },
    { SYS_GETDENTS64,     "getdents64",    { LOG_FD, LOG_HEX, LOG_INT } },
    { SYS_SET_TID_ADDRESS, "set_tid_address", { LOG_HEX, LOG_NONE, LOG_NONE } },
    { SYS_CLOCK_GETTIME,  "clock_gettime", { LOG_INT, LOG_HEX, LOG_NONE } },
    { SYS_EXIT,           "exit",          { LOG_INT, LOG_NONE, LOG_NONE } },
    { SYS_EXIT_GROUP,     "exit_group",    { LOG_INT, LOG_NONE, LOG_NONE } },
    { SYS_OPENAT,         "openat",        { LOG_FD, LOG_PATH, LOG_OPEN } },
    { SYS_NEWFSTATAT,     "newfstatat",    { LOG_FD, LOG_PATH, LOG_HEX } },
    { SYS_SET_ROBUST_LIST, "set_robust_list", { LOG_HEX, LOG_INT, LOG_NONE } },
    { SYS_PRLIMIT64,      "prlimit64",     { LOG_INT, LOG_INT, LOG_HEX } },
    { SYS_GETRANDOM,      "getrandom",     { LOG_HEX, LOG_INT, LOG_HEX } },
    { SYS_READLINKAT,     "readlinkat",    { LOG_FD, LOG_PATH, LOG_HEX } },
    { SYS_FORK,           "fork",          { LOG_NONE, LOG_NONE, LOG_NONE } },
    { SYS_VFORK,          "vfork",         { LOG_NONE, LOG_NONE, LOG_NONE } },
    { SYS_CLONE,          "clone",         { LOG_HEX, LOG_HEX, LOG_HEX } },
    { SYS_EXECVE,         "execve",        { LOG_PATH, LOG_HEX, LOG_HEX } },
    { SYS_WAIT4,          "wait4",         { LOG_INT, LOG_HEX, LOG_INT } },
    { SYS_PIPE,           "pipe",          { LOG_HEX, LOG_NONE, LOG_NONE } },
    { SYS_PIPE2,          "pipe2",         { LOG_HEX, LOG_HEX, LOG_NONE } },
    { SYS_UNLINK,         "unlink",        { LOG_PATH, LOG_NONE, LOG_NONE } },
    { SYS_MKDIR,          "mkdir",         { LOG_PATH, LOG_INT, LOG_NONE } },
    { SYS_RENAME,         "rename",        { LOG_PATH, LOG_PATH, LOG_NONE } },
    { SYS_CHDIR,          "chdir",         { LOG_PATH, LOG_NONE, LOG_NONE } },
    { SYS_STAT,           "stat",          { LOG_PATH, LOG_HEX, LOG_NONE } },
    { SYS_ACCESS,         "access",        { LOG_PATH, LOG_INT, LOG_NONE } },
    { SYS_DUP2,           "dup2",          { LOG_FD, LOG_FD, LOG_NONE } },
    { SYS_FCNTL,          "fcntl",         { LOG_FD, LOG_INT, LOG_HEX } },
    { SYS_FTRUNCATE,      "ftruncate",     { LOG_FD, LOG_INT, LOG_NONE } },
    { SYS_STATFS,         "statfs",        { LOG_PATH, LOG_HEX, LOG_NONE } },
    { SYS_SYSINFO,        "sysinfo",       { LOG_HEX, LOG_NONE, LOG_NONE } },
};

/* The errors the kernel actually hands back. */
static const struct {
    int16_t     value;
    const char *name;
} errors[] = {
    { -2,  "ENOENT" }, { -9,  "EBADF" },  { -12, "ENOMEM" },
    { -14, "EFAULT" }, { -22, "EINVAL" }, { -25, "ENOTTY" },
    { -38, "ENOSYS" }, { -1,  "EPERM"  }, { -24, "EMFILE" },
};

#define ERROR_COUNT (sizeof errors / sizeof errors[0])

static const enum log_kind *log_kinds(uint64_t number) {
    static const enum log_kind none[3] = { LOG_NONE, LOG_NONE, LOG_NONE };

    for (size_t i = 0; i < NAME_COUNT; i++) {
        if (names[i].number == number) {
            return names[i].kinds;
        }
    }
    return none;
}

const char *log_error(uint64_t result) {
    int64_t signed_result = (int64_t)result;

    /* A libc tells a result from an error by the latter being a small
       negative, which is the same test used here. */
    if (signed_result >= 0 || signed_result < -4095) {
        return NULL;
    }
    for (size_t i = 0; i < ERROR_COUNT; i++) {
        if (errors[i].value == signed_result) {
            return errors[i].name;
        }
    }
    return "error";
}

const char *log_name(uint64_t number) {
    for (size_t i = 0; i < NAME_COUNT; i++) {
        if (names[i].number == number) {
            return names[i].name;
        }
    }
    return NULL;
}

/* ---- who --------------------------------------------------------------- */

const char *log_who(const struct log_entry *entry) {
    return entry->who == 0 ? "kernel" : who_names[entry->who - 1];
}

/* Where in the ring the entries that have yet to go to disk begin. */
static unsigned unwritten(void) {
    return log_count() - (total - written);
}

/* Whether an entry still waiting to be written names slot i, which is what
   keeps a name from being reused before the lines that want it are out. */
static bool who_live(unsigned slot) {
    for (unsigned i = unwritten(); i < log_count(); i++) {
        if (log_get(i)->who == slot + 1) {
            return true;
        }
    }
    return false;
}

const char *log_program(const char *name) {
    const char *was = who_now == 0 ? NULL : who_names[who_now - 1];
    unsigned free_slot = LOG_PROGRAMS;

    if (name == NULL) {
        who_now = 0;
        return was;
    }
    /* The last part of a path: /pkg/linux-coreutils/ls logs as ls, which is
       also the name its file takes. */
    for (const char *p = name; *p != '\0'; p++) {
        if (*p == '/') {
            name = p + 1;
        }
    }
    for (unsigned i = 0; i < LOG_PROGRAMS; i++) {
        if (who_names[i][0] != '\0' && strcmp(who_names[i], name) == 0) {
            who_now = i + 1;
            return was;
        }
        if (free_slot == LOG_PROGRAMS &&
            (who_names[i][0] == '\0' || (i + 1 != who_now && !who_live(i)))) {
            free_slot = i;
        }
    }
    if (free_slot == LOG_PROGRAMS) {
        free_slot = LOG_PROGRAMS - 1;   /* nothing to spare: the last is reused */
    }
    size_t n = strlen(name);

    if (n > LOG_NAME - 1) {
        n = LOG_NAME - 1;
    }
    memcpy(who_names[free_slot], name, n);
    who_names[free_slot][n] = '\0';
    who_now = free_slot + 1;
    return was;
}

/* ---- the ring ---------------------------------------------------------- */

struct log_entry *log_begin(uint32_t number, uint64_t a, uint64_t b, uint64_t c) {
    if (!enabled || flushing) {
        return NULL;
    }
    if (total - written >= LOG_SIZE) {
        /* Full, and nothing has been idle enough to write it out, so the
           oldest goes: the file keeps the last ring's worth of what the
           program did. Writing it all out here instead would be a disk write
           in the middle of a running program, and felt as one. */
        written++;
        dropped++;
    }
    struct log_entry *entry = &entries[total % LOG_SIZE];

    total++;
    *entry = (struct log_entry){ .number = number, .a = a, .b = b, .c = c,
                                 .who = (uint8_t)who_now };
    return entry;
}

void log_end(struct log_entry *entry, uint64_t result) {
    if (entry != NULL) {
        entry->result = result;
        entry->returned = 1;
    }
}

unsigned log_count(void) {
    return total < LOG_SIZE ? total : LOG_SIZE;
}

unsigned log_total(void) {
    return total;
}

const struct log_entry *log_get(unsigned i) {
    if (i >= log_count()) {
        return NULL;
    }
    /* Once the ring has wrapped the oldest entry sits where the next write
       will go, so counting starts there instead of at slot 0. */
    unsigned oldest = total < LOG_SIZE ? 0 : total % LOG_SIZE;
    return &entries[(oldest + i) % LOG_SIZE];
}

unsigned log_dropped(void) {
    return dropped;
}

void log_clear(void) {
    char running[LOG_NAME] = "";

    /* Whatever is running is still running, and its next call is still its
       own - so its name survives the entries being thrown away. */
    if (who_now != 0) {
        strcpy(running, who_names[who_now - 1]);
    }
    total = written = dropped = 0;
    who_now = 0;
    memset(who_names, 0, sizeof who_names);
    if (running[0] != '\0') {
        log_program(running);
    }
}

int log_enabled(void) {
    return enabled;
}

void log_enable(int on) {
    if (!on) {
        log_flush();
    }
    enabled = on;
}

/* ---- formatting -------------------------------------------------------- */

/* Writes one argument the way its kind reads best, and returns where the
   next one goes. */
static char *log_arg(char *out, enum log_kind kind, uint64_t value) {
    static const char *const whence[] = { "SEEK_SET", "SEEK_CUR", "SEEK_END" };

    switch (kind) {
    case LOG_FD:
        if (value == 0 || value == 1 || value == 2) {
            ksprintf(out, "%s", value == 0 ? "stdin" : value == 1 ? "stdout" : "stderr");
        } else if ((int32_t)value == -100) {
            /* What openat calls the cwd. A descriptor is an int, so only the
               low half is meaningful. */
            ksprintf(out, "AT_FDCWD");
        } else {
            ksprintf(out, "fd %u", (unsigned)value);
        }
        break;
    case LOG_INT:
        if ((int64_t)value < 0 && (int64_t)value > -4096) {
            ksprintf(out, "-%u", (unsigned)-(int64_t)value);
        } else {
            ksprintf(out, "%u", (unsigned)value);
        }
        break;
    case LOG_OPEN:
        ksprintf(out, value == 0 ? "O_RDONLY" : "0x%x", value);
        break;
    case LOG_WHENCE:
        ksprintf(out, value < 3 ? whence[value] : "%u", (unsigned)value);
        break;
    case LOG_PATH:
    case LOG_HEX:
    default:
        ksprintf(out, "0x%x", value);
        break;
    }
    return out + strlen(out);
}

size_t log_format(const struct log_entry *e, char *buf) {
    const char *name = log_name(e->number);
    const enum log_kind *kinds = log_kinds(e->number);
    const uint64_t value[3] = { e->a, e->b, e->c };
    const char *failed;
    char *out = buf;

    if (name != NULL) {
        ksprintf(out, "  %s(", name);
    } else {
        ksprintf(out, "  syscall %u(", (unsigned)e->number);
        kinds = NULL;
    }
    out += strlen(out);

    for (unsigned i = 0; i < 3; i++) {
        enum log_kind kind = kinds != NULL ? kinds[i] : LOG_HEX;

        if (kind == LOG_NONE) {
            break;                  /* the call takes no more than this */
        }
        if (i > 0) {
            *out++ = ',';
            *out++ = ' ';
        }
        out = log_arg(out, kind, value[i]);
    }
    *out++ = ')';

    /* A call with no return is the normal end of a program as often as it is
       a crash, so it says only that much. */
    if (!e->returned) {
        ksprintf(out, " -> no return\n");
    } else if ((failed = log_error(e->result)) != NULL) {
        ksprintf(out, " -> %s\n", failed);
    } else if (e->result > 0xFFFFFF) {
        ksprintf(out, " -> 0x%x\n", e->result);
    } else {
        ksprintf(out, " -> %u\n", (unsigned)e->result);
    }
    return strlen(buf);
}

/* ---- to disk ----------------------------------------------------------- */

/* Appends text to /log/<who>.log, making the file - and the folder - if
   there is none, and starting the file over once it has grown past LOG_MAX.
   A program's own file rather than one log for the machine: what `ls` did is
   worth reading without `bash` interleaved through it. */
static void append(const char *who, const char *text, size_t size) {
    char path[FS_NAME_LEN];
    struct fs_file file;

    if (size == 0) {
        return;
    }
    ksprintf(path, LOG_DIR "/%s.log", who);
    if (fs_stat(path, &file) < 0) {
        if (fs_stat(LOG_DIR "/", &file) < 0) {
            fs_mkdir(LOG_DIR);
        }
        if (fs_write(path, text, size) < 0 || fs_stat(path, &file) < 0) {
            enabled = 0;            /* no disk for it: stop rather than try
                                       again on every call from here on */
        }
        return;
    }
    if (file.size + size > LOG_MAX) {
        fs_write(path, text, size);
        return;
    }
    if (fs_write_at(path, file.size, text, size) < 0) {
        enabled = 0;
    }
}

void log_flush(void) {
    /* Held here rather than on the stack: this runs from inside whatever the
       machine was doing when it went idle, which may be a program started by
       a program, and two kilobytes at that depth sets what the kernel stack
       has to be. */
    static char chunk[LOG_CHUNK];
    size_t held = 0;
    unsigned run = 0;               /* who the text in chunk belongs to */

    if (flushing || written == total) {
        return;
    }
    flushing = true;                /* the writes below are the kernel's own */
    for (unsigned i = unwritten(); i < log_count(); i++) {
        const struct log_entry *e = log_get(i);
        char line[LOG_LINE];
        size_t n = log_format(e, line);

        if (held > 0 && (e->who != run || held + n > sizeof chunk)) {
            append(run == 0 ? "kernel" : who_names[run - 1], chunk, held);
            held = 0;
        }
        run = e->who;
        memcpy(chunk + held, line, n);
        held += n;
    }
    if (held > 0) {
        append(run == 0 ? "kernel" : who_names[run - 1], chunk, held);
    }
    written = total;
    flushing = false;
}
