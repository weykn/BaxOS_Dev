#include "util.h"

/* Puts a line of text in a file, replacing whatever was there. */

int main(int argc, char **argv) {
    long fd;

    if (argc < 2) {
        put_error("write", NULL, "usage: write <file> [text]");
        return 1;
    }
    fd = sys_open(argv[1], O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        put_error("write", argv[1], "cannot write it");
        return 1;
    }
    for (int i = 2; i < argc; i++) {
        if (i > 2) {
            sys_write((int)fd, " ", 1);
        }
        sys_write((int)fd, argv[i], (long)ulen(argv[i]));
    }
    if (argc > 2) {
        sys_write((int)fd, "\n", 1);    /* a text file ends in a newline */
    }
    sys_close((int)fd);
    return 0;
}
