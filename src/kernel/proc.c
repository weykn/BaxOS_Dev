#include "proc.h"
#include "share.h"

#include <stdint.h>

#include "driver.h"
#include "efi_kernel.h"
#include "fs.h"
#include "mem.h"
#include "module.h"
#include "net.h"
#include "string.h"
#include "shell.h"
#include "vga.h"

#define PROC_NAME "ctl"
#define PROC_DIR  "/" PROC_NAME

#define BAR_WIDTH 30        /* cells in a usage bar */

/* The palette, all on black - the same one the shell prints in. */
#define COL_TEXT   VGA_LIGHTGRAY    /* ordinary output */
#define COL_DIM    VGA_DARKGRAY     /* labels, units, the empty part of a bar */
#define COL_ACCENT VGA_LIGHTCYAN    /* names of things */
#define COL_FILL   VGA_LIGHTGREEN   /* the used part of a bar */
#define COL_ERROR  VGA_LIGHTRED

static void color(enum vga_color fg) {
    vga_set_color(fg, VGA_BLACK);
}

/* kprintf for complaints: in red, then back to ordinary output. */
#define error(...) (color(COL_ERROR), kprintf(__VA_ARGS__), color(COL_TEXT))

/* Pads with spaces from column used to column width, to line up tables. */
static void pad(size_t used, size_t width) {
    while (used++ < width) {
        vga_putc(' ');
    }
}

/* Prints value right-aligned in width columns. */
static void put_right(unsigned value, size_t width) {
    size_t digits = 1;

    for (unsigned v = value; v >= 10; v /= 10) {
        digits++;
    }
    pad(digits, width);
    kprintf("%u", value);
}

/* Prints a label, a bar of how much of total is used, and the figures. */
void usage_bar(const char *label, unsigned used, unsigned total, const char *unit) {
    /* Rounded up, so any use at all shows. */
    unsigned filled = total == 0 ? 0 : (used * BAR_WIDTH + total - 1) / total;

    color(COL_DIM);
    kprintf("  %s", label);
    pad(strlen(label), 8);
    for (unsigned i = 0; i < BAR_WIDTH; i++) {
        color(i < filled ? COL_FILL : COL_DIM);
        vga_puts(i < filled ? "\u2588" : "\u2591");    /* full and light blocks */
    }
    color(COL_TEXT);
    kprintf("  %u", used);
    color(COL_DIM);
    kprintf(" of %u %s\n", total, unit);
    color(COL_TEXT);
}

/* Prints a line of a breakdown - a label, and a byte count lined up with the
   others - leaving it open for the caller to finish. */
static void detail(const char *label, unsigned bytes) {
    color(COL_DIM);
    kprintf("  %s", label);
    pad(strlen(label), 16);
    color(COL_TEXT);
    put_right(bytes, 8);        /* the firmware's is tens of megabytes */
    color(COL_DIM);
    vga_puts(" B");
}

/* Lists what name(0), name(1) ... give, marking the one equal to now. */
static void list(const char *(*name)(unsigned), const char *now) {
    for (unsigned i = 0; name(i) != NULL; i++) {
        bool current = strcmp(name(i), now) == 0;

        color(current ? COL_ACCENT : COL_TEXT);
        kprintf("  %s", name(i));
        color(COL_DIM);
        kprintf("%s\n", current ? "  current" : "");
    }
    color(COL_TEXT);
}

/* ---- the screen ---------------------------------------------------------- */

static void cmd_mode(char *args) {
    const char *name = str_word(&args);

    if (*name != '\0' && vga_mode_name(0) == NULL) {
        /* The modes are the firmware's to switch, and it has been let go. */
        error("mode: cannot change after boot\n");
        return;
    }
    if (*name != '\0') {
        if (vga_set_mode(name) < 0) {
            error("mode: %s: no such mode\n", name);
        }
        return;
    }
    if (vga_mode_name(0) == NULL) {
        kprintf("  %s  current\n", vga_mode());   /* nothing else to list */
        return;
    }
    list(vga_mode_name, vga_mode());
}

/* Text the way echo and greet print it. Three names in braces mean
   something: {bold} switches to the highlight colour, and the next one
   back; {n} starts a new line; {uptime} is the time since the loader
   started. Anything else is printed as it is. */
