#include "util.h"

/* The shell.
 *
 * It is an ordinary program: it reads a line from the console, splits it into
 * words, works out what the first one names, and runs it. Nothing about it is
 * special to the kernel - the kernel starts it because something has to be
 * started, and if it ends the kernel starts it again.
 *
 * A name is one of two things, looked for in this order:
 *
 *   a word this file knows        cd, path, sh, run, set-default, help
 *   a file on the path            a program, ours or one off a Linux system,
 *                                 or one of the kernel's own commands, which
 *                                 are files in /proc
 *
 * The first group is here rather than in /proc because each of them is the
 * shell's own state - where it looks for commands, which command opens which
 * kind of file - or, in cd's case, something that has to outlive the command
 * that did it. Everything the kernel keeps has to run in the kernel, and
 * /proc is how a program reaches that: nothing in this file singles it out,
 * it is simply the first folder on the path.
 *
 * Running a program is one syscall, SYS_spawn, and what it does is in
 * syscall.c: this one's memory is put aside, the other runs in it, and this
 * one gets it back. */

#define LINE      512       /* the longest command line, typed or filled in */
#define PATHS     8         /* folders a bare name is looked for in */
#define DEFAULTS  8         /* extensions with a command to open them */
#define EXT_LEN   8
#define CMD_LEN   16
#define ARGS      32        /* words in a command, the name counted in;
                               PROGRAM_ARGS in the kernel is the same */
#define SH_DEPTH  4         /* scripts calling scripts, at most */
#define SCRIPT    4096      /* the biggest script that can be run */
#define NAME_LEN  48        /* a whole path, as the filesystem holds one */

/* The shell's own settings, read when it starts: where it looks for
   commands, and which command opens which kind of file. A program's settings
   live in /conf/pkg, and this is a program. The machine's own settings are
   /conf/sys/boot.conf, and the kernel has run those before this ever starts. */
#define SH_CONF "/conf/pkg/sh.conf"
#define HELP_FILE   "/pkg/bax-coreutils/HELP.md"

#define GLYPH_PROMPT "\xAF"         /* >>, in the font the console draws */

static char paths[PATHS][NAME_LEN];         /* each with a trailing slash */

static struct {
    char ext[EXT_LEN];
    char cmd[CMD_LEN];
} defaults[DEFAULTS];

static unsigned depth;              /* scripts running inside scripts */
static bool     capturing;          /* inside a $(...), where one cannot start */

static void run_line(char *text, char *out, long out_size);

/* ---- the little that stands in for a C library -------------------------- */

static char lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}

