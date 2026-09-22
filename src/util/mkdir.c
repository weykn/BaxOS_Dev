#include "util.h"

/* Makes folders. */

int main(int argc, char **argv) {
    int bad = 0;

    if (argc < 2) {
        put_error("mkdir", NULL, "usage: mkdir <folder>...");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (sys_mkdir(argv[i]) < 0) {
            put_error("mkdir", argv[i], "cannot make it");
            bad = 1;
        }
    }
    return bad;
}
