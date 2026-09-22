#include "util.h"

/* Prints files, one after another. */

static char buffer[4096];

int main(int argc, char **argv) {
    char last = '\n';
    int bad = 0;

    for (int i = 1; i < argc; i++) {
        long fd = sys_open(argv[i], O_RDONLY);
        long got;

        if (fd < 0) {
            put_error("cat", argv[i], "no such file");
            bad = 1;
            continue;
        }
        while ((got = sys_read((int)fd, buffer, sizeof buffer)) > 0) {
            sys_write(STDOUT, buffer, got);
            last = buffer[got - 1];
        }
        sys_close((int)fd);
    }
    if (argc < 2) {
        put_error("cat", NULL, "usage: cat <file>...");
        return 1;
    }
    /* Finish the last line, and leave a blank one before the prompt. */
    if (last != '\n') {
        put("\n");
    }
    put("\n");
    return bad;
}