#define HIGHLIGHT "\033[92m"           /* bright green */

void proc_print(const char *text) {
    bool bold = false;

    for (const char *p = text; *p != '\0'; p++) {
        if (memcmp(p, "{bold}", 6) == 0) {
            kprintf(bold ? "\033[0m" : HIGHLIGHT);
            bold = !bold;
            p += 5;
        } else if (memcmp(p, "{n}", 3) == 0) {
            vga_putc('\n');
            p += 2;
        } else if (memcmp(p, "{uptime}", 8) == 0) {
            uint64_t ms = efi_uptime_ms();

            kprintf("%u.%02us", (unsigned)(ms / 1000), (unsigned)(ms % 1000) / 10);
            p += 7;
        } else {
            vga_putc(*p);
        }
    }
    kprintf(bold ? "\033[0m\n" : "\n");
}

/* greet <text>: keeps text for the kernel to print, as echo would, once
   the boot script is through - so {uptime} is the time the boot took.
   Only booting prints it; set later, it waits for the next boot script
   that never comes. */
static char greeting[160];

static void cmd_greet(char *args) {
    size_t n = strlen(args);

    n = n < sizeof greeting - 1 ? n : sizeof greeting - 1;
    memcpy(greeting, args, n);
    greeting[n] = '\0';
}

void proc_greet(void) {
    if (greeting[0] != '\0') {
        proc_print(greeting);
    }
}

/* Prints its line, which is how /etc/tuxlet/boot says anything. */
static void cmd_echo(char *args) {
    proc_print(args);
}

static void cmd_scale(char *args) {
    const char *name = str_word(&args);

    if (*name == '\0') {
        kprintf("  %s\n", vga_scale());
    } else if (vga_set_scale(name) < 0) {
        error("scale: %s: not a size within %s\n", name, vga_mode());
    }
}

static void cmd_font(char *args) {
    const char *name = str_word(&args);

    if (*name != '\0') {
        if (vga_set_font(name) < 0) {
            error("font: %s: no such size\n", name);
        }
        return;
    }
    list(vga_font_name, vga_font());
}

/* ---- the machine --------------------------------------------------------- */

/* What the OS itself uses, out of the RAM it has - which, as on Linux, is
   the machine's less what the firmware keeps for good. "mem all" is all of
   the machine's, the firmware's share counted in. */
/* mem top: every process, biggest first. */
#define TOP_MAX 32

static void mem_top(void) {
    struct process_info p[TOP_MAX];
    unsigned n = 0;
    uint64_t total = 0;

    while (n < TOP_MAX && process_info(n, &p[n])) {
        total += p[n++].bytes;
    }
    for (unsigned i = 1; i < n; i++) {
        for (unsigned j = i; j > 0 && p[j].bytes > p[j - 1].bytes; j--) {
            struct process_info swap = p[j];

            p[j] = p[j - 1];
            p[j - 1] = swap;
        }
    }
    color(COL_DIM);
    kprintf("    pid      KiB  program\n");
    for (unsigned i = 0; i < n; i++) {
        color(COL_TEXT);
        put_right((unsigned)p[i].pid, 7);
        put_right((unsigned)((p[i].bytes + 1023) / 1024), 9);
        color(COL_DIM);
        kprintf("  %s\n", p[i].name[0] != '\0' ? p[i].name : "?");
    }
    /* Libraries' pages, mapped by every program that has them and counted
       once, here rather than in each. */
    size_t common = shared_bytes();

    color(COL_TEXT);
    put_right((unsigned)((common + 1023) / 1024), 16);
    color(COL_DIM);
    kprintf("  shared\n");
    total += common;
    color(COL_TEXT);
    put_right((unsigned)((total + 1023) / 1024), 16);
    color(COL_DIM);
    kprintf("  in all\n");
    color(COL_TEXT);
}