static bool same_ext(const char *a, const char *b) {
    while (*a != '\0' && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Splits the first word off *text: NUL-terminates it, points *text at
   whatever follows the spaces after it, and returns the word. */
static char *take_word(char **text) {
    char *word = *text;
    char *p = word;

    while (*p != '\0' && *p != ' ') {
        p++;
    }
    if (*p != '\0') {
        *p++ = '\0';
    }
    while (*p == ' ') {
        p++;
    }
    *text = p;
    return word;
}

/* ---- running ------------------------------------------------------------ */

/* Runs path with what was typed as its arguments. Returns false if there is
   nothing of that name to run, which is all `run` needs to tell apart; every
   other way it can fail says so itself. */
static bool spawn(const char *path, const char *called, char *args,
                  char *out, long out_size) {
    const char *argv[ARGS];
    unsigned argc = 0;
    long code;

    argv[argc++] = called;
    while (*args != '\0' && argc < ARGS - 1) {
        argv[argc++] = take_word(&args);
    }
    argv[argc] = NULL;
    code = sys_spawn(path, argv, out, out_size);
    if (code == -2) {
        return false;               /* ENOENT: nothing of that name */
    }
    if (code == -8) {
        put_error(called, NULL, "not a program this machine can run");
    } else if (code == -12) {
        put_error(called, NULL, "not enough memory to start it");
    } else if (code < 0) {
        put_error(called, NULL, "could not be run");
    } else if (code != 0) {
        put(RED);
        put(called);
        put(" exited with ");
        put_number((uint64_t)code, 0);
        put(PLAIN "\n");
    }
    return true;
}

/* Runs a file named at the prompt: opened with whatever set-default says for
   its kind, or run as a program. */
static void open_file(char *path, char *name, char *args, char *out, long out_size) {
    char *dot = ufind_last(name, '.');

    if (dot != NULL && ufind(dot, '/') == NULL) {
        for (unsigned i = 0; i < DEFAULTS; i++) {
            if (defaults[i].cmd[0] != '\0' && same_ext(defaults[i].ext, dot + 1)) {
                char text[LINE];

                text[0] = '\0';
                uappend(text, sizeof text, defaults[i].cmd);
                uappend(text, sizeof text, " ");
                uappend(text, sizeof text, name);
                run_line(text, out, out_size);
                return;
            }
        }
    }
    spawn(path, name, args, out, out_size);
}

/* ---- the words this file knows ------------------------------------------ */

static void cmd_cd(char *args) {
    const char *name = take_word(&args);

    if (sys_chdir(*name == '\0' ? "/" : name) < 0) {
        put_error("cd", name, "no such folder");
    }
}

/* Copies dir into out with a trailing slash. False if it will not fit. */
static bool as_folder(const char *dir, char *out, size_t max) {
    size_t n = ulen(dir);

    if (n == 0 || n + 2 > max) {
        return false;
    }
    ucopy(out, dir, n + 1);
    if (out[n - 1] != '/') {
        out[n] = '/';
        out[n + 1] = '\0';
    }
    return true;
}

static void cmd_path(char *args) {
    const char *what = take_word(&args);
    const char *dir = take_word(&args);
    char folder[NAME_LEN];
    unsigned slot = PATHS;

    if (*what == '\0') {
        for (unsigned i = 0; i < PATHS; i++) {
            if (paths[i][0] != '\0') {
                put(ACCENT "  ");
                put(paths[i]);
                put(PLAIN "\n");
            }
        }
        return;
    }
    bool add = usame(what, "add");

    if ((!add && !usame(what, "remove")) || *dir == '\0') {
        put_error("path", NULL, "usage: path [add|remove <folder>]");
        return;
    }
    if (!as_folder(dir, folder, sizeof folder)) {
        put_error("path", dir, "name too long");
        return;
    }
    for (unsigned i = 0; i < PATHS; i++) {
        if (usame(paths[i], folder)) {
            if (!add) {
                paths[i][0] = '\0';
            }
            return;                 /* already on it, or now off it */
        }
        if (paths[i][0] == '\0' && slot == PATHS) {
            slot = i;
        }
    }
    if (!add) {
        put_error("path", folder, "not on the path");
    } else if (slot == PATHS) {
        put_error("path", folder, "no room for it");
    } else {
        ucopy(paths[slot], folder, ulen(folder) + 1);
    }
}


static void set_default(const char *ext, const char *cmd) {
    unsigned slot = DEFAULTS;

    if (ulen(ext) >= EXT_LEN || ulen(cmd) >= CMD_LEN) {
        put_error("set-default", ext, "name too long");
        return;
    }
    for (unsigned i = 0; i < DEFAULTS; i++) {
        if (defaults[i].cmd[0] != '\0' && same_ext(defaults[i].ext, ext)) {
            slot = i;               /* already set: replace it */
            break;
        }
        if (defaults[i].cmd[0] == '\0' && slot == DEFAULTS) {
            slot = i;
        }
    }
    if (slot == DEFAULTS) {
        put_error("set-default", ext, "no room for it");
        return;
    }
    ucopy(defaults[slot].ext, ext, ulen(ext) + 1);
    ucopy(defaults[slot].cmd, cmd, ulen(cmd) + 1);
}

static void cmd_set_default(char *args) {
    const char *name = take_word(&args);
    char *exts = take_word(&args);

    if (*name == '\0') {
        for (unsigned i = 0; i < DEFAULTS; i++) {
            if (defaults[i].cmd[0] != '\0') {
                put(DIM "  .");
                put(defaults[i].ext);
                put("  " ACCENT);
                put(defaults[i].cmd);
                put(PLAIN "\n");
            }
        }
        return;
    }
    if (*exts == '\0') {
        put_error("set-default", NULL, "usage: set-default <cmd> <ext[;ext...]>");
        return;
    }
    /* "md;markdown" sets each in turn. */
    for (char *ext = exts; *ext != '\0';) {
        char *end = ext;

        while (*end != '\0' && *end != ';') {
            end++;
        }
        bool last = *end == '\0';

        *end = '\0';
        if (*ext != '\0') {
            set_default(ext, name);
        }
        ext = last ? end : end + 1;
    }
}

/* Says how far through the start-up script we are, which is what draws the
   loading screen's bar. Only the outermost script reports: the ones it calls
   are steps within it. */
static void report(unsigned done, unsigned total) {
    if (depth != 1) {
        return;
    }
    put("\033[");
    put_number(done, 0);
    put(";");
    put_number(total, 0);
    put("q");
}

/* Runs a file a line at a time, as if each had been typed. The whole of it
   is read first: the commands in it read the disk themselves, and a script
   read a line at a time would have the disk fetching its next line back
   between every pair of them. */
static void cmd_sh(char *args) {
    const char *name = take_word(&args);
    char text[SCRIPT];
    long size;
    int fd;

    if (depth == SH_DEPTH) {
        put_error("sh", name, "scripts nested too deep");
        return;
    }
    fd = (int)sys_open(name, O_RDONLY);
    if (fd < 0) {
        put_error("sh", name, "no such file");
        return;
    }
    size = sys_read(fd, text, sizeof text - 1);
    sys_close(fd);
    if (size < 0) {
        size = 0;
    }
    text[size] = '\n';              /* the one a last line may not have */

    /* While it runs, the working folder is the script's own, so it can name
       its neighbours - which is how /conf/sys/boot.conf calls the rest of
       /conf/sys. */
    char was[NAME_LEN], here[NAME_LEN];

    was[0] = '\0';
    sys_getcwd(was, sizeof was);
    ucopy(here, name, ulen(name) + 1);

    char *slash = ufind_last(here, '/');

    if (slash != NULL) {
        *slash = '\0';
        sys_chdir(here[0] == '\0' ? "/" : here);
    }

    depth++;
    unsigned lines = 1;

    for (long i = 0; i < size; i++) {
        lines += text[i] == '\n';
    }
    for (long at = 0, start = 0, done = 0; at <= size; at++) {
        if (text[at] != '\n') {
            continue;
        }
        text[at] = '\0';
        run_line(text + start, NULL, 0);
        start = at + 1;
        report((unsigned)++done, lines);
    }
    depth--;
    if (was[0] != '\0') {
        sys_chdir(was);
    }
}

static void cmd_help(char *args) {
    char text[] = "md " HELP_FILE;

    (void)args;
    run_line(text, NULL, 0);
}

static void cmd_run(char *args) {
    char *name = take_word(&args);

    if (*name == '\0' || !spawn(name, name, args, NULL, 0)) {
        put_error("run", name, "no such file");
    }
}

static const struct {
    const char *name;
    void      (*run)(char *args);
} builtins[] = {
    { "cd",          cmd_cd          },
    { "path",        cmd_path        },
    { "set-default", cmd_set_default },
    { "sh",          cmd_sh          },
    { "run",         cmd_run         },
    { "help",        cmd_help        },
};

#define BUILTIN_COUNT (sizeof builtins / sizeof builtins[0])

/* Where a bare name is: a file in one of the folders on the path, in the
   order they were added. /proc is one of those folders and nothing here
   knows it is special - it is put on the path like any other, by
   /conf/sys/path.conf - so `mode` is found there for the same reason `ls` is
   found in /pkg/bax-coreutils.

   The answer is a path, so that what was typed is split into words exactly
   once: a lookup that took the words apart as it went would leave nothing of
   them for the next folder to try. */
static bool find_command(const char *name, char *path, size_t max) {
    struct stat st;

    for (unsigned i = 0; i < PATHS; i++) {
        if (paths[i][0] == '\0' || ulen(paths[i]) + ulen(name) + 1 > max) {
            continue;
        }
        path[0] = '\0';
        uappend(path, max, paths[i]);
        uappend(path, max, name);
        if (sys_stat(path, &st) == 0 && st.size > 0) {
            return true;
        }
    }
    return false;
}

/* ---- $(command) ----------------------------------------------------------
 *
 * What a command prints, put back into the line that asked for it, so that
 * `echo Booted in $(uptime)` reads the way it looks. The kernel takes the
 * output rather than showing it, and newlines are dropped so that a
 * command's line ending does not break the one it is standing in. */

static char inner[LINE], said[LINE];

/* Builds text with each $(command) replaced into out, which holds LINE
   bytes. False if the line outgrows that. */
static bool expand(const char *text, char *out) {
    size_t len = 0;

    while (*text != '\0') {
        if (text[0] != '$' || text[1] != '(') {
            if (len + 1 >= LINE) {
                return false;
            }
            out[len++] = *text++;
            continue;
        }

        size_t n = 0;

        for (text += 2; *text != '\0' && *text != ')'; text++) {
            if (n + 1 >= sizeof inner) {
                return false;
            }
            inner[n++] = *text;
        }
        inner[n] = '\0';
        if (*text == ')') {
            text++;
        }

        said[0] = '\0';
        capturing = true;
        run_line(inner, said, (long)sizeof said);
        capturing = false;

        /* A line of output is a break between words, not a break in one, so
           its newline becomes a space - and the last one, which every
           command ends on, is not a word break at all. */
        size_t from = len;

        for (const char *p = said; *p != '\0'; p++) {
            if (len + 1 >= LINE) {
                return false;
            }
            out[len++] = *p == '\n' ? ' ' : *p;
        }
        while (len > from && out[len - 1] == ' ') {
            len--;
        }
    }
    out[len] = '\0';
    return true;
}

/* ---- one line ----------------------------------------------------------- */

/* Runs one line. With out set, what it printed is taken into out instead of
   shown; a word this file knows prints as it always does, there being no
   program in the way to take it from. */
static void run_line(char *text, char *out, long out_size) {
    char expanded[LINE];
    char path[NAME_LEN];

    while (*text == ' ' || *text == '\t') {
        text++;
    }
    if (*text == '#' || *text == '\0') {
        return;                     /* a comment, or nothing at all */
    }
    if (!capturing && ufind(text, '$') != NULL) {
        if (!expand(text, expanded)) {
            put_error("sh", NULL, "line too long once $(...) is filled in");
            return;
        }
        text = expanded;
    }

    char *args = text;
    char *name = take_word(&args);

    for (unsigned i = 0; i < BUILTIN_COUNT; i++) {
        if (usame(builtins[i].name, name)) {
            builtins[i].run(args);
            return;
        }
    }
    /* A name with a slash in it is a path, and is the only way to reach a
       file in the working folder - which keeps what a name means from
       following you about. */
    if (ufind(name, '/') != NULL) {
        open_file(name, name, args, out, out_size);
        return;
    }
    if (!find_command(name, path, sizeof path)) {
        struct stat st;

        if (sys_stat(name, &st) == 0) {
            put_error(name, NULL, "not on the path, try ./ in front of it");
        } else {
            put_error(name, NULL, "unknown command, try help");
        }
        return;
    }
    open_file(path, name, args, out, out_size);
}

/* ---- the prompt --------------------------------------------------------- */

static void prompt(void) {
    char cwd[NAME_LEN];

    put(ACCENT "baxos " PLAIN DIM);
    if (sys_getcwd(cwd, sizeof cwd) > 0) {
        put(cwd);
    }
    put(" " GLYPH_PROMPT " " BRIGHT);
}

int main(int argc, char **argv) {
    static char line[LINE];
    char own[] = "sh " SH_CONF;
    struct stat st;

    (void)argv;
    /* Its own settings first, however it was started: a shell running a
       script needs to know where commands are just as much as one taking
       them from the keyboard. */
    if (sys_stat(SH_CONF, &st) == 0) {
        run_line(own, NULL, 0);
    }

    /* Started with a file, as `sh file` at the prompt does through a program
       of its own: that file, and then out. */
    if (argc > 1) {
        char text[LINE];

        text[0] = '\0';
        uappend(text, sizeof text, "sh ");
        uappend(text, sizeof text, argv[1]);
        run_line(text, NULL, 0);
        return 0;
    }

    for (;;) {
        prompt();

        long len = sys_read(0, line, sizeof line - 1);

        put(PLAIN);
        if (len <= 0) {
            put("\n");
            continue;               /* nothing typed: ask again */
        }
        if (line[len - 1] == '\n') {
            len--;
        }
        line[len] = '\0';
        run_line(line, NULL, 0);
    }
}
