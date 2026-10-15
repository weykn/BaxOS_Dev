#include "tef.h"

/* cp <file> <to>: to, or into it if it is a folder. */

static char buf[4096];

int main(int argc, char **argv, char **envp) {
    char dest[256];
    const char *to;
    int from, into;
    long got;

    (void)envp;
    if (argc != 3) {
        out(2, "usage: cp <file> <to>\n", NULL);
        return 1;
    }
    to = dest_of(argv[1], argv[2], dest, sizeof dest);
    if ((from = open(argv[1], O_RDONLY, 0)) < 0) {
        out(2, "cp: ", argv[1], ": not found\n", NULL);
        return 1;
    }
    if ((into = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
        out(2, "cp: ", to, ": cannot write\n", NULL);
        return 1;
    }
    while ((got = read(from, buf, sizeof buf)) > 0) {
        if (write(into, buf, got) != got) {
            out(2, "cp: ", to, ": cannot write\n", NULL);
            return 1;
        }
    }
    return 0;
}
