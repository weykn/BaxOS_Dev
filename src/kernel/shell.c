#include "shell.h"

#include <stdbool.h>

#include "debug.h"
#include "driver.h"
#include "mem.h"
#include "fs.h"
#include "io.h"
#include "string.h"
#include "proc.h"
#include "console.h"
#include "syscall.h"
#include "vga.h"

/* Bringing the machine up: the system's configuration, and then the shell.
 *
 * /etc/tuxlet/boot is the machine itself - the modules, the size of the
 * screen, the size of the text - and it is a tsh script, run before
 * anything else, in /etc/tuxlet, so that it calls the rest of it as
 * `tsh <file>`. Its lines
 * print as they run, the way a Unix says what it is doing on the way up.
 * Then /etc/tuxlet/shell, a script too, starts the shell: `tsh`, or a
 * program such as bash.
 *
 * Where commands are looked for, and what every program is started with,
 * is the environment - /etc/tuxlet/env, which the boot script runs first,
 * exports PATH, HOME and the rest.
 *
 * The shell is in here too, as /ctl/tsh: reading a line is the console's
 * own line editor, and running a program is what the kernel does anyway, so
 * a shell that is a program of its own costs a region, its page tables and
 * its pages for nothing but a loop. Another shell - bash, say - is a
 * program on the disk like any other, which /etc/tuxlet/shell may name. */

#define BOOT_DIR      "/etc/tuxlet"
#define BOOT_CONF     BOOT_DIR "/boot"
#define SHELL_CONF    "/etc/tuxlet/shell"

#define SCRIPT_LINE   128       /* the longest line a script may hold */
#define SCRIPT_DEPTH  4         /* scripts running scripts, at most */

/* The kernel's stack is 8 KiB, and a script can run a script that loads a
   module through the firmware: each built-in keeps its buffers to itself
   rather than every line paying for all of them. */
#define NOINLINE __attribute__((noinline))

static bool memcmp_n(const char *a, const char *b, size_t n) {
    while (n > 0 && *a == *b) {
        a++;
        b++;
        n--;
    }
    return n == 0;
}

/* ---- the environment -------------------------------------------------------
 *
 * NAME=value strings, set by `export` - in /etc/tuxlet/env at boot - and
 * handed to every program tsh starts, which passes them on from there. */

#define ENV_MAX   16
#define ENV_BYTES 512

static char        env_text[ENV_BYTES];
static const char *env[ENV_MAX + 1];
static unsigned    env_count;

const char *shell_env(const char *name) {
    size_t n = strlen(name);

    for (unsigned i = 0; i < env_count; i++) {
        const char *e = env[i];
        size_t k = 0;

        while (k < n && e[k] == name[k]) {
            k++;
        }
        if (k == n && e[n] == '=') {
            return e + n + 1;
        }
    }
    return NULL;
}

/* Sets NAME=value, replacing what NAME had. The text is packed again from
   the list each time, so a value changed over and over does not use it up. */
static NOINLINE void env_set(const char *pair) {
    const char *eq = strchr(pair, '=');
    char packed[ENV_BYTES];
    size_t used = 0, n;

    if (eq == NULL || eq == pair) {
        kprintf("export: %s: not NAME=value\n", pair);
        return;
    }
    n = (size_t)(eq - pair);
    for (unsigned i = 0; i < env_count;) {
        if (strlen(env[i]) > n && env[i][n] == '=' && memcmp_n(env[i], pair, n)) {
            memmove(&env[i], &env[i + 1], (env_count - i) * sizeof env[0]);
            env_count--;
        } else {
            i++;
        }
    }
    if (env_count == ENV_MAX) {
        kprintf("export: no room for %s\n", pair);
        return;
    }
    env[env_count++] = pair;
    for (unsigned i = 0; i < env_count; i++) {
        size_t len = strlen(env[i]) + 1;

        if (used + len > sizeof packed) {
            kprintf("export: no room for %s\n", pair);
            env_count--;
            return;
        }
        memcpy(packed + used, env[i], len);
        used += len;
    }
    memcpy(env_text, packed, used);
    for (unsigned i = 0, at = 0; i < env_count; i++) {
        env[i] = env_text + at;
        at += (unsigned)strlen(env_text + at) + 1;
    }
    env[env_count] = NULL;
}

/* ---- tsh -------------------------------------------------------------------
 *
 * As small as a shell can be and still be one: it reads a line, splits it
 * into words on blanks, and runs the first with the rest as its arguments -
 * whatever path it names, or the first of that name in PATH: a command of
 * the kernel's from /ctl, or a program. `cd`, `ls`, `cat`, `cp`, `mv`,
 * `rm`, `mkdir`, `put`, `export`, `exit` and `help` are its own. There are
 * no pipes, redirections, variables or quoting: bash is there to install
 * for those. */

