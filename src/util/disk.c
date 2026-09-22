#include "util.h"

/* What the disk holds and what is left of it. */

#define BAR 30

static void bar(uint64_t used, uint64_t total) {
    unsigned filled = total == 0 ? 0 : (unsigned)((used * BAR + total - 1) / total);

    put("  " DIM "disk    ");
    for (unsigned i = 0; i < BAR; i++) {
        put(i < filled ? GREEN "\xDB" : DIM "\xB0");
    }
    put(PLAIN);
}

int main(int argc, char **argv) {
    struct statfs info;

    (void)argc;
    (void)argv;
    if (sys_statfs("/", &info) != 0) {
        put_error("disk", NULL, "cannot read the disk");
        return 1;
    }
    uint64_t used = info.blocks - info.bfree;

    bar(used, info.blocks);
    put("  ");
    put_number((used * (uint64_t)info.bsize + 1023) / 1024, 0);
    put(DIM " of " PLAIN);
    put_number(info.blocks * (uint64_t)info.bsize / 1024, 0);
    put(DIM " KiB\n");
    put("  files   " PLAIN);
    put_number(info.files - info.ffree, 0);
    put(DIM " of " PLAIN);
    put_number(info.files, 0);
    put(DIM " entries\n" PLAIN);
    return 0;
}
