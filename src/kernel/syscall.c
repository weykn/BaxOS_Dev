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
#include "share.h"
#include "linux.h"
#include "input.h"
#include "module.h"

#define MSR_EFER  0xC0000080
#define MSR_STAR  0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084

#define EFER_SCE    0x001
#define RFLAGS_MASK 0x700       /* clear TF, IF and DF on entry to the kernel */

#define SECTOR_SIZE 512
#define PAGE_SIZE   4096

static uint64_t write_protect(bool on);


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
static uint64_t sys_timer_delete(uint64_t id, uint64_t b, uint64_t c);
static uint64_t sys_alarm(uint64_t seconds, uint64_t b, uint64_t c);
static uint64_t sys_getitimer(uint64_t which, uint64_t value, uint64_t c);
static uint64_t sys_setitimer(uint64_t which, uint64_t value, uint64_t old);
static uint64_t sys_timer_settime(uint64_t id, uint64_t flags, uint64_t spec);
static uint64_t sys_timer_gettime(uint64_t id, uint64_t spec, uint64_t c);
static uint64_t sys_wait4(uint64_t pid, uint64_t status, uint64_t options);
static uint64_t sys_pipe(uint64_t out, uint64_t b, uint64_t c);
static uint64_t sys_pipe2(uint64_t out, uint64_t flags, uint64_t c);
static uint64_t sys_eventfd(uint64_t count, uint64_t b, uint64_t c);
static uint64_t sys_eventfd2(uint64_t count, uint64_t flags, uint64_t c);
static uint64_t sys_inotify_init1(uint64_t flags, uint64_t b, uint64_t c);
static uint64_t sys_memfd_create(uint64_t name, uint64_t flags, uint64_t c);
static uint64_t sys_getuid(uint64_t a, uint64_t b, uint64_t c);
static uint64_t sys_geteuid(uint64_t a, uint64_t b, uint64_t c);
static uint64_t sys_getgid(uint64_t a, uint64_t b, uint64_t c);
static uint64_t sys_getegid(uint64_t a, uint64_t b, uint64_t c);
static uint64_t sys_inotify_add_watch(uint64_t fd, uint64_t path, uint64_t mask);
static uint64_t sys_socketpair(uint64_t domain, uint64_t type, uint64_t c);
static uint64_t sys_getrandom(uint64_t buf, uint64_t length, uint64_t flags);
static bool fits(uint64_t addr, uint64_t size);
static bool claim(uint64_t addr, uint64_t size);
static int  read_at(const struct fs_file *file, uint64_t offset, void *dest, uint64_t size);
struct handle;

/* What the loaded program turned out to be, for the auxiliary vector its
   libc reads off the stack. */
static uint64_t started_base;   /* where the loader went, 0 without one */
static uint64_t started_phdr, started_entry;
static uint64_t started_phent, started_phnum;