#define TSH_LINE  256
#define TSH_WORDS 32
#define CP_RUN    32            /* sectors a copy moves at a time */

static bool     failed;             /* a command was not there to run */
static unsigned ran;                /* commands run, for the shell script */

/* A complaint about name, in red: what went wrong with it. */
static void tsh_fail(const char *what, const char *name, const char *why) {
    vga_set_color(VGA_LIGHTRED, VGA_BLACK);
    kprintf("tsh: %s%s%s: %s\n", what, *what != '\0' ? ": " : "", name, why);
    vga_set_color(VGA_LIGHTGRAY, VGA_BLACK);
}

static NOINLINE void tsh_help(void) {
    kprintf("built-in: cd ls cat cp mv rm mkdir put export exit help\n/ctl:");
    for (unsigned i = 0; proc_at(i) != NULL; i++) {
        kprintf(" %s", proc_at(i)->name);
    }
    vga_putc('\n');
}

/* The names in a folder, a folder's with a slash after it. */
static NOINLINE void tsh_ls(const char *path) {
    char folder[FS_NAME_LEN];
    struct fs_file entry;
    size_t cursor = 0, index;

    if (proc_folder(path)) {
        for (unsigned i = 0; proc_at(i) != NULL; i++) {
            kprintf("%s\n", proc_at(i)->name);
        }
        return;
    }
    if (fs_folder(path, folder, sizeof folder) != 0) {
        tsh_fail("ls", path, "no such folder");
        return;
    }
    while (fs_list(folder, &cursor, &entry, &index) == 0) {
        kprintf("%s\n", fs_inside(folder, entry.name));
    }
}

static NOINLINE void tsh_cat(unsigned argc, char **argv) {
    struct fs_file file;

    for (unsigned i = 1; i < argc; i++) {
        if (fs_stat(argv[i], &file) != 0) {
            tsh_fail("cat", argv[i], "not found");
            continue;
        }
        for (uint32_t at = 0; at < file.size; at += FS_SECTOR) {
            const char *sector = fs_sector(file.start, at / FS_SECTOR);
            uint32_t n = file.size - at < FS_SECTOR ? file.size - at : FS_SECTOR;

            for (uint32_t k = 0; sector != NULL && k < n; k++) {
                vga_putc(sector[k]);
            }
        }
    }
}

/* Where from goes when it is put at to: into to, if that is a folder. */
static const char *tsh_dest(const char *from, const char *to, char *out) {
    const char *base = strrchr(from, '/');
    size_t n = strlen(to);

    base = base != NULL ? base + 1 : from;
    if (fs_folder(to, out, FS_NAME_LEN) != 0 || n + strlen(base) + 2 > FS_NAME_LEN) {
        return to;
    }
    memcpy(out, to, n);
    n -= n > 1 && to[n - 1] == '/';
    out[n] = '/';
    strcpy(out + n + 1, base);
    return out;
}

/* A file's contents into another, a run of sectors at a time. */
static NOINLINE void tsh_cp(const char *from, const char *to) {
    char dest[FS_NAME_LEN];
    struct fs_file file, there;
    char *run;
    int err;

    if (fs_stat(from, &file) != 0) {
        tsh_fail("cp", from, "not found");
        return;
    }
    to = tsh_dest(from, to, dest);
    if (fs_stat(to, &there) == 0 && there.start == file.start && file.size > 0) {
        tsh_fail("cp", to, "the same file");
        return;
    }
    if ((run = mem_alloc(CP_RUN * FS_SECTOR)) == NULL) {
        tsh_fail("cp", to, "out of memory");
        return;
    }
    err = fs_write(to, "", 0);
    for (uint32_t at = 0, n; err == 0 && at < file.size; at += n) {
        n = file.size - at < CP_RUN * FS_SECTOR ? file.size - at : CP_RUN * FS_SECTOR;
        err = fs_read_many(file.start, at / FS_SECTOR, (n + FS_SECTOR - 1) / FS_SECTOR, run) < 0
            ? FS_EIO : fs_write_at(to, at, run, n);
    }
    mem_free(run);
    if (err != 0) {
        tsh_fail("cp", to, fs_error(err));
    }
}

static NOINLINE void tsh_mv(const char *from, const char *to) {
    char dest[FS_NAME_LEN];
    int err = fs_rename(from, tsh_dest(from, to, dest));

    if (err != 0) {
        tsh_fail("mv", from, fs_error(err));
    }
}