static void cmd_mem(char *args) {
    struct mem_stats m;
    const char *word = str_word(&args);
    bool all = strcmp(word, "all") == 0;

    if (strcmp(word, "top") == 0) {
        mem_top();
        return;
    }

    mem_get_stats(&m);
    uint32_t ours = m.kernel_kib * 1024 + m.window;

    struct { const char *label; uint32_t bytes; } parts[12];
    unsigned n = 0;

#define PART(l, b) (parts[n].label = (l), parts[n++].bytes = (b))
    if (!all) {
        usage_bar("memory", (ours + 1023) / 1024, m.total_kib, "KiB");
    } else {
        /* The firmware's is what is in use that is not ours: what it keeps
           for good, and while it still runs, its drivers too. Once it is
           gone, what is left over is the kernel's, just not counted apart. */
        uint32_t lent = ours + m.disk_cache;    /* the cache is ours until asked for */
        uint32_t firmware = m.used_kib * 1024 > lent ? m.used_kib * 1024 - lent : 0;
        uint32_t kept = m.firmware_kib * 1024;

        /* The cache is free memory lent out until asked for: not in use. */
        usage_bar("memory", m.used_kib - m.disk_cache / 1024, m.ram_kib, "KiB");
        PART("firmware (kept)", kept);
        if (firmware > kept) {
            PART(mem_ours() ? "other" : "firmware (live)", firmware - kept);
        }
    }
    PART("kernel image", m.image);
    PART("kernel data", m.data);
    PART("kernel stack", m.stack);
    PART("page tables", m.page_tables);
    PART("console", m.console);
    if (m.modules > 0) {
        PART("modules", m.modules);
    }
    if (m.network > 0) {
        PART("network", m.network);
    }
    PART("the program", m.window);
#undef PART

    /* Biggest first. A dozen lines: sorting them any cleverer way is not
       worth the code. */
    for (unsigned i = 1; i < n; i++) {
        for (unsigned j = i; j > 0 && parts[j].bytes > parts[j - 1].bytes; j--) {
            __typeof__(parts[0]) swap = parts[j];

            parts[j] = parts[j - 1];
            parts[j - 1] = swap;
        }
    }
    for (unsigned i = 0; i < n; i++) {
        detail(parts[i].label, parts[i].bytes);
        if (strcmp(parts[i].label, "kernel stack") == 0) {
            kprintf(", %u at peak", m.stack_peak);
        }
        vga_putc('\n');
    }
    color(COL_TEXT);
}

/* Switching the machine off, and starting it again. Both are one call to the
   firmware, so there is nothing for a program on the disk to be: the power
   menu in the status bar types these at the prompt like anything else. */
static void cmd_poweroff(char *args) {
    (void)args;
    efi_power_off();
}

static void cmd_reboot(char *args) {
    (void)args;
    efi_restart();
}

/* The terminal's own clear, so it clears whichever screen this prints on:
   the console, or an xterm's window. */
static void cmd_clear(char *args) {
    (void)args;
    vga_raw("\x1b[H\x1b[2J\x1b[3J");
}

/* ---- Linux's files about the machine ----------------------------------------
 *
 * What a program off a Linux system reads to say what it runs on - neofetch,
 * free, uptime - made when it is opened, from what the kernel knows. */

static void cpu_name(char *vendor, char *brand) {
    uint32_t r[12];

    __asm__ volatile("cpuid" : "=a"(r[3]), "=b"(r[0]), "=c"(r[2]), "=d"(r[1]) : "a"(0));
    memcpy(vendor, r, 12);          /* EBX, EDX, ECX: "GenuineIntel" */
    vendor[12] = '\0';
    brand[0] = '\0';
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(0x80000000));
    if (r[0] < 0x80000004) {
        return;
    }
    for (uint32_t leaf = 0; leaf < 3; leaf++) {
        __asm__ volatile("cpuid" : "=a"(r[leaf * 4]), "=b"(r[leaf * 4 + 1]),
                         "=c"(r[leaf * 4 + 2]), "=d"(r[leaf * 4 + 3]) : "a"(0x80000002 + leaf));
    }
    const char *s = (const char *)r;
    unsigned n = 0;

    while (*s == ' ') {
        s++;                        /* some pad it on the left */
    }
    while (n < 47 && s + n < (const char *)r + 48 && s[n] != '\0') {
        brand[n] = s[n];
        n++;
    }
    while (n > 0 && brand[n - 1] == ' ') {
        n--;                        /* and some on the right */
    }
    brand[n] = '\0';
}