/* In syscall_entry.asm. */
void syscall_entry(void);
extern const char trap_stubs[];     /* one 32-byte stub per exception vector */
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
const char *user_string(uint64_t addr) {
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

/* ---- processes -------------------------------------------------------------
 *
 * Everything that is one program's rather than the machine's: what it has
 * open, where it is in the filesystem, its heap, what it does with each
 * signal, the mmaps it is still owed, and its memory (vm.c). One page each,
 * borrowed while it lives. The threads running it are thread.c's; the one
 * running now decides which of these `proc` is, and the names below are its
 * fields - so the calls that use them read as they did when there was only
 * ever one program. */

#define SIGNALS  32                 /* 1 to 31, as Linux numbers them */
#define PROCESSES 128
#define SIGNALLED 0x1000            /* an exit code that is a signal's, in its low bits */
#define SIGPIPE  13

struct sig_action {                 /* rt_sigaction's, the kernel's own layout */
    uint64_t handler, flags, restorer, mask;
};

struct mapping {
    uint64_t at, end;       /* the memory it covers; at == end when free */
    uint64_t offset;        /* where in the file `at` is */
    uint32_t start, size;   /* the file: its first sector, and its length */
};

struct process {
    int      pid, ppid, pgid, sid;  /* ppid 0: nobody waits for it */
    bool     zombie;                /* ended, its status not yet collected */
    bool     borrowed;              /* a vfork's child, in its parent's memory */
    int      status;                /* as wait4 reports it, once ended */
    struct space own, *space;
    struct handle *files;           /* its descriptors, mem_alloc'd: a page's */
    uint32_t files_room;            /* worth to start, doubled when full */
    struct handle *files_old;       /* the table before it grew, kept to the
                                       syscall's end: a caller may still hold
                                       a pointer into it */
    char     cwd[FS_NAME_LEN];
    uint64_t brk, map, map_high;
    struct times times;
    struct sig_action actions[SIGNALS];
    uint64_t signals;               /* 1 << each signal sent and not yet acted on */
    int      chld_pid, chld_code;   /* what SIGCHLD's siginfo says */
    uint64_t ofd_touched[PROGRAM_FILES / 64];  /* descriptors whose shared
                                       position this syscall loaded, to write back */
    struct mapping *maps;           /* the file mappings owed, mem_alloc'd: a */
    unsigned maps_room;             /* program with Mesa in it has hundreds */
    char     name[TRACE_NAME];      /* the program it runs, as debug/trace names it */
    char     exe[FS_NAME_LEN];      /* and its whole path, links followed: /proc/self/exe */
    struct creds cred;              /* who it runs as: inherited, kept across execve */
    char    *cmdline;               /* its arguments, each ending in a NUL, as
                                       /proc/<pid>/cmdline gives them: mem_alloc'd */
    uint32_t cmdline_len;
};

_Static_assert(sizeof(struct process) <= 4096 - 16, "a process is one page");

static struct process *procs[PROCESSES];
static struct process *proc;        /* the running thread's, NULL for the kernel */
static struct times kernel_times;
static char kernel_cwd[FS_NAME_LEN];
static int fg_pgrp;                 /* the group the keyboard's signals go to */

struct process;
static void signal_to(struct process *p, uint64_t sig);

#define FIRST_FILES   ((4096 - 16) / sizeof(struct handle))   /* a page's worth */
#define handles       (proc->files)
#define file_room     (proc->files_room)
#define TOUCHED(fd)   (proc->ofd_touched[(fd) / 64] & 1ull << ((fd) % 64))
#define on_signal     (proc->actions)
#define pending       (proc->signals)
#define program_break (proc->brk)
#define program_map   (proc->map)
#define program_map_high (proc->map_high)
#define mappings      (proc->maps)
#define map_room      (proc->maps_room)
#define child_pid     (proc->chld_pid)
#define child_code    (proc->chld_code)

struct times *process_times(void) {
    return proc != NULL ? &proc->times : &kernel_times;
}


int process_pid(void *p) {
    return ((struct process *)p)->pid;
}

/* thread.c, as the processor passes from a thread of one process to one of
   another: the working folder and the memory change hands. */
void process_switch(void *from, void *to) {
    struct process *f = from, *t = to;

    strcpy(f != NULL ? f->cwd : kernel_cwd, fs_cwd());
    fs_cwd_set(t != NULL ? t->cwd : kernel_cwd);
    proc = t;
    vm_space_use(t != NULL ? t->space : NULL);
    if (tracer != NULL) {
        tracer->program(t != NULL && t->name[0] != '\0' ? t->name : NULL);
    }
}

/* The program the running process now runs, for its calls' log: the last
   part of its path, /usr/bin/ls logging as ls. */
/* Its arguments as /proc/<pid>/cmdline gives them, kept as it starts. */
#define CMDLINE_MAX 1024

static void process_cmdline(unsigned argc, const char *const *argv) {
    uint32_t n = 0;

    for (unsigned i = 0; i < argc && n < CMDLINE_MAX; i++) {
        n += (uint32_t)strlen(argv[i]) + 1;
    }
    n = n < CMDLINE_MAX ? n : CMDLINE_MAX;
    mem_free(proc->cmdline);
    proc->cmdline_len = 0;
    if (n == 0 || (proc->cmdline = mem_alloc(n)) == NULL) {
        proc->cmdline = NULL;
        return;
    }
    for (unsigned i = 0; i < argc && proc->cmdline_len < n; i++) {
        uint32_t k = (uint32_t)strlen(argv[i]) + 1;

        k = k < n - proc->cmdline_len ? k : n - proc->cmdline_len;
        memcpy(proc->cmdline + proc->cmdline_len, argv[i], k);
        proc->cmdline_len += k;
    }
    proc->cmdline[n - 1] = '\0';
}

static void process_named(const char *path) {
    const char *base = path;

    for (const char *c = path; *c != '\0'; c++) {
        base = *c == '/' && c[1] != '\0' ? c + 1 : base;
    }
    size_t n = strlen(base);

    n = n < TRACE_NAME - 1 ? n : TRACE_NAME - 1;
    memcpy(proc->name, base, n);
    proc->name[n] = '\0';
    if (tracer != NULL) {
        tracer->program(proc->name);
    }
}

static struct process *proc_find(int pid) {
    for (unsigned i = 0; i < PROCESSES; i++) {
        if (procs[i] != NULL && procs[i]->pid == pid) {
            return procs[i];
        }
    }
    return NULL;
}

static void proc_free(struct process *p) {
    for (unsigned i = 0; i < PROCESSES; i++) {
        if (procs[i] == p) {
            procs[i] = NULL;
        }
    }
    mem_free(p->maps);
    mem_free(p->files);
    mem_free(p->files_old);
    mem_free(p->cmdline);
    mem_free(p);
}

/* A new one, empty; NULL if there is no room. Ended ones nobody will wait
   for go first. */
static struct process *proc_new(void) {
    unsigned slot = PROCESSES;

    for (unsigned i = 0; i < PROCESSES; i++) {
        if (procs[i] != NULL && procs[i]->zombie && procs[i]->ppid == 0 && procs[i] != proc) {
            proc_free(procs[i]);
        }
        if (procs[i] == NULL && slot == PROCESSES) {
            slot = i;
        }
    }
    struct process *p = slot < PROCESSES ? mem_alloc(sizeof *p) : NULL;

    if (p != NULL) {
        memset(p, 0, sizeof *p);
        if ((p->files = mem_alloc(FIRST_FILES * sizeof *p->files)) == NULL) {
            procs[slot] = NULL;
            mem_free(p);
            return NULL;
        }
        memset(p->files, 0, FIRST_FILES * sizeof *p->files);
        p->files_room = FIRST_FILES;
        p->pid = thread_new_id();
        procs[slot] = p;
    }
    return p;
}

bool process_info(unsigned index, struct process_info *out) {
    for (unsigned i = 0; i < PROCESSES; i++) {
        struct process *p = procs[i];

        if (p == NULL || p->zombie || index-- > 0) {
            continue;
        }
        out->pid = p->pid;
        out->ppid = p->ppid;
        out->bytes = 4096 + (uint64_t)thread_count(p) * THREAD_STACK_BYTES;
        if (!p->borrowed && p->space != NULL) {
            out->bytes += p->space->bought + p->space->low_owned * VM_PAGE + VM_PAGE;
        }
        memcpy(out->name, p->name, sizeof out->name);
        out->name[sizeof out->name - 1] = '\0';
        return true;
    }
    return false;
}

/* /proc/<pid>/..., or /proc/self/...: the process, and what follows - NULL
   if path names no live process. */
static struct process *proc_path(const char *path, const char **rest) {
    unsigned pid = 0;
    const char *p;

    if (path == NULL || memcmp(path, "/proc/", 6) != 0) {
        return NULL;
    }
    p = path + 6;
    if (memcmp(p, "self", 4) == 0 && (p[4] == '/' || p[4] == '\0')) {
        pid = proc != NULL ? (unsigned)proc->pid : 0;
        p += 4;
    } else {
        if (*p < '1' || *p > '9') {
            return NULL;
        }
        for (; *p >= '0' && *p <= '9'; p++) {
            pid = pid * 10 + (unsigned)(*p - '0');
        }
    }
    if (*p != '/' && *p != '\0') {
        return NULL;
    }
    struct process *found = proc_find((int)pid);

    *rest = *p == '/' ? p + 1 : p;
    return found;
}

int proc_pid_folder(const char *name, uint32_t *uid, uint32_t *gid) {
    const char *rest;
    struct process *p = proc_path(name, &rest);

    if (p == NULL || *rest != '\0') {
        return 0;
    }
    *uid = p->cred.euid;
    *gid = p->cred.egid;
    return p->pid;
}

int proc_pid_at(unsigned index) {
    for (unsigned i = 0; i < PROCESSES; i++) {
        if (procs[i] != NULL && index-- == 0) {
            return procs[i]->pid;
        }
    }
    return 0;
}

/* What ps, top and pkill read of a process: stat, status, statm, cmdline
   and comm. Times are in Linux's clock ticks, 100 a second. */
size_t process_file(const char *path, char *out, size_t max) {
    const char *rest;
    struct process *p = proc_path(path, &rest);
    char text[640];
    size_t len;

    /* The machine's: every live process's time, and the rest idle. */
    if (path != NULL && (strcmp(path, "/proc/stat") == 0 || strcmp(path, "/proc/loadavg") == 0)) {
        uint64_t busy = 0, user = 0, now = efi_uptime_us();
        unsigned live = 0, last = 0;

        if (out == NULL) {
            return 0;
        }
        for (unsigned i = 0; i < PROCESSES; i++) {
            if (procs[i] != NULL) {
                user += procs[i]->times.user;
                busy += procs[i]->times.user + procs[i]->times.sys;
                live++;
                last = (unsigned)procs[i]->pid > last ? (unsigned)procs[i]->pid : last;
            }
        }
        busy = busy < now ? busy : now;
        if (path[6] == 'l') {
            ksprintf(text, "0.00 0.00 0.00 1/%u %u\n", live, last);
        } else {
            unsigned u = (unsigned)(user / 10000), y = (unsigned)((busy - user) / 10000),
                     idle = (unsigned)((now - busy) / 10000);

            ksprintf(text, "cpu  %u 0 %u %u 0 0 0 0 0 0\ncpu0 %u 0 %u %u 0 0 0 0 0 0\n"
                     "intr 0\nctxt 0\nbtime %u\nprocesses %u\nprocs_running 1\nprocs_blocked 0\n",
                     u, y, idle, u, y, idle, (unsigned)((realtime_us() - now) / 1000000), last, 0);
        }
        len = strlen(text);
        memcpy(out, text, len < max ? len : max);
        return len < max ? len : max;
    }
    if (p == NULL || (strcmp(rest, "stat") != 0 && strcmp(rest, "status") != 0 &&
                      strcmp(rest, "statm") != 0 && strcmp(rest, "cmdline") != 0 &&
                      strcmp(rest, "comm") != 0)) {
        return (size_t)-1;
    }
    if (out == NULL) {
        return 0;
    }
    uint64_t pages = 1 + thread_count(p) * (THREAD_STACK_BYTES / VM_PAGE);
    const char *state = p->zombie ? "Z" : p == proc ? "R" : "S";

    if (!p->borrowed && p->space != NULL) {
        pages += p->space->bought / VM_PAGE + p->space->low_owned + 1;
    }
    if (strcmp(rest, "cmdline") == 0) {
        len = p->cmdline_len < max ? p->cmdline_len : max;
        if (p->cmdline != NULL) {
            memcpy(out, p->cmdline, len);
        }
        return p->cmdline != NULL ? len : 0;
    }
    if (strcmp(rest, "comm") == 0) {
        ksprintf(text, "%s\n", p->name);
    } else if (strcmp(rest, "statm") == 0) {
        ksprintf(text, "%u %u 0 0 0 %u 0\n", (unsigned)pages, (unsigned)pages, (unsigned)pages);
    } else if (strcmp(rest, "status") == 0) {
        ksprintf(text, "Name:\t%s\nUmask:\t0022\nState:\t%s\nTgid:\t%u\nNgid:\t0\nPid:\t%u\n"
                 "PPid:\t%u\nTracerPid:\t0\nUid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\n"
                 "VmSize:\t%u kB\nVmRSS:\t%u kB\nThreads:\t%u\n",
                 p->name, state, (unsigned)p->pid, (unsigned)p->pid, (unsigned)p->ppid,
                 p->cred.uid, p->cred.euid, p->cred.suid, p->cred.euid,
                 p->cred.gid, p->cred.egid, p->cred.sgid, p->cred.egid,
                 (unsigned)(pages * 4), (unsigned)(pages * 4), thread_count(p));
    } else {
        ksprintf(text, "%u (%s) %s %u %u %u 0 -1 4194304 0 0 0 0 %u %u %u %u 20 0 %u 0 %u %u %u "
                 "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
                 (unsigned)p->pid, p->name, state, (unsigned)p->ppid, (unsigned)p->pgid,
                 (unsigned)p->sid, (unsigned)(p->times.user / 10000),
                 (unsigned)(p->times.sys / 10000), (unsigned)(p->times.children_user / 10000),
                 (unsigned)(p->times.children_sys / 10000), thread_count(p),
                 (unsigned)(p->times.started / 10000), (unsigned)(pages * VM_PAGE),
                 (unsigned)pages);
    }
    len = strlen(text);
    memcpy(out, text, len < max ? len : max);
    return len < max ? len : max;
}

/* Whether any program at all is still running. */
static bool any_process(void) {
    for (unsigned i = 0; i < PROCESSES; i++) {
        if (procs[i] != NULL && !procs[i]->zombie) {
            return true;
        }
    }
    return false;
}


/* ---- /dev ----------------------------------------------------------------
 *
 * The handful of files every Linux program expects to be able to open. None
 * of them is on the disk and none of them needs to be: what they do is so
 * little that they are a switch in read and another in write.
 *
 * /dev/null is where a shell sends output it does not want, and a program
 * given nowhere to put something and no /dev/null to put it stops. */


const struct device devices[DEVICES] = {
    { "/dev/null",    DEV_NULL,   0x0103 },
    { "/dev/zero",    DEV_ZERO,   0x0105 },
    { "/dev/full",    DEV_FULL,   0x0107 },
    { "/dev/random",  DEV_RANDOM, 0x0108 },
    { "/dev/urandom", DEV_RANDOM, 0x0109 },
    { "/dev/tty",     DEV_TTY,    0x0500 },
    { "/dev/stdin",   DEV_TTY,    0x0500 },
    { "/dev/stdout",  DEV_TTY,    0x0500 },
    { "/dev/stderr",  DEV_TTY,    0x0500 },
    { "/dev/console", DEV_TTY,    0x0501 },
    { "/dev/tty0",    DEV_TTY,    0x0400 },
    { "/dev/tty1",    DEV_TTY,    0x0401 },
    { "/dev/fb0",     DEV_FB,     0x1D00 },
    { "/dev/input/event0", DEV_EVENT0, 0x0D40 },
    { "/dev/input/event1", DEV_EVENT1, 0x0D41 },
};


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
enum dev dev_named(const char *name) {
    const char *leaf = in_dev(name);

    for (unsigned i = 0; leaf != NULL && *leaf != '\0' && i < DEVICES; i++) {
        if (strcmp(leaf, devices[i].name + 5) == 0) {
            return devices[i].which;
        }
    }
    return 0;
}

unsigned dev_folder(const char *name) {
    const char *leaf = in_dev(name);

    return leaf == NULL ? 0 : *leaf == '\0' ? 1 :
           strcmp(leaf, "input") == 0 || strcmp(leaf, "input/") == 0 ? 2 :
           strcmp(leaf, "pts") == 0 || strcmp(leaf, "pts/") == 0 ? 3 : 0;
}

const struct file_ops *ops_named(const char *path, const char **leaf) {
    const char *in = dev_folder(path) == 0 ? in_dev(path) : NULL;

    for (unsigned i = 0; in != NULL && i < FILE_OPS; i++) {
        size_t n = file_ops[i] != NULL ? strlen(file_ops[i]->prefix) : 0;

        if (n > 0 && memcmp(in, file_ops[i]->prefix, n) == 0) {
            *leaf = in;
            return file_ops[i];
        }
    }
    return NULL;
}

/* ---- shared positions ----------------------------------------------------
 *
 * On Linux an open file's position belongs to the opening, not to each
 * descriptor: dup and fork share it. `cmd > log 2>&1` writes stdout and
 * stderr one after the other, and `{ a; b; } > out` has b go on where a
 * stopped. So a file's position and size live here, shared by every copy of
 * its descriptor, counted; a syscall loads them into the descriptor when it
 * picks it up, and they are written back when the syscall ends. A file read
 * or written never waits half way, so nothing else runs in between. */

#define OFDS 512

/* The memory of a file opened for writing and mapped MAP_SHARED: pages of
   share.c's, one per page of the file, counted once here and once more by
   each mapping of them - so every process mapping it, by a descriptor passed
   to it or its own, has the same memory, and it lasts while any of them
   does. Chromium's processes, and Firefox's, talk through it. */
struct mobj {
    uint32_t refs;                  /* the open files that have it */
    uint32_t room;
    uint64_t *frame;                /* room of them, 0 where nothing is yet */
};

static struct ofd {
    uint32_t refs, offset, size;
    struct mobj *mem;
    char *name;                     /* a file open for writing: what it is written back to */
} ofds[OFDS];

/* The name a file open for writing goes back to - kept with the open file,
   not the process, so every descriptor of it has it: a dup's, a fork's, one
   passed to another process. */
const char *write_name(const struct handle *h) {
    return h != NULL && h->ofd != 0 && ofds[h->ofd - 1].name != NULL ? ofds[h->ofd - 1].name : "";
}

/* Whether h's bytes are memory, not the disk: a memory file, or a file open
   for writing whose name has since been deleted - kept, as Linux keeps it,
   for as long as anything has it open. Chromium shares memory that way. */
static bool mem_backed(const struct handle *h) {
    return h->start == MEM_MARK ||
           (h->start == WRITE_MARK && h->ofd != 0 && ofds[h->ofd - 1].name == NULL &&
            ofds[h->ofd - 1].mem != NULL);
}

static void ofd_drop(struct handle *h);
static uint64_t with_cloexec(uint64_t fd, bool on);

static struct mobj *mobj_new(void) {
    struct mobj *m = mem_alloc(sizeof *m);

    if (m != NULL) {
        *m = (struct mobj){ .refs = 1 };
    }
    return m;
}

static void mobj_free(struct mobj *m) {
    if (m != NULL && --m->refs == 0) {
        for (uint32_t i = 0; i < m->room; i++) {
            if (m->frame[i] != 0) {
                shared_drop(m->frame[i]);
            }
        }
        mem_free(m->frame);
        mem_free(m);
    }
}

/* Page index of m, made (empty) if it is not there yet and make is set; 0 if
   it is not, or there is no memory for it. */
static uint64_t mobj_page(struct mobj *m, uint32_t index, bool make) {
    if (index >= m->room) {
        if (!make) {
            return 0;
        }
        uint32_t room = m->room == 0 ? 16 : m->room;

        while (room <= index) {
            room *= 2;
        }
        uint64_t *more = mem_alloc(room * sizeof *more);

        if (more == NULL) {
            return 0;
        }
        memset(more, 0, room * sizeof *more);
        if (m->frame != NULL) {
            memcpy(more, m->frame, m->room * sizeof *more);
            mem_free(m->frame);
        }
        m->frame = more;
        m->room = room;
    }
    if (m->frame[index] == 0 && make) {
        uint64_t page = mem_pages(1);

        if (page == 0) {
            return 0;
        }
        memset((void *)page, 0, PAGE_SIZE);
        if (!shared_adopt(page)) {
            mem_pages_free(page, 1);
            return 0;
        }
        m->frame[index] = page;
    }
    return m->frame[index];
}

/* A memory file's bytes in and out, at its position. */
static uint64_t mem_io(struct handle *h, uint64_t buf, uint64_t count, bool in) {
    struct mobj *m = h->ofd != 0 ? ofds[h->ofd - 1].mem : NULL;
    uint64_t done = 0;

    if (m == NULL || !user_range(buf, count)) {
        return ERR(m == NULL ? EBADF : EFAULT);
    }
    if (!in) {
        count = h->offset < h->size ? (count < h->size - h->offset ? count : h->size - h->offset) : 0;
    }
    while (done < count) {
        uint64_t at = (uint64_t)h->offset + done;
        uint64_t k = PAGE_SIZE - at % PAGE_SIZE;
        uint64_t page = mobj_page(m, (uint32_t)(at / PAGE_SIZE), in);

        k = k < count - done ? k : count - done;
        if (in && page == 0) {
            break;                  /* no memory: as far as it got */
        }
        if (in) {
            memcpy((char *)page + at % PAGE_SIZE, (const char *)buf + done, k);
        } else if (page != 0) {
            memcpy((char *)buf + done, (const char *)page + at % PAGE_SIZE, k);
        } else {
            memset((char *)buf + done, 0, k);
        }
        done += k;
    }
    h->offset += (uint32_t)done;
    if (in && h->offset > h->size) {
        h->size = h->offset;
    }
    return done;
}

/* memfd_create: a file that is only memory - shared, mapped, passed. What
   Chromium and Firefox share their memory through on Linux. */
#define MFD_CLOEXEC 1

static uint64_t sys_memfd_create(uint64_t name, uint64_t flags, uint64_t c) {
    unsigned slot = OFDS;
    struct mobj *m;

    (void)name;
    (void)c;
    for (unsigned i = 0; i < OFDS && slot == OFDS; i++) {
        slot = ofds[i].refs == 0 ? i : slot;
    }
    if (slot == OFDS || (m = mobj_new()) == NULL) {
        return ERR(slot == OFDS ? ENFILE : ENOMEM);
    }
    ofds[slot] = (struct ofd){ 1, 0, 0, m, NULL };

    struct handle h = { .start = MEM_MARK, .ofd = slot + 1 };
    uint64_t fd = give_handle(h);

    if ((int64_t)fd < 0) {
        ofd_drop(&h);
        return fd;
    }
    return with_cloexec(fd, (flags & MFD_CLOEXEC) != 0);
}

/* /proc/self/fd/N opened again, N a memory file: another open file of the
   same memory - Chromium's read-only view of what it shares. */
static uint64_t mem_reopen(const struct handle *of) {
    unsigned slot = OFDS;

    for (unsigned i = 0; i < OFDS && slot == OFDS; i++) {
        slot = ofds[i].refs == 0 ? i : slot;
    }
    if (slot == OFDS) {
        return ERR(ENFILE);
    }
    struct ofd *was = &ofds[of->ofd - 1];

    /* A file on the disk, deleted as like as not, that is shared as memory:
       its memory made now if it has none yet, so both views have the same. */
    if (was->mem == NULL && (was->mem = mobj_new()) == NULL) {
        return ERR(ENOMEM);
    }
    was->mem->refs++;
    ofds[slot] = (struct ofd){ 1, 0, was->size, was->mem, NULL };

    struct handle h = { .start = MEM_MARK, .ofd = slot + 1, .size = was->size };
    uint64_t fd = give_handle(h);

    if ((int64_t)fd < 0) {
        ofd_drop(&h);
    }
    return fd;
}

static bool ofd_kind(const struct handle *h) {
    return h->start == WRITE_MARK || h->start < FIRST_MARK;
}

static void ofd_hold(const struct handle *h) {
    if (h->ofd != 0) {
        ofds[h->ofd - 1].refs++;
    }
}

static void ofd_drop(struct handle *h) {
    if (h->ofd != 0 && ofds[h->ofd - 1].refs > 0 && --ofds[h->ofd - 1].refs == 0) {
        mobj_free(ofds[h->ofd - 1].mem);    /* what is still mapped keeps its pages */
        ofds[h->ofd - 1].mem = NULL;
        mem_free(ofds[h->ofd - 1].name);
        ofds[h->ofd - 1].name = NULL;
    }
    h->ofd = 0;
}

/* Back from the descriptors this syscall used to the positions they share. */
static void ofd_sync(void) {
    if (proc == NULL) {
        return;
    }
    if (proc->files_old != NULL) {
        mem_free(proc->files_old);  /* nobody is holding on to it now */
        proc->files_old = NULL;
    }
    for (unsigned word = 0; word < PROGRAM_FILES / 64; word++) {
        for (unsigned fd = word * 64; proc->ofd_touched[word] != 0 && fd < word * 64 + 64; fd++) {
            struct handle *h = &handles[fd];

            if (TOUCHED(fd) && fd < file_room && h->used != 0 && h->ofd != 0) {
                ofds[h->ofd - 1].offset = h->offset;
                ofds[h->ofd - 1].size = h->size;
            }
        }
        proc->ofd_touched[word] = 0;
    }
}

struct handle *handle_of(uint64_t fd) {
    fd = (uint32_t)fd;              /* an int: what is above it is anybody's */
    if (proc == NULL || fd >= file_room) {
        return NULL;
    }
    struct handle *h = &handles[fd];

    if (h->used == 0) {
        return NULL;
    }
    if (h->ofd == 0 && ofd_kind(h)) {
        for (unsigned i = 0; i < OFDS; i++) {
            if (ofds[i].refs == 0) {
                ofds[i] = (struct ofd){ 1, h->offset, h->size, NULL, NULL };
                h->ofd = i + 1;
                break;
            }
        }
    }
    /* Loaded once a syscall: after that the descriptor is the newer of the
       two - pread moves it and reads through here again. */
    if (h->ofd != 0 && !TOUCHED(fd)) {
        h->offset = ofds[h->ofd - 1].offset;
        h->size = ofds[h->ofd - 1].size;
        proc->ofd_touched[fd / 64] |= 1ull << (fd % 64);
    }
    return h;
}

/* Close-on-exec, as a bit of the descriptor's `used`: on, the descriptor
   goes at the next execve - what keeps a pipe's write end out of a program
   that would otherwise hold it open for ever. dup clears it; fork keeps it. */
void fd_cloexec(uint64_t fd, bool on) {
    struct handle *h = handle_of(fd);

    if (h != NULL) {
        h->used = on ? h->used | HANDLE_CLOEXEC : h->used & ~HANDLE_CLOEXEC;
    }
}

static uint64_t with_cloexec(uint64_t fd, bool on) {
    if ((int64_t)fd >= 0 && on) {
        fd_cloexec(fd, true);
    }
    return fd;
}

/* True if fd is the console rather than a file, which is what decides
   whether a terminal's questions have an answer. */
bool is_console(uint64_t fd) {
    struct handle *h = handle_of(fd);

    return h != NULL && h->start == CONSOLE_MARK;
}

/* What a program starts with: the console on 0, 1 and 2, nothing else open.
   A program that exits without closing its files leaves them behind, so this
   runs before each one rather than after. */
static void handles_reset(void) {
    memset(handles, 0, file_room * sizeof *handles);
    for (unsigned fd = 0; fd < 3; fd++) {
        handles[fd].used = 1;
        handles[fd].start = CONSOLE_MARK;
    }
}

/* ---- pipes ---------------------------------------------------------------
 *
 * A buffer with a read end and a write end, both ordinary descriptors. It
 * grows as it is written to, up to PIPE_CAP, and then its writer waits for
 * the program at the other end - running beside it - to drain it. */

#define PIPES      256              /* pipes, eventfds and socketpair ends open at once:
                                       an X server and its windows hold dozens, a
                                       browser's processes hundreds */
#define PIPE_FIRST 8192
#define PIPE_CAP   (64 * 1024)      /* the most one holds: then its writer waits */

#define PIPE_TEXT  8192             /* the most of one */
#define EFD_SEMAPHORE 1

/* A message on a socketpair: where its bytes start in what has ever been
   written, how many there are (a packet's; a stream's is 0), and the
   descriptors sent with it, held until they are received. */
#define PAIR_FDS 16

struct pmsg {
    uint64_t at;
    uint32_t len, nfd;
    int      pid;                   /* who sent it, for SCM_CREDENTIALS */
    struct handle fd[PAIR_FDS];
};

static struct pipe {
    char    *data;
    uint32_t size, len, read_at;
    unsigned refs;                  /* descriptors on either end */
    uint64_t count;                 /* an eventfd's; a pipe's, the write ends open on it */
    uint32_t fifo;                  /* a FIFO's: the number of the file it is */
    bool     packets;               /* a SEQPACKET or DGRAM pair's: a write is one message */
    uint64_t wrote, taken;          /* bytes ever put in, and taken out */
    struct pmsg *msgs;              /* its messages' bounds and descriptors, oldest first */
    uint32_t nmsgs, msg_room;
    bool     passcred;              /* its reader asked who sends (SO_PASSCRED) */
    int      last_pid;              /* who wrote to it last */
    char    *made_from;             /* a /proc file's name, to make it again
                                       when it is read from the start again */
} *pipes;                           /* PIPES of them, mem_alloc'd at start: too many
                                       for the kernel's own image */

static void handle_release(struct handle *h);
struct iovec {
    uint64_t base;
    uint64_t length;
};
static uint64_t pair_take(struct handle *h, struct pipe *p, const struct iovec *iov, uint64_t n,
                          struct handle *fds, uint32_t *nfd, bool *cut);
extern int pair_sender;
static uint64_t pair_put(struct handle *h, struct pipe *p, const struct iovec *iov, uint64_t n,
                         const struct handle *fds, uint32_t nfd, bool nonblock);

struct pipe *pipe_of(const struct handle *h) {
    return h != NULL && h->start == PIPE_MARK && h->folder > 0 &&
           h->folder <= PIPES && pipes[h->folder - 1].refs > 0
         ? &pipes[h->folder - 1] : NULL;
}

static void pipe_drop(struct pipe *p) {
    if (p != NULL && p->refs > 0 && --p->refs == 0) {
        if (p->data != NULL) {
            mem_free(p->data);
        }
        mem_free(p->made_from);
        /* Descriptors sent and never received go with it. */
        struct pmsg *msgs = p->msgs;
        uint32_t n = p->nmsgs;

        *p = (struct pipe){ 0 };
        for (uint32_t i = 0; i < n; i++) {
            for (uint32_t k = 0; k < msgs[i].nfd; k++) {
                handle_release(&msgs[i].fd[k]);
            }
        }
        mem_free(msgs);
    }
}

/* The pipe a socketpair end writes to, or NULL for anything else. */
static struct pipe *pair_out(const struct handle *h) {
    uint32_t slot = h != NULL && h->start == PIPE_MARK && h->size == PIPE_PAIR ? h->offset >> 16 : 0;

    return slot > 0 && slot <= PIPES && pipes[slot - 1].refs > 0 ? &pipes[slot - 1] : NULL;
}

/* The pipe a handle writes to - a write end's, or a pair end's other one -
   or NULL. */
static struct pipe *written(const struct handle *h) {
    return h->size == PIPE_WRITE ? pipe_of(h) : pair_out(h);
}

/* One descriptor more, or fewer, on a pipe handle: both of a pair's pipes,
   and the count of write ends on the one it writes to. */
static void pipe_hold(const struct handle *h) {
    struct pipe *p = pipe_of(h), *out = pair_out(h), *w = written(h);

    if (p != NULL) {
        p->refs++;
    }
    if (out != NULL) {
        out->refs++;
    }
    if (w != NULL) {
        w->count++;
    }
}

static void pipe_release(const struct handle *h) {
    struct pipe *w = written(h);

    if (w != NULL && w->count > 0) {
        w->count--;                 /* at 0 the other end reads its end */
    }
    pipe_drop(pair_out(h));
    pipe_drop(pipe_of(h));
}

/* Whether a pipe's read end, or a pair end, has something to read - or
   nothing can be written to it any more, which is its end. An empty one
   with a write end still open is not ready, as on Linux: a program waiting
   on a pipe its own signal handler writes to is waiting for that. */
bool readable(const struct handle *h) {
    struct pipe *p = pipe_of(h);

    if (h->size != PIPE_PAIR && h->size != PIPE_READ) {
        return false;               /* an eventfd's count is not this */
    }
    return p == NULL || p->len > p->read_at || p->count == 0;
}


/* ---- sockets, whichever module's ---------------------------------------- */

void sock_hold(const struct handle *h) {
    if ((h->offset & SOCK_UNIX) != 0) {
        if (unix_sock != NULL) {
            unix_sock->hold((int)h->folder);
        }
    } else if (net != NULL) {
        net->hold((int)h->folder);
    }
}

void sock_drop(const struct handle *h) {
    if ((h->offset & SOCK_UNIX) != 0) {
        if (unix_sock != NULL) {
            unix_sock->drop((int)h->folder);
        }
    } else if (net != NULL) {
        net->drop((int)h->folder);
    }
}

unsigned sock_ready(const struct handle *h) {
    if ((h->offset & SOCK_UNIX) != 0) {
        return unix_sock != NULL ? unix_sock->ready((int)h->folder) : NET_ERR;
    }
    return net != NULL ? net->ready((int)h->folder) : NET_ERR;
}

/* A free one, or PIPES. */
static unsigned pipe_slot(void);

uint64_t buffer_open(uint32_t bytes) {
    unsigned slot = pipe_slot();
    void *data = slot < PIPES ? mem_alloc(bytes) : NULL;
    uint64_t fd;

    if (data == NULL) {
        return ERR(ENFILE);
    }
    memset(data, 0, bytes);
    pipes[slot] = (struct pipe){ .data = data, .size = bytes, .refs = 1 };
    fd = give_handle((struct handle){ .start = PIPE_MARK, .folder = slot + 1, .size = PIPE_BUFFER });
    if ((int64_t)fd < 0) {
        mem_free(data);
        pipes[slot] = (struct pipe){ 0 };
    }
    return fd;
}

void *pipe_data(const struct handle *h) {
    struct pipe *p = pipe_of(h);

    return p != NULL && h->size == PIPE_BUFFER ? p->data : NULL;
}

static unsigned pipe_slot(void) {
    unsigned slot = 0;

    while (slot < PIPES && pipes[slot].refs > 0) {
        slot++;
    }
    return slot;
}

/* The name of a /proc/net file the network module has - or the whole path
   of one of the kernel's own (proc_linux) - or NULL. */
const char *proc_net_name(const char *path) {
    static const char dir[] = "/proc/net/";

    if (path != NULL && proc_linux(path, NULL, 0) != (size_t)-1) {
        return path;
    }
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
    size_t len = name[0] == '/' ? proc_linux(name, text, PIPE_TEXT) : net->proc(name, text, PIPE_TEXT);

    fd = give_handle((struct handle){ .start = PIPE_MARK, .folder = slot + 1, .size = PIPE_FILE });
    if ((int64_t)fd < 0) {
        mem_free(text);
        return fd;
    }
    pipes[slot] = (struct pipe){ .data = text, .size = PIPE_TEXT, .refs = 1,
                                 .len = (uint32_t)(len < PIPE_TEXT ? len : PIPE_TEXT) };
    if ((pipes[slot].made_from = mem_alloc(strlen(name) + 1)) != NULL) {
        strcpy(pipes[slot].made_from, name);
    }
    return fd;
}

bool event_ready(const struct handle *h) {
    struct pipe *p = pipe_of(h);

    return p != NULL && h->size == PIPE_EVENT && p->count > 0;
}


/* Adds to the buffer, as much as there is room for, growing it up to
   PIPE_CAP; what has been read is moved out of the way first. Returns how
   much went in, which may be none. */
static uint64_t pipe_put(struct pipe *p, const char *from, uint64_t count) {
    if (p->read_at > 0) {
        memmove(p->data, p->data + p->read_at, p->len - p->read_at);
        p->len -= p->read_at;
        p->read_at = 0;
    }
    if (count > PIPE_CAP - p->len) {
        count = PIPE_CAP - p->len;
    }
    if (p->len + count > p->size) {
        uint32_t want = p->size;
        void *bigger = NULL;

        while (want < p->len + count) {
            want *= 2;
        }
        if ((bigger = mem_alloc(want)) == NULL) {
            return 0;
        }
        memcpy(bigger, p->data, p->len);
        mem_free(p->data);
        p->data = bigger;
        p->size = want;
    }
    memcpy(p->data + p->len, from, (size_t)count);
    p->len += (uint32_t)count;
    p->wrote += count;
    return count;
}

/* A write to a pipe, all of it, waiting for the reader while it is full -
   the other program is running beside this one, and drains it. Nobody left
   to read is EPIPE, and SIGPIPE, as on Linux: how `yes | head` ends. */
static uint64_t pipe_write(const struct handle *h, struct pipe *p, const char *from,
                           uint64_t count) {
    uint64_t done = 0;

    while (done < count) {
        if (h->size == PIPE_WRITE && p->refs <= p->count) {
            signal_to(proc, SIGPIPE);
            return done > 0 ? done : ERR(EPIPE);
        }
        uint64_t n = pipe_put(p, from + done, count - done);

        done += n;
        if (n > 0 || done == count) {
            continue;
        }
        if ((h->offset & O_NONBLOCK) != 0 || thread_only()) {
            return done > 0 ? done : ERR(EAGAIN);
        }
        if (interrupt_check()) {
            return done > 0 ? done : ERR(EINTR);
        }
        thread_yield();
    }
    return done;
}

uint32_t pipe_left(const struct pipe *p) {
    return p->len - p->read_at;
}

static uint64_t pipe_read(struct pipe *p, char *to, uint64_t count) {
    uint32_t left = p->len - p->read_at;

    if (count > left) {
        count = left;
    }
    if (to != NULL) {
        memcpy(to, p->data + p->read_at, (size_t)count);
    }
    p->read_at += (uint32_t)count;
    p->taken += count;
    if (p->read_at == p->len) {
        p->read_at = p->len = 0;    /* all read: a pair is written to again */
    }
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
        if ((h->offset & O_NONBLOCK) != 0 || thread_only()) {
            return ERR(EAGAIN);
        }
        if (interrupt_check()) {
            return ERR(EINTR);
        }
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

/* /dev/fb0 read or written like a file: the screen's bytes, from where the
   descriptor is. */
static uint64_t fb_copy(struct handle *h, void *buf, uint64_t count, bool out) {
    struct vga_screen s;

    vga_screen(&s);
    uint64_t size = (uint64_t)s.pitch * s.height;

    if (h->offset >= size) {
        return out ? ERR(ENOSPC) : 0;
    }
    if (count > size - h->offset) {
        count = size - h->offset;
    }
    if (out) {
        memcpy((char *)s.base + h->offset, buf, count);
    } else {
        memcpy(buf, (const char *)s.base + h->offset, count);
    }
    h->offset += (uint32_t)count;
    return count;
}

static uint64_t mem_io(struct handle *h, uint64_t buf, uint64_t count, bool in);

static uint64_t write_to(struct handle *h, uint64_t text, uint64_t length) {
    if (h == NULL) {
        return ERR(EBADF);
    }
    if (mem_backed(h)) {
        return mem_io(h, text, length, true);
    }
    if (h->start == WRITE_MARK) {
        /* Into the file where the descriptor is, which is its end when the
           program has only been writing and anywhere in it once the program
           has moved about - what a program keeping a scratch file of its own
           does, ed's buffer among them. */
        if (fs_write_at(write_name(h), h->offset,
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
        if (h->size == PIPE_PAIR) {
            p = pair_out(h);
            if (p != NULL && p->packets) {
                struct iovec one = { (uint64_t)text, length };

                return pair_put(h, p, &one, 1, NULL, 0, false);
            }
            return p == NULL ? ERR(EPIPE) : pipe_write(h, p, (const char *)text, length);
        }
        return p == NULL || h->size != PIPE_WRITE ? ERR(EBADF)
                                                  : pipe_write(h, p, (const char *)text, length);
    }
    if (h->start == SOCK_MARK) {
        return (h->offset & SOCK_UNIX) != 0 ? ERR(ENOTCONN)
             : net != NULL ? net->write(h, text, length) : ERR(EBADF);
    }
    if (h->start == DEV_MARK && h->folder == DEV_FB) {
        return fb_copy(h, (void *)text, length, true);
    }
    if (h->start == MOD_MARK) {
        return ops_of(h) != NULL ? ops_of(h)->write(h, text, length) : ERR(EIO);
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



uint64_t self_us(void) {
    return now_running.user + now_running.sys;
}

/* A yield that let something else run: that time was not the caller's, so
   it is taken off its system time as a wait's is. */
void process_yielded(uint64_t us) {
    if (proc != NULL) {
        proc->times.idle += us;
    }
}

uint64_t user_us(void) {
    return now_running.user;
}

/* Marks the start of a wait, and then its end, which counts it idle. */
uint64_t wait_began(void) {
    return efi_uptime_us();
}

void wait_ended(uint64_t began) {
    now_running.idle += efi_uptime_us() - began;
}

/* ---- Ctrl-C and Ctrl-\\ ---------------------------------------------------
 *
 * The keyboard's SIGINT and SIGQUIT, the only signals anything here sends.
 * Left to their default, each ends the program, as on Linux. A program that
 * ignores one hears nothing of it. One with a handler for SIGINT - a shell,
 * an editor, a pager - has the call it is waiting in end with EINTR, and on
 * the way back out of the syscall its handler runs, on its own stack, in a
 * frame laid out as Linux lays one out; rt_sigreturn puts it back. Nothing
 * preempts a program, so one is told at its next syscall, and checked every
 * so often between waits as well as in them. */

#define SIGINT       2
#define SIGQUIT      3
#define SIGCHLD      17
#define SIG_DFL      0
#define SIG_IGN      1
#define SA_RESTORER  0x04000000
#define SA_RESETHAND 0x80000000u
#define SA_NODEFER   0x40000000u

#define SIGHUP   1
#define SIGABRT  6
#define SIGKILL  9
#define SIGSEGV  11
#define SIGTERM  15
#define SIGCONT  18
#define SIGSTOP  19
#define SIGURG   23
#define SIGWINCH 28
#define ESRCH    3

extern struct user_regs *user_frame;    /* the program's registers, in syscall_entry.asm */

#define blocked (*thread_sigmask())     /* rt_sigprocmask's set, bit sig - 1: per thread */

static struct sig_action *action_for(uint64_t sig) {
    return sig > 0 && sig < SIGNALS ? &on_signal[sig] : NULL;
}

/* A handler to run, not the default or ignoring. */
static bool handled(uint64_t sig) {
    const struct sig_action *a = action_for(sig);

    return a != NULL && a->handler > SIG_IGN && (a->flags & SA_RESTORER) != 0;
}

/* What a signal does left to itself: these are let go, and nothing can be
   stopped here, so the stopping ones are let go too. The rest end it. */
static bool ignored_by_default(uint64_t sig) {
    return sig == SIGCHLD || sig == SIGCONT || sig == SIGURG || sig == SIGWINCH ||
           (sig >= SIGSTOP && sig <= 22);
}

/* Sends sig to p: kept to be acted on the next time p runs, unless p lets
   it go. */
static void signal_to(struct process *p, uint64_t sig) {
    const struct sig_action *a;

    if (p == NULL || p->zombie || sig == 0 || sig >= SIGNALS) {
        return;
    }
    a = &p->actions[sig];
    if (sig != SIGKILL && (a->handler == SIG_IGN ||
                           (a->handler == SIG_DFL && ignored_by_default(sig)))) {
        return;
    }
    p->signals |= 1ull << sig;
}

/* Sends sig to every process in group pgid; false if there was none. */
static bool signal_group(int pgid, uint64_t sig) {
    bool any = false;

    for (unsigned i = 0; i < PROCESSES; i++) {
        if (procs[i] != NULL && !procs[i]->zombie && procs[i]->pgid == pgid) {
            signal_to(procs[i], sig);
            any = true;
        }
    }
    return any;
}

void signal_pgrp(int pgid, int sig) {
    signal_group(pgid, (uint64_t)sig);
}

void process_ids(int *pid, int *pgid, int *sid) {
    *pid = proc != NULL ? proc->pid : 0;
    *pgid = proc != NULL ? proc->pgid : 0;
    *sid = proc != NULL ? proc->sid : 0;
}

/* The signal to handle now, if one is waiting, not blocked, and has a
   handler: the lowest first. */
uint64_t deliverable(void) {
    if (proc == NULL) {
        return 0;
    }
    for (uint64_t sig = 1; sig < SIGNALS; sig++) {
        if ((pending & 1ull << sig) != 0 && (blocked & 1ull << (sig - 1)) == 0 && handled(sig)) {
            return sig;
        }
    }
    return 0;
}

/* Ends the process if a signal waiting for it ends it: SIGKILL, or one left
   to its default that is not blocked. */
static void signal_fatal(void) {
    for (uint64_t sig = 1; proc != NULL && pending != 0 && sig < SIGNALS; sig++) {
        if ((pending & 1ull << sig) != 0 &&
            (sig == SIGKILL || ((blocked & 1ull << (sig - 1)) == 0 && !handled(sig)))) {
            pending &= ~(1ull << sig);
            if (sig == SIGKILL || on_signal[sig].handler == SIG_DFL) {
                process_exit(SIGNALLED | (int)sig);
            }
        }
    }
}

/* Whether the keyboard's signals have anyone to go to: a group still
   running. While nothing has, they are the kernel shell's. */
static bool foreground_alive(void) {
    for (unsigned i = 0; i < PROCESSES; i++) {
        if (procs[i] != NULL && !procs[i]->zombie && procs[i]->pgid == fg_pgrp) {
            return true;
        }
    }
    return false;
}

/* What the program does with a signal typed or sent; true if one is waiting
   to be handled by this thread, which is the moment for a wait to end with
   EINTR. A signal that ends it ends it here. */
bool interrupt_check(void) {
    if (proc == NULL) {
        return false;
    }
    int sig = foreground_alive() ? console_signal() : 0;

    if (sig == CONSOLE_FORCE) {
        vga_puts("^C\n");
        signal_group(fg_pgrp, SIGKILL);
    } else if (sig != 0) {
        struct process *leader = proc_find(fg_pgrp);

        if (leader != NULL && leader->actions[sig].handler == SIG_DFL) {
            vga_puts(sig == SIGINT ? "^C\n" : "^\\\n");
        }
        signal_group(fg_pgrp, (uint64_t)sig);
    }
    signal_fatal();
    return deliverable() != 0;
}

/* kill: to one process, to the caller's own group (0), to every other
   process (-1), or to a group (-pgid). Signal 0 only asks whether there is
   anyone there. pid_t is 32 bits: -1 arrives as 0xffffffff. */

static uint64_t sys_kill(uint64_t pid, uint64_t sig, uint64_t c) {
    int target = (int)pid;
    bool any = false;

    (void)c;
    if (sig >= SIGNALS) {
        return ERR(EINVAL);
    }
    if (target > 0) {
        struct process *p = proc_find(target);

        if (p == NULL || p->zombie) {
            return ERR(ESRCH);
        }
        signal_to(p, sig);
        return 0;
    }
    if (target == -1) {
        for (unsigned i = 0; i < PROCESSES; i++) {
            if (procs[i] != NULL && !procs[i]->zombie && procs[i] != proc) {
                signal_to(procs[i], sig);
                any = true;
            }
        }
        return any ? 0 : ERR(ESRCH);
    }
    return signal_group(target == 0 ? proc->pgid : -target, sig) ? 0 : ERR(ESRCH);
}

/* To one thread: here, to the process it is in - and a thread of the
   caller's own is the caller. */
static uint64_t sys_tgkill(uint64_t tgid, uint64_t tid, uint64_t sig) {
    (void)tid;
    return sys_kill((uint32_t)tgid, sig, 0);
}

/* Blocked signals wait in pending until they are let through. SIGKILL and
   SIGSTOP cannot be blocked. */
static uint64_t sys_rt_sigprocmask(uint64_t how, uint64_t set, uint64_t old) {
    const uint64_t always = 1ull << (9 - 1) | 1ull << (19 - 1);

    if ((set != 0 && !user_range(set, 8)) || (old != 0 && !user_range(old, 8))) {
        return ERR(EFAULT);
    }
    if (set != 0 && how > 2) {
        return ERR(EINVAL);
    }
    if (old != 0) {
        *(uint64_t *)old = blocked;
    }
    if (set != 0) {
        uint64_t s = *(const uint64_t *)set & ~always;

        blocked = how == 0 ? blocked | s : how == 1 ? blocked & ~s : s;    /* BLOCK, UNBLOCK, SETMASK */
    }
    return 0;
}

static uint64_t sys_rt_sigaction(uint64_t sig, uint64_t act, uint64_t old) {
    struct sig_action *a = action_for(sig);

    if ((uint32_t)sig < 1 || (uint32_t)sig > 64) {
        return ERR(EINVAL);         /* there is no such signal: how a program
                                       counting its way through them knows to stop */
    }
    if ((act != 0 && !user_range(act, sizeof *a)) || (old != 0 && !user_range(old, sizeof *a))) {
        return ERR(EFAULT);
    }
    if (old != 0) {
        memset((void *)old, 0, sizeof *a);          /* the rest are left at their default */
        if (a != NULL) {
            memcpy((void *)old, a, sizeof *a);
        }
    }
    if (act != 0 && (sig == SIGKILL || sig == SIGSTOP)) {
        return ERR(EINVAL);         /* neither can be caught */
    }
    if (act != 0 && a != NULL) {
        memcpy(a, (const void *)act, sizeof *a);
        if (a->handler == SIG_IGN || (a->handler == SIG_DFL && ignored_by_default(sig))) {
            pending &= ~(1ull << sig);
        }
    }
    return 0;
}

/* The frame a handler is called with, as Linux builds it: where to return
   to - the program's restorer, which calls rt_sigreturn - then the
   ucontext, its registers in sigcontext's order, then the siginfo. */
struct sig_frame {
    uint64_t restorer;
    struct {
        uint64_t flags, link, ss_sp;
        uint32_t ss_flags, pad;
        uint64_t ss_size;
        uint64_t gregs[23];         /* r8-r15, rdi, rsi, rbp, rbx, rdx, rax, rcx,
                                       rsp, rip, eflags, segments, err, trapno,
                                       oldmask, cr2 */
        uint64_t fpstate, reserved[8];
        uint64_t sigmask;
    } uc;
    struct {
        int32_t signo, errnum, code, pad;
        uint8_t rest[112];
    } info;
};

enum { G_R8, G_R9, G_R10, G_R11, G_R12, G_R13, G_R14, G_R15, G_RDI, G_RSI, G_RBP,
       G_RBX, G_RDX, G_RAX, G_RCX, G_RSP, G_RIP, G_EFLAGS };

/* Sends the program into the handler for a signal waiting, from this
   syscall, which would have answered result: SIGINT first, then SIGCHLD,
   one a call. The handler's return puts the mask back to restore. */
static uint64_t signal_deliver(uint64_t result, uint64_t restore) {
    struct user_regs *f = user_frame;
    uint64_t sig = deliverable();
    struct sig_action *a = action_for(sig);
    uint64_t at = ((f->rsp - 128 - sizeof(struct sig_frame)) & ~15ull) - 8;   /* past
                                       the red zone; as a call leaves it */
    struct sig_frame *frame = (struct sig_frame *)at;

    pending &= ~(1ull << sig);
    if (!user_range(at, sizeof *frame)) {
        process_exit(SIGNALLED | SIGSEGV);  /* nowhere to put it, as Linux */
    }
    memset(frame, 0, sizeof *frame);
    frame->restorer = a->restorer;
    frame->uc.sigmask = restore;    /* back as it was once the handler returns */
    blocked |= a->mask | ((a->flags & SA_NODEFER) != 0 ? 0 : 1ull << (sig - 1));

    uint64_t *g = frame->uc.gregs;

    g[G_R8] = f->r8;
    g[G_R9] = f->r9;
    g[G_R10] = f->r10;
    g[G_R11] = f->rflags;           /* what syscall left in R11 */
    g[G_R12] = f->r12;
    g[G_R13] = f->r13;
    g[G_R14] = f->r14;
    g[G_R15] = f->r15;
    g[G_RDI] = f->rdi;
    g[G_RSI] = f->rsi;
    g[G_RBP] = f->rbp;
    g[G_RBX] = f->rbx;
    g[G_RDX] = f->rdx;
    g[G_RAX] = result;
    g[G_RCX] = f->rip;              /* and in RCX */
    g[G_RSP] = f->rsp;
    g[G_RIP] = f->rip;
    g[G_EFLAGS] = f->rflags;
    frame->info.signo = (int32_t)sig;
    frame->info.code = 0x80;        /* SI_KERNEL */
    if (sig == SIGCHLD) {
        int32_t *child = (int32_t *)frame->info.rest;

        frame->info.code = 1;       /* CLD_EXITED */
        child[0] = (int32_t)child_pid;
        child[2] = (int32_t)child_code;     /* past si_uid */
    }

    f->rdi = sig;
    f->rsi = (uint64_t)&frame->info;
    f->rdx = (uint64_t)&frame->uc;
    f->rsp = at;
    f->rip = a->handler;
    if ((a->flags & SA_RESETHAND) != 0) {
        *a = (struct sig_action){ 0 };
    }
    return 0;
}

/* The handler has returned, through its restorer: the registers the frame
   holds are the program's again - what it was doing, or what the handler
   chose instead. */
/* Waiting, under a mask of its own, for a signal - which zsh does for every
   child it starts, SIGCHLD blocked until then. The signal goes to its
   handler before the call answers EINTR, and the handler's return puts the
   old mask back. */
static uint64_t sys_rt_sigsuspend(uint64_t mask, uint64_t size, uint64_t c) {
    const uint64_t always = 1ull << (9 - 1) | 1ull << (19 - 1);
    uint64_t old = blocked;

    (void)c;
    if (size != 8) {
        return ERR(EINVAL);
    }
    if (!user_range(mask, 8)) {
        return ERR(EFAULT);
    }
    blocked = *(const uint64_t *)mask & ~always;
    while (!interrupt_check()) {
        if (thread_only()) {
            __asm__ volatile("pause");
        } else {
            thread_yield();
        }
    }
    return signal_deliver(ERR(EINTR), old);
}

static uint64_t sys_rt_sigreturn(uint64_t a, uint64_t b, uint64_t c) {
    struct user_regs *f = user_frame;
    struct sig_frame *frame = (struct sig_frame *)(f->rsp - 8);   /* past the
                                       restorer the handler's ret took */
    const uint64_t *g = frame->uc.gregs;
    const uint64_t flags = 0xDD5;   /* CF, PF, AF, ZF, SF, DF and OF: all a
                                       program may set */
    (void)a;
    (void)b;
    (void)c;
    if (!user_range((uint64_t)frame, sizeof *frame)) {
        process_exit(SIGNALLED | SIGSEGV);  /* as Linux would */
    }
    f->r8 = g[G_R8];
    f->r9 = g[G_R9];
    f->r10 = g[G_R10];
    f->r12 = g[G_R12];
    f->r13 = g[G_R13];
    f->r14 = g[G_R14];
    f->r15 = g[G_R15];
    f->rdi = g[G_RDI];
    f->rsi = g[G_RSI];
    f->rbp = g[G_RBP];
    f->rbx = g[G_RBX];
    f->rdx = g[G_RDX];
    f->rsp = g[G_RSP];
    f->rip = g[G_RIP];
    f->rflags = (f->rflags & ~flags) | (g[G_EFLAGS] & flags);
    blocked = frame->uc.sigmask & ~(1ull << (9 - 1) | 1ull << (19 - 1));
    return g[G_RAX];
}

/* The same around the kernel working for a program: what it took is system
   time, less what went on waiting - other programs run then. */
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
uint64_t realtime_us(void) {
    static uint64_t offset;

    if (offset == 0) {
        offset = efi_epoch() * 1000000 - efi_uptime_us();
    }
    return offset + efi_uptime_us();
}

uint64_t realtime_ms(void) {
    return realtime_us() / 1000;
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
    if (mem_backed(h)) {
        return mem_io(h, buf, count, false);
    }
    if (h->start == CONSOLE_MARK) {
        uint64_t began = wait_began();      /* waiting on whoever is typing */
        uint64_t got = console_read((char *)buf, count);

        wait_ended(began);
        if (got == CONSOLE_SIGNAL) {
            interrupt_check();              /* ends it, or its handler is next */
            return ERR(EINTR);
        }
        return got;
    }
    if (h->start == WRITE_MARK) {
        /* Open for writing, and being read: where its sectors are has to be
           asked for, since a write may have moved the whole file. */
        struct fs_file file;

        if (fs_stat(write_name(h), &file) < 0) {
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
        /* An empty one waits for whatever its writer - another process, or
           another thread - puts in it, and is at its end once nothing can.
           With nothing else running at all, nothing ever will. */
        while (p != NULL && (h->size == PIPE_PAIR || h->size == PIPE_READ) && !readable(h)) {
            if ((h->offset & O_NONBLOCK) != 0) {
                return ERR(EAGAIN);
            }
            if (thread_only()) {
                break;
            }
            if (interrupt_check()) {
                return ERR(EINTR);
            }
            thread_yield();
        }
        if (p != NULL && h->size == PIPE_PAIR) {
            if (p->packets || p->nmsgs > 0) {
                struct iovec one = { buf, count };

                return pair_take(h, p, &one, 1, NULL, NULL, NULL);
            }
            return pipe_read(p, (char *)buf, count);
        }
        return p == NULL || (h->size != PIPE_READ && h->size != PIPE_FILE)
             ? ERR(EBADF) : pipe_read(p, (char *)buf, count);
    }
    if (h->start == SOCK_MARK) {
        return (h->offset & SOCK_UNIX) != 0 ? ERR(ENOTCONN)
             : net != NULL ? net->read(h, buf, count) : ERR(EBADF);
    }
    if (h->start == DEV_MARK) {
        if (h->folder == DEV_NULL) {
            return 0;               /* nothing in it, ever */
        }
        if (h->folder == DEV_FB) {
            return fb_copy(h, (void *)buf, count, false);
        }
        if (h->folder >= DEV_EVENT0) {
            /* Events, waited for unless the descriptor says not to. */
            uint64_t began = wait_began(), n;

            while ((n = input_read(h->folder - DEV_EVENT0, (void *)buf, count)) == 0) {
                if ((h->offset & O_NONBLOCK) != 0 || count < 24) {
                    wait_ended(began);
                    return ERR(count < 24 ? EINVAL : EAGAIN);
                }
                if (interrupt_check()) {
                    wait_ended(began);
                    return ERR(EINTR);
                }
                thread_yield();
            }
            wait_ended(began);
            return n;
        }
        if (h->folder == DEV_RANDOM) {
            return sys_getrandom(buf, count, 0);
        }
        memset((void *)buf, 0, (size_t)count);
        return count;
    }
    if (h->start == MOD_MARK) {
        return ops_of(h) != NULL ? ops_of(h)->read(h, buf, count) : ERR(EIO);
    }
    if (h->start >= FIRST_MARK) {
        return ERR(EBADF);          /* a folder, or a file being written */
    }

    return read_run(h, h->start, buf, count);
}

/* The lowest free descriptor, holding what was found - which is the one
   Linux hands out too, and what a program closing 1 and opening a file
   counts on. */
/* Room for descriptor need: the table doubled, up to PROGRAM_FILES. */
static bool files_grow(uint32_t need) {
    uint32_t room = file_room;

    while (room <= need && room < PROGRAM_FILES) {
        room *= 2;
    }
    room = room < PROGRAM_FILES ? room : PROGRAM_FILES;
    if (need >= room) {
        return false;
    }
    struct handle *bigger = mem_alloc(room * sizeof *bigger);

    if (bigger == NULL) {
        return false;
    }
    memset(bigger, 0, room * sizeof *bigger);
    memcpy(bigger, handles, file_room * sizeof *bigger);
    mem_free(proc->files_old);
    proc->files_old = handles;
    handles = bigger;
    file_room = room;
    return true;
}

uint64_t give_handle(struct handle h) {
    for (unsigned i = 0;; i++) {
        if (i == file_room && !files_grow(i)) {
            return ERR(EMFILE);
        }
        if (handles[i].used == 0) {
            h.used = 1;             /* close-on-exec is said separately */
            handles[i] = h;
            return i;
        }
    }
}

/* Opens a file, or the folder of that name if there is no such file - which
   is what getdents64 needs a descriptor for. "." is a folder like any other
   here, since the filesystem resolves it. */
uint32_t file_ino(const char *name, bool follow);

bool is_fifo(const struct fs_file *file) {
    return file->size == 0 && (file->start & FS_MODE) != 0 && (file->start & S_IFMT) == S_IFIFO;
}

/* A FIFO opened: the pipe it is, made by whoever opens it first. Read and
   write both is one end that reads what it writes, as a socketpair end
   would if its other end were itself. */
static uint64_t fifo_open(uint32_t number, uint64_t flags) {
    unsigned slot = 0, mode = flags & O_ACCMODE;
    struct pipe *p;
    uint64_t fd;

    while (slot < PIPES && !(pipes[slot].refs > 0 && pipes[slot].fifo == number)) {
        slot++;
    }
    if (slot == PIPES) {
        void *data;

        if ((slot = pipe_slot()) == PIPES || (data = mem_alloc(PIPE_FIRST)) == NULL) {
            return ERR(ENFILE);
        }
        pipes[slot] = (struct pipe){ .data = data, .size = PIPE_FIRST, .fifo = number };
    }
    p = &pipes[slot];
    struct handle h = {
        .start = PIPE_MARK, .folder = slot + 1,
        .size = mode == O_RDONLY ? PIPE_READ : mode == O_WRONLY ? PIPE_WRITE : PIPE_PAIR,
        .offset = (uint32_t)(flags & O_NONBLOCK) | (mode == O_RDWR ? (slot + 1) << 16 : 0),
    };

    p->refs += mode == O_RDWR ? 2 : 1;
    p->count += mode != O_RDONLY;
    fd = give_handle(h);
    if ((int64_t)fd < 0) {
        pipe_release(&h);
    }
    return fd;
}

/* ---- /proc/self/fd -----------------------------------------------------------
 *
 * Linux's magic links: /proc/self/fd/N - or /proc/<pid>/fd/N, for the
 * caller's own pid - names what descriptor N is open on. Read as a link it
 * says the path; opened or looked at, it is that file. A program that wants
 * the path a descriptor came from asks here, and systemd's libraries take
 * its absence for /proc not being mounted at all. */

static bool fd_path(uint64_t fd, char *out, size_t max) {
    struct handle *h = handle_of(fd);
    struct fs_file entry;
    const char *name = NULL;

    if (h == NULL || max < FS_NAME_LEN + 16) {
        return false;
    }
    switch (h->start) {
    case CONSOLE_MARK:
        name = "dev/tty1";
        break;
    case PROCDIR_MARK:
        name = h->folder == 0 ? "ctl" : h->folder == PROC_PIDS_FOLDER ? "proc" :
               h->folder == 2 ? "dev/input" : h->folder == 3 ? "dev/pts" : "dev";
        break;
    case DEV_MARK:
        for (unsigned i = 0; i < DEVICES && name == NULL; i++) {
            name = devices[i].which == h->folder ? devices[i].name + 1 : NULL;
        }
        break;
    case PIPE_MARK:
    case SOCK_MARK:
        ksprintf(out, "%s:[%u]", h->start == PIPE_MARK ? "pipe" : "socket", (uint64_t)h->folder);
        return true;
    case WRITE_MARK:
        name = write_name(h);
        break;
    case MEM_MARK:
        name = "memfd:shm (deleted)";
        break;
    default:
        if (h->start == FOLDER_MARK && h->folder == 0) {
            name = "";
        } else if ((h->start == FOLDER_MARK || h->start < FIRST_MARK) && h->folder != 0 &&
                   fs_file(h->folder - 1, &entry) == 0) {
            size_t n = strlen(entry.name);

            if (n > 0 && entry.name[n - 1] == '/') {
                entry.name[n - 1] = '\0';     /* a folder's trailing slash */
            }
            name = entry.name;
            out[0] = '/';
            strcpy(out + 1, name[0] == '/' ? name + 1 : name);
            return true;
        }
        break;
    }
    if (name == NULL) {
        return false;
    }
    out[0] = '/';
    strcpy(out + 1, name[0] == '/' ? name + 1 : name);
    return true;
}

/* Whether name is /proc/self/fd, or /proc/<own pid>/fd: the folder of the
   caller's descriptors, listed by number (opendir of it is how Chromium and
   many another program close what they did not mean to keep). */
bool proc_fd_folder(const char *name) {
    unsigned pid = 0;
    const char *p;

    if (name == NULL || memcmp(name, "/proc/", 6) != 0 || proc == NULL) {
        return false;
    }
    p = name + 6;
    if (memcmp(p, "self/", 5) == 0) {
        p += 5;
    } else {
        for (; *p >= '0' && *p <= '9'; p++) {
            pid = pid * 10 + (unsigned)(*p - '0');
        }
        if (*p++ != '/' || (int)pid != proc->pid) {
            return false;
        }
    }
    return strcmp(p, "fd") == 0 || strcmp(p, "fd/") == 0;
}

/* /proc/self/task (or /proc/<own pid>/task): a folder whose link count is
   its threads and two, as Linux's is - Chromium counts them by that. 0 for
   any other name. */
unsigned proc_task_links(const char *name) {
    char self[24];

    if (name == NULL || proc == NULL) {
        return 0;
    }
    ksprintf(self, "/proc/%u/", (unsigned)proc->pid);
    const char *p = memcmp(name, "/proc/self/", 11) == 0 ? name + 11
                  : memcmp(name, self, strlen(self)) == 0 ? name + strlen(self) : NULL;

    if (p == NULL || (strcmp(p, "task") != 0 && strcmp(p, "task/") != 0)) {
        return 0;
    }
    return thread_count(proc) + 2;
}

const char *proc_fd_target(const char *name, char *out, size_t max) {
    unsigned pid = 0, fd = 0;
    const char *p;

    if (name == NULL || memcmp(name, "/proc/", 6) != 0 || proc == NULL) {
        return NULL;
    }
    p = name + 6;
    if (memcmp(p, "self/", 5) == 0) {
        p += 5;
    } else {
        for (; *p >= '0' && *p <= '9'; p++) {
            pid = pid * 10 + (unsigned)(*p - '0');
        }
        if (*p++ != '/' || (int)pid != proc->pid) {
            return NULL;
        }
    }
    if (strcmp(p, "exe") == 0) {
        /* The program it runs: Firefox finds the folder it is installed in
           from here, and many another program its own files. */
        if (proc->exe[0] == '\0') {
            return NULL;
        }
        out[0] = '/';
        strcpy(out + 1, proc->exe[0] == '/' ? proc->exe + 1 : proc->exe);
        return out;
    }
    if (memcmp(p, "fd/", 3) != 0 || p[3] == '\0') {
        return NULL;
    }
    for (p += 3; *p >= '0' && *p <= '9'; p++) {
        fd = fd * 10 + (unsigned)(*p - '0');
    }
    return *p == '\0' && fd_path(fd, out, max) ? out : NULL;
}

static uint64_t open_name(const char *name, uint64_t flags) {
    char target[FS_NAME_LEN + 16];
    struct fs_file file;
    unsigned folder;
    enum dev which;

    if (name == NULL) {
        return ERR(EINVAL);
    }
    if (memcmp(name, "/proc/self/fd/", 14) == 0) {
        uint64_t n = 0;

        for (const char *d = name + 14; *d >= '0' && *d <= '9'; d++) {
            n = n * 10 + (uint64_t)(*d - '0');
        }
        struct handle *of = handle_of(n);

        if (of != NULL && (of->start == MEM_MARK || of->start == WRITE_MARK) && of->ofd != 0) {
            return mem_reopen(of);  /* the same memory, opened again */
        }
    }
    if (proc_fd_target(name, target, sizeof target) != NULL) {
        name = target;              /* opened, it is what it names */
    }
    /* Made only if it is not there: how a temporary file's name is claimed. */
    if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL) &&
        (fs_lstat(name, &file) == 0 || fs_folder_at(name, &folder) == 0 || dev_named(name) != 0)) {
        return ERR(EEXIST);
    }
    /* Asked not to go through a link at the end, and there is one there. */
    if ((flags & O_NOFOLLOW) != 0 && fs_lstat(name, &file) == 0 &&
        (file.size & FS_LINK) != 0) {
        return ERR(ELOOP);
    }
    const char *leaf;
    const struct file_ops *ops = ops_named(name, &leaf);

    if (ops != NULL) {
        return ops->open(leaf, flags);
    }
    if ((which = dev_named(name)) != 0) {
        if (which == DEV_TTY) {
            return give_handle((struct handle){ .start = CONSOLE_MARK });
        }
        if (which >= DEV_EVENT0) {
            if (!input_open(which - DEV_EVENT0)) {
                return ERR(ENOMEM);
            }
            graphics_take();
            if (which == DEV_EVENT0) {
                console_keys(false);    /* its keys, until it ends: an X server
                                           started away from the console never
                                           says so itself */
            }
        }
        struct vga_screen screen;

        vga_screen(&screen);
        /* An event device keeps O_NONBLOCK in its offset, as a pipe does;
           the screen's size is what it ends at. */
        return give_handle((struct handle){ .start = DEV_MARK, .folder = which,
            .offset = which >= DEV_EVENT0 ? (uint32_t)(flags & O_NONBLOCK) : 0,
            .size = which == DEV_FB ? screen.pitch * screen.height : 0 });
    }
    if (fs_stat(name, &file) == 0 && is_fifo(&file)) {
        return fifo_open(file_ino(name, true), flags);
    }
    if ((flags & O_ACCMODE) != O_RDONLY) {
        /* Open for writing. The name is kept, because that is what a write
           goes back to; what is in the file is kept too, unless the program
           asked for it to be thrown away. */
        struct handle h = { .start = WRITE_MARK };
        char scratch[FS_NAME_LEN];
        static unsigned tmpfiles;
        unsigned slot = OFDS;
        size_t length;
        bool empty;

        for (unsigned i = 0; i < OFDS && slot == OFDS; i++) {
            slot = ofds[i].refs == 0 ? i : slot;
        }
        if (slot == OFDS) {
            return ERR(ENFILE);
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
            ksprintf(scratch, "%s/.tmp%u", name, tmpfiles++ % 64);
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
        char *kept = mem_alloc(length + 1);

        if (kept == NULL) {
            return ERR(ENOMEM);
        }
        memcpy(kept, name, length + 1);
        h.writer = 1;
        h.size = file.size;
        h.offset = (flags & O_APPEND) != 0 ? file.size : 0;
        ofds[slot] = (struct ofd){ 1, h.offset, h.size, NULL, kept };
        h.ofd = slot + 1;

        uint64_t fd = give_handle(h);

        if ((int64_t)fd < 0) {
            ofd_drop(&h);
        }
        return fd;
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
    if (proc_fd_folder(name)) {
        return give_handle((struct handle){ .start = PROCDIR_MARK, .folder = PROC_FD_FOLDER });
    }
    if (strcmp(name, "/proc") == 0 || strcmp(name, "/proc/") == 0) {
        return give_handle((struct handle){ .start = PROCDIR_MARK, .folder = PROC_PIDS_FOLDER });
    }
    if (proc_folder(name) || dev_folder(name)) {
        return give_handle((struct handle){ .start = PROCDIR_MARK, .folder = dev_folder(name) });
    }
    if (fs_folder_at(name, &folder) == 0) {
        return give_handle((struct handle){ .start = FOLDER_MARK, .folder = folder });
    }
    if (fs_stat(name, &file) == 0) {
        unsigned index = 0;

        fs_entry(name, true, &index);
        return give_handle((struct handle){ .start = file.start, .size = file.size,
                                            .folder = index });
    }
    return ERR(ENOENT);
}

static uint64_t sys_open(uint64_t path, uint64_t flags, uint64_t mode) {
    (void)mode;
    return with_cloexec(open_name(user_string(path), flags), (flags & O_CLOEXEC) != 0);
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

const char *at_path(uint64_t dirfd, const char *name, char *out, size_t max) {
    struct handle *h;
    struct fs_file folder;
    size_t n;

    if (name == NULL || name[0] == '/' || (int32_t)dirfd == AT_FDCWD) {
        return name;
    }
    h = handle_of(dirfd);
    if (h != NULL && h->start == PROCDIR_MARK) {
        /* /proc or /dev opened as a folder: a name relative to it is under it
           - how Chromium looks at self/task through a descriptor of /proc. */
        static const char *const bases[] = { "/proc/", "/dev/", "/dev/input/", "/dev/pts/",
                                             "/proc/self/fd/" };
        const char *base = h->folder < sizeof bases / sizeof bases[0] ? bases[h->folder] : "/proc/";

        if (strlen(base) + strlen(name) + 1 > max) {
            return NULL;
        }
        strcpy(out, base);
        strcpy(out + strlen(base), name);
        return out;
    }
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

    return with_cloexec(open_name(at_path(dirfd, user_string(path), joined, sizeof joined), flags),
                        (flags & O_CLOEXEC) != 0);
}

static uint64_t sys_close(uint64_t fd, uint64_t b, uint64_t c) {
    struct handle *h = handle_of(fd);

    (void)b;
    (void)c;
    if (h == NULL) {
        return ERR(EBADF);
    }
    h->used = 0;
    ofd_drop(h);
    if (h->start == PIPE_MARK) {
        pipe_release(h);
        return 0;
    }
    if (h->start == SOCK_MARK) {
        sock_drop(h);
        return 0;
    }
    if (h->start == MOD_MARK) {
        if (ops_of(h) != NULL) {
            ops_of(h)->drop(h);
        }
        return 0;
    }
    return 0;                       /* a written file's name goes with its last descriptor (ofd_drop) */
}

/* A second descriptor for the same open file. Everything a handle holds is
   copied, where Linux would share it - the two then move through a file
   independently, which is the one thing a program doing this rarely
   notices, since it is redirecting rather than reading. */
static uint64_t dup_to(uint64_t fd, uint64_t to) {
    struct handle *h = handle_of(fd);

    to = (uint32_t)to;
    if (h == NULL || to >= PROGRAM_FILES) {
        return ERR(EBADF);
    }
    if (to >= file_room) {
        if (!files_grow((uint32_t)to)) {
            return ERR(EMFILE);
        }
        h = handle_of(fd);          /* where it is now */
    }
    if (to != fd) {
        if (handles[to].used != 0) {
            sys_close(to, 0, 0);
        }
        handles[to] = *h;
        handles[to].used = 1;       /* a duplicate is not closed on exec */
        pipe_hold(h);
        ofd_hold(h);
        if (h->start == SOCK_MARK) {
            sock_hold(h);
        }
        if (ops_of(h) != NULL) {
            ops_of(h)->hold(h);
        }
    }
    return to;
}

static uint64_t sys_dup(uint64_t fd, uint64_t b, uint64_t c) {
    struct handle *h = handle_of(fd);
    uint64_t made;

    (void)b;
    (void)c;
    if (h == NULL) {
        return ERR(EBADF);
    }
    made = give_handle(*h);
    if ((int64_t)made >= 0) {
        pipe_hold(h);
        ofd_hold(h);
    }
    if ((int64_t)made >= 0 && h->start == SOCK_MARK) {
        sock_hold(h);
    }
    if ((int64_t)made >= 0 && ops_of(h) != NULL) {
        ops_of(h)->hold(h);
    }
    return made;
}

static uint64_t sys_dup2(uint64_t fd, uint64_t to, uint64_t c) {
    (void)c;
    return dup_to(fd, to);
}

/* Every descriptor from first to last closed - or, with CLOSE_RANGE_CLOEXEC,
   marked to close at the next execve: what a C library's posix_spawn does to
   keep its parent's descriptors out of the child. */
#define CLOSE_RANGE_CLOEXEC 4

static uint64_t sys_close_range(uint64_t first, uint64_t last, uint64_t flags) {
    first = (uint32_t)first;
    last = (uint32_t)last;
    if (first > last) {
        return ERR(EINVAL);
    }
    for (uint64_t fd = first; fd <= last && fd < file_room; fd++) {
        if (handle_of(fd) == NULL) {
            continue;
        }
        if ((flags & CLOSE_RANGE_CLOEXEC) != 0) {
            fd_cloexec(fd, true);
        } else {
            sys_close(fd, 0, 0);
        }
    }
    return 0;
}

static uint64_t sys_dup3(uint64_t fd, uint64_t to, uint64_t flags) {
    if ((uint32_t)fd == (uint32_t)to) {
        return ERR(EINVAL);
    }
    return with_cloexec(dup_to(fd, to), (flags & O_CLOEXEC) != 0);
}

static uint64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence) {
    struct handle *h = handle_of(fd);

    if (h == NULL || whence > 2) {
        return ERR(h == NULL ? EBADF : EINVAL);
    }
    /* A /proc file is text made as it was opened, read like a pipe - but
       it can be gone back over, and gone back to its start it is made
       again, as top rereads /proc/stat. */
    struct pipe *made = h->start == PIPE_MARK && h->size == PIPE_FILE ? pipe_of(h) : NULL;

    if (made != NULL && made->made_from != NULL) {
        int64_t at = whence == 0 ? 0 : whence == 1 ? (int64_t)made->read_at : (int64_t)made->len;

        at += (int64_t)offset;
        if (at < 0) {
            return ERR(EINVAL);
        }
        if (at == 0) {
            size_t len = made->made_from[0] == '/'
                       ? proc_linux(made->made_from, made->data, PIPE_TEXT)
                       : net != NULL ? net->proc(made->made_from, made->data, PIPE_TEXT) : 0;

            made->len = (uint32_t)(len < PIPE_TEXT ? len : PIPE_TEXT);
        }
        made->read_at = (uint32_t)(at < (int64_t)made->len ? at : (int64_t)made->len);
        return (uint64_t)at;
    }
    /* The terminal, a pipe and a socket are streams, as on Linux - and a
       pipe's offset holds its flags, not a place in it. */
    if (h->start == CONSOLE_MARK || h->start == PIPE_MARK || h->start == SOCK_MARK ||
        h->start == MOD_MARK || (h->start == DEV_MARK && h->folder >= DEV_EVENT0)) {
        return ERR(ESPIPE);
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
    if (h == NULL) {
        return ERR(EBADF);
    }
    if (mem_backed(h)) {
        h->size = (uint32_t)length; /* its pages come when touched */
        return 0;
    }
    if (h->start != WRITE_MARK) {
        return ERR(EINVAL);         /* not open for writing */
    }
    const char *name = write_name(h);
    struct fs_file there;

    if (fs_stat(name, &there) != 0) {
        /* Deleted while open, as a temporary file used for memory is: it
           has no disk to change, only the size its descriptors see. */
        h->size = (uint32_t)length;
        return 0;
    }
    if (length == 0) {
        if (fs_write(name, NULL, 0) < 0) {
            return ERR(EIO);
        }
    } else if (length > h->size) {
        /* Longer: what is past the old end reads as zeroes. */
        if (fs_write_at(name, (uint32_t)length - 1, "", 1) < 0) {
            return ERR(EIO);
        }
    } else if (length < h->size) {
        /* Shorter: what stays, written again on its own. */
        struct fs_file file;
        char *kept = mem_alloc(length);

        if (kept == NULL) {
            return ERR(ENOMEM);
        }
        if (fs_stat(name, &file) != 0 || read_at(&file, 0, kept, length) < 0 ||
            fs_write(name, kept, length) < 0) {
            mem_free(kept);
            return ERR(EIO);
        }
        mem_free(kept);
    }
    h->size = (uint32_t)length;      /* the position stays where it was, as on Linux */
    return 0;
}

/* /proc/self/maps, as far as a program reads it: where its heap and its
   stack are. glibc finds the main thread's stack here for
   pthread_getattr_np, and Firefox will not start without that. */
void process_maps(char *out, size_t max) {
    out[0] = '\0';
    if (proc == NULL || vm_base() == 0 || max < 200) {
        return;
    }
    uint64_t top = vm_base() + USER_STACK;

    ksprintf(out, "%x-%x rw-p 00000000 00:00 0                          [heap]\n"
                  "%x-%x rw-p 00000000 00:00 0                          [stack]\n",
             vm_base() + USER_BRK, program_break > vm_base() + USER_BRK ? program_break : vm_base() + USER_BRK + PAGE_SIZE,
             top - USER_STACK_BYTES, top);
}

struct creds *process_creds(void) {
    static struct creds kernel;     /* the kernel's own programs: root */

    return proc != NULL ? &proc->cred : &kernel;
}

static uint64_t sys_getuid(uint64_t a, uint64_t b, uint64_t c) {
    (void)a; (void)b; (void)c;
    return process_creds()->uid;
}

static uint64_t sys_geteuid(uint64_t a, uint64_t b, uint64_t c) {
    (void)a; (void)b; (void)c;
    return process_creds()->euid;
}

static uint64_t sys_getgid(uint64_t a, uint64_t b, uint64_t c) {
    (void)a; (void)b; (void)c;
    return process_creds()->gid;
}

static uint64_t sys_getegid(uint64_t a, uint64_t b, uint64_t c) {
    (void)a; (void)b; (void)c;
    return process_creds()->egid;
}

uint64_t fd_truncate(uint64_t fd, uint64_t length) {
    return sys_ftruncate(fd, length, 0);
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

/* A program's working directory - a shell's cd. Each process has its own:
   the filesystem's is swapped as the processes take turns. */
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
 * is a terminal. Each is answered honestly for a machine with one screen
 * and memory that is already readable, writable and executable. */

#define ARCH_SET_FS 0x1002
#define MSR_FS_BASE 0xC0000100


/* Where the heap starts, and where mmap starts handing memory out: in the
   program's own region where it has one, and otherwise sharing the fixed
   window with the program, the heap from the top of it and mmap from the
   far end growing down. */
static void program_memory_start(void) {
    program_break = vm_base() + USER_BRK;
    program_map = vm_base() + USER_MMAP;
    program_map_high = vm_base() + USER_HIGH;
}

static uint64_t sys_brk(uint64_t addr, uint64_t b, uint64_t c) {
    (void)b;
    (void)c;
    /* Linux answers a request it cannot meet with the break unchanged. */
    if (addr >= program_break && fits(addr, 0) &&
        addr < program_break + 0x10000000 && (vm_base() == 0 || addr <= vm_base() + USER_HIGH)) {
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

#define FILL_PAGES 4        /* pages read around the one that faulted */

/* Forgets whatever was promised for addr .. addr + size, which is what a
   mapping laid over an older one means. A hole in the middle of one leaves
   the part before it, since that is the only part a loader ever goes back
   to. */
static void map_trim(uint64_t at, uint64_t size) {
    uint64_t end = at + size;

    for (unsigned i = 0; i < map_room; i++) {
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

/* Room for twice as many promises, a page to start with: whole pages, since
   that is what an allocation costs anyway. */
static bool map_grow(void) {
    size_t pages = map_room == 0 ? 1 : (map_room * sizeof(struct mapping) + 16 + PAGE_SIZE - 1) / PAGE_SIZE * 2;
    unsigned room = (unsigned)((pages * PAGE_SIZE - 16) / sizeof(struct mapping));
    struct mapping *bigger = mem_alloc(room * sizeof(struct mapping));

    if (bigger == NULL) {
        return false;
    }
    memset(bigger, 0, room * sizeof(struct mapping));
    if (mappings != NULL) {
        memcpy(bigger, mappings, map_room * sizeof(struct mapping));
        mem_free(mappings);
    }
    mappings = bigger;
    map_room = room;
    return true;
}

static bool map_record(uint64_t at, uint64_t size, uint32_t start, uint32_t bytes,
                       uint64_t offset) {
    map_trim(at, size);
    for (unsigned i = 0;; i++) {
        if (i == map_room && !map_grow()) {
            return false;       /* no room to promise: read it now instead */
        }
        if (mappings[i].at == mappings[i].end) {
            mappings[i] = (struct mapping){ .at = at, .end = at + size,
                                            .offset = offset,
                                            .start = start, .size = bytes };
            return true;
        }
    }
}

/* Makes good on the promises covering the page that faulted: a chunk of
   pages around it, with every file that has something to say about them read
   into it.
 *
 * A promise need not start or end on a page boundary - an ELF segment rarely
 * does - so the page that faulted is matched against the bytes a promise
 * covers rather than against whole pages, and a page two promises share gets
 * both of their stretches. */
/* A whole page of one file, page-aligned in it, that no other mapping has a
   say in: the same in every process, so the one every process shares
   (share.c). False if the page is not that, or there is no memory for it. */
static bool map_share(uint64_t page) {
    const struct mapping *found = NULL;

    for (unsigned i = 0; i < map_room; i++) {
        const struct mapping *m = &mappings[i];

        if (m->at == m->end || m->at >= page + PAGE_SIZE || m->end <= page) {
            continue;
        }
        if (found != NULL || m->at > page || m->end < page + PAGE_SIZE) {
            return false;           /* two have a say, or one only in part */
        }
        found = m;
    }
    if (found == NULL) {
        return false;
    }
    uint64_t from = found->offset + (page - found->at);

    if (from % PAGE_SIZE != 0 || from >= found->size) {
        return false;
    }
    uint32_t index = (uint32_t)(from / PAGE_SIZE);
    uint64_t common = shared_find(found->start, index);

    if (common == 0) {
        struct fs_file file = { .start = found->start, .size = found->size };
        uint64_t count = found->size - from < PAGE_SIZE ? found->size - from : PAGE_SIZE;

        if ((common = mem_pages(1)) == 0) {
            return false;
        }
        memset((char *)common + count, 0, PAGE_SIZE - count);
        if (read_at(&file, from, (void *)common, count) < 0 ||
            !shared_add(found->start, index, common)) {
            mem_pages_free(common, 1);
            return false;
        }
    }
    if (!vm_map_shared(page, common, false)) {
        shared_drop(common);
        return false;
    }
    return true;
}

static bool map_fill(uint64_t addr) {
    uint64_t page = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t chunk = FILL_PAGES * PAGE_SIZE;
    uint64_t first = 0, last = 0;
    bool found = false;

    /* The chunk to fill: the one this page falls in, counting from the start
       of the first promise that covers the page. */
    for (unsigned i = 0; i < map_room && !found; i++) {
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
    /* A library's page is the one every process has, where it can be - the
       rest of the chunk with it, as the read-ahead. */
    if (map_share(page)) {
        for (uint64_t at = first; at < last; at += PAGE_SIZE) {
            if (at != page && !vm_mapped(at)) {
                map_share(at);
            }
        }
        return true;
    }
    /* Every page of the chunk new, or else only the one that faulted: a page
       left mapped but never read into would be zeroes where the file's bytes
       belong, and nothing would ever fill it after. */
    uint32_t fresh = 0;
    bool whole = true;

    for (uint64_t at = first; at < last; at += PAGE_SIZE) {
        if (!vm_mapped(at) && vm_fault(at)) {
            fresh |= 1u << ((at - first) / PAGE_SIZE);
        } else {
            whole = false;
        }
    }
    if (!whole) {
        for (uint64_t at = first; at < last; at += PAGE_SIZE) {
            if (at != page && (fresh & 1u << ((at - first) / PAGE_SIZE)) != 0) {
                vm_release(at, PAGE_SIZE);
            }
        }
        if ((fresh & 1u << ((page - first) / PAGE_SIZE)) == 0) {
            return false;
        }
        first = page;
        last = page + PAGE_SIZE;
    }
    /* Whatever the pages are owed. Anything no promise covers, and anything
       past the end of a file, is the zeroes the pages arrived as. */
    for (unsigned i = 0; i < map_room; i++) {
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

#define MAP_SHARED 0x01

/* A file open for writing, mapped MAP_SHARED: its pages (struct mobj),
   mapped writable here and counted once more, all at once - what is
   shared this way is a few hundred kilobytes of memory, not a file read. */
static uint64_t map_shared_file(struct handle *h, uint64_t at, uint64_t size, uint64_t offset) {
    struct ofd *o = h->ofd != 0 ? &ofds[h->ofd - 1] : NULL;
    uint32_t first = (uint32_t)(offset / PAGE_SIZE), count = (uint32_t)(size / PAGE_SIZE);

    if (o == NULL || offset % PAGE_SIZE != 0) {
        return ERR(o == NULL ? ENFILE : EINVAL);
    }
    if (o->mem == NULL && (o->mem = mobj_new()) == NULL) {
        return ERR(ENOMEM);
    }
    map_trim(at, size);
    vm_release(at, size);           /* whatever was there before */
    for (uint32_t i = 0; i < count; i++) {
        bool fresh = o->mem->room <= first + i || o->mem->frame[first + i] == 0;
        uint64_t page = mobj_page(o->mem, first + i, true);
        uint64_t from = (uint64_t)(first + i) * PAGE_SIZE;

        if (page == 0) {
            return ERR(ENOMEM);
        }
        /* A file on the disk: what it already holds, where it holds anything. */
        if (fresh && h->start == WRITE_MARK && from < h->size) {
            struct fs_file file;
            uint64_t n = h->size - from < PAGE_SIZE ? h->size - from : PAGE_SIZE;

            if (fs_stat(write_name(h), &file) == 0 && from < file.size) {
                read_at(&file, from, (void *)page, n < file.size - from ? n : file.size - from);
            }
        }
        shared_hold(page);
        if (!vm_map_shared(at + (uint64_t)i * PAGE_SIZE, page, true)) {
            shared_drop(page);
            return ERR(ENOMEM);
        }
    }
    return at;
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
        /* Up to the heap, and no further: a reservation too big for what is
           left fails, as V8's first try at a terabyte does, and leaves room
           for the smaller one it tries next. */
        if (size + PAGE_SIZE <= vm_base() + USER_BRK - program_map) {
            at = program_map;
            program_map += size + PAGE_SIZE;     /* a gap, so a fixed map nearby is safe */
        } else if (size + PAGE_SIZE <= vm_end() - program_map_high) {
            at = program_map_high;
            program_map_high += size + PAGE_SIZE;
        } else {
            return ERR(ENOMEM);
        }
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

    if (h != NULL && h->start == DEV_MARK && h->folder == DEV_FB) {
        /* The screen itself, not a copy: what a program draws in it is seen. */
        struct vga_screen s;

        vga_screen(&s);
        if (offset + size > (((uint64_t)s.pitch * s.height + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1))) {
            return ERR(EINVAL);
        }
        map_trim(at, size);
        if (!vm_map_device(at, s.base + offset, size)) {
            return ERR(ENOMEM);
        }
        /* Mapping the screen is taking it: the console stops drawing over
           whatever the program draws, until the program ends. */
        graphics_take();
        vga_lend(true);
        return at;
    }
    if (h != NULL && (h->start == WRITE_MARK || h->start == MEM_MARK) && (flags & MAP_SHARED) != 0) {
        return map_shared_file(h, at, size, offset);
    }
    if (h != NULL && mem_backed(h)) {
        /* Private: a copy of what it holds now. */
        uint32_t was = h->offset;

        if (!claim(at, size)) {
            return ERR(ENOMEM);
        }
        memset((void *)at, 0, size);
        h->offset = (uint32_t)offset;
        mem_io(h, at, size, false);
        h->offset = was;
        return at;
    }
    if (h == NULL || h->start >= FIRST_MARK) {
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

/* What the filesystem said, as a program's libc expects to hear it. Getting
   this wrong is not cosmetic: `mkdir -p a/b` creates the parent only when it
   is told the parent is missing, and anything else it reports and stops. */
uint64_t fs_errno(int err) {
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

/* name is about to go: whatever has it open for writing keeps its bytes,
   as memory - what it held copied in, the name forgotten (mem_backed). */
static void unlinked_open(const char *name) {
    struct fs_file file;
    bool there = fs_stat(name, &file) == 0;
    struct mobj *kept = NULL;

    for (unsigned i = 0; i < OFDS; i++) {
        struct ofd *o = &ofds[i];

        if (o->refs == 0 || o->name == NULL ||
            (strcmp(o->name, name) != 0 && strcmp(o->name + (o->name[0] == '/'), name + (name[0] == '/')) != 0)) {
            continue;
        }
        if (o->mem == NULL && (o->mem = mobj_new()) == NULL) {
            continue;               /* no memory: it goes with its name */
        }
        for (uint64_t at = 0; there && at < file.size && at < o->size; at += PAGE_SIZE) {
            bool fresh = o->mem->room <= at / PAGE_SIZE || o->mem->frame[at / PAGE_SIZE] == 0;
            uint64_t page = mobj_page(o->mem, (uint32_t)(at / PAGE_SIZE), true);
            uint64_t n = file.size - at < PAGE_SIZE ? file.size - at : PAGE_SIZE;

            if (page != 0 && fresh) {
                read_at(&file, at, (void *)page, n);
            }
        }
        mem_free(o->name);
        o->name = NULL;
        kept = kept != NULL ? kept : o->mem;
    }
    /* Opened for reading too, by its name before it went - Chromium's
       read-only view of memory it shares, which its renderers map: those
       become views of the same memory. */
    for (unsigned i = 0; kept != NULL && there && file.start != 0 && i < PROCESSES; i++) {
        struct process *p = procs[i];

        for (uint32_t fd = 0; p != NULL && !p->zombie && fd < p->files_room; fd++) {
            struct handle *h = &p->files[fd];

            if (h->used == 0 || h->start != file.start || h->ofd == 0) {
                continue;
            }
            struct ofd *o = &ofds[h->ofd - 1];

            if (o->mem == NULL) {
                kept->refs++;
                o->mem = kept;
                o->size = file.size;
            }
            h->start = MEM_MARK;
            h->size = file.size;
        }
    }
}

static uint64_t sys_unlink(uint64_t path, uint64_t b, uint64_t c) {
    const char *name = user_string(path);

    (void)b;
    (void)c;
    if (name == NULL) {
        return ERR(EFAULT);
    }
    unlinked_open(name);
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
    unlinked_open(name);
    return fs_errno(fs_remove(name));
}

/* A folder made with other than the usual 0755, once umask is taken off,
   keeps its mode: fish will not use a runtime folder anyone else can
   read. */
static uint64_t make_folder(const char *name, uint64_t mode) {
    int err;

    if (name == NULL) {
        return ERR(EFAULT);
    }
    mode &= ~(uint64_t)(process_creds()->umask_off ^ 022u) & 07777;
    err = fs_mkdir(name);
    if (err == 0 && mode != 0755) {
        err = fs_set_mode(name, (unsigned)(S_IFDIR | mode));
    }
    return fs_errno(err);
}

static uint64_t sys_mkdir(uint64_t path, uint64_t mode, uint64_t c) {
    (void)c;
    return make_folder(user_string(path), mode);
}

static uint64_t sys_mkdirat(uint64_t dirfd, uint64_t path, uint64_t mode) {
    char joined[FS_NAME_LEN];

    return make_folder(at_path(dirfd, user_string(path), joined, sizeof joined), mode);
}

static uint64_t sys_getpid(uint64_t a, uint64_t b, uint64_t c) {
    (void)a;
    (void)b;
    (void)c;
    return (uint64_t)proc->pid;
}

static uint64_t sys_getppid(uint64_t a, uint64_t b, uint64_t c) {
    (void)a;
    (void)b;
    (void)c;
    return (uint64_t)proc->ppid;
}

/* A process's group, by pid - 0 for the caller's; getpgrp is the caller's,
   getsid its session's. */
static uint64_t sys_getpgid(uint64_t pid, uint64_t b, uint64_t c) {
    struct process *p = (int)pid == 0 ? proc : proc_find((int)pid);

    (void)b;
    (void)c;
    return p != NULL ? (uint64_t)p->pgid : ERR(ESRCH);
}

static uint64_t sys_getsid(uint64_t pid, uint64_t b, uint64_t c) {
    struct process *p = (int)pid == 0 ? proc : proc_find((int)pid);

    (void)b;
    (void)c;
    return p != NULL ? (uint64_t)p->sid : ERR(ESRCH);
}

/* Moves pid (0: the caller) into group pgid (0: one of its own) - how a
   shell gives each job a group the keyboard can be handed to. */
static uint64_t sys_setpgid(uint64_t pid, uint64_t pgid, uint64_t c) {
    struct process *p = (int)pid == 0 ? proc : proc_find((int)pid);

    (void)c;
    if (p == NULL || p->zombie) {
        return ERR(ESRCH);
    }
    p->pgid = (int)pgid == 0 ? p->pid : (int)pgid;
    return 0;
}

static uint64_t sys_setsid(uint64_t a, uint64_t b, uint64_t c) {
    (void)a;
    (void)b;
    (void)c;
    proc->pgid = proc->sid = proc->pid;
    return (uint64_t)proc->pid;
}

/* The group the keyboard's signals go to (TIOCGPGRP, TIOCSPGRP). */
int tty_foreground(void) {
    return fg_pgrp != 0 ? fg_pgrp : (proc != NULL ? proc->pgid : 0);
}

void tty_set_foreground(int pgid) {
    fg_pgrp = pgid;
}

/* Only ARCH_SET_FS, which is how a libc points at its thread-local data.
   Even a program with one thread has to have it set, or the first access to
   a thread variable reads address zero; each thread keeps its own. */
/* FS and GS bases: FS a thread's TLS, GS whatever the program makes of it -
   Firefox's sandboxed WebAssembly libraries reach their memory through it. */
#define ARCH_SET_GS 0x1001
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004
#define MSR_GS_BASE 0xC0000101

static uint64_t sys_arch_prctl(uint64_t code, uint64_t addr, uint64_t c) {
    (void)c;
    switch (code) {
    case ARCH_SET_FS:
    case ARCH_SET_GS:
        if (!user_range(addr, 0)) {
            return ERR(EPERM);
        }
        wrmsr(code == ARCH_SET_FS ? MSR_FS_BASE : MSR_GS_BASE, addr);
        return 0;
    case ARCH_GET_FS:
    case ARCH_GET_GS:
        if (!user_range(addr, 8)) {
            return ERR(EFAULT);
        }
        *(uint64_t *)addr = rdmsr(code == ARCH_GET_FS ? MSR_FS_BASE : MSR_GS_BASE);
        return 0;
    }
    return ERR(EINVAL);
}

/* A read from a given place that leaves the file where it was. */
static uint64_t sys_pread64(uint64_t fd, uint64_t buf, uint64_t count) {
    struct handle *h = handle_of(fd);
    uint64_t offset = arg[3];
    uint32_t was;
    uint64_t got;

    if (h == NULL || (h->start >= FIRST_MARK && h->start != WRITE_MARK && h->start != MEM_MARK)) {
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

uint64_t realtime_ms(void);

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
        if (interrupt_check()) {
            thread_sleep_on(0);
            return ERR(EINTR);
        }
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

/* The priority-inheritance locks (FUTEX_LOCK_PI and kin): the word is its
   owner's thread id, with FUTEX_WAITERS set while anyone waits. Nothing
   here has priorities to lend, so this is a lock and no more - which is all
   a mutex made with PTHREAD_PRIO_INHERIT needs. Firefox's are. */
#define FUTEX_LOCK_PI    6
#define FUTEX_UNLOCK_PI  7
#define FUTEX_TRYLOCK_PI 8
#define FUTEX_LOCK_PI2   13
#define FUTEX_WAITERS    0x80000000u
#define FUTEX_TID_MASK   0x3FFFFFFFu

static uint64_t futex_lock_pi(uint64_t address, bool wait, uint64_t timeout) {
    volatile uint32_t *word = (volatile uint32_t *)address;
    uint32_t me = (uint32_t)thread_id();
    uint64_t until = timeout != 0 && user_range(timeout, 16) ? efi_uptime_ms() + timespec_ms(timeout) : 0;

    for (;;) {
        uint32_t v = *word;

        if ((v & FUTEX_TID_MASK) == 0) {
            *word = me | (v & FUTEX_WAITERS);   /* free: the caller's */
            return 0;
        }
        if ((v & FUTEX_TID_MASK) == me) {
            return ERR(EDEADLK);
        }
        if (!wait) {
            return ERR(EAGAIN);
        }
        *word = v | FUTEX_WAITERS;          /* the owner unlocks through here */
        if (until != 0 && efi_uptime_ms() >= until) {
            return ERR(ETIMEDOUT);
        }
        if (thread_only()) {
            return ERR(EDEADLK);            /* nobody could ever let go of it */
        }
        thread_yield();
    }
}

static uint64_t futex_unlock_pi(uint64_t address) {
    volatile uint32_t *word = (volatile uint32_t *)address;

    if ((*word & FUTEX_TID_MASK) != (uint32_t)thread_id()) {
        return ERR(EPERM);
    }
    *word = 0;                              /* a waiter takes it on its next look */
    return 0;
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
    case FUTEX_LOCK_PI:
    case FUTEX_LOCK_PI2:
        return futex_lock_pi(address, true, timeout);
    case FUTEX_TRYLOCK_PI:
        return futex_lock_pi(address, false, 0);
    case FUTEX_UNLOCK_PI:
        return futex_unlock_pi(address);
    }
    return ERR(ENOSYS);
}


/* The caller's group - what a shell compares with the terminal's before it
   decides it may run jobs. */
static uint64_t sys_getpgrp(uint64_t a, uint64_t b, uint64_t c) {
    (void)a;
    (void)b;
    (void)c;
    return (uint64_t)proc->pgid;
}

/* The file mode a program's own files are trimmed by: kept, since a folder
   keeps its mode - mkdir -m 1777 clears it first to make /tmp/.X11-unix. */
static uint64_t sys_umask(uint64_t mask, uint64_t b, uint64_t c) {
    struct creds *cred = process_creds();
    uint64_t old = cred->umask_off ^ 022u;

    (void)b;
    (void)c;
    cred->umask_off = (uint16_t)((mask & 0777) ^ 022u);
    return old;
}


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
#define F_GETLK         5
#define F_SETLK         6
#define F_SETLKW        7
#define F_OFD_GETLK     36
#define F_OFD_SETLK     37
#define F_OFD_SETLKW    38
#define F_UNLCK         2
#define F_DUPFD_CLOEXEC 1030
#define F_ADD_SEALS     1033
#define F_GET_SEALS     1034

static uint64_t sys_fcntl(uint64_t fd, uint64_t command, uint64_t c) {
    struct handle *h = handle_of(fd);

    (void)c;
    if (h == NULL) {
        return ERR(EBADF);
    }
    switch (command) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        return with_cloexec(sys_dup(fd, 0, 0), command == F_DUPFD_CLOEXEC);
    case F_GETFL:
        /* A file open for writing can be read back too, so that is what it
           says: a libc that asked for "w+" and is told write-only gives up
           on the file it has just been handed. */
        if (h->start == SOCK_MARK || h->start == PIPE_MARK) {
            return (h->start == SOCK_MARK || h->size == PIPE_EVENT || h->size == PIPE_PAIR ? O_RDWR :
                    h->size == PIPE_WRITE ? O_WRONLY : O_RDONLY) | (h->offset & O_NONBLOCK);
        }
        if (h->start == DEV_MARK) {
            return O_RDWR | (h->folder >= DEV_EVENT0 ? h->offset & O_NONBLOCK : 0);
        }
        if (h->start == MOD_MARK) {
            return O_RDWR | (h->offset & O_NONBLOCK);
        }
        return h->start == CONSOLE_MARK || h->start == WRITE_MARK ? O_RDWR : O_RDONLY;
    case F_SETFL:
        if (h->start == SOCK_MARK || h->start == PIPE_MARK || h->start == MOD_MARK ||
            (h->start == DEV_MARK && h->folder >= DEV_EVENT0)) {
            h->offset = (h->offset & ~O_NONBLOCK) | (arg[2] & O_NONBLOCK);
        }
        return 0;
    case F_GETFD:
        return (h->used & HANDLE_CLOEXEC) != 0 ? 1 : 0;     /* FD_CLOEXEC */
    case F_SETFD:
        fd_cloexec(fd, (arg[2] & 1) != 0);
        return 0;
    case F_GETLK:
    case F_OFD_GETLK:
        /* Record locks: granted, every one. Programs take them against each
           other - systemd-sysusers on /etc/passwd, dpkg on its database -
           and here nothing ever holds one another wants, so a lock asked
           about is always free. */
        if (!user_range(arg[2], 2)) {
            return ERR(EFAULT);
        }
        *(uint16_t *)arg[2] = F_UNLCK;
        return 0;
    case F_SETLK:
    case F_SETLKW:
    case F_OFD_SETLK:
    case F_OFD_SETLKW:
    case F_ADD_SEALS:               /* sealed: nothing here would change it anyway */
        return 0;
    case F_GET_SEALS:
        return 0;
    }
    return ERR(EINVAL);
}

static uint64_t sys_readv(uint64_t fd, uint64_t vectors, uint64_t count);

static uint64_t sys_writev(uint64_t fd, uint64_t vectors, uint64_t count) {
    uint64_t written = 0;

    if (!user_range(vectors, count * sizeof(struct iovec))) {
        return ERR(EFAULT);
    }
    for (uint64_t i = 0; i < count; i++) {
        const struct iovec *v = (const struct iovec *)vectors + i;

        if (v->length == 0) {
            continue;
        }
        uint64_t n = sys_write(fd, v->base, v->length);

        /* An error after something was written is that much written; and
           a write that took less than it was given is where it stops. */
        if ((int64_t)n < 0) {
            return written > 0 ? written : n;
        }
        written += n;
        if (n < v->length) {
            break;
        }
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
    if (h != NULL && mem_backed(h)) {
        uint32_t was = h->offset;
        uint64_t done;

        h->offset = (uint32_t)offset;
        done = mem_io(h, text, count, true);
        h->offset = was;
        return done;
    }
    if (h == NULL || h->start != WRITE_MARK) {
        return ERR(EBADF);
    }
    if (fs_write_at(write_name(h), (uint32_t)offset,
                    (const void *)text, count) < 0) {
        return ERR(EIO);
    }
    if (offset + count > h->size) {
        h->size = (uint32_t)(offset + count);
    }
    return count;
}

/* Not cryptographic, but never the same twice: one state for the machine,
   stirred with the cycle counter on every call. Seeded from the clock alone,
   every program started in the same second drew the same bytes - and so the
   same "unique" temporary file name. */
static uint64_t sys_getrandom(uint64_t buf, uint64_t length, uint64_t flags) {
    static uint64_t state;
    uint32_t low, high;

    (void)flags;
    if (!user_range(buf, length)) {
        return ERR(EFAULT);
    }
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    state ^= ((uint64_t)high << 32 | low) + efi_seconds();
    for (uint64_t i = 0; i < length; i++) {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);    /* splitmix64 */

        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        ((uint8_t *)buf)[i] = (uint8_t)(z ^ (z >> 31));
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

/* A file is numbered the same way, by its table entry: unlike its sectors,
   which move when it grows, that stays put while it is written - and a
   program saving a file checks that it is still the one it opened. */
uint32_t file_ino(const char *name, bool follow) {
    unsigned index;

    return fs_entry(name, follow, &index) == 0 ? folder_ino(index) : 1;
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
                                               : (uint64_t)trap_stubs + i * 32;
        idt[i] = (struct idt_gate){
            .offset_low  = (uint16_t)handler,
            .selector    = kernel_cs,
            /* present 64-bit interrupt gates - int3 and into ones a program
               may use itself, as a debugger's breakpoint and as Chromium's
               way of ending on purpose */
            .type        = i == 3 || i == 4 ? 0xEE : 0x8E,
            .offset_mid  = (uint16_t)(handler >> 16),
            .offset_high = (uint32_t)(handler >> 32),
        };
    }
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

/* A real binary's memcpy is written in SSE, which faults unless the operating
   system says it is prepared to have those registers used. thread.c keeps
   each thread's with FXSAVE as it switches - which covers SSE and no more, so
   the larger AVX state stays off: a libc only uses it when told it may. */
static void sse_init(void) {
    uint64_t cr0, cr4;

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 = (cr0 & ~(uint64_t)(1 << 2)) | (1 << 1);   /* not emulated, monitored */
    cr0 |= 1 << 16;     /* WP: the kernel too faults on a read-only page, so a
                           write into a page processes share copies it first */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 9) | (1 << 10);        /* OSFXSR, OSXMMEXCPT */
    cr4 &= ~(uint64_t)(1 << 18);        /* not OSXSAVE, whatever the firmware left */
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
/* madvise: MADV_DONTNEED (and FREE) give the private pages back - read
   again they are zeroes, or the file's bytes again - as a JavaScript engine
   counts on when it lets go of memory. Shared memory keeps what it holds.
   Every other advice is taken as read. */
static uint64_t sys_madvise(uint64_t addr, uint64_t length, uint64_t advice) {
    if (advice == 4 || advice == 8) {               /* MADV_DONTNEED, MADV_FREE */
        if (addr % PAGE_SIZE != 0) {
            return ERR(EINVAL);
        }
        vm_discard(addr, (length + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1));
    }
    return 0;
}

static const uint16_t numbers[] = {
    SYS_READ,
    SYS_WRITE,
    SYS_OPEN,
    SYS_CLOSE,
    SYS_LSEEK,
    SYS_MMAP,
    SYS_MPROTECT,
    SYS_MUNMAP,
    SYS_BRK,
    SYS_WRITEV,
    SYS_GETPID,
    SYS_SCHED_YIELD,
    SYS_MADVISE,
    SYS_FSYNC,
    SYS_FDATASYNC,
    SYS_SYNC,
    SYS_RT_SIGSUSPEND,
    SYS_KILL,
    SYS_TGKILL,
    SYS_GETTID,
    SYS_GETCWD,
    SYS_ARCH_PRCTL,
    SYS_SET_TID_ADDRESS,
    SYS_GETRANDOM,
    SYS_SET_ROBUST_LIST,
    SYS_PREAD64,
    SYS_UNLINK,
    SYS_RMDIR,
    SYS_MKDIR,
    SYS_FCNTL,
    SYS_FUTEX,
    SYS_GETUID,
    SYS_GETGID,
    SYS_GETEUID,
    SYS_GETEGID,
    SYS_RT_SIGACTION,
    SYS_RT_SIGPROCMASK,
    SYS_EXIT,
    SYS_EXIT_GROUP,
    SYS_OPENAT,
    SYS_CHDIR,
    SYS_FCHDIR,
    SYS_DUP,
    SYS_DUP2,
    SYS_DUP3,
    SYS_CLOSE_RANGE,
    SYS_FTRUNCATE,
    SYS_UMASK,
    SYS_GETPPID,
    SYS_GETPGRP,
    SYS_GETPGID,
    SYS_SETPGID,
    SYS_SETSID,
    SYS_SIGALTSTACK,
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
    SYS_ALARM,
    SYS_GETITIMER,
    SYS_SETITIMER,
    SYS_EXECVE,
    SYS_WAIT4,
    SYS_PIPE,
    SYS_PIPE2,
    SYS_EVENTFD,
    SYS_EVENTFD2,
    SYS_INOTIFY_INIT,
    SYS_INOTIFY_INIT1,
    SYS_INOTIFY_ADD_WATCH,
    SYS_INOTIFY_RM_WATCH,
    SYS_MEMFD_CREATE,
    SYS_UNLINKAT,
    SYS_MKDIRAT,
    SYS_READV,
    SYS_PWRITE64,
    SYS_SYNCFS,
    SYS_SOCKETPAIR,
    SYS_RT_SIGRETURN,
    SYS_GETSID,
};

static const syscall_fn handlers[] = {
    sys_read,
    sys_write,
    sys_open,
    sys_close,
    sys_lseek,
    sys_mmap,
    sys_ok,
    sys_munmap,
    sys_brk,
    sys_writev,
    sys_getpid,
    sys_sched_yield,
    sys_madvise,
    sys_sync,
    sys_sync,
    sys_sync,
    sys_rt_sigsuspend,
    sys_kill,
    sys_tgkill,
    sys_gettid,
    sys_getcwd,
    sys_arch_prctl,
    sys_set_tid_address,
    sys_getrandom,
    sys_set_robust_list,
    sys_pread64,
    sys_unlink,
    sys_unlink,
    sys_mkdir,
    sys_fcntl,
    sys_futex,
    sys_getuid,
    sys_getgid,
    sys_geteuid,
    sys_getegid,
    sys_rt_sigaction,
    sys_rt_sigprocmask,
    sys_exit,
    sys_exit_group,
    sys_openat,
    sys_chdir,
    sys_fchdir,
    sys_dup,
    sys_dup2,
    sys_dup3,
    sys_close_range,
    sys_ftruncate,
    sys_umask,
    sys_getppid,
    sys_getpgrp,
    sys_getpgid,
    sys_setpgid,
    sys_setsid,
    sys_ok,
    sys_fork,
    sys_fork,
    sys_clone,
    sys_clone3,
    sys_rt_sigtimedwait,
    sys_timer_create,
    sys_timer_settime,
    sys_timer_gettime,
    sys_ok,
    sys_timer_delete,
    sys_alarm,
    sys_getitimer,
    sys_setitimer,
    sys_execve,
    sys_wait4,
    sys_pipe,
    sys_pipe2,
    sys_eventfd,
    sys_eventfd2,
    sys_inotify_init1,
    sys_inotify_init1,
    sys_inotify_add_watch,
    sys_ok,
    sys_memfd_create,
    sys_unlinkat,
    sys_mkdirat,
    sys_readv,
    sys_pwrite64,
    sys_sync,
    sys_socketpair,
    sys_rt_sigreturn,
    sys_getsid,
};

#define SYSCALLS (sizeof numbers / sizeof numbers[0])

void syscall_init(void) {
    if (pipes == NULL && (pipes = mem_alloc(PIPES * sizeof *pipes)) != NULL) {
        memset(pipes, 0, PIPES * sizeof *pipes);
    }
    gdt_init();
    sse_init();
    traps_init();
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

static unsigned calls;              /* syscalls made, for the look at Ctrl-C */

/* compat/linux's calls, while it is in. The kernel loads it at the first
   call that is its, and drops it once no program is running - unless it
   was enabled by hand, which is then whoever enabled it's to undo. */
static syscall_fn (*linux_find)(uint64_t number);
static bool linux_ours;             /* the kernel loaded it, or tried to */

void linux_register(syscall_fn (*find)(uint64_t number)) {
    linux_find = find;
}

/* Called by syscall_entry. Every call is recorded on the way through,
   including the numbers this kernel has no handler for. */
uint64_t syscall_dispatch(uint64_t a, uint64_t b, uint64_t c,
                          uint64_t d, uint64_t e, uint64_t f, uint64_t number) {
    void *entry = tracer != NULL ? tracer->begin((uint32_t)number, a, b, c) : NULL;
    uint64_t result = ERR(ENOSYS);

    if (proc != NULL && now_running.left != 0) {
        now_running.user += efi_uptime_us() - now_running.left;
    }
    arg[0] = a;
    arg[1] = b;
    arg[2] = c;
    arg[3] = d;
    arg[4] = e;
    arg[5] = f;

    syscall_fn handler = handler_for(number);
    struct handle *h = number == SYS_SENDTO || number == SYS_RECVFROM ? handle_of(a) : NULL;

    if (h != NULL && h->start == PIPE_MARK && h->size == PIPE_PAIR) {
        handler = number == SYS_SENDTO ? sys_write : sys_read;     /* a socketpair */
    }

    if (handler == NULL && unix_sock != NULL) {
        handler = unix_sock->syscall(number, a);    /* AF_UNIX's, if it is */
    }
    if (handler == NULL && net != NULL) {
        handler = net->syscall(number);     /* the socket calls are the module's */
    }
    if (handler == NULL && linux_find == NULL && !linux_ours) {
        linux_ours = true;
        module_need(LINUX_MODULE);
    }
    if (handler == NULL && linux_find != NULL) {
        handler = linux_find(number);
    }

    if (handler != NULL) {
        struct kernel_mark mark = kernel_began();

        struct user_regs *frame = user_frame;

        result = handler(a, b, c);
        kernel_ended(mark);
        user_frame = frame;         /* a fork's child left its own there */
    }

    /* A program that never waits is asked every so often whether Ctrl-C
       was typed - nothing else would ask - and a SIGINT it handles goes to
       its handler here, on the way out. */
    if ((++calls & 63) == 0) {
        interrupt_check();
    }
    /* Nothing interrupts a program, so one polling - a non-blocking read
       answered EAGAIN, again and again - would have the processor for good,
       and the program it waits on would never run. Every syscall is a point
       where the others can have a turn, and a busy one gives them it. */
    ofd_sync();
    if ((calls & 31) == 0 || result == ERR(EAGAIN)) {
        thread_yield();
    }
    signal_fatal();
    if (deliverable() != 0 && number != SYS_RT_SIGRETURN && program_running()) {
        result = signal_deliver(result, blocked);
    }

    /* SYS_EXIT does not come back, and neither does a program killed
       mid-call, so those entries stay unfinished - which is worth seeing. */
    if (entry != NULL && tracer != NULL) {
        tracer->end(entry, result);
    }
    if (proc != NULL) {
        now_running.left = efi_uptime_us();
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
/* The registers of a program at an exception, as trap_common and
   page_fault_entry save them: everything, and the processor's own frame. */
struct trap_regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdx, rcx, rbx, rax, rsi, rdi;
    uint64_t err, rip, cs, rflags, rsp, ss;
};

#define SIGILL  4
#define SIGTRAP 5
#define SIGBUS  7
#define SIGFPE  8

/* The program's handler for sig, run instead of the instruction that went
   wrong: the frame Linux builds, from every register, with si_addr. False
   if it has none to run - or this is not the program's own fault. */
static bool fault_deliver(uint64_t sig, int code, uint64_t addr, unsigned vector,
                          struct trap_regs *r) {
    struct sig_action *a = action_for(sig);

    if (proc == NULL || (r->cs & 3) != 3 || a == NULL || !handled(sig) ||
        (blocked & 1ull << (sig - 1)) != 0) {
        return false;
    }
    uint64_t at = ((r->rsp - 128 - sizeof(struct sig_frame)) & ~15ull) - 8;
    struct sig_frame *frame = (struct sig_frame *)at;

    if (!user_range(at, sizeof *frame)) {
        return false;
    }
    memset(frame, 0, sizeof *frame);
    frame->restorer = a->restorer;
    frame->uc.sigmask = blocked;
    blocked |= a->mask | ((a->flags & SA_NODEFER) != 0 ? 0 : 1ull << (sig - 1));

    uint64_t *g = frame->uc.gregs;

    g[G_R8] = r->r8;
    g[G_R9] = r->r9;
    g[G_R10] = r->r10;
    g[G_R11] = r->r11;
    g[G_R12] = r->r12;
    g[G_R13] = r->r13;
    g[G_R14] = r->r14;
    g[G_R15] = r->r15;
    g[G_RDI] = r->rdi;
    g[G_RSI] = r->rsi;
    g[G_RBP] = r->rbp;
    g[G_RBX] = r->rbx;
    g[G_RDX] = r->rdx;
    g[G_RAX] = r->rax;
    g[G_RCX] = r->rcx;
    g[G_RSP] = r->rsp;
    g[G_RIP] = r->rip;
    g[G_EFLAGS] = r->rflags;
    g[19] = r->err;                 /* REG_ERR, REG_TRAPNO, REG_CR2 */
    g[20] = vector;
    g[22] = addr;
    frame->info.signo = (int32_t)sig;
    frame->info.code = code;
    *(uint64_t *)frame->info.rest = addr;   /* si_addr */

    r->rdi = sig;
    r->rsi = (uint64_t)&frame->info;
    r->rdx = (uint64_t)&frame->uc;
    r->rax = 0;
    r->rsp = at;
    r->rip = a->handler;
    if ((a->flags & SA_RESETHAND) != 0) {
        *a = (struct sig_action){ 0 };
    }
    return true;
}

/* Called by trap_common: the signal the exception is - to the program's
   handler if it has one (and this returns, into it), else its end, as on
   Linux: a shell says "Segmentation fault", "Illegal instruction". */
void trap_signal(unsigned vector, struct trap_regs *r) {
    uint64_t sig = vector == 0 || vector == 16 || vector == 19 ? SIGFPE :
                   vector == 1 || vector == 3 ? SIGTRAP :
                   vector == 6 ? SIGILL : vector == 17 ? SIGBUS : SIGSEGV;
    int code = sig == SIGTRAP ? 0x80 : sig == SIGILL ? 2 /* ILL_ILLOPN */ :
               sig == SIGFPE ? 1 /* FPE_INTDIV */ : 0x80 /* SI_KERNEL */;

    if (fault_deliver(sig, code, r->rip, vector, r)) {
        return;
    }
    dbg("trap %u at %x code %x: %s %u, signal %u\n", (uint64_t)vector, r->rip, r->err,
        proc != NULL ? proc->name : "?", (uint64_t)(proc != NULL ? proc->pid : 0), sig);
    process_exit(SIGNALLED | (int)sig);
}

/* Called by page_fault_entry with the address that faulted, and the registers. */
void page_fault(uint64_t addr, struct trap_regs *r) {
    struct kernel_mark mark = kernel_began();

    /* Something promised to a mapping comes first: the page is not merely
       empty, it has a stretch of a file that belongs in it. */
    if (map_fill(addr) || vm_fault(addr)) {
        kernel_ended(mark);
        return;
    }
    /* SEGV_MAPERR where nothing is, SEGV_ACCERR where something is that may
       not be touched so - what a JavaScript engine's guard pages rely on. */
    if (fault_deliver(SIGSEGV, (r->err & 1) != 0 ? 2 : 1, addr, 14, r)) {
        kernel_ended(mark);
        return;
    }
    dbg("fault at %x from %x: %s %u killed\n", addr, r->rip, proc != NULL ? proc->name : "?",
        (uint64_t)(proc != NULL ? proc->pid : 0));
    process_exit(SIGNALLED | SIGSEGV);
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
    if (mappings != NULL) {         /* nothing of the last one is owed */
        memset(mappings, 0, map_room * sizeof(struct mapping));
    }
    vm_reset();
    vm_start();                     /* the region it runs in */
    started_base = 0;
    started_phdr = started_phent = started_phnum = 0;

    /* Everything on the disk is linked: an ELF, or not a program at all. */
    if (file->size < sizeof header || read_at(file, 0, &header, sizeof header) != 0 ||
        header.ident[0] != 0x7F || header.ident[1] != 'E' ||
        header.ident[2] != 'L' || header.ident[3] != 'F') {
        dbg("load: not ELF: size %u start %x read %d magic %x\n", (uint64_t)file->size, (uint64_t)file->start,
            (uint64_t)(int64_t)read_at(file, 0, &header, sizeof header), (uint64_t)header.ident[0]);
        err = PROGRAM_EINVAL;
        goto done;
    }
    uint64_t bias = header.type == ET_DYN ? vm_base() + USER_EXEC : 0;
    uint64_t low, high;

    /* Something linked to run at a fixed address goes there, in low memory;
       anything position-independent goes in its region. */
    if (vm_base() == 0) {
        dbg("load: no region\n");
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

/* The process that took the screen or the input devices: they are given
   back once it ends - crashed or not. */
static int graphics_pid;

void graphics_take(void) {
    if (proc != NULL && graphics_pid == 0) {
        graphics_pid = proc->pid;
    }
}

bool program_running(void) {
    return proc != NULL;
}

/* Runs the loaded program in the running process, on this thread, until
   the process ends; returns its exit code. */
static int program_run(uint64_t entry, unsigned argc, const char *const *argv,
                       unsigned envc, const char *const *envv) {
    uint64_t rsp = build_stack(argc, argv, envc, envv);

    dbg("run: entry %x rsp %x phdr %x base %x\n", entry, rsp, started_phdr,
        started_base);
    if (rsp == 0) {
        return PROGRAM_KILLED;
    }
    /* Its calls are recorded under its own name, which is what gives it a
       file of its own in /var/log. What it did stays in the ring to be
       written out when the machine is next idle. */
    process_named(argc > 0 ? argv[0] : "program");
    process_cmdline(argc, argv);
    now_running.left = efi_uptime_us();
    return user_enter(entry, rsp);
}

/* ---- one program starting another ----------------------------------------
 *
 * A shell runs a command by forking and then, in the child, replacing itself
 * with the program. fork makes a process: a copy of the caller's memory
 * (vm.c), its files, its signal actions, and a thread that carries on from
 * the same syscall with 0 for its answer. The two then run side by side,
 * taking turns whenever either waits (thread.c). execve replaces what the
 * process runs and keeps the process: its pid, its files, its parent.
 *
 * vfork, and posix_spawn built on it, share the parent's memory instead,
 * the parent waiting until the child has started its program or ended -
 * which is what they promise, and costs no copy at all. */

#define EXEC_LINE  4096     /* arguments and environment handed over, in bytes */
#define EXEC_ARGS  64
#define EXEC_ENV   64
#define PROC_OUT   16384    /* the most of a kernel command's output that
                               can be sent somewhere other than the screen */

static uint64_t sys_pipe2(uint64_t out, uint64_t flags, uint64_t c) {
    uint32_t *fds = (uint32_t *)out;
    unsigned slot;
    uint64_t read_fd, write_fd;
    void *data = NULL;

    (void)c;
    if (!user_range(out, 8)) {
        return ERR(EFAULT);
    }
    if ((slot = pipe_slot()) == PIPES || (data = mem_alloc(PIPE_FIRST)) == NULL) {
        return ERR(ENFILE);
    }
    pipes[slot] = (struct pipe){ .data = data, .size = PIPE_FIRST, .refs = 2, .count = 1 };

    struct handle h = { .start = PIPE_MARK, .folder = slot + 1,
                        .offset = (uint32_t)flags & O_NONBLOCK };

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
    fds[0] = (uint32_t)with_cloexec(read_fd, (flags & O_CLOEXEC) != 0);
    fds[1] = (uint32_t)with_cloexec(write_fd, (flags & O_CLOEXEC) != 0);
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
    return with_cloexec(fd, (flags & O_CLOEXEC) != 0);
}

/* inotify: a descriptor that is told what to watch and never has anything to
   say - nothing here watches files change. A program waiting on it for a
   change it would then reread carries on as on a machine where none came:
   Chromium's, GTK's file chooser, a desktop's menu. */
static uint64_t sys_inotify_init1(uint64_t flags, uint64_t b, uint64_t c) {
    (void)b;
    (void)c;
    return sys_eventfd2(0, flags & (O_NONBLOCK | O_CLOEXEC), 0);
}

static uint64_t sys_inotify_add_watch(uint64_t fd, uint64_t path, uint64_t mask) {
    static uint32_t next;

    (void)mask;
    if (handle_of(fd) == NULL) {
        return ERR(EBADF);
    }
    return user_string(path) == NULL ? ERR(EFAULT) : ++next;
}

static uint64_t sys_eventfd(uint64_t count, uint64_t b, uint64_t c) {
    (void)b;
    return sys_eventfd2(count, 0, c);
}

/* Two connected AF_UNIX stream sockets, each end a pipe to read and the
   other's to write - what a program wakes its own poll with from a
   thread, curl's resolver among them. send and recv on one are write and
   read (syscall_dispatch). */
#define AF_UNIX     1
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_SEQPACKET 5
#define SOCK_TYPE   0xF

bool pair_new(struct handle out[2], uint32_t flags) {
    unsigned slot[2];

    for (int i = 0; i < 2; i++) {
        void *data = (slot[i] = pipe_slot()) < PIPES ? mem_alloc(PIPE_FIRST) : NULL;

        if (data == NULL) {
            if (i == 1) {
                pipe_drop(&pipes[slot[0]]);
            }
            return false;
        }
        pipes[slot[i]] = (struct pipe){ .data = data, .size = PIPE_FIRST, .refs = 2, .count = 1 };
    }
    for (int i = 0; i < 2; i++) {
        out[i] = (struct handle){ .used = 1, .start = PIPE_MARK, .folder = slot[i] + 1,
                                  .size = PIPE_PAIR,
                                  .offset = (slot[1 - i] + 1) << 16 | (flags & O_NONBLOCK) };
    }
    return true;
}

void handle_close(struct handle *h) {
    ofd_drop(h);
    if (h->start == PIPE_MARK) {
        pipe_release(h);
    } else if (h->start == SOCK_MARK) {
        sock_drop(h);
    }
    h->used = 0;
}

/* ---- messages, and descriptors passed, on a socketpair ----------------------
 *
 * A SEQPACKET or DGRAM pair keeps each write as one message, and a read
 * takes one - the rest of a message too long for it is lost, as Linux has
 * it. Either kind carries descriptors with what is sent (SCM_RIGHTS): a copy
 * of each, its pipe, socket or file held as dup would hold it, waiting with
 * the bytes it came with and handed out as a new descriptor of the process
 * that reads them. Chromium's processes and Firefox's talk this way, and
 * pass each other the memory they share. */

/* What fd is, held once more for somewhere else to have it; false if fd is not. */
bool handle_share(uint64_t fd, struct handle *out) {
    struct handle *h = handle_of(fd);

    if (h == NULL) {
        return false;
    }
    *out = *h;
    out->used = 1;                  /* not close-on-exec where it arrives */
    pipe_hold(h);
    ofd_hold(h);
    if (h->start == SOCK_MARK) {
        sock_hold(h);
    }
    if (ops_of(h) != NULL) {
        ops_of(h)->hold(h);
    }
    return true;
}

/* A held copy let go of: what closing a descriptor of it would do. */
static void handle_release(struct handle *h) {
    if (ops_of(h) != NULL) {
        ops_of(h)->drop(h);
    }
    handle_close(h);
}

static bool pmsg_push(struct pipe *p, uint64_t at, uint32_t len, const struct handle *fds, uint32_t nfd) {
    p->last_pid = proc != NULL ? proc->pid : 0;
    if (p->nmsgs == p->msg_room) {
        uint32_t room = p->msg_room == 0 ? 4 : p->msg_room * 2;
        struct pmsg *more = mem_alloc(room * sizeof *more);

        if (more == NULL) {
            return false;
        }
        if (p->msgs != NULL) {
            memcpy(more, p->msgs, p->nmsgs * sizeof *more);
            mem_free(p->msgs);
        }
        p->msgs = more;
        p->msg_room = room;
    }
    struct pmsg *m = &p->msgs[p->nmsgs++];

    m->at = at;
    m->len = len;
    m->nfd = nfd;
    m->pid = p->last_pid;
    if (nfd > 0) {
        memcpy(m->fd, fds, nfd * sizeof *fds);
    }
    return true;
}

static uint64_t pair_put(struct handle *h, struct pipe *p, const struct iovec *iov, uint64_t n,
                         const struct handle *fds, uint32_t nfd, bool nonblock) {
    uint64_t total = 0, done = 0;

    for (uint64_t i = 0; i < n; i++) {
        if (!user_range(iov[i].base, iov[i].length)) {
            return ERR(EFAULT);
        }
        total += iov[i].length;
    }
    if (p->packets) {
        if (total > PIPE_CAP) {
            return ERR(EMSGSIZE);
        }
        /* A message goes in whole: room for all of it first. */
        while (PIPE_CAP - (p->len - p->read_at) < total) {
            if (nonblock || (h->offset & O_NONBLOCK) != 0 || thread_only()) {
                return ERR(EAGAIN);
            }
            if (interrupt_check()) {
                return ERR(EINTR);
            }
            thread_yield();
            if (pair_out(h) != p) {
                return ERR(EPIPE);
            }
        }
        if (!pmsg_push(p, p->wrote, (uint32_t)total, fds, nfd)) {
            return ERR(ENOBUFS);
        }
        for (uint64_t i = 0; i < n; i++) {
            pipe_put(p, (const char *)iov[i].base, iov[i].length);
        }
        return total;
    }
    if (nfd > 0 && !pmsg_push(p, p->wrote, 0, fds, nfd)) {
        return ERR(ENOBUFS);
    }
    p->last_pid = proc != NULL ? proc->pid : 0;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t r = iov[i].length != 0 ? pipe_write(h, p, (const char *)iov[i].base, iov[i].length) : 0;

        if ((int64_t)r < 0) {
            return done > 0 ? done : r;
        }
        done += r;
    }
    return done;
}

static uint64_t pair_take(struct handle *h, struct pipe *p, const struct iovec *iov, uint64_t n,
                          struct handle *fds, uint32_t *nfd, bool *cut) {
    uint64_t want = 0, done = 0;

    pair_sender = p->last_pid;

    for (uint64_t i = 0; i < n; i++) {
        if (!user_range(iov[i].base, iov[i].length)) {
            return ERR(EFAULT);
        }
        want += iov[i].length;
    }
    while (!readable(h)) {
        if ((h->offset & O_NONBLOCK) != 0 || (cut != NULL && *cut)) {
            return ERR(EAGAIN);     /* *cut on the way in: MSG_DONTWAIT */
        }
        if (thread_only()) {
            break;
        }
        if (interrupt_check()) {
            return ERR(EINTR);
        }
        thread_yield();
    }
    if (cut != NULL) {
        *cut = false;
    }
    if (nfd != NULL) {
        *nfd = 0;
    }
    uint64_t avail = p->len - p->read_at, limit = avail;
    struct pmsg *m = p->nmsgs > 0 ? &p->msgs[0] : NULL;
    bool whole = false;

    if (p->packets) {
        if (m == NULL) {
            return 0;               /* nothing, and nobody left to send: the end */
        }
        limit = m->len;
        whole = true;
    } else if (m != NULL) {
        if (m->at <= p->taken) {
            whole = true;           /* its descriptors come with its first byte */
            limit = p->nmsgs > 1 ? p->msgs[1].at - p->taken : avail;
        } else {
            limit = m->at - p->taken;   /* up to it, not into it */
        }
    }
    limit = limit < avail ? limit : avail;
    for (uint64_t i = 0; i < n && done < limit && done < want; i++) {
        uint64_t k = iov[i].length < limit - done ? iov[i].length : limit - done;

        done += pipe_read(p, (char *)iov[i].base, k);
    }
    if (p->packets && done < m->len) {
        pipe_read(p, NULL, m->len - done);     /* what did not fit is lost */
        if (cut != NULL) {
            *cut = true;
        }
    }
    if (whole) {
        pair_sender = m->pid;
        for (uint32_t k = 0; k < m->nfd; k++) {
            if (fds != NULL && nfd != NULL) {
                fds[(*nfd)++] = m->fd[k];
            } else {
                handle_release(&m->fd[k]);  /* read without asking for them */
            }
        }
        memmove(p->msgs, p->msgs + 1, (p->nmsgs - 1) * sizeof *m);
        p->nmsgs--;
    }
    return done;
}

/* For the ipc/unix module: a pair end's SO_PASSCRED, set or cleared (on < 0
   leaves it), and what it is - bit 0 passcred, bit 1 a packet pair; -1 if
   fd is no pair end. pair_sender: who sent what recvmsg last took. */
int pair_sender;

int pair_flags(uint64_t fd, int on) {
    struct handle *h = handle_of(fd);
    struct pipe *p = h != NULL && h->start == PIPE_MARK && h->size == PIPE_PAIR ? pipe_of(h) : NULL;

    if (p == NULL) {
        return -1;
    }
    if (on >= 0) {
        p->passcred = on != 0;
    }
    return (p->passcred ? 1 : 0) | (p->packets ? 2 : 0);
}

/* For the ipc/unix module's sendmsg and recvmsg on a pair end: the iovecs at
   iov in the program's memory, and descriptors to send or that came. */
uint64_t pair_send(uint64_t fd, uint64_t iov, uint64_t n, const struct handle *fds, uint32_t nfd,
                   bool nonblock) {
    struct handle *h = handle_of(fd);
    struct pipe *p = h != NULL && h->start == PIPE_MARK && h->size == PIPE_PAIR ? pair_out(h) : NULL;

    if (h == NULL || !user_range(iov, n * sizeof(struct iovec))) {
        return ERR(h == NULL ? EBADF : EFAULT);
    }
    return p == NULL ? ERR(EPIPE) : pair_put(h, p, (const struct iovec *)iov, n, fds, nfd, nonblock);
}

uint64_t pair_recv(uint64_t fd, uint64_t iov, uint64_t n, struct handle *fds, uint32_t *nfd,
                   bool *cut) {
    struct handle *h = handle_of(fd);
    struct pipe *p = h != NULL && h->start == PIPE_MARK && h->size == PIPE_PAIR ? pipe_of(h) : NULL;

    if (h == NULL || p == NULL) {
        return ERR(EBADF);
    }
    if (!user_range(iov, n * sizeof(struct iovec))) {
        return ERR(EFAULT);
    }
    return pair_take(h, p, (const struct iovec *)iov, n, fds, nfd, cut);
}

static uint64_t sys_socketpair(uint64_t domain, uint64_t type, uint64_t c) {
    uint32_t *fds = (uint32_t *)arg[3];
    struct handle pair[2];
    uint64_t fd[2];

    (void)c;
    if (domain != AF_UNIX) {
        return ERR(EAFNOSUPPORT);
    }
    uint32_t kind = (uint32_t)type & SOCK_TYPE;

    if (kind != SOCK_STREAM && kind != SOCK_DGRAM && kind != SOCK_SEQPACKET) {
        return ERR(EOPNOTSUPP);
    }
    if (!user_range(arg[3], 8)) {
        return ERR(EFAULT);
    }
    if (!pair_new(pair, (uint32_t)type)) {
        return ERR(ENFILE);
    }
    if (kind != SOCK_STREAM) {
        pipe_of(&pair[0])->packets = pipe_of(&pair[1])->packets = true;
    }
    fd[0] = give_handle(pair[0]);
    fd[1] = (int64_t)fd[0] < 0 ? fd[0] : give_handle(pair[1]);
    if ((int64_t)fd[1] < 0) {
        if ((int64_t)fd[0] >= 0) {
            handles[fd[0]].used = 0;
        }
        handle_close(&pair[0]);
        handle_close(&pair[1]);
        return ERR(EMFILE);
    }
    fds[0] = (uint32_t)with_cloexec(fd[0], (type & O_CLOEXEC) != 0);
    fds[1] = (uint32_t)with_cloexec(fd[1], (type & O_CLOEXEC) != 0);
    return 0;
}

uint64_t fd_read(uint64_t fd, uint64_t buf, uint64_t count) {
    return sys_read(fd, buf, count);
}

uint64_t fd_write(uint64_t fd, uint64_t buf, uint64_t count) {
    return sys_write(fd, buf, count);
}

/* ---- a process ending ---------------------------------------------------- */

/* Linux's wait status: the exit code in the second byte, or the signal that
   ended it in the low seven bits. */
static int wait_status(int code) {
    return (code & SIGNALLED) != 0 ? code & 0x7F
         : code == PROGRAM_KILLED ? SIGSEGV : (code & 0xFF) << 8;
}

/* What is left once the running process's last thread is done: its files
   are closed, its memory goes, its children are nobody's, and its parent
   hears - SIGCHLD - and collects it with wait4. One nobody waits for is
   forgotten at the next fork. */
static void process_end(int code) {
    struct process *p = proc, *parent;

    for (unsigned fd = 0; fd < file_room; fd++) {
        if (handles[fd].used != 0) {
            sys_close(fd, 0, 0);
        }
    }
    if (graphics_pid == p->pid) {
        graphics_pid = 0;
        vga_lend(false);
        console_keys(true);
        input_release();
    }
    if (p->borrowed) {
        p->borrowed = false;        /* the parent's again, and it carries on */
    } else if (p->space != NULL) {
        vm_space_free(p->space);
    }
    p->space = NULL;
    vm_space_use(NULL);
    for (unsigned i = 0; i < PROCESSES; i++) {
        if (procs[i] != NULL && procs[i]->ppid == p->pid) {
            procs[i]->ppid = 0;
        }
    }
    p->status = wait_status(code);
    p->zombie = true;
    parent = proc_find(p->ppid);
    if (parent == NULL || parent->zombie) {
        p->ppid = 0;
        return;
    }
    /* All the child ran is its parent's children's time. */
    parent->times.children_wall += efi_uptime_us() - p->times.started;
    parent->times.children_user += user_us() + p->times.children_user;
    parent->times.children_sys += p->times.sys + p->times.children_sys;
    parent->chld_pid = p->pid;
    parent->chld_code = code & 0xFF;
    signal_to(parent, SIGCHLD);
}

/* thread.c, once a forked process's first thread has returned from it. */
void process_finished(int code) {
    process_end(code);
}

/* A child that has ended, collected: what it said, and it is gone. Waits
   for one unless WNOHANG; ECHILD if there are none to wait for. pid is a
   child's, or -1 for any, or 0 or -pgid for one of a group. */
static uint64_t sys_wait4(uint64_t pid, uint64_t status, uint64_t options) {
    uint64_t usage_at = arg[3];
    int want = (int)pid;            /* pid_t: -1 arrives as 0xffffffff */
    uint64_t began = wait_began();

    if ((status != 0 && !user_range(status, 4)) || (usage_at != 0 && !user_range(usage_at, 144))) {
        return ERR(EFAULT);
    }
    for (;;) {
        bool any = false;

        for (unsigned i = 0; i < PROCESSES; i++) {
            struct process *c = procs[i];

            if (c == NULL || c->ppid != proc->pid ||
                (want > 0 && c->pid != want) ||
                (want == 0 && c->pgid != proc->pgid) || (want < -1 && c->pgid != -want)) {
                continue;
            }
            any = true;
            if (!c->zombie) {
                continue;
            }
            int got = c->pid;

            if (usage_at != 0) {    /* its rusage: the times, nothing else */
                int64_t *usage = (int64_t *)usage_at;
                uint64_t user = c->times.children_user, sys = c->times.sys + c->times.children_sys;

                memset(usage, 0, 144);
                usage[0] = (int64_t)(user / 1000000);
                usage[1] = (int64_t)(user % 1000000);
                usage[2] = (int64_t)(sys / 1000000);
                usage[3] = (int64_t)(sys % 1000000);
            }
            if (status != 0) {
                *(int32_t *)status = c->status;
            }
            proc_free(c);
            wait_ended(began);
            return (uint64_t)got;
        }
        if (!any || (options & 1) != 0) {   /* WNOHANG */
            wait_ended(began);
            return any ? 0 : ERR(ECHILD);
        }
        if (interrupt_check()) {
            wait_ended(began);
            return ERR(EINTR);
        }
        thread_yield();
    }
}

/* ---- fork and execve ----------------------------------------------------- */

extern struct user_regs *user_frame;
extern int user_resume(const struct user_regs *regs, uint64_t rax);
extern uint64_t user_flags;

/* One more descriptor on everything a file table holds: a fork's child has
   them too, and its closing them must not take them from its parent. */
static void hold_all(struct handle *table) {
    for (unsigned fd = 0; fd < file_room; fd++) {
        if (table[fd].used == 0) {
            continue;
        }
        if (table[fd].start == SOCK_MARK) {
            sock_hold(&table[fd]);
        }
        if (ops_of(&table[fd]) != NULL) {
            ops_of(&table[fd])->hold(&table[fd]);
        }
        if (pipe_of(&table[fd]) != NULL) {
            pipe_hold(&table[fd]);
        }
        ofd_hold(&table[fd]);
    }
}

static uint64_t fork_on(uint64_t stack, bool share);

static uint64_t sys_fork(uint64_t a, uint64_t b, uint64_t c) {
    (void)a;
    (void)b;
    (void)c;
    return fork_on(0, false);
}

/* A new process, a copy of this one, carrying on from this syscall - on
   stack if that is not 0, what vfork and posix_spawn ask for - in its own
   memory, or with share in this one's until it starts a program. */
static uint64_t fork_on(uint64_t stack, bool share) {
    struct process *parent = proc, *child = proc_new();
    struct user_regs regs = *user_frame;

    if (child == NULL) {
        return ERR(EAGAIN);
    }
    if (stack != 0) {
        regs.rsp = stack;
    }
    child->ppid = parent->pid;
    child->pgid = parent->pgid;
    child->sid = parent->sid;
    if (child->files_room < parent->files_room) {
        struct handle *bigger = mem_alloc(parent->files_room * sizeof *bigger);

        if (bigger == NULL) {
            return FS_ENOSPC;
        }
        mem_free(child->files);
        child->files = bigger;
        child->files_room = parent->files_room;
    }
    memcpy(child->files, parent->files, parent->files_room * sizeof *child->files);
    strcpy(child->cwd, fs_cwd());
    child->brk = parent->brk;
    child->map = parent->map;
    child->map_high = parent->map_high;
    child->times = (struct times){ .started = efi_uptime_us(), .left = efi_uptime_us() };
    if (parent->cmdline != NULL && (child->cmdline = mem_alloc(parent->cmdline_len)) != NULL) {
        memcpy(child->cmdline, parent->cmdline, parent->cmdline_len);
        child->cmdline_len = parent->cmdline_len;
    }
    memcpy(child->actions, parent->actions, sizeof child->actions);
    if (parent->maps != NULL) {
        /* Its own copy, even sharing memory: an execve empties the child's. */
        child->maps = mem_alloc(parent->maps_room * sizeof(struct mapping));
        if (child->maps == NULL) {
            return FS_ENOSPC;
        }
        memcpy(child->maps, parent->maps, parent->maps_room * sizeof(struct mapping));
        child->maps_room = parent->maps_room;
    }
    memcpy(child->name, parent->name, sizeof child->name);
    memcpy(child->exe, parent->exe, sizeof child->exe);
    child->cred = parent->cred;
    if (share) {
        child->space = parent->space;
        child->borrowed = true;
    } else if (vm_fork(&child->own)) {
        child->space = &child->own;
    } else {
        proc_free(child);
        return ERR(ENOMEM);
    }
    if (!thread_spawn(child, &regs, rdmsr(MSR_FS_BASE), blocked)) {
        if (!share) {
            vm_space_free(&child->own);
        }
        proc_free(child);
        return ERR(EAGAIN);
    }
    hold_all(child->files);
    int pid = child->pid;

    /* The parent of a vfork waits: the child is running in its memory. */
    while (share && child->borrowed) {
        thread_yield();
    }
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
    /* Namespaces are not had here: asked for, the answer a kernel built
       without them gives - and a browser then runs without its sandbox. */
    if ((flags & 0x7E020000u) != 0) {   /* CLONE_NEWNS ... CLONE_NEWNET */
        return ERR(EINVAL);
    }
    /* vfork, and posix_spawn built on it: shared memory until the child
       starts a program, its parent waiting. */
    if ((flags & CLONE_VFORK) && !(flags & CLONE_THREAD)) {
        return fork_on(stack, (flags & CLONE_VM) != 0);
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
    if (thread_only()) {
        return ERR(EAGAIN);
    }
    if (timeout != 0 && !user_range(timeout, 16)) {
        return ERR(EFAULT);
    }
    uint64_t until = timeout != 0 ? efi_uptime_ms() + timespec_ms(timeout) : 0;

    while ((until == 0 || efi_uptime_ms() < until) && !interrupt_check()) {
        thread_yield();
    }
    return ERR(deliverable() != 0 ? EINTR : EAGAIN);
}
/* ---- timers ---------------------------------------------------------------
 *
 * A timer is a deadline and a signal. Every wait gives way to the others
 * through thread_yield, and each time it does the timers due are sent: a
 * program waiting - `timeout` in sigsuspend, a poll with an alarm behind it -
 * is woken as on Linux. One computing without a syscall is not interrupted
 * (nothing preempts here); it hears at its next. alarm and setitimer's real
 * timer are one more per process, id -1. */

#define TIMERS       32
#define SIGALRM      14
#define SIGEV_NONE   1
#define TIMER_ABSTIME 1
#define ALARM_ID     (-1)

static struct ktimer {
    int      pid;                   /* 0: free */
    int32_t  id;
    int      signo;                 /* 0: SIGEV_NONE */
    bool     realtime;              /* an absolute time is on CLOCK_REALTIME */
    uint64_t due, every;            /* ms of uptime, 0 unarmed; and the period */
} timers[TIMERS];

void timers_tick(void) {
    uint64_t now = 0;

    for (unsigned i = 0; i < TIMERS; i++) {
        struct ktimer *t = &timers[i];

        if (t->pid == 0 || t->due == 0) {
            continue;
        }
        now = now != 0 ? now : efi_uptime_ms();
        if (t->due > now) {
            continue;
        }
        struct process *p = proc_find(t->pid);

        if (p == NULL || p->zombie) {
            *t = (struct ktimer){ 0 };  /* its process is gone */
            continue;
        }
        t->due = t->every != 0 ? now + t->every : 0;
        if (t->signo != 0) {
            signal_to(p, (uint64_t)t->signo);
        }
    }
}

static struct ktimer *timer_of(int32_t id) {
    for (unsigned i = 0; i < TIMERS; i++) {
        if (timers[i].pid == proc->pid && timers[i].id == id) {
            return &timers[i];
        }
    }
    return NULL;
}

static struct ktimer *timer_new(int32_t id, int signo) {
    for (unsigned i = 0; i < TIMERS; i++) {
        struct process *p = timers[i].pid != 0 ? proc_find(timers[i].pid) : NULL;

        if (timers[i].pid == 0 || p == NULL || p->zombie) {
            timers[i] = (struct ktimer){ .pid = proc->pid, .id = id, .signo = signo };
            return &timers[i];
        }
    }
    return NULL;
}

static uint64_t timer_left(const struct ktimer *t) {
    uint64_t now = efi_uptime_ms();

    return t == NULL || t->due == 0 ? 0 : t->due > now ? t->due - now : 1;
}

/* ms as a timespec (nsec) or a timeval (usec) at out. */
static void put_time(uint64_t out, uint64_t ms, bool usec) {
    ((uint64_t *)out)[0] = ms / 1000;
    ((uint64_t *)out)[1] = (ms % 1000) * (usec ? 1000 : 1000000);
}

static uint64_t get_time(uint64_t at, bool usec) {
    const uint64_t *v = (const uint64_t *)at;

    return v[0] * 1000 + v[1] / (usec ? 1000 : 1000000);
}

static void timer_arm(struct ktimer *t, uint64_t value, uint64_t interval, bool absolute) {
    uint64_t now = efi_uptime_ms();

    if (absolute && value != 0) {
        uint64_t clock = t->realtime ? realtime_ms() : now;

        value = value > clock ? value - clock : 1;
    }
    t->due = value != 0 ? now + value : 0;
    t->every = interval;
}

static uint64_t sys_timer_create(uint64_t clock, uint64_t event, uint64_t id) {
    int signo = SIGALRM;

    if (!user_range(id, 4) || (event != 0 && !user_range(event, 16))) {
        return ERR(EFAULT);
    }
    if (event != 0) {
        signo = *(const int32_t *)(event + 12) == SIGEV_NONE ? 0 : *(const int32_t *)(event + 8);
    }
    for (int32_t want = 0; want < TIMERS; want++) {
        if (timer_of(want) == NULL) {
            struct ktimer *t = timer_new(want, signo);

            if (t == NULL) {
                return ERR(EAGAIN);
            }
            t->realtime = clock == 0;       /* CLOCK_REALTIME */
            *(int32_t *)id = want;
            return 0;
        }
    }
    return ERR(EAGAIN);
}

static uint64_t sys_timer_settime(uint64_t id, uint64_t flags, uint64_t spec) {
    struct ktimer *t = timer_of((int32_t)id);
    uint64_t old = arg[3];

    if (t == NULL) {
        return ERR(EINVAL);
    }
    if (!user_range(spec, 32) || (old != 0 && !user_range(old, 32))) {
        return ERR(EFAULT);
    }
    if (old != 0) {
        put_time(old, t->every, false);
        put_time(old + 16, timer_left(t), false);
    }
    timer_arm(t, get_time(spec + 16, false), get_time(spec, false), (flags & TIMER_ABSTIME) != 0);
    return 0;
}

static uint64_t sys_timer_gettime(uint64_t id, uint64_t spec, uint64_t c) {
    struct ktimer *t = timer_of((int32_t)id);

    (void)c;
    if (t == NULL) {
        return ERR(EINVAL);
    }
    if (!user_range(spec, 32)) {
        return ERR(EFAULT);
    }
    put_time(spec, t->every, false);
    put_time(spec + 16, timer_left(t), false);
    return 0;
}

static uint64_t sys_timer_delete(uint64_t id, uint64_t b, uint64_t c) {
    struct ktimer *t = timer_of((int32_t)id);

    (void)b;
    (void)c;
    if (t == NULL) {
        return ERR(EINVAL);
    }
    *t = (struct ktimer){ 0 };
    return 0;
}

/* alarm, and setitimer/getitimer's ITIMER_REAL: the process's one timer. */
static uint64_t sys_alarm(uint64_t seconds, uint64_t b, uint64_t c) {
    struct ktimer *t = timer_of(ALARM_ID);
    uint64_t left = (timer_left(t) + 999) / 1000;

    (void)b;
    (void)c;
    if (t == NULL && seconds != 0 && (t = timer_new(ALARM_ID, SIGALRM)) == NULL) {
        return 0;
    }
    if (t != NULL) {
        timer_arm(t, seconds * 1000, 0, false);
    }
    return left;
}

static uint64_t sys_getitimer(uint64_t which, uint64_t value, uint64_t c) {
    struct ktimer *t = which == 0 ? timer_of(ALARM_ID) : NULL;

    (void)c;
    if (!user_range(value, 32)) {
        return ERR(EFAULT);
    }
    put_time(value, t != NULL ? t->every : 0, true);
    put_time(value + 16, timer_left(t), true);
    return 0;
}

static uint64_t sys_setitimer(uint64_t which, uint64_t value, uint64_t old) {
    struct ktimer *t = timer_of(ALARM_ID);

    if (which != 0) {
        return 0;                   /* the CPU-time ones: never due */
    }
    if (old != 0 && sys_getitimer(which, old, 0) != 0) {
        return ERR(EFAULT);
    }
    if (value == 0 || !user_range(value, 32)) {
        return value == 0 ? 0 : ERR(EFAULT);
    }
    if (t == NULL && (t = timer_new(ALARM_ID, SIGALRM)) == NULL) {
        return ERR(EAGAIN);
    }
    timer_arm(t, get_time(value + 16, true), get_time(value, true), false);
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
   as a process of its own, with a clean terminal and the machine's
   environment, and waits for it. A script that begins with "#!" is handed to
   the program it names, as execve would. Returns its exit code - 128 and the
   signal for one a signal ended - or a negative FS_E* or PROGRAM_E* code if
   it could not start. Whatever it started that is still running carries on. */
int program_start(const char *path, unsigned argc, const char *const *argv,
                  unsigned envc, const char *const *envv) {
    struct {
        char        line[FS_NAME_LEN * 2];
        const char *words[EXEC_ARGS + 3];
    } *w = mem_alloc(sizeof *w);
    const char *interp, *extra;
    struct fs_file file;
    struct process *p;
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
    if (err == 0 && (p = proc_new()) == NULL) {
        err = FS_ENOSPC;
    }
    if (err == 0 && !vm_space_new(&p->own)) {
        proc_free(p);
        err = FS_ENOSPC;
    }
    if (err == 0) {
        void *was = thread_adopt(p);

        /* Its own group, and the keyboard's: Ctrl-C is for it. */
        p->pgid = p->sid = p->pid;
        p->space = &p->own;
        strcpy(p->cwd, fs_cwd());
        p->times = (struct times){ .started = efi_uptime_us() };
        process_switch(was, p);
        handles_reset();
        console_reset();
        fg_pgrp = p->pid;
        err = program_load(&file, &entry);
        if (err == 0) {
            int code = program_run(entry, argc, argv, envc, envv);

            err = (code & SIGNALLED) != 0 ? 128 + (code & 0x7F) : code;
        }
        process_end(err);
        thread_disown(was);
        process_switch(p, was);
        proc_free(p);
        fg_pgrp = 0;
    }
    mem_free(w);
    if (linux_ours && !any_process()) {
        module_drop(LINUX_MODULE);
        linux_ours = false;
    }
    return err;
}

static uint64_t sys_execve(uint64_t path, uint64_t argv, uint64_t envp) {
    const char *given = user_string(path);
    const struct proc_cmd *cmd;
    struct exec_args *held = NULL;
    struct fs_file file;
    size_t used = 0;
    unsigned argc, envc;
    uint64_t entry, rsp;
    int code;

    if (given == NULL || strlen(given) + 1 > FS_NAME_LEN) {
        return ERR(EINVAL);
    }
    if ((held = mem_alloc(sizeof *held)) == NULL) {
        return ERR(ENOMEM);
    }
    strcpy(held->name, given);
    if (proc_fd_target(given, held->script, sizeof held->script) != NULL) {
        strcpy(held->name, held->script);   /* /proc/self/exe: the program itself -
                                               how Chromium starts its helpers */
    }
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

    /* From here the old program is gone: its other threads, its memory - or,
       for a vfork's child, the parent's memory is handed back and the
       child has a region of its own - and what it had open to close on exec. */
    thread_end_others();
    for (unsigned fd = 0; fd < file_room; fd++) {
        if ((handles[fd].used & HANDLE_CLOEXEC) != 0) {
            sys_close(fd, 0, 0);
        }
    }
    if (proc->borrowed) {
        if (!vm_space_new(&proc->own)) {
            mem_free(held);
            return ERR(ENOMEM);
        }
        proc->space = &proc->own;
        proc->borrowed = false;     /* its parent carries on */
        vm_space_use(proc->space);
    }
    code = program_load(&file, &entry);
    if (code != 0 || (rsp = build_stack(argc, held->words, envc, held->env)) == 0) {
        dbg("exec: %s could not be loaded (%d)\n", held->name, (uint64_t)code);
        mem_free(held);
        process_exit(127);
    }
    /* What the old image caught, the new one cannot: back to the default -
       an ignored signal stays ignored, as Linux keeps it. */
    for (unsigned i = 0; i < SIGNALS; i++) {
        if (on_signal[i].handler != SIG_IGN) {
            on_signal[i] = (struct sig_action){ 0 };
        }
    }
    process_named(held->words[0]);
    process_cmdline(argc, (const char *const *)held->words);
    strcpy(proc->exe, file.name);
    proc->cred.keepcaps = proc->cred.capable = 0;
    mem_free(held);

    /* The syscall returns into the new program: its entry, its stack, every
       register clear, no thread data yet. */
    wrmsr(MSR_FS_BASE, 0);
    wrmsr(MSR_GS_BASE, 0);
    memset(user_frame, 0, sizeof *user_frame);
    user_frame->rip = entry;
    user_frame->rsp = rsp;
    user_frame->rflags = user_flags;
    return 0;
}
