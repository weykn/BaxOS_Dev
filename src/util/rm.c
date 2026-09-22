#include "util.h"

/* Deletes files, and folders that have nothing in them. */

int main(int argc, char **argv) {
    int bad = 0;

    if (argc < 2) {
        put_error("rm", NULL, "usage: rm <name>...");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (sys_unlink(argv[i]) < 0) {
            put_error("rm", argv[i], "not there, or a folder with things in it");
            bad = 1;
        }
    }
    return bad;
}