/* The flags line of /proc/cpuinfo, Linux's names for what CPUID says - what
   a program reads to decide whether it can run (SSE3 is "pni"). Only what the
   kernel lets programs use: AVX and its kin need XSAVE turned on, and it is
   left off, so they are not claimed even where the processor has them. */
static void cpu_flags(char *p) {
    static const struct { uint8_t leaf, reg, bit; const char *name; } known[] = {
        /* leaf 1 EDX (reg 3) */
        { 0, 3, 0, "fpu" }, { 0, 3, 1, "vme" }, { 0, 3, 2, "de" }, { 0, 3, 3, "pse" },
        { 0, 3, 4, "tsc" }, { 0, 3, 5, "msr" }, { 0, 3, 6, "pae" }, { 0, 3, 7, "mce" },
        { 0, 3, 8, "cx8" }, { 0, 3, 9, "apic" }, { 0, 3, 11, "sep" }, { 0, 3, 12, "mtrr" },
        { 0, 3, 13, "pge" }, { 0, 3, 14, "mca" }, { 0, 3, 15, "cmov" }, { 0, 3, 16, "pat" },
        { 0, 3, 17, "pse36" }, { 0, 3, 19, "clflush" }, { 0, 3, 23, "mmx" }, { 0, 3, 24, "fxsr" },
        { 0, 3, 25, "sse" }, { 0, 3, 26, "sse2" }, { 0, 3, 28, "ht" },
        /* leaf 0x80000001 EDX */
        { 2, 3, 11, "syscall" }, { 2, 3, 20, "nx" }, { 2, 3, 26, "pdpe1gb" }, { 2, 3, 27, "rdtscp" },
        { 2, 3, 29, "lm" },
        /* leaf 1 ECX (reg 2) */
        { 0, 2, 0, "pni" }, { 0, 2, 1, "pclmulqdq" }, { 0, 2, 9, "ssse3" }, { 0, 2, 13, "cx16" },
        { 0, 2, 19, "sse4_1" }, { 0, 2, 20, "sse4_2" }, { 0, 2, 22, "movbe" }, { 0, 2, 23, "popcnt" },
        { 0, 2, 25, "aes" }, { 0, 2, 30, "rdrand" }, { 0, 2, 31, "hypervisor" },
        /* leaf 0x80000001 ECX */
        { 2, 2, 0, "lahf_lm" }, { 2, 2, 5, "abm" }, { 2, 2, 6, "sse4a" }, { 2, 2, 8, "3dnowprefetch" },
        /* leaf 7 EBX (reg 1) */
        { 1, 1, 0, "fsgsbase" }, { 1, 1, 3, "bmi1" }, { 1, 1, 8, "bmi2" }, { 1, 1, 9, "erms" },
        { 1, 1, 18, "rdseed" }, { 1, 1, 19, "adx" }, { 1, 1, 23, "clflushopt" }, { 1, 1, 29, "sha_ni" },
    };
    uint32_t r[3][4] = { { 0 } };

    __asm__ volatile("cpuid" : "=a"(r[0][0]), "=b"(r[0][1]), "=c"(r[0][2]), "=d"(r[0][3]) : "a"(1));
    __asm__ volatile("cpuid" : "=a"(r[1][0]), "=b"(r[1][1]), "=c"(r[1][2]), "=d"(r[1][3]) : "a"(7), "c"(0));
    __asm__ volatile("cpuid" : "=a"(r[2][0]), "=b"(r[2][1]), "=c"(r[2][2]), "=d"(r[2][3]) : "a"(0x80000001));
    strcpy(p, "flags\t\t:");
    p += strlen(p);
    for (unsigned i = 0; i < sizeof known / sizeof known[0]; i++) {
        if (r[known[i].leaf][known[i].reg] & (1u << known[i].bit)) {
            ksprintf(p, " %s", known[i].name);
            p += strlen(p);
        }
    }
    strcpy(p, "\n");
}

