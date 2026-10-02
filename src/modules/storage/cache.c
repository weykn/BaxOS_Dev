/* storage/cache: the disk cache - what the disk said, kept in memory, so that
 * the next program to want the same library does not go to the disk for it.
 *
 * It works as Linux's page cache does: every read of the disk goes through
 * it, whatever it read stays, and it grows into memory nobody else is using.
 * The moment anything else wants that memory the kernel asks for some back
 * (shrink), and the lines used longest ago go first - so what it holds is
 * free memory lent out, and `mem` counts it apart from what is used.
 *
 * It holds lines of 64 KiB, the disk read in whole lines: a read costs the
 * call, not the bytes, so a line is as quick as a sector, and it is the
 * reading ahead Linux does. A line is also what is given back, whole, which
 * keeps the kernel's list of free memory short.
 *
 * Everything it keeps about the lines is in one block it allocates, and
 * reallocates as the lines outgrow it, so the module itself is a page.
 *
 *   cache                  what it holds, and the files it starts with
 *   cache auto, cache on   as much as is free, less a reserve (the default)
 *   cache <size>           no more than size: 64M, 512K
 *   cache off              nothing; what it held is given back
 *   cache add|rm <file>    files read in whenever it starts, and now */

#include "driver.h"
#include "fs.h"
#include "mem.h"
#include "module.h"
#include "proc.h"
#include "string.h"
#include "vga.h"

#define LINE_SECTORS 128
#define LINE_BYTES   (LINE_SECTORS * 512)
#define LINE_PAGES   (LINE_BYTES / 4096)
#define RESERVE_KIB  8192           /* what auto leaves free for everything else */
#define BOOT_LINES   256            /* auto while the firmware has the memory:
                                       asking it what is free costs a memory map */
#define BUCKETS      256
#define NONE         0xFFFFFFFFu

struct line {
    uint32_t first;                 /* its first sector; NONE when it holds nothing */
    uint32_t used;                  /* when it was last read, for the LRU */
    uint32_t next;                  /* the next in its bucket, one-based */
    char    *data;                  /* NULL when it has given its memory back */
};

static struct state {
    uint32_t bucket[BUCKETS];       /* each chain's first line, one-based */
    char     files[256];            /* read in at the start, a NUL after each */
    uint32_t files_len, room;       /* ...and how many lines there is room for */
    struct line lines[];
} *st;

static unsigned count;              /* lines with memory */
static uint32_t clock;
static size_t   limit;              /* in lines; 0 is auto */
static bool     off, busy;          /* busy: inside a read, not to be shrunk */

#define HEAD(n) (sizeof(struct state) + (n) * sizeof(struct line))

static uint32_t *chain(uint32_t first) {
    return &st->bucket[first / LINE_SECTORS % BUCKETS];
}

static void unhook(unsigned i) {
    uint32_t *at = chain(st->lines[i].first);

    while (*at != 0 && *at != i + 1) {
        at = &st->lines[*at - 1].next;
    }
    if (*at == i + 1) {
        *at = st->lines[i].next;
    }
    st->lines[i].first = NONE;
}

static struct line *find(uint32_t first) {
    for (uint32_t at = *chain(first); at != 0; at = st->lines[at - 1].next) {
        if (st->lines[at - 1].first == first) {
            return &st->lines[at - 1];
        }
    }
    return NULL;
}

/* The one used longest ago that has memory, or -1. */
static int oldest(void) {
    int best = -1;

    for (unsigned i = 0; i < st->room; i++) {
        if (st->lines[i].data != NULL && (best < 0 || st->lines[i].used < st->lines[best].used)) {
            best = (int)i;
        }
    }
    return best;
}

/* Gives lines back, oldest first, until no more than most are left. */
static size_t cap(size_t most) {
    size_t given = 0;
    int i;

    while (count > most && (i = oldest()) >= 0) {
        if (st->lines[i].first != NONE) {
            unhook((unsigned)i);
        }
        mem_pages_free((uint64_t)st->lines[i].data, LINE_PAGES);
        st->lines[i].data = NULL;
        count--;
        given += LINE_BYTES;
    }
    return given;
}

