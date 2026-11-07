#include "tcr.h"

/* tsh, the Tuxlet shell, /usr/bin/tsh.
 *
 * As small as a shell can be and still be one: it reads a line, splits it
 * into words on blanks, and runs the first with the rest as its arguments -
 * whatever path it names, or the first of that name in PATH: a command of
 * the kernel's from /ctl, or a program. `cd`, `export`, `exit` and `help`
 * are its own; ls, cat and the rest are tuxlet-coreutils in /usr/bin. There
 * are no pipes, redirections, variables or quoting: bash is there to install
 * for those.
 *
 *     tsh             reads and runs lines until `exit`
 *     tsh <script>    runs the script's lines, each shown as it runs
 *     -tsh [boot]     what the kernel starts: the boot script, in its own
 *                     folder, then the greeting, then - for good -
 *                     /etc/tuxlet/shell, or tsh itself where that names
 *                     nothing it could run, or is not there */

#define SHELL_CONF "/etc/tuxlet/shell"

#define LINE    256
#define WORDS   32
#define ENV_MAX 16
#define ENV_BYTES 512

#define RED   "\033[91m"
#define GRAY  "\033[90m"
#define PLAIN "\033[0m"

#define NOINLINE __attribute__((noinline))

static bool     failed;             /* a command was not there to run */
static unsigned ran;                /* lines run, for the shell script */

/* ---- the environment ----------------------------------------------------- */

static char  env_text[ENV_BYTES];
static char *env[ENV_MAX + 1];
static unsigned env_count;

static bool same(const char *a, const char *b, size_t n) {
    while (n > 0 && *a == *b) {
        a++;
        b++;
        n--;
    }
    return n == 0;
}

static const char *env_get(const char *name) {
    size_t n = strlen(name);

    for (unsigned i = 0; i < env_count; i++) {
        if (strlen(env[i]) > n && env[i][n] == '=' && same(env[i], name, n)) {
            return env[i] + n + 1;
        }
    }
    return NULL;
}

/* Sets NAME=value, replacing what NAME had. The text is packed again from
   the list each time, so a value changed over and over does not use it up. */
