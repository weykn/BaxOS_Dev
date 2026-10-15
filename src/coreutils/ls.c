#include "tef.h"

/* ls [path...]: the names in a folder, one a line, a folder's with a slash
   after it. */

struct dirent64 {
    uint64_t ino, off;
    uint16_t reclen;
    uint8_t  type;
    char     name[];
};

#define DT_DIR 4

static char buf[2048];

static int list(const char *path) {
    int fd = open(path, O_RDONLY, 0);
    long got;

    if (fd < 0) {
        out(2, "ls: ", path, ": not found\n", NULL);
        return 1;
    }
    if (!is_folder(path)) {
        out(1, path, "\n", NULL);
    }
    while ((got = getdents64(fd, buf, sizeof buf)) > 0) {
        for (long at = 0; at < got; at += ((struct dirent64 *)(buf + at))->reclen) {
            struct dirent64 *d = (struct dirent64 *)(buf + at);

            if (strcmp(d->name, ".") != 0 && strcmp(d->name, "..") != 0) {
                out(1, d->name, d->type == DT_DIR ? "/\n" : "\n", NULL);
            }
        }
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv, char **envp) {
    int status = 0;

    (void)envp;
    if (argc < 2) {
        return list(".");
    }
    for (int i = 1; i < argc; i++) {
        if (argc > 2) {
            out(1, i > 1 ? "\n" : "", argv[i], ":\n", NULL);
        }
        status |= list(argv[i]);
    }
    return status;
}