static bool may_grow(void) {
    if (limit != 0) {
        return count < limit;
    }
    if (!mem_ours()) {
        return count < BOOT_LINES;
    }
    return mem_free_kib() > RESERVE_KIB + LINE_BYTES / 1024;
}

/* A line with memory to fill: a new one bought, or the oldest. */
static int slot(void) {
    if (may_grow()) {
        if (count == st->room) {
            struct state *bigger = mem_alloc(HEAD(st->room * 2));

            if (bigger != NULL) {
                memcpy(bigger, st, HEAD(st->room));
                memset(bigger->lines + st->room, 0, st->room * sizeof(struct line));
                bigger->room = st->room * 2;
                mem_free(st);
                st = bigger;
            }
        }
        for (unsigned i = 0; i < st->room; i++) {
            if (st->lines[i].data == NULL) {
                uint64_t at = mem_pages(LINE_PAGES);

                if (at == 0) {
                    break;
                }
                st->lines[i] = (struct line){ NONE, 0, 0, (char *)at };
                count++;
                return (int)i;
            }
        }
    }
    int i = oldest();

    if (i >= 0 && st->lines[i].first != NONE) {
        unhook((unsigned)i);
    }
    return i;
}

static int cache_read(uint32_t lba, unsigned n, void *out,
                      int (*disk)(uint32_t, unsigned, void *)) {
    char *to = out;
    int err = 0;

    if (off) {
        return disk(lba, n, out);
    }
    busy = true;
    while (n > 0 && err == 0) {
        uint32_t first = lba - lba % LINE_SECTORS, within = lba - first;
        unsigned take = LINE_SECTORS - within < n ? LINE_SECTORS - within : n;
        struct line *l = find(first);

        if (l == NULL) {
            int i = slot();

            if (i < 0 || disk(first, LINE_SECTORS, st->lines[i].data) < 0) {
                err = disk(lba, n, to);     /* around it */
                break;
            }
            l = &st->lines[i];
            l->first = first;
            l->next = *chain(first);
            *chain(first) = (uint32_t)i + 1;
        }
        l->used = ++clock;
        memcpy(to, l->data + (size_t)within * 512, (size_t)take * 512);
        to += (size_t)take * 512;
        lba += take;
        n -= take;
    }
    busy = false;
    return err;
}

/* What is written goes into whatever line holds those sectors, so the line
   stays true: the file table is written by every change to the disk. */
static void cache_written(uint32_t lba, unsigned n, const void *data) {
    for (uint32_t first = lba - lba % LINE_SECTORS; first < lba + n; first += LINE_SECTORS) {
        struct line *l = find(first);
        uint32_t from = lba > first ? lba : first;
        uint32_t to = lba + n < first + LINE_SECTORS ? lba + n : first + LINE_SECTORS;

        if (l != NULL) {
            memcpy(l->data + (size_t)(from - first) * 512,
                   (const char *)data + (size_t)(from - lba) * 512, (size_t)(to - from) * 512);
        }
    }
}

static size_t cache_shrink(size_t bytes) {
    return busy ? 0 : cap(count > bytes / LINE_BYTES ? count - (bytes + LINE_BYTES - 1) / LINE_BYTES : 0);
}

static size_t cache_memory(void) {
    return (size_t)count * LINE_BYTES + HEAD(st->room);
}

static const struct disk_cache cache = { cache_read, cache_written, cache_shrink, cache_memory };

/* ---- the command ---------------------------------------------------------- */

/* sectors from lba read in, through the cache, a line at a time. */
static void read_in(uint32_t lba, unsigned sectors) {
    char *scratch = mem_alloc(LINE_BYTES);

    for (unsigned at = 0; scratch != NULL && at < sectors; at += LINE_SECTORS) {
        fs_read_many(lba + at, 0, sectors - at < LINE_SECTORS ? sectors - at : LINE_SECTORS,
                     scratch);
    }
    mem_free(scratch);
}