/* Where a command is: argv[0] itself if it has a slash in it, or the first
   folder of PATH holding it - a kernel command in /ctl, or a file. */
static bool tsh_find(const char *name, char *out, size_t max) {
    const char *dirs = shell_env("PATH");
    struct fs_file file;

    if (strchr(name, '/') != NULL) {
        if (strlen(name) >= max) {
            return false;
        }
        strcpy(out, name);
        return true;
    }
    while (dirs != NULL && *dirs != '\0') {
        const char *end = strchr(dirs, ':');
        size_t n = end != NULL ? (size_t)(end - dirs) : strlen(dirs);

        if (n > 0 && n + 1 + strlen(name) < max) {
            memcpy(out, dirs, n);
            out[n] = '/';
            strcpy(out + n + 1, name);
            if (proc_command(out) != NULL || fs_stat(out, &file) == 0) {
                return true;
            }
        }
        dirs = end != NULL ? end + 1 : dirs + n;
    }
    return false;
}

/* Words from, to the end of the line, back into one string: what a
   command takes as its arguments, or put as its text. */
static char *rest_of(unsigned argc, char **argv, unsigned from) {
    for (unsigned i = from + 1; i < argc; i++) {
        for (char *gap = argv[i - 1] + strlen(argv[i - 1]); gap < argv[i]; gap++) {
            *gap = ' ';
        }
    }
    return from < argc ? argv[from] : argv[from - 1] + strlen(argv[from - 1]);
}

/* put <file> [text]: the file holds text and a newline, or nothing. */
static NOINLINE void tsh_put(unsigned argc, char **argv) {
    char *text = rest_of(argc, argv, 2);
    size_t n = strlen(text);
    int err;

    if (n > 0) {
        text[n++] = '\n';          /* over the NUL: the line has room for it */
    }
    if ((err = fs_write(argv[1], text, n)) < 0) {
        tsh_fail("put", argv[1], fs_error(err));
    }
}

/* Runs one line's words: a kernel command with the rest of the line as its
   arguments, or a program. */
static NOINLINE void tsh_run(unsigned argc, char **argv) {
    char path[FS_NAME_LEN];
    const struct proc_cmd *cmd;
    int code;

    if (!tsh_find(argv[0], path, sizeof path)) {
        path[0] = '\0';
    }
    if ((cmd = path[0] != '\0' ? proc_command(path) : NULL) != NULL) {
        proc_run(cmd, rest_of(argc, argv, 1));
        return;
    }
    code = path[0] != '\0' ? program_start(path, argc, (const char *const *)argv,
                                            env_count, env) : FS_ENOENT;
    if (tracer != NULL) {
        tracer->flush();
    }
    if (code < 0) {
        failed = true;
        tsh_fail("", argv[0], code == FS_ENOENT ? "not found" :
                              code == PROGRAM_EINVAL ? "not a program" : fs_error(code));
    }
}

/* Runs one line, typed or out of a script: split into words on blanks, a
   '#' starting a comment. False for `exit`. */
static bool tsh_line(char *line) {
    char *argv[TSH_WORDS + 1];
    unsigned argc = 0;

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
        return true;
    }
    ran++;
    if (strcmp(argv[0], "exit") == 0) {
        return false;
    } else if (strcmp(argv[0], "help") == 0) {
        tsh_help();
    } else if (strcmp(argv[0], "ls") == 0) {
        tsh_ls(argc > 1 ? argv[1] : "");
    } else if (strcmp(argv[0], "cd") == 0) {
        const char *home = shell_env("HOME");

        if (fs_chdir(argc > 1 ? argv[1] : home != NULL ? home : "/") < 0) {
            tsh_fail("cd", argc > 1 ? argv[1] : "", "no such folder");
        }
    } else if (strcmp(argv[0], "cat") == 0) {
        tsh_cat(argc, argv);
    } else if (strcmp(argv[0], "cp") == 0 || strcmp(argv[0], "mv") == 0) {
        if (argc != 3) {
            kprintf("usage: %s <file> <to>\n", argv[0]);
        } else if (argv[0][0] == 'c') {
            tsh_cp(argv[1], argv[2]);
        } else {
            tsh_mv(argv[1], argv[2]);
        }
    } else if (strcmp(argv[0], "rm") == 0 || strcmp(argv[0], "mkdir") == 0) {
        bool rm = argv[0][0] == 'r';

        for (unsigned i = 1; i < argc; i++) {
            int err = rm ? fs_remove(argv[i]) : fs_mkdir(argv[i]);

            if (err != 0) {
                tsh_fail(argv[0], argv[i], fs_error(err));
            }
        }
    } else if (strcmp(argv[0], "put") == 0) {
        if (argc < 2) {
            kprintf("usage: put <file> [text]\n");
        } else {
            tsh_put(argc, argv);
        }
    } else if (strcmp(argv[0], "export") == 0) {
        for (unsigned i = 0; argc == 1 && i < env_count; i++) {
            kprintf("%s\n", env[i]);
        }
        for (unsigned i = 1; i < argc; i++) {
            env_set(argv[i]);
        }
    } else {
        tsh_run(argc, argv);
    }
    return true;
}

