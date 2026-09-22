#include "util.h"

/* Copies a file. The copy is written as it is read, a block at a time, so
   the size of the file is not the size of anything it needs. */

static char buffer[4096];
static char target[256];

int main(int argc, char **argv) {
    struct stat info;
    long from, to, got;

    if (argc != 3) {
        put_error("cp", NULL, "usage: cp <file> <file or folder>");
        return 1;
    }
    /* Copying into a folder keeps the name, as it does everywhere else. */
    ucopy(target, argv[2], ulen(argv[2]) + 1);
    if (sys_stat(argv[2], &info) == 0 && (info.mode & S_IFMT) == S_IFDIR) {
        const char *leaf = argv[1];

        for (const char *p = argv[1]; *p != '\0'; p++) {
            if (*p == '/') {
                leaf = p + 1;
            }
        }
        join(target, sizeof target, argv[2], leaf);
    }

    from = sys_open(argv[1], O_RDONLY);
    if (from < 0) {
        put_error("cp", argv[1], "no such file");
        return 1;
    }
    to = sys_open(target, O_WRONLY | O_CREAT | O_TRUNC);
    if (to < 0) {
        put_error("cp", target, "cannot write it");
        return 1;
    }
    while ((got = sys_read((int)from, buffer, sizeof buffer)) > 0) {
        if (sys_write((int)to, buffer, got) != got) {
            put_error("cp", target, "the disk is full");
            return 1;
        }
    }
    sys_close((int)from);
    sys_close((int)to);
    return 0;
}
