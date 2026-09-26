/* tsh - the Tuxlet shell, and the one the machine starts.
 *
 * As small as a shell can be and still be one: it reads a line, splits it
 * into words on blanks, and runs the first as a program with the rest as its
 * arguments. `cd` and `exit` are its own, since no program can do them for
 * it, and so is `help`, which says as much. There are no pipes,
 * redirections, variables or quoting.
 *
 * It uses no C library, only the kernel's syscalls under Linux's numbers, so
 * it is one small static file with nothing to load beside it. Build it with
 * build.sh in this folder. */

typedef unsigned long size_t;
typedef long          ssize_t;

#define NULL ((void *)0)

#define SYS_READ    0
#define SYS_WRITE   1
#define SYS_FORK    57
#define SYS_EXECVE  59
#define SYS_EXIT    60
#define SYS_WAIT4   61
#define SYS_GETCWD  79
#define SYS_CHDIR   80

#define LINE  256       /* the longest line read */
#define WORDS 32        /* the most words in one */
#define PATH  256       /* the longest path tried */

static long sys(long n, long a, long b, long c) {
    long r;

    __asm__ volatile ("syscall"
                      : "=a"(r)
                      : "a"(n), "D"(a), "S"(b), "d"(c)
                      : "rcx", "r11", "memory");
    return r;
}

static size_t len(const char *s) {
    size_t n = 0;

    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static void put(const char *s) {
    sys(SYS_WRITE, 2, (long)s, (long)len(s));
}

static int same(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* The value of name in the environment, or NULL. */
static const char *env(char **envp, const char *name) {
    size_t n = len(name);

    for (; *envp != NULL; envp++) {
        size_t i = 0;

        while (i < n && (*envp)[i] == name[i]) {
            i++;
        }
        if (i == n && (*envp)[n] == '=') {
            return *envp + n + 1;
        }
    }
    return NULL;
}

/* Runs argv[0] in a child, looking it up on PATH unless it names a path of
   its own, and waits for it. */
static void run(char **argv, char **envp) {
    const char *dirs = env(envp, "PATH");
    char path[PATH];
    int status;

    if (sys(SYS_FORK, 0, 0, 0) != 0) {
        sys(SYS_WAIT4, -1, (long)&status, 0);
        return;
    }
    for (const char *p = argv[0]; *p != '\0'; p++) {
        if (*p == '/') {
            dirs = NULL;
        }
    }
    if (dirs == NULL) {
        sys(SYS_EXECVE, (long)argv[0], (long)argv, (long)envp);
    }
    while (dirs != NULL && *dirs != '\0') {
        size_t n = 0, name = len(argv[0]);

        while (dirs[n] != '\0' && dirs[n] != ':') {
            n++;
        }
        if (n + name + 2 <= sizeof path) {
            for (size_t i = 0; i < n; i++) {
                path[i] = dirs[i];
            }
            path[n] = '/';
            for (size_t i = 0; i <= name; i++) {
                path[n + 1 + i] = argv[0][i];
            }
            sys(SYS_EXECVE, (long)path, (long)argv, (long)envp);
        }
        dirs += dirs[n] == ':' ? n + 1 : n;
    }
    put("tsh: ");
    put(argv[0]);
    put(": not found\n");
    sys(SYS_EXIT, 127, 0, 0);
}

static void help(void) {
    put("Built-in commands:\n"
        "\n"
        "  cd [dir]      change directory\n"
        "  exit [code]   leave the shell\n"
        "  help          show this message\n"
        "\n"
        "Other commands are in /usr/bin and /proc\n");
}

static void prompt(void) {
    char cwd[PATH];

    if (sys(SYS_GETCWD, (long)cwd, sizeof cwd, 0) < 0) {
        cwd[0] = '?';
        cwd[1] = '\0';
    }
    put(cwd);
    put(" $ ");
}

__attribute__((used)) void main_start(long *stack) {
    char **envp = (char **)(stack + 1 + stack[0] + 1);
    static char line[LINE];
    char *argv[WORDS + 1];

    for (;;) {
        ssize_t got;
        int argc = 0;

        prompt();
        got = sys(SYS_READ, 0, (long)line, sizeof line - 1);
        if (got <= 0) {
            sys(SYS_EXIT, 0, 0, 0);     /* the end of the input */
        }
        line[got] = '\0';

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
            continue;
        }

        if (same(argv[0], "exit")) {
            long code = 0;

            for (const char *d = argc > 1 ? argv[1] : ""; *d >= '0' && *d <= '9'; d++) {
                code = code * 10 + (*d - '0');
            }
            sys(SYS_EXIT, code, 0, 0);
        } else if (same(argv[0], "help")) {
            help();
        } else if (same(argv[0], "cd")) {
            const char *to = argc > 1 ? argv[1] : env(envp, "HOME");

            if (sys(SYS_CHDIR, (long)(to != NULL ? to : "/"), 0, 0) < 0) {
                put("tsh: cd: no such folder\n");
            }
        } else {
            run(argv, envp);
        }
    }
}

/* The stack as the kernel leaves it: argc, then argv, then the environment. */
__asm__(".globl _start\n"
        "_start:\n"
        "    mov %rsp, %rdi\n"
        "    and $-16, %rsp\n"
        "    call main_start\n"
        "    hlt\n");
