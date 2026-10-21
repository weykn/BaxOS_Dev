#include "tcr.h"

/* mkdir [-p] [-m mode] <folder...>: -p makes the folders on the way too,
   and is quiet about one that is there. */

int main(int argc, char **argv, char **envp) {
    bool parents = false;
    long mode = 0755;
    int status = 0;

    (void)envp;
    for (int i = 1; i < argc; i++) {
        char *path = argv[i];

        if (strcmp(path, "-p") == 0) {
            parents = true;
            continue;
        }
        if (strcmp(path, "-m") == 0 && i + 1 < argc) {
            mode = 0;
            for (const char *m = argv[++i]; *m >= '0' && *m <= '7'; m++) {
                mode = mode * 8 + (*m - '0');
            }
            continue;
        }
        for (char *p = path + 1; parents && *p != '\0'; p++) {
            if (*p == '/') {
                *p = '\0';
                mkdir(path, 0755);
                *p = '/';
            }
        }
        long err = mkdir(path, mode);

        if (err < 0 && !(parents && err == -EEXIST)) {
            out(2, "mkdir: ", path, err == -EEXIST ? ": already there\n" : ": cannot make\n", NULL);
            status = 1;
        }
    }
    return status;
}
