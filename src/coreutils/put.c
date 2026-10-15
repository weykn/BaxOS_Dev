#include "tef.h"

/* put <file> [text...]: the file holds the text and a newline, or nothing. */

int main(int argc, char **argv, char **envp) {
    int fd;

    (void)envp;
    if (argc < 2) {
        out(2, "usage: put <file> [text]\n", NULL);
        return 1;
    }
    if ((fd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
        out(2, "put: ", argv[1], ": cannot write\n", NULL);
        return 1;
    }
    for (int i = 2; i < argc; i++) {
        out(fd, argv[i], i + 1 < argc ? " " : "\n", NULL);
    }
    return 0;
}
