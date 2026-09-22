#include "util.h"

/* A pager: text a screenful at a time, stopping at the bottom until a key
 * says to go on.
 *
 * It looks a name up the way the shell does: a bare name is a command, run
 * for its output, and anything else is a file. So `more log` pages what the
 * log command prints, and `more /conf/sys/boot.conf` reads the file.
 *
 * Any key gives another screenful, q stops. */

#define CHUNK    4096       /* read at a time */
#define HELD     16384      /* the most of a command's output it will hold */
#define NAME_LEN 48
#define PATHS    8

static char text[HELD];
static long held;

static unsigned rows = 24, taken;
static bool quit;

#define PROMPT "-- more --"

/* Waits at the bottom of a screenful. False if it was told to stop. */
static bool wait_for_key(void) {
    struct termios was, raw;
    bool restore = sys_termios_get(STDERR, &was) == 0;
    char key = 0;

    if (restore) {
        raw = was;
        raw.lflag &= ~(uint32_t)(ICANON | ECHO);
        sys_termios_set(STDERR, &raw);
    }
    put(DIM PROMPT PLAIN);
    long got = sys_read(STDERR, &key, 1);

    if (restore) {
        sys_termios_set(STDERR, &was);
    }
    if (got != 1 || key == 'q' || key == 'Q') {
        put("\n");
        return false;
    }
    /* Rub the prompt out, so the text closes up behind it: a backspace here
       moves back over a character and blanks it. */
    for (unsigned i = 0; i < sizeof PROMPT - 1; i++) {
        put("\b");
    }
    taken = 0;
    return true;
}

/* Prints one stretch, stopping every screenful. */
static void page(const char *from, long size) {
    long start = 0;

    for (long at = 0; at < size && !quit; at++) {
        if (from[at] != '\n') {
            continue;
        }
        sys_write(STDOUT, from + start, at - start + 1);
        start = at + 1;
        if (++taken >= rows - 1 && !wait_for_key()) {
            quit = true;
            return;
        }
    }
    if (start < size && !quit) {
        sys_write(STDOUT, from + start, size - start);
        put("\n");
    }
}

/* ---- where the text comes from ------------------------------------------ */

/* Everything that can be read from fd, into text. */
static void take_all(int fd) {
    long got;

    held = 0;
    while (held < HELD && (got = sys_read(fd, text + held, CHUNK)) > 0) {
        held += got;
    }
}

/* The command a bare name is, looked up on PATH as the shell looks it up.
   False if it is not one. */
static bool as_command(const char *name, char *path, char **env) {
    struct stat st;

    if (ufind(name, '/') != NULL) {
        return false;               /* a path is a file, not a command */
    }
    for (; env != NULL && *env != NULL; env++) {
        const char *value = *env;

        if (!(value[0] == 'P' && value[1] == 'A' && value[2] == 'T' &&
              value[3] == 'H' && value[4] == '=')) {
            continue;
        }
        for (value += 5; *value != '\0';) {
            size_t n = 0;

            while (value[n] != '\0' && value[n] != ':') {
                n++;
            }
            if (n > 0 && n + ulen(name) + 2 < NAME_LEN) {
                ucopy(path, value, n);
                path[n] = '\0';
                if (path[n - 1] != '/') {
                    path[n] = '/';
                    path[n + 1] = '\0';
                }
                uappend(path, NAME_LEN, name);
                if (sys_stat(path, &st) == 0 && st.size > 0) {
                    return true;
                }
            }
            value += value[n] == ':' ? n + 1 : n;
        }
        return false;
    }
    return false;
}

int main(int argc, char **argv) {
    char **env = argv + argc + 1;
    struct winsize screen;
    char path[NAME_LEN];

    if (sys_winsize(&screen) == 0 && screen.rows > 2) {
        rows = screen.rows;
    }
    if (argc < 2) {
        take_all(0);                /* what the command before it printed */
        page(text, held);
        return 0;
    }
    for (int i = 1; i < argc && !quit; i++) {
        if (as_command(argv[i], path, env)) {
            const char *words[2] = { argv[i], NULL };

            text[0] = '\0';
            if (sys_spawn(path, words, text, HELD) < 0) {
                put_error("more", argv[i], "could not be run");
                return 1;
            }
            page(text, (long)ulen(text));
            continue;
        }

        struct stat st;

        if (sys_stat(argv[i], &st) == 0 && (st.mode & S_IFMT) == S_IFDIR) {
            put_error("more", argv[i], "that is a folder");
            return 1;
        }

        int fd = (int)sys_open(argv[i], O_RDONLY);

        if (fd < 0) {
            put_error("more", argv[i], "no such file or command");
            return 1;
        }
        take_all(fd);
        sys_close(fd);
        page(text, held);
    }
    return 0;
}
