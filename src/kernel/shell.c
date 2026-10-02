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
 * The shell is in here too, as /proc/tsh: reading a line is the console's
 * own line editor, and running a program is what the kernel does anyway, so
 * a shell that is a program of its own costs a region, its page tables and
 * its code for nothing but a loop. Another shell - bash, say - is a program
 * on the disk like any other, which /etc/tuxlet/shell may name instead. */

#define BOOT_DIR      "/etc/tuxlet"
#define BOOT_CONF     BOOT_DIR "/boot"
#define SHELL_CONF    "/etc/tuxlet/shell"

#define SCRIPT_LINE   128       /* the longest line a script may hold */
#define SCRIPT_DEPTH  4         /* scripts running scripts, at most */

static const char *path_now = BOOT_CONF;    /* the file being run, to name it */

bool shell_running(const char *path) {
    return strcmp(path_now, path[0] == '/' ? path + 1 : path) == 0;
}

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
static void env_set(const char *pair) {
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
 * the kernel's from /proc, or a program. `cd`, `export`, `exit` and `help`
 * are its own. There are no
 * pipes, redirections, variables or quoting: bash is on the disk for those. */

#define TSH_LINE  256
#define TSH_WORDS 32

static bool     failed;             /* a command was not there to run */
static unsigned ran;                /* commands run, for the shell script */

static void tsh_help(void) {
    kprintf("Built-in commands:\n"
            "\n"
            "  cd [dir]      change directory\n"
            "  export [N=v]  set a variable, or list them\n"
            "  exit          exit the shell\n"
            "  help          show this message\n"
            "\n"
            "Kernel commands, in /proc:\n"
            "\n ");
    for (unsigned i = 0; proc_at(i) != NULL; i++) {
        kprintf(" %s", proc_at(i)->name);
    }
    kprintf("\n\n");
}

/* Where a command is: argv[0] itself if it has a slash in it, or the first
   folder of PATH holding it - a kernel command in /proc, or a file. */
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

/* Runs one line's words: a kernel command with the rest of the line as its
   arguments, or a program. */
static void tsh_run(unsigned argc, char **argv) {
    char path[FS_NAME_LEN];
    const struct proc_cmd *cmd;
    int code;

    if (!tsh_find(argv[0], path, sizeof path)) {
        path[0] = '\0';
    }
    if ((cmd = path[0] != '\0' ? proc_command(path) : NULL) != NULL) {
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
    code = path[0] != '\0' ? program_start(path, argc, (const char *const *)argv,
                                            env_count, env) : FS_ENOENT;
    if (tracer != NULL) {
        tracer->flush();
    }
    if (code < 0) {
        failed = true;
        vga_set_color(VGA_LIGHTRED, VGA_BLACK);
        kprintf("tsh: %s: %s\n", argv[0],
                code == FS_ENOENT ? "not found" :
                code == PROGRAM_EINVAL ? "not a program" : fs_error(code));
        vga_set_color(VGA_LIGHTGRAY, VGA_BLACK);
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
    } else if (strcmp(argv[0], "cd") == 0) {
        const char *home = shell_env("HOME");

        if (fs_chdir(argc > 1 ? argv[1] : home != NULL ? home : "/") < 0) {
            kprintf("tsh: cd: no such folder\n");
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
    const char *outer = path_now;
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

    path_now = file.name;
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
        if (!tsh_line(text)) {
            break;                  /* `exit`: the script is done */
        }
    }

    scripts--;
    path_now = outer;
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
        line[got] = '\0';
        if (!tsh_line(line)) {
            return;
        }
    }
}

/* /proc/tsh: the shell, until `exit` - or, given a file, that script. Only
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

    /* The boot script loads the modules first (/etc/tuxlet/modules): the
       rest of it may want one - set-bg is the wallpaper's - and the firmware
       goes only where it says `modman takeover`, once the disk and keyboard
       drivers are in. */
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
       machine to do. A script that starts nothing, or names a shell that is
       not there, gets the one in the kernel, which always is. */
    for (;;) {
        unsigned before = ran;

        failed = false;
        script_run(SHELL_CONF);
        if (failed || ran == before) {
            tsh();
        }
    }
}