size_t proc_linux(const char *path, char *out, size_t max) {
    static const char *const names[] = {
        "/proc/uptime", "/proc/meminfo", "/proc/cpuinfo",
        "/sys/devices/virtual/dmi/id/product_name", "/sys/devices/virtual/dmi/id/product_version",
        "/sys/class/drm/card0/modes", "/proc/mounts", "/proc/self/mounts",
        "/proc/sys/fs/inotify/max_user_watches", "/proc/sys/fs/inotify/max_user_instances",
        "/proc/self/maps",
    };
    char text[1024], *p = text;
    unsigned which = 0;
    size_t own = process_file(path, out, max);

    if (own != (size_t)-1) {
        return own;
    }
    while (which < sizeof names / sizeof names[0] && strcmp(path, names[which]) != 0) {
        which++;
    }
    if (which == sizeof names / sizeof names[0]) {
        return (size_t)-1;
    }
    if (out == NULL) {
        return 0;
    }
    if (which == 0) {
        uint64_t ms = efi_uptime_ms();

        ksprintf(p, "%u.%02u %u.%02u\n", (unsigned)(ms / 1000), (unsigned)(ms / 10 % 100),
                 (unsigned)(ms / 1000), (unsigned)(ms / 10 % 100));
    } else if (which == 1) {
        struct mem_stats m;

        mem_get_stats(&m);
        ksprintf(p, "MemTotal: %u kB\nMemFree: %u kB\nMemAvailable: %u kB\n"
                 "Buffers: 0 kB\nCached: %u kB\nShmem: 0 kB\nSReclaimable: 0 kB\n"
                 "SwapTotal: 0 kB\nSwapFree: 0 kB\n",
                 m.total_kib, m.free_kib, m.free_kib + m.disk_cache / 1024, m.disk_cache / 1024);
    } else if (which == 2) {
        char vendor[13], brand[48];

        uint32_t sig, unused;

        cpu_name(vendor, brand);
        __asm__ volatile("cpuid" : "=a"(sig), "=b"(unused), "=c"(unused), "=d"(unused) : "a"(1));
        unsigned family = (sig >> 8 & 0xF) + (((sig >> 8 & 0xF) == 0xF) ? (sig >> 20 & 0xFF) : 0);
        unsigned model = (sig >> 4 & 0xF) | ((sig >> 8 & 0xF) >= 6 ? (sig >> 12 & 0xF0) : 0);

        ksprintf(p, "processor\t: 0\nvendor_id\t: %s\ncpu family\t: %u\nmodel\t\t: %u\n"
                 "model name\t: %s\nstepping\t: %u\ncpu MHz\t\t: %u.000\n"
                 "physical id\t: 0\nsiblings\t: 1\ncore id\t\t: 0\ncpu cores\t: 1\n",
                 vendor, family, model, brand, sig & 0xF, (unsigned)(efi_tsc_hz() / 1000000));
        p += strlen(p);
        cpu_flags(p);
        p += strlen(p);
        ksprintf(p, "bogomips\t: %u.00\naddress sizes\t: 39 bits physical, 48 bits virtual\n\n",
                 (unsigned)(efi_tsc_hz() / 500000));
        p = text;
    } else if (which == 6 || which == 7) {
        /* What is mounted where, as the compat module answers statfs: the
           disk at /, and Linux's own filesystems over the kernel's files. */
        strcpy(p, "/dev/root / ext4 rw,relatime 0 0\n"
                  "proc /proc proc rw,nosuid,nodev,noexec,relatime 0 0\n"
                  "sysfs /sys sysfs rw,nosuid,nodev,noexec,relatime 0 0\n"
                  "devtmpfs /dev devtmpfs rw,nosuid,relatime 0 0\n"
                  "devpts /dev/pts devpts rw,nosuid,noexec,relatime 0 0\n"
                  "tmpfs /dev/shm tmpfs rw,nosuid,nodev 0 0\n"
                  "tmpfs /run tmpfs rw,nosuid,nodev 0 0\n");
    } else if (which == 10) {
        process_maps(p, sizeof text);
    } else if (which == 8 || which == 9) {
        strcpy(p, which == 8 ? "8192\n" : "128\n");
    } else if (which == 5) {
        ksprintf(p, "%ux%u\n", vga_pixel_width(), vga_pixel_height());
    } else if (efi_machine(which - 3)[0] != '\0') {
        ksprintf(p, "%s\n", efi_machine(which - 3));
    } else {
        text[0] = '\0';
    }
    size_t len = strlen(text);

    memcpy(out, text, len < max ? len : max);
    return len;
}