/* A script is lines of tsh, run one after another. */
static unsigned scripts;            /* how many are running, one inside another */

static void script_run(const char *path) {
    char text[SCRIPT_LINE];
    struct fs_file file;
    void *script = NULL;
    size_t len = 0;
    unsigned sectors;

    if (fs_stat(path, &file) != 0) {
        kprintf("tsh: %s: not found\n", path);
        failed = true;
        return;
    }
    if (scripts == SCRIPT_DEPTH) {
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

    scripts++;

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
        /* Nothing after this line but blanks: the script is not needed while
           it runs, which for the shell's own script is for good. */
        size_t rest = at + 1;

        while (rest < file.size && (((const char *)script)[rest] == '\n' ||
                                    ((const char *)script)[rest] == '\r' ||
                                    ((const char *)script)[rest] == ' ')) {
            rest++;
        }
        if (rest >= file.size && script != NULL) {
            mem_free(script);
            script = NULL;
            at = file.size;         /* and the loop ends after it */
        }
        /* The line, before it runs - tsh_line cuts it into words. */
        const char *shown = text;

        while (*shown == ' ' || *shown == '\t') {
            shown++;
        }
        if (*shown != '\0' && *shown != '#') {
            vga_set_color(VGA_DARKGRAY, VGA_BLACK);
            kprintf("+ %s\n", shown);
            vga_set_color(VGA_LIGHTGRAY, VGA_BLACK);
        }
        if (!tsh_line(text)) {
            break;                  /* `exit`: the script is done */
        }
    }

    scripts--;
    if (script != NULL) {
        mem_free(script);
    }
}

/* Reads and runs lines until `exit`. */
static void tsh(void) {
    static char line[TSH_LINE];

    for (;;) {
        const char *cwd = fs_cwd();
        size_t cwd_n = strlen(cwd);
        uint64_t got;

        console_reset();            /* whatever the last program left it as */
        vga_putc('/');
        for (size_t i = 0; i + 1 < cwd_n; i++) {
            vga_putc(cwd[i]);       /* "root/" is shown as /root */
        }
        kprintf(" $ ");
        got = console_read(line, sizeof line - 1);
        if (got == CONSOLE_SIGNAL) {
            console_signal();       /* Ctrl-C: the line is dropped */
            vga_puts("^C\n");
            continue;
        }
        line[got] = '\0';
        if (!tsh_line(line)) {
            return;
        }
    }
}

/* /ctl/tsh: the shell, until `exit` - or, given a file, that script. Only
   from tsh itself, or a script: run by a program, it would be starting
   programs from inside that one's execve. */
void shell_tsh(char *args) {
    const char *path = str_word(&args);

    if (program_running()) {
        kprintf("tsh: already the machine's shell - exit to get back to it\n");
    } else if (*path == '\0') {
        tsh();
    } else {
        script_run(path);
    }
}

__attribute__((noreturn)) void shell_run(void) {
    struct fs_file file;

    /* The boot script loads the modules first, and the firmware goes only
       where it says `modman takeover`, once the disk and keyboard drivers
       are in. */
    /* It runs in its own folder, so that it can call the rest of /etc/tuxlet
       by name - `tsh cache`. */
    if (fs_stat(BOOT_CONF, &file) == 0) {
        fs_chdir(BOOT_DIR);
        script_run(BOOT_CONF);
    }
    const char *home = shell_env("HOME");

    fs_chdir(home != NULL ? home : "/");    /* a shell starts at home, as a login does */

    /* The shell script starts the shell, and when that ends - only `exit`
       ends one - it is started again, there being nothing else for the
       machine to do. Without a script, or one that starts nothing or names
       a shell that is not there, the shell is tsh, which always is. */
    for (;;) {
        unsigned before = ran;

        failed = false;
        if (fs_stat(SHELL_CONF, &file) == 0) {
            script_run(SHELL_CONF);
        }
        if (failed || ran == before) {
            tsh();
        }
    }
}
