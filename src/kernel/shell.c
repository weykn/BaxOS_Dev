#include "shell.h"

#include <stdbool.h>

#include "debug.h"
#include "efi_kernel.h"
#include "mem.h"
#include "fs.h"
#include "io.h"
#include "string.h"
#include "log.h"
#include "proc.h"
#include "console.h"
#include "syscall.h"
#include "vga.h"

/* Bringing the machine up: the system's configuration, and then the shell.
 *
 * /etc/tuxlet/boot is the machine itself - the size of the screen, the size of the
 * text, the wallpaper - and the kernel reads and runs it, because all of it
 * is the kernel's own state and none of it needs a shell. Its lines print as they run, the way a Unix says what it is
 * doing on the way up. Only once it is through does /etc/tuxlet/shell say what to
 * start.
 *
 * The shell is in here too, as /proc/tsh: reading a line is the console's
 * own line editor, and running a program is what the kernel does anyway, so
 * a shell that is a program of its own costs a region, its page tables and
 * its code for nothing but a loop. Another shell - bash, say - is a program
 * on the disk like any other, which /etc/tuxlet/shell may name instead. */

#define BOOT_CONF     "/etc/tuxlet/boot"
#define SHELL_CONF    "/etc/tuxlet/shell"
#define SHELL_DEFAULT "/proc/tsh"
#define HOME          "/root"   /* root's, as on Linux; /home is for users */

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
 * /etc. The commands it can run are the kernel's own, the ones under
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
        kprintf("%s: %s: not found\n", path_now, name);
        vga_set_color(VGA_LIGHTGRAY, VGA_BLACK);
        return;
    }
    proc_run(cmd, args);
}

static void conf_run(const char *path, unsigned depth) {
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
        if ((script = mem_alloc(sectors * FS_SECTOR)) == NULL ||
            fs_read_many(file.start, 0, sectors, script) < 0) {
            if (script != NULL) {
                mem_free(script);
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
        mem_free(script);
    }
}

/* ---- tsh -------------------------------------------------------------------
 *
 * As small as a shell can be and still be one: it reads a line, splits it
 * into words on blanks, and runs the first with the rest as its arguments -
 * a command of the kernel's from /proc, or a program from /usr/bin, or
 * whatever path it names. `cd`, `exit` and `help` are its own. There are no
 * pipes, redirections, variables or quoting: bash is on the disk for those. */

#define TSH_LINE  256
#define TSH_WORDS 32

static void tsh_help(void) {
    kprintf("Built-in commands:\n"
            "\n"
            "  cd [dir]      change directory\n"
            "  exit          exit the shell\n"
            "  help          show this message\n"
            "\n"
            "Kernel commands, in /proc:\n"
            "\n ");
    for (unsigned i = 0; proc_at(i) != NULL; i++) {
        kprintf(" %s", proc_at(i)->name);
    }
    kprintf("\n\nOther commands are in /usr/bin\n");
}

/* Runs one line's words: a kernel command with the rest of the line as its
   arguments, or a program. */
static void tsh_run(unsigned argc, char **argv) {
    char path[FS_NAME_LEN] = "/proc/";
    const struct proc_cmd *cmd = NULL;
    int code;

    if (strchr(argv[0], '/') == NULL && strlen(argv[0]) + sizeof "/usr/bin/" <= sizeof path) {
        strcpy(path + sizeof "/proc/" - 1, argv[0]);
        cmd = proc_command(path);
        strcpy(path, "/usr/bin/");
        strcpy(path + sizeof "/usr/bin/" - 1, argv[0]);
    } else if (strlen(argv[0]) < sizeof path) {
        strcpy(path, argv[0]);
    } else {
        path[0] = '\0';
    }
    if (cmd != NULL) {
        /* The words back into one line, which is how a command takes them. */
        char *args = argc > 1 ? argv[1] : argv[0] + strlen(argv[0]);

        for (unsigned i = 2; i < argc; i++) {
            for (char *gap = argv[i - 1] + strlen(argv[i - 1]); gap < argv[i]; gap++) {
                *gap = ' ';
            }
        }
        proc_run(cmd, args);
        return;
    }
    code = path[0] != '\0' ? program_start(path, argc, (const char *const *)argv) : FS_ENOENT;
    log_flush();
    if (code < 0) {
        vga_set_color(VGA_LIGHTRED, VGA_BLACK);
        kprintf("tsh: %s: %s\n", argv[0],
                code == FS_ENOENT ? "not found" :
                code == PROGRAM_EINVAL ? "not a program" : fs_error(code));
        vga_set_color(VGA_LIGHTGRAY, VGA_BLACK);
    }
}

/* Reads and runs lines until `exit`. */
static void tsh(void) {
    static char line[TSH_LINE];
    char *argv[TSH_WORDS + 1];

    for (;;) {
        const char *cwd = fs_cwd();
        size_t cwd_n = strlen(cwd);
        unsigned argc = 0;
        uint64_t got;

        console_reset();            /* whatever the last program left it as */
        vga_putc('/');
        for (size_t i = 0; i + 1 < cwd_n; i++) {
            vga_putc(cwd[i]);       /* "root/" is shown as /root */
        }
        kprintf(" $ ");
        got = console_read(line, sizeof line - 1);
        line[got] = '\0';

        for (char *p = line; *p != '\0' && argc < TSH_WORDS;) {
            while (*p == ' ' || *p == '\t' || *p == '\n') {
                *p++ = '\0';
            }
            if (*p == '\0') {
                break;
            }
            argv[argc++] = p;
            while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n') {
                p++;
            }
        }
        argv[argc] = NULL;
        if (argc == 0 || argv[0][0] == '#') {
            continue;
        }
        if (strcmp(argv[0], "exit") == 0) {
            return;
        } else if (strcmp(argv[0], "help") == 0) {
            tsh_help();
        } else if (strcmp(argv[0], "cd") == 0) {
            if (fs_chdir(argc > 1 ? argv[1] : HOME) < 0) {
                kprintf("tsh: cd: no such folder\n");
            }
        } else {
            tsh_run(argc, argv);
        }
    }
}

/* What /proc/tsh does when a program runs it: there is one of it, and it is
   already underneath. */
void shell_tsh(char *args) {
    (void)args;
    kprintf("tsh: already the machine's shell - exit to get back to it\n");
}

__attribute__((noreturn)) void shell_run(void) {
    char wanted[FS_NAME_LEN];
    struct fs_file file;
    const char *shell, *argv[1];
    uint64_t entry;

    if (fs_stat(BOOT_CONF, &file) == 0) {
        conf_run(BOOT_CONF, 0);
    }
    /* The screen mode is set, which only the firmware can do: now it can go,
       and everything it held with it. */
    efi_leave();
    shell = shell_wanted(wanted, sizeof wanted);
    fs_chdir(HOME);                 /* a shell starts at home, as a login does */

    for (;;) {
        if (strcmp(shell, SHELL_DEFAULT) == 0) {
            tsh();                  /* ends only with `exit`: started again */
            continue;
        }
        int err = fs_stat(shell, &file);

        if (err == 0) {
            err = program_load(&file, &entry);
        }
        if (err < 0) {
            vga_set_color(VGA_LIGHTRED, VGA_BLACK);
            kprintf("%s: %s\n", shell,
                    err == PROGRAM_EINVAL ? "not a program" : fs_error(err));
            vga_set_color(VGA_LIGHTGRAY, VGA_BLACK);
            /* Whatever was asked for is not there; the one in the kernel
               always is. */
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
