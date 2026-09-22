#include "util.h"

/* Lists a folder: the names in it, folders marked with a slash, and how big
   each file is. With no argument it lists the working folder. */

static char buffer[8192];
static char path[256];

static int list(const char *folder) {
    long fd = sys_open(folder, O_RDONLY);
    long got;

    if (fd < 0) {
        put_error("ls", folder, "no such folder");
        return 1;
    }
    while ((got = sys_getdents64((int)fd, buffer, sizeof buffer)) > 0) {
        for (long at = 0; at < got;) {
            struct dirent64 *entry = (struct dirent64 *)(buffer + at);
            struct stat info;
            bool folder_entry = entry->type == DT_DIR;

            at += entry->reclen;
            put("  ");
            put(entry->name);
            if (folder_entry) {
                put("/");
            }
            /* The size goes in a column of its own, which needs to know how
               far the name reached. */
            size_t used = ulen(entry->name) + (folder_entry ? 1 : 0);

            put_spaces(used < 28 ? 28 - used : 1);
            join(path, sizeof path, folder, entry->name);
            if (!folder_entry && sys_stat(path, &info) == 0) {
                put_number((uint64_t)info.size, 8);
                put(" B");
            }
            put("\n");
        }
    }
    sys_close((int)fd);
    return 0;
}

int main(int argc, char **argv) {
    int bad = 0;

    if (argc < 2) {
        return list(".");
    }
    for (int i = 1; i < argc; i++) {
        if (argc > 2) {
            put(argv[i]);
            put(":\n");
        }
        bad |= list(argv[i]);
    }
    return bad;
}
