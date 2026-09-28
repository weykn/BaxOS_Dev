#include "proc.h"

#include <stdint.h>

#include "bg.h"
#include "efi_kernel.h"
#include "ata.h"
#include "fs.h"
#include "log.h"
#include "mem.h"
#include "string.h"
#include "shell.h"
#include "vga.h"

#define PROC_NAME "proc"
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
static void usage_bar(const char *label, unsigned used, unsigned total, const char *unit) {
    /* Rounded up, so any use at all shows. */
    unsigned filled = total == 0 ? 0 : (used * BAR_WIDTH + total - 1) / total;

    color(COL_DIM);
    kprintf("  %s", label);
    pad(strlen(label), 8);
    for (unsigned i = 0; i < BAR_WIDTH; i++) {
        color(i < filled ? COL_FILL : COL_DIM);
        vga_putc(i < filled ? '\xDB' : '\xB0');
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
        } else {
            bg_refresh();           /* the picture is the size of the screen */
        }
        return;
    }
    if (vga_mode_name(0) == NULL) {
        kprintf("  %s  current\n", vga_mode());   /* nothing else to list */
        return;
    }
    list(vga_mode_name, vga_mode());
}

/* Prints its line, which is how /etc/tuxlet/boot says anything. */
static void cmd_echo(char *args) {
    kprintf("%s\n", args);
}

