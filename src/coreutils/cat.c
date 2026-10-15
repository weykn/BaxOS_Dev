#include "tef.h"

/* cat [file...]: the files one after another, or what comes in. */

static char buf[4096];

int main(int argc, char **argv, char **envp) {
    int status = 0;

    (void)envp;
    for (int i = argc > 1 ? 1 : 0; i < argc; i++) {
        int fd = i == 0 ? 0 : open(argv[i], O_RDONLY, 0);
        long got;

        if (fd < 0) {
            out(2, "cat: ", argv[i], ": not found\n", NULL);
            status = 1;
            continue;
        }
        while ((got = read(fd, buf, sizeof buf)) > 0) {
            write(1, buf, got);
        }
        if (fd != 0) {
            close(fd);
        }
    }
    return status;
}
