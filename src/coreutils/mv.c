#include "tcr.h"

/* mv <file> <to>: renamed to, or moved into it if it is a folder. */

int main(int argc, char **argv, char **envp) {
    char dest[256];

    (void)envp;
    if (argc != 3) {
        out(2, "usage: mv <file> <to>\n", NULL);
        return 1;
    }
    if (rename(argv[1], dest_of(argv[1], argv[2], dest, sizeof dest)) < 0) {
        out(2, "mv: ", argv[1], ": cannot move\n", NULL);
        return 1;
    }
    return 0;
}