static void cmd_scale(char *args) {
    const char *name = str_word(&args);

    if (*name == '\0') {
        kprintf("  %s\n", vga_scale());
    } else if (vga_set_scale(name) < 0) {
        error("scale: %s: not a size within %s\n", name, vga_mode());
    } else {
        bg_refresh();               /* the picture is the size of the screen */
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

/* Reads "0.5", "1", "100" or "50%" as a percentage. Anything unreadable is
   taken as none of the picture at all, which shows rather than hides the
   mistake. */
static unsigned percent(const char *text) {
    unsigned whole = 0, part = 0, scale = 1;

    while (*text >= '0' && *text <= '9') {
        whole = whole * 10 + (unsigned)(*text++ - '0');
    }
    if (*text == '.') {
        text++;
        while (*text >= '0' && *text <= '9' && scale < 100) {
            part = part * 10 + (unsigned)(*text++ - '0');
            scale *= 10;
        }
    }
    if (*text == '%') {
        return whole;               /* already a percentage */
    }
    /* A fraction: 0.5 is half, and 1 is all of it. */
    return whole * 100 + part * 100 / scale;
}

static void cmd_set_bg(char *args) {
    const char *path = str_word(&args);
    const char *alpha = str_word(&args);
    int err;

    if (strcmp(path, "none") == 0 || strcmp(path, "off") == 0) {
        bg_clear();
        return;
    }
    err = bg_set(path, *alpha == '\0' ? 25 : percent(alpha));
    if (err < 0) {
        error("set-bg: %s: %s\n", path,
              err == BG_EFORMAT ? "not a PNG this can read" :
              err == BG_EMEMORY ? "not enough memory" :
              err == BG_EDATA ? "damaged" : fs_error(err));
    }
}

/* ---- the machine --------------------------------------------------------- */

/* What the OS itself uses, out of the RAM it has - which, as on Linux, is
   the machine's less what the firmware keeps for good. "mem all" is all of
   the machine's, the firmware's share counted in. */
static void cmd_mem(char *args) {
    struct mem_stats m;
    bool all = strcmp(str_word(&args), "all") == 0;

    mem_get_stats(&m);
    uint32_t ours = m.kernel_kib * 1024 + m.window;

    struct { const char *label; uint32_t bytes; } parts[12];
    unsigned n = 0;

#define PART(l, b) (parts[n].label = (l), parts[n++].bytes = (b))
    if (!all) {
        usage_bar("memory", (ours + 1023) / 1024, m.total_kib, "KiB");
    } else {
        /* The firmware's is what is in use that is not ours: what it keeps
           for good, and while it still runs, its drivers too. */
        uint32_t firmware = m.used_kib * 1024 > ours ? m.used_kib * 1024 - ours : 0;
        uint32_t kept = m.firmware_kib * 1024;

        usage_bar("memory", m.used_kib, m.ram_kib, "KiB");
        PART("firmware (kept)", kept);
        if (firmware > kept) {
            PART("firmware (live)", firmware - kept);
        }
    }
    PART("kernel image", m.image);
    PART("kernel data", m.data);
    PART("kernel stack", m.stack);
    PART("page tables", m.page_tables);
    PART("console", m.console);
    if (m.disk_cache > 0) {
        PART("disk cache", m.disk_cache);
    }
    if (m.wallpaper > 0) {
        PART("wallpaper", m.wallpaper);
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

static void cmd_clear(char *args) {
    (void)args;
    vga_clear();
}

/* How long the machine has been up, counted from the first thing the loader
   did - so it covers loading the kernel, not only running it. Shown at the
   scale that reads best: a fresh boot in seconds and hundredths, an old one
   in the units that matter. */
/* ---- the disk cache ------------------------------------------------------
 *
 * Every program is the same loader and the same C library read off the disk
 * again, and a read costs about the same whatever its size: three hundred
 * microseconds of round trip through the firmware. Keeping them is what makes
 * one command as quick as the last.
 *
 * It costs megabytes, so it is off until it is asked for - and asking also
 * reads the loader and the library in, so that the first program after is as
 * quick as the second. */

#define LOADER  "/usr/lib/ld-linux-x86-64.so.2"
#define LIBRARY "/usr/lib/libc.so.6"

static size_t cache_preload(const char *path) {
    struct fs_file file;

    if (fs_stat(path, &file) != 0 || file.size == 0) {
        return 0;
    }
    return ata_cache_read(file.start, (file.size + FS_SECTOR - 1) / FS_SECTOR);
}

/* A size as "512K", "3M" or "1G", or a bare number of bytes. */
static bool parse_bytes(const char *text, size_t *out) {
    size_t n = 0;

    if (*text < '0' || *text > '9') {
        return false;
    }
    while (*text >= '0' && *text <= '9') {
        n = n * 10 + (size_t)(*text++ - '0');
    }
    switch (*text) {
    case 'k': case 'K': n <<= 10; text++; break;
    case 'm': case 'M': n <<= 20; text++; break;
    case 'g': case 'G': n <<= 30; text++; break;
    }
    *out = n;
    return *text == '\0';
}

#define CACHE_DEFAULT (3 << 20)

static size_t cache_bytes = CACHE_DEFAULT;  /* what `cache on` gives it */

/* Gives the cache cache_bytes, and the loader and C library to start with -
   then the file table and its index, if what is left has room for them:
   every path looked up reads them, and a line pushed out for them would
   only be read back. */
static void cache_start(void) {
    uint32_t lba[2];
    unsigned count[2];

    if (cache_bytes / 1024 > mem_free_kib()) {
        error("cache: not enough memory\n");
        return;
    }
    if (ata_cache_size(cache_bytes) > 0) {
        cache_preload(LOADER);
        cache_preload(LIBRARY);
        if (fs_runs(lba, count) == 0) {
            for (unsigned i = 0; i < 2; i++) {
                /* Whole lines are what it holds, and a run can straddle one
                   more than its size says. */
                size_t need = ((size_t)count[i] + 2 * 256) * 512;

                if (ata_cache_room() - ata_cache_held() >= need) {
                    ata_cache_read(lba[i], count[i]);
                }
            }
        }
    }
}

static void cmd_cache(char *args) {
    const char *what = str_word(&args);
    bool on = ata_cache_room() > 0;

    if (strcmp(what, "on") == 0) {
        cache_start();
    } else if (strcmp(what, "off") == 0) {
        ata_cache_size(0);
    } else if (*what != '\0') {
        size_t bytes;

        if (!parse_bytes(what, &bytes)) {
            error("usage: cache [on|off|size]\n");
            return;
        }
        cache_bytes = bytes;
        if (on) {
            cache_start();          /* a new size for one already running */
        }
    } else if (!on) {
        kprintf("  off, %u KiB\n", (unsigned)(cache_bytes / 1024));
    } else {
        usage_bar("cache", (unsigned)(ata_cache_held() / 1024),
                  (unsigned)(ata_cache_room() / 1024), "KiB");
    }
}

/* Anything after it goes in front, which is how /etc/tuxlet/boot says how long
   the machine took to come up. */
static void cmd_uptime(char *args) {
    uint64_t ms = efi_uptime_ms();
    unsigned seconds = (unsigned)(ms / 1000);

    if (*args != '\0') {
        kprintf("%s ", args);
    }
    if (seconds < 60) {
        kprintf("%u.%02us\n", seconds, (unsigned)(ms % 1000) / 10);
    } else if (seconds < 3600) {
        kprintf("%um %02us\n", seconds / 60, seconds % 60);
    } else {
        kprintf("%uh %02um\n", seconds / 3600, seconds / 60 % 60);
    }
}

/* ---- the log -------------------------------------------------------------
 *
 * What is on screen is the ring: the last few dozen calls, whoever made
 * them. What has been written out is on the disk, under /var/log, a file per
 * program, put there whenever the machine is idle. */

static void cmd_log(char *args) {
    const char *what = str_word(&args);

    if (strcmp(what, "clear") == 0) {
        log_clear();
        return;
    }
    if (strcmp(what, "on") == 0 || strcmp(what, "off") == 0) {
        log_enable(strcmp(what, "on") == 0);
        return;
    }
    if (*what != '\0') {
        error("usage: log [on|off|clear]\n");
        return;
    }
    for (unsigned i = 0; i < log_count(); i++) {
        const struct log_entry *e = log_get(i);
        char text[LOG_LINE];

        log_format(e, text);
        color(COL_ACCENT);
        kprintf("%s", log_who(e));
        color(COL_TEXT);
        vga_puts(text);
    }
    color(COL_DIM);
    kprintf("  %u call%s since boot", log_total(), log_total() == 1 ? "" : "s");
    if (log_total() > log_count()) {
        kprintf(", last %u held", log_count());
    }
    if (!log_enabled()) {
        vga_puts(", logging off");
    } else if (log_dropped() > 0) {
        kprintf(", %u dropped", log_dropped());
    }
    vga_putc('\n');
    color(COL_TEXT);
}

/* ---- the table ----------------------------------------------------------- */

static const struct proc_cmd commands[] = {
    { "mode",   "[size]",            cmd_mode   },
    { "scale",  "[size|off]",        cmd_scale  },
    { "font",   "[size]",            cmd_font   },
    { "set-bg", "<file> [alpha]",    cmd_set_bg },
    { "mem",    "[all]",             cmd_mem    },
    { "uptime", "",                  cmd_uptime },
    { "log",    "[on|off|clear]",    cmd_log },
    { "cache",  "[on|off|size]",     cmd_cache },
    { "clear",  "",                  cmd_clear },
    { "echo",   "[text]",            cmd_echo },
    { "tsh",    "",                  shell_tsh },
    { "reboot", "",                  cmd_reboot },
    { "poweroff", "",                cmd_poweroff },
};

#define COMMAND_COUNT (sizeof commands / sizeof commands[0])

const struct proc_cmd *proc_at(unsigned i) {
    return i < COMMAND_COUNT ? &commands[i] : NULL;
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

/* Strips the folder off a path, if it is the one /proc names. A path is
   spelled either from the root or against it - the shell looks a bare
   command name up as "/proc/<name>", and a program's own path arrives
   already resolved - so both spellings are taken here. */
static const char *in_proc(const char *path) {
    const char *rest;

    if (path == NULL) {
        return NULL;
    }
    rest = past(path, PROC_DIR "/");
    return rest != NULL ? rest : past(path, PROC_NAME "/");
}

bool proc_folder(const char *path) {
    if (path == NULL) {
        return false;
    }
    const char *rest = in_proc(path);

    if (rest != NULL) {
        return *rest == '\0';       /* "/proc/" is the folder too */
    }
    return strcmp(path, PROC_DIR) == 0 || strcmp(path, PROC_NAME) == 0;
}

const struct proc_cmd *proc_command(const char *path) {
    const char *name = in_proc(path);

    if (name == NULL || *name == '\0') {
        return NULL;
    }
    for (unsigned i = 0; i < COMMAND_COUNT; i++) {
        if (strcmp(commands[i].name, name) == 0) {
            return &commands[i];
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
