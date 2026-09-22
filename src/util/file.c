#include "util.h"

/* Says what a file is: how big, whether it reads as text, and where it sits
   on the disk - the first sector and how many, which stat reports as the
   file's number and its count of blocks. */

static char buffer[512];

int main(int argc, char **argv) {
    struct stat info;
    long fd, got;
    bool text = true;

    if (argc != 2) {
        put_error("file", NULL, "usage: file <file>");
        return 1;
    }
    if (sys_stat(argv[1], &info) != 0) {
        put_error("file", argv[1], "no such file");
        return 1;
    }
    put(argv[1]);
    put(": ");
    if ((info.mode & S_IFMT) == S_IFDIR) {
        put("folder\n");
        return 0;
    }
    if (info.size == 0) {
        put("empty\n");
        return 0;
    }
    /* Judged by its first block, as `file` has always judged things. */
    fd = sys_open(argv[1], O_RDONLY);
    if (fd >= 0) {
        got = sys_read((int)fd, buffer, sizeof buffer);
        for (long i = 0; i < got; i++) {
            if ((buffer[i] < ' ' && buffer[i] != '\n' && buffer[i] != '\t') ||
                buffer[i] > '~') {
                text = false;
                break;
            }
        }
        sys_close((int)fd);
    }
    put(text ? "text" : "binary");
    put(", ");
    put_number((uint64_t)info.size, 0);
    put(" bytes, sectors ");
    put_number(info.ino, 0);
    put("-");
    put_number(info.ino + (uint64_t)info.blocks - 1, 0);
    put("\n");
    return 0;
}