/* ---- the table ----------------------------------------------------------- */

static const struct proc_cmd commands[] = {
    { "mode",   "[size]",            cmd_mode   },
    { "scale",  "[size|off]",        cmd_scale  },
    { "font",   "[size]",            cmd_font   },
    { "mem",    "[all|top]",         cmd_mem    },
    { "modman", "[enable|disable <module>|auto <category>|takeover]", module_command },
    { "clear",  "",                  cmd_clear },
    { "echo",   "[text]",            cmd_echo },
    { "greet",  "<text>",            cmd_greet },
    { "tsh",    "[script]",          shell_tsh },
    { "reboot", "",                  cmd_reboot },
    { "poweroff", "",                cmd_poweroff },
};

#define COMMAND_COUNT (sizeof commands / sizeof commands[0])

/* Commands a module added - cache and net - packed at the front. */
#define ADDED 2
static const struct proc_cmd *added[ADDED];

const struct proc_cmd *proc_at(unsigned i) {
    return i < COMMAND_COUNT ? &commands[i]
         : i < COMMAND_COUNT + ADDED ? added[i - COMMAND_COUNT] : NULL;
}

bool proc_add(const struct proc_cmd *cmd) {
    for (unsigned i = 0; i < ADDED; i++) {
        if (added[i] == NULL) {
            added[i] = cmd;
            return true;
        }
    }
    return false;
}

void proc_remove(const struct proc_cmd *cmd) {
    for (unsigned i = 0; i < ADDED; i++) {
        if (added[i] == cmd) {
            memmove(&added[i], &added[i + 1], (ADDED - i - 1) * sizeof added[0]);
            added[ADDED - 1] = NULL;
        }
    }
}

/* What follows prefix in text, or NULL if text does not start with it. */
static const char *past(const char *text, const char *prefix) {
    while (*prefix != '\0') {
        if (*text++ != *prefix++) {
            return NULL;
        }
    }
    return text;
}

/* Strips the folder off a path, if it is the one /ctl names. A path is
   spelled either from the root or against it - the shell looks a bare
   command name up as "/ctl/<name>", and a program's own path arrives
   already resolved - so both spellings are taken here. From inside /ctl,
   a name is spelled against it, and "." is the folder itself. */
static const char *in_proc(const char *path) {
    const char *rest;

    if (path == NULL) {
        return NULL;
    }
    if ((rest = past(path, PROC_DIR "/")) != NULL || (rest = past(path, PROC_NAME "/")) != NULL) {
        return rest;
    }
    if (path[0] == '/' || strcmp(fs_cwd(), PROC_NAME "/") != 0) {
        return NULL;
    }
    if (strcmp(path, ".") == 0) {
        return "";
    }
    return (rest = past(path, "./")) != NULL ? rest : path;
}

bool proc_folder(const char *path) {
    if (path == NULL) {
        return false;
    }
    const char *rest = in_proc(path);

    if (rest != NULL) {
        return *rest == '\0';       /* "/ctl/" is the folder too */
    }
    return strcmp(path, PROC_DIR) == 0 || strcmp(path, PROC_NAME) == 0;
}

const struct proc_cmd *proc_command(const char *path) {
    const char *name = in_proc(path);

    if (name == NULL || *name == '\0') {
        return NULL;
    }
    for (unsigned i = 0; proc_at(i) != NULL; i++) {
        if (strcmp(proc_at(i)->name, name) == 0) {
            return proc_at(i);
        }
    }
    return NULL;
}

size_t proc_read(const struct proc_cmd *cmd, unsigned offset, char *out, size_t max) {
    char line[64];
    size_t len;

    ksprintf(line, "%s %s\n", cmd->name, cmd->usage);
    len = strlen(line);
    if (out == NULL) {
        return len;                 /* asked how big the file is */
    }
    if (offset >= len) {
        return 0;
    }
    len -= offset;
    if (len > max) {
        len = max;
    }
    memcpy(out, line + offset, len);
    return len;
}

void proc_run(const struct proc_cmd *cmd, char *args) {
    if (cmd->usage[0] == '<' && *args == '\0') {
        error("usage: %s %s\n", cmd->name, cmd->usage);
        return;
    }
    cmd->run(args);
}