static NOINLINE void env_set(char *pair) {
    char *eq = strchr(pair, '=');
    char packed[ENV_BYTES];
    size_t used = 0, n;

    if (eq == NULL || eq == pair) {
        out(2, "export: ", pair, ": not NAME=value\n", NULL);
        return;
    }
    n = (size_t)(eq - pair);
    for (unsigned i = 0; i < env_count;) {
        if (strlen(env[i]) > n && env[i][n] == '=' && same(env[i], pair, n)) {
            memcpy(&env[i], &env[i + 1], (env_count - i) * sizeof env[0]);
            env_count--;
        } else {
            i++;
        }
    }
    if (env_count < ENV_MAX) {
        env[env_count++] = pair;
    }
    for (unsigned i = 0; i < env_count; i++) {
        size_t len = strlen(env[i]) + 1;

        if (used + len > sizeof packed) {
            out(2, "export: no room for ", pair, "\n", NULL);
            env_count = i;
            break;
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

/* ---- running ------------------------------------------------------------- */

static void on_signal(int sig) {
    (void)sig;                      /* Ctrl-C is for what is running, not tsh */
}

/* The vfork's child: in tsh's memory until execve takes it elsewhere. Each
   folder of PATH is tried in turn, the way execvp does it. */
static NOINLINE __attribute__((noreturn)) void child(char **argv) {
    const char *dirs = env_get("PATH");
    char path[LINE];
    long err = -ENOENT, e;

    if (strchr(argv[0], '/') != NULL) {
        err = execve(argv[0], argv, env);
    }
    while (strchr(argv[0], '/') == NULL && dirs != NULL && *dirs != '\0') {
        const char *end = strchr(dirs, ':');
        size_t n = end != NULL ? (size_t)(end - dirs) : strlen(dirs), k = strlen(argv[0]);

        if (n > 0 && n + k + 2 <= sizeof path) {
            memcpy(path, dirs, n);
            path[n] = '/';
            memcpy(path + n + 1, argv[0], k + 1);
            if ((e = execve(path, argv, env)) != -ENOENT) {
                err = e;
            }
        }
        dirs = end != NULL ? end + 1 : dirs + n;
    }
    out(2, RED "tsh: ", argv[0], err == -ENOEXEC ? ": not a program" : ": not found",
        PLAIN "\n", NULL);
    exit(127);
    __builtin_unreachable();
}

static NOINLINE void run(char **argv) {
    int status = 0, pid = vfork();

    if (pid == 0) {
        child(argv);
    }
    if (pid < 0) {
        out(2, RED "tsh: ", argv[0], ": no room for another process" PLAIN "\n", NULL);
        return;
    }
    while (wait4(pid, &status) == -EINTR) {
    }
    if ((status & 0x7F) == 2) {
        out(1, "^C\n", NULL);     /* SIGINT: the kernel leaves saying so to tsh */
    }
    if ((status & 0x7F) == 0 && (status >> 8 & 0xFF) == 127) {
        failed = true;
    }
}

/* Runs one line, typed or out of a script: split into words on blanks, a
   '#' starting a comment. False for `exit`. */
static bool line_run(char *line) {
    char *argv[WORDS + 1];
    unsigned argc = 0;

    for (char *p = line; *p != '\0' && argc < WORDS;) {
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
        out(1, "built-in: cd export exit help\n", NULL);
    } else if (strcmp(argv[0], "cd") == 0) {
        const char *to = argc > 1 ? argv[1] : env_get("HOME");

        if (chdir(to != NULL ? to : "/") < 0) {
            out(2, RED "tsh: cd: ", argc > 1 ? argv[1] : "", ": no such folder" PLAIN "\n", NULL);
        }
    } else if (strcmp(argv[0], "export") == 0) {
        for (unsigned i = 0; argc == 1 && i < env_count; i++) {
            out(1, env[i], "\n", NULL);
        }
        for (unsigned i = 1; i < argc; i++) {
            env_set(argv[i]);
        }
    } else {
        run(argv);
    }
    return true;
}

/* A script's lines, one after another, each shown before it runs. False if
   it is not there. */
static bool script(const char *path, bool quiet) {
    static char buf[128];
    char line[LINE];
    size_t len = 0;
    long got = 1;
    int fd = open(path, O_RDONLY, 0);

    if (fd < 0) {
        if (!quiet) {
            out(2, RED "tsh: ", path, ": not found" PLAIN "\n", NULL);
            failed = true;
        }
        return false;
    }
    while (got > 0) {
        got = read(fd, buf, sizeof buf);
        for (long i = 0; i < got || (got <= 0 && i == 0); i++) {
            char c = got > 0 ? buf[i] : '\n';     /* the last line may have no newline */
            const char *shown = line;

            if (c != '\n') {
                if (c != '\r' && len < sizeof line - 1) {
                    line[len++] = c;
                }
                continue;
            }
            line[len] = '\0';
            len = 0;
            while (*shown == ' ' || *shown == '\t') {
                shown++;
            }
            if (*shown != '\0' && *shown != '#') {
                out(1, GRAY "+ ", shown, PLAIN "\n", NULL);
            }
            if (!line_run(line)) {
                got = 0;            /* `exit`: the script is done */
                break;
            }
        }
    }
    close(fd);
    return true;
}

/* Reads and runs lines until `exit`. */
static void interactive(void) {
    static char line[LINE];

    for (;;) {
        long got;

        if (getcwd(line, sizeof line) < 0) {
            line[0] = '\0';
        }
        out(1, line, " $ ", NULL);
        got = read(0, line, sizeof line - 1);
        if (got == -EINTR) {
            out(1, "^C\n", NULL);
            continue;
        }
        line[got > 0 ? got : 0] = '\0';
        if (!line_run(line)) {
            return;
        }
    }
}

/* The machine's shell, as the kernel starts it: the boot script in its own
   folder, so that it can call the rest of it by name, then the shell script
   - or tsh - for good, from home. */
static __attribute__((noreturn)) void login(char *boot) {
    if (boot != NULL) {
        char *cut = (char *)base_of(boot);

        if (cut > boot) {
            char was = cut[-1];

            cut[-1] = '\0';
            chdir(cut - 1 == boot ? "/" : boot);
            cut[-1] = was;
        }
        if (script(boot, true)) {
            char greet[] = "/ctl/greet";
            char *words[] = { greet, NULL };

            run(words);
        }
    }
    const char *home = env_get("HOME");

    chdir(home != NULL ? home : "/");
    for (;;) {
        unsigned before = ran;

        failed = false;
        script(SHELL_CONF, true);
        if (failed || ran == before) {
            interactive();
        }
    }
}

int main(int argc, char **argv, char **envp) {
    struct { void *handler; unsigned long flags; void *restorer; unsigned long long mask; } act = { 0 };

    for (; *envp != NULL; envp++) {
        env_set(*envp);
    }
    act.handler = on_signal;
    act.flags = 0x04000000;         /* SA_RESTORER */
    act.restorer = tcr_restorer;
    rt_sigaction(2, &act);          /* SIGINT */

    if (argv[0][0] == '-') {
        login(argc > 1 ? argv[1] : NULL);
    }
    if (argc > 1) {
        script(argv[1], false);
        return failed;
    }
    interactive();
    return 0;
}
