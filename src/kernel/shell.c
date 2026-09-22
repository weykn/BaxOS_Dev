#include "shell.h"

#include <stdbool.h>

#include "efi_kernel.h"
#include "fs.h"
#include "io.h"
#include "string.h"
#include "log.h"
#include "proc.h"
#include "syscall.h"
#include "vga.h"

/* Bringing the machine up: the system's configuration, and then the shell.
 *
 * /conf/sys/boot.conf is the machine itself - the size of the screen, the
 * size of the text, the folders standing in for other folders, the picture
 * behind the text - and the kernel reads and runs it, because all of it is
 * the kernel's own state and none of it needs a shell. Only once it is
 * through does /conf/sys/shell.conf say what to start, and whatever that is
 * arrives on a machine already set up the way it was asked to be.
 *
 * The shell itself is a program on the disk and nothing of it is in here:
 * reading a line, splitting it into words, looking a name up on the path,
 * running what it finds, are all its own. */

#define BOOT_CONF     "/conf/sys/boot.conf"
#define SHELL_CONF    "/conf/sys/shell.conf"
#define SHELL_DEFAULT "/pkg/bax-coreutils/sh"

#define CONF_LINE     128       /* the longest line one of them may hold */
#define CONF_DEPTH    4         /* files calling files, at most */

/* ---- the loading screen --------------------------------------------------
 *
 * /conf/sys/boot.conf is what makes the screen the machine's own - its size,
 * the size of the text, the wallpaper - and each of those clears or repaints
 * as it takes effect. There is nothing worth watching in that, so the name
 * and a bar sit in the middle of the screen until the script is through.
 *
 * The bar fills as the lines of boot.conf are run. A line that calls
 * another file counts as one, however much is in it: what is being shown is
 * that something is happening, not an accountant's figure. */

#define SPLASH_NAME "BaxOS"
#define BAR_WIDTH   30

#define COL_ACCENT VGA_LIGHTCYAN
#define COL_DIM    VGA_DARKGRAY
#define COL_FILL   VGA_LIGHTGREEN

static bool        booting;                 /* the loading screen is up */
static const char *path_now = BOOT_CONF;    /* the file being run, to name it */
static unsigned boot_done, boot_total;      /* lines of the script run so far */
static unsigned drawn_width, drawn_rows;    /* the screen it was last drawn on */

static void splash(void) {
    unsigned width = vga_width(), rows = vga_height();
    unsigned name = (width - (sizeof SPLASH_NAME - 1)) / 2;
    unsigned bar = (width - BAR_WIDTH) / 2;
    unsigned filled = boot_total == 0 ? BAR_WIDTH : boot_done * BAR_WIDTH / boot_total;

    /* Clearing paints the whole screen, which is the slowest thing the boot
       does, so it happens only when the screen has changed shape under the
       loading screen - a new mode, or a new text size. Every other frame is
       the handful of cells the bar grew by, and a cell already holding what
       it is given is not drawn again. */
    if (width != drawn_width || rows != drawn_rows) {
        drawn_width = width;
        drawn_rows = rows;
        vga_clear();
        /* Clearing leaves the cursor blinking under the title bar, which has
           nothing to do with loading. Writing its cell takes it off, and
           black on black differs from the blank left there, so the write is
           not skipped as a cell that already holds what it is given. */
        vga_put(0, 1, (uint16_t)(' ' | VGA_ATTR(VGA_BLACK, VGA_BLACK) << 8));
    }
    for (unsigned i = 0; i < sizeof SPLASH_NAME - 1; i++) {
        vga_put(name + i, rows / 2 - 1,
                (uint16_t)((uint8_t)SPLASH_NAME[i] | VGA_ATTR(COL_ACCENT, VGA_BLACK) << 8));
    }
    for (unsigned i = 0; i < BAR_WIDTH; i++) {
        uint8_t attr = VGA_ATTR(i < filled ? COL_FILL : COL_DIM, VGA_BLACK);
        char glyph = i < filled ? '\xDB' : '\xB0';

        vga_put(bar + i, rows / 2 + 1, (uint16_t)((uint8_t)glyph | attr << 8));
    }
}

/* Takes the loading screen down, leaving a clear screen to print on. The
   console calls this itself before the first character anything prints, so
   a boot with something to say - a complaint, or a line of its own - says it
   on a clear screen rather than over the bar. */
static void boot_stop(void) {
    if (booting) {
        booting = false;
        vga_on_print(NULL);
        vga_clear();
    }
}

/* The first line of SHELL_CONF that is neither blank nor a comment, copied
   into out. The built-in default if there is no such file, no such line, or
   it is too long to be a path - a machine whose configuration has gone
   missing still has to come up. */
static const char *shell_wanted(char *out, size_t max) {
    struct fs_file file;
    const char *data;
    unsigned size;

    if (fs_stat(SHELL_CONF, &file) != 0 || file.size == 0 ||
        (data = fs_sector(file.start, 0)) == NULL) {
        return SHELL_DEFAULT;
    }
    size = file.size < FS_SECTOR ? file.size : FS_SECTOR;
    for (unsigned at = 0; at < size;) {
        unsigned end = at, start, stop;

        while (end < size && data[end] != '\n') {
            end++;
        }
        for (start = at; start < end && (data[start] == ' ' || data[start] == '\t'); start++) {
        }
        for (stop = end; stop > start && (data[stop - 1] == ' ' || data[stop - 1] == '\t' ||
                                          data[stop - 1] == '\r'); stop--) {
        }
        if (stop > start && data[start] != '#') {
            size_t n = stop - start;

            if (n + 1 > max) {
                break;              /* not a path: take the one we know */
            }
            memcpy(out, data + start, n);
            out[n] = '\0';
            return out;
        }
        at = end + 1;
    }
    return SHELL_DEFAULT;
}