static void preload(const char *path) {
    struct fs_file file;

    if (fs_stat(path, &file) != 0 || file.size == 0) {
        kprintf("cache: %s: nothing to read\n", path);
        return;
    }
    read_in(file.start, (file.size + 511) / 512);
}

/* The listed files, then the file table and its index, which every path
   looked up reads. */
static void start(void) {
    uint32_t lba[2];
    unsigned n[2];

    for (size_t at = 0; at < st->files_len; at += strlen(st->files + at) + 1) {
        preload(st->files + at);
    }
    if (fs_runs(lba, n) == 0) {
        read_in(lba[0], n[0]);
        read_in(lba[1], n[1]);
    }
}

static char *listed(const char *path) {
    for (size_t at = 0; at < st->files_len; at += strlen(st->files + at) + 1) {
        if (strcmp(st->files + at, path) == 0) {
            return st->files + at;
        }
    }
    return NULL;
}

/* A size as "512K", "64M" or "1G"; 0 if it is not one. */
static size_t parse_bytes(const char *text) {
    size_t n = 0;

    while (*text >= '0' && *text <= '9') {
        n = n * 10 + (size_t)(*text++ - '0');
    }
    n <<= *text == 'k' || *text == 'K' ? 10 : *text == 'm' || *text == 'M' ? 20 :
          *text == 'g' || *text == 'G' ? 30 : 0;
    return text[*text != '\0'] == '\0' ? n : 0;
}

static void cmd_cache(char *args) {
    const char *what = str_word(&args);
    bool was = off;

    if (strcmp(what, "off") == 0) {
        off = true;
        cap(0);
    } else if (strcmp(what, "add") == 0 || strcmp(what, "rm") == 0) {
        for (const char *path; *(path = str_word(&args)) != '\0';) {
            char *at = listed(path);
            size_t n = strlen(path) + 1;

            if (what[0] == 'a' && at == NULL && st->files_len + n <= sizeof st->files) {
                memcpy(st->files + st->files_len, path, n);
                st->files_len += (uint32_t)n;
                if (!off) {
                    preload(path);
                }
            } else if (what[0] == 'r' && at != NULL) {
                memmove(at, at + n, st->files_len - (size_t)(at - st->files) - n);
                st->files_len -= (uint32_t)n;
            }
        }
    } else if (*what != '\0') {
        size_t bytes = parse_bytes(what);

        if (strcmp(what, "auto") != 0 && strcmp(what, "on") != 0 && bytes < LINE_BYTES) {
            kprintf("usage: cache [auto|on|off|<size>|add <file>...|rm <file>...]\n");
            return;
        }
        off = false;
        limit = bytes / LINE_BYTES;
        cap(limit != 0 ? limit : count);
        if (was) {
            start();
        }
    } else {
        unsigned held = count * (LINE_BYTES / 1024), free = (unsigned)mem_free_kib();

        if (off) {
            kprintf("  off\n");
        } else {
            usage_bar("cache", held, limit != 0 ? (unsigned)limit * (LINE_BYTES / 1024)
                      : held + (free > RESERVE_KIB ? free - RESERVE_KIB : 0),
                      limit != 0 ? "KiB" : "KiB, auto");
        }
        for (size_t at = 0; at < st->files_len; at += strlen(st->files + at) + 1) {
            kprintf("  %s\n", st->files + at);
        }
    }
}

static const struct proc_cmd cache_cmd = { "cache", "[auto|on|off|size|add|rm]", cmd_cache };

/* ---- the module ----------------------------------------------------------- */

MODULE_EXPORT int module_init(void) {
    if ((st = mem_alloc(HEAD(64))) == NULL) {
        return -12;                 /* ENOMEM */
    }
    memset(st, 0, HEAD(64));
    st->room = 64;
    disk_cache_register(&cache);
    proc_add(&cache_cmd);
    return 0;
}

MODULE_EXPORT int module_exit(void) {
    disk_cache_register(NULL);
    proc_remove(&cache_cmd);
    cap(0);
    mem_free(st);
    return 0;
}
