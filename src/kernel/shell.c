#include "shell.h"

#include <stdbool.h>

#include "debug.h"
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
 * /conf/sys/boot is the machine itself - the size of the screen, the size of
 * the text, the folders standing in for other folders - and the kernel reads
 * and runs it, because all of it is the kernel's own state and none of it
 * needs a shell. Its lines print as they run, the way a Unix says what it is
 * doing on the way up. Only once it is through does /conf/sys/shell say what
 * to start.
 *
 * The shell itself is a program on the disk and nothing of it is in here:
 * reading a line, splitting it into words, looking a name up on the path,
 * running what it finds, are all its own. */

#define BOOT_CONF     "/conf/sys/boot"
#define SHELL_CONF    "/conf/sys/shell"
#define SHELL_DEFAULT "/pkg/linux-coreutils/bash"

#define CONF_LINE     128       /* the longest line one of them may hold */
#define CONF_DEPTH    4         /* files calling files, at most */

static const char *path_now = BOOT_CONF;    /* the file being run, to name it */

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
 * file read the same way - which is how the boot script calls the rest of
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
       neighbours - which is how the boot script calls its neighbours. */
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
    }

    path_now = outer;
    fs_chdir(was);
    if (script != NULL) {
        bs->free_pool(script);
    }
}

/* Says how long the machine took to come up, the way a Unix does before it
   hands the screen to a shell. */
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
        conf_run(BOOT_CONF, 0);
    }
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
        /* No environment of its own: the first program gets the machine's,
           and everything it starts inherits what it passes on. */
        program_run(entry, 1, argv, 0, NULL, true);
        log_flush();
    }
}
