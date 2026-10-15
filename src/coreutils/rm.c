#include "tef.h"

/* rm <file...>: files, and folders with nothing in them. */

int main(int argc, char **argv, char **envp) {
    int status = 0;

    (void)envp;
    for (int i = 1; i < argc; i++) {
        if (unlink(argv[i]) < 0 && sys(84, (long)argv[i], 0, 0, 0) < 0) {     /* rmdir */
            out(2, "rm: ", argv[i], ": cannot remove\n", NULL);
            status = 1;
        }
    }
    return status;
}
