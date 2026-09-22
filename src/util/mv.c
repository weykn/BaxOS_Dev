#include "util.h"

/* Moves a file, which on this disk is renaming it: the sectors stay where
   they are and only the name changes. */

static char target[256];

int main(int argc, char **argv) {
    struct stat info;

    if (argc != 3) {
        put_error("mv", NULL, "usage: mv <file> <file or folder>");
        return 1;
    }
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
    if (sys_rename(argv[1], target) < 0) {
        put_error("mv", argv[1], "cannot move it");
        return 1;
    }
    return 0;
}