/* Runs the shell, over and over: one that ends - and only `exit` ends it -
   is started again, there being nothing else for the machine to do. */
/* ---- the system's configuration ------------------------------------------
 *
 * One command a line, '#' a comment, blanks ignored, and `sh <file>` another
 * file read the same way - which is how boot.conf calls the rest of
 * /conf/sys. The commands it can run are the kernel's own, the ones under
 * /proc: the shell's words are not here, and are not wanted, because what
 * this file sets up is the machine rather than the shell. */

static void conf_run(const char *path, unsigned depth);

/* Runs one line of one. */
static void conf_line(char *text, unsigned depth) {
    char at[FS_NAME_LEN] = "/proc/";
    char *args = text;
    char *name;

    while (*text == ' ' || *text == '\t') {
        text++;
    }
    args = text;
    name = str_word(&args);
    if (*name == '\0' || *name == '#') {
        return;
    }
    if (strcmp(name, "sh") == 0) {
        conf_run(args, depth + 1);
        return;
    }
    if (strlen(name) + sizeof "/proc/" > sizeof at) {
        return;
    }
    strcpy(at + sizeof "/proc/" - 1, name);

    const struct proc_cmd *cmd = proc_command(at);

    if (cmd == NULL) {
        boot_stop();
        vga_set_color(VGA_LIGHTRED, VGA_BLACK);
        kprintf("%s: %s: not one of the machine's own commands\n", path_now, name);
        vga_set_color(VGA_LIGHTGRAY, VGA_BLACK);
        return;
    }
    proc_run(cmd, args);
}

static void conf_run(const char *path, unsigned depth) {
    struct efi_boot_services *bs = efi_boot()->system->boot;
    char was[FS_NAME_LEN + 1] = "/";
    char here[FS_NAME_LEN + 1] = "/";
    char text[CONF_LINE];
    const char *outer = path_now;
    struct fs_file file;
    void *script = NULL;
    size_t len = 0;
    unsigned sectors;

    if (depth == CONF_DEPTH || fs_stat(path, &file) != 0) {
        return;
    }
    sectors = (file.size + FS_SECTOR - 1) / FS_SECTOR;
    if (sectors > 0) {
        if (EFI_ERROR(bs->allocate_pool(EFI_LOADER_DATA, sectors * FS_SECTOR, &script)) ||
            fs_read_many(file.start, 0, sectors, script) < 0) {
            if (script != NULL) {
                bs->free_pool(script);
            }
            return;
        }
    }

    /* While it runs, the working folder is its own, so that it can name its
       neighbours - which is how boot.conf calls the rest of /conf/sys. */
    strcpy(was + 1, fs_cwd());
    strcpy(here + 1, file.name);
    *strrchr(here, '/') = '\0';
    fs_chdir(here[0] == '\0' ? "/" : here);
    path_now = file.name;

    for (unsigned at = 0; at <= file.size; at++) {
        char c = at < file.size ? ((const char *)script)[at] : '\n';

        if (c != '\n') {
            if (c != '\r' && len < sizeof text - 1) {
                text[len++] = c;
            }
            continue;
        }
        text[len] = '\0';
        len = 0;
        conf_line(text, depth);
        if (booting && depth == 0) {
            boot_done = at;
            splash();
        }
    }

    path_now = outer;
    fs_chdir(was);
    if (script != NULL) {
        bs->free_pool(script);
    }
}

/* Says how long the machine took to come up, the way boot.conf used to. */
static void boot_time(void) {
    uint64_t ms = efi_uptime_ms();

    vga_set_color(VGA_DARKGRAY, VGA_BLACK);
    kprintf("Booted in %u.%02us\n", (unsigned)(ms / 1000), (unsigned)(ms % 1000) / 10);
    vga_set_color(VGA_LIGHTGRAY, VGA_BLACK);
}

__attribute__((noreturn)) void shell_run(void) {
    char wanted[FS_NAME_LEN];
    struct fs_file file;
    const char *shell, *argv[1];
    uint64_t entry;

    if (fs_stat(BOOT_CONF, &file) == 0) {
        booting = true;
        boot_total = file.size;
        splash();
        vga_on_print(boot_stop);
        conf_run(BOOT_CONF, 0);
    }
    boot_stop();
    boot_time();
    shell = shell_wanted(wanted, sizeof wanted);

    for (;;) {
        int err = fs_stat(shell, &file);

        if (err == 0) {
            err = program_load(&file, &entry);
        }
        if (err < 0) {
            vga_set_color(VGA_LIGHTRED, VGA_BLACK);
            kprintf("%s: %s\n", shell,
                    err == PROGRAM_EINVAL ? "not a program" : fs_error(err));
            vga_set_color(VGA_LIGHTGRAY, VGA_BLACK);
            /* Whatever was asked for is not there; the one that came with
               the machine is, and is worth trying before giving up. */
            if (strcmp(shell, SHELL_DEFAULT) == 0) {
                halt_forever();
            }
            shell = SHELL_DEFAULT;
            continue;
        }
        argv[0] = shell;
        program_run(entry, 1, argv);
        log_flush();
    }
}
