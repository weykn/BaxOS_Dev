#include "fs.h"

#include <stdbool.h>

#include "ata.h"
#include "mem.h"
#include "string.h"

/* On-disk layout: sector FS_LBA, right after the boot sector, is a header
 * saying where everything else is. The file table is a run of 128-byte
 * entries, four to a sector, and an entry never moves while its file exists:
 * a program holding a folder open is holding its entry's number. Beside it is
 * an index - a hash table of 12-byte records, a path's hash, its folder's and
 * the entry it names - which is what finds a path: one sector of the index,
 * then the entry's. Everything else is file data, each file one contiguous
 * run of sectors.
 *
 * Nothing of this is kept in memory but the header. What was read lately is
 * in the disk cache like any other sector - the same cache the C library
 * sits in - and `cache on` loads the table and index into it when there is
 * room. A disk with a hundred thousand files costs no more RAM than one with
 * ten.
 *
 * Folders are only a spelling of the names in the table. tools/mkfs is built
 * from this file too, to create the disk image. */

#define SECTOR_SIZE  FS_SECTOR
#define DATA_LBA     (FS_LBA + 1)
#define PER_SECTOR   (SECTOR_SIZE / sizeof(struct fs_file))
#define TABLE_FIRST  64             /* entries a new filesystem starts with */
#define TABLE_STEP   256            /* and how many more each time it fills */
#define INDEX_FIRST  4              /* index sectors a new one starts with */
#define CHUNK        64             /* sectors moved in one call at most */

_Static_assert(SECTOR_SIZE % sizeof(struct fs_file) == 0,
               "an entry must not straddle two sectors");

struct header {
    uint32_t magic;
    uint32_t disk_sectors;          /* every file must end before this */
    uint32_t table_lba, table_size; /* the table's run, in entries */
    uint32_t table_next;            /* entries ever used: the rest are new */
    uint32_t free_head;             /* a freed entry, plus one; 0 for none */
    uint32_t index_lba, index_sectors;
    uint32_t index_taken;           /* records in it, removed ones counted */
    uint32_t data_end;              /* nothing has been put at or past this */
};

/* A record of the index. entry is the entry's number plus one; 0 is a
   record never used, which ends a search, and GONE one whose file was
   removed, which a search steps over. */
struct record {
    uint32_t hash, parent, entry;
};

#define RECORDS  (SECTOR_SIZE / sizeof(struct record))
#define GONE     0xFFFFFFFFu

/* What find gives: an entry's number and what it says. */
struct slot {
    uint32_t index, start, size;
};

static struct header head;
/* The last few sectors read, whatever they were - the index and table
   sectors a path's lookup goes through are the same few again and again, and
   through the firmware each read costs milliseconds. So while the firmware
   reads, at boot, a borrowed page holds eight; once the disk driver module
   has the disk a read is quick, and four of the kernel's own are enough
   (fs_cache). sector points at the one in use, which stays good until the
   next call here. */
#define RECENT     4
#define RECENT_MAX 8
#define MOVE_RUN   128              /* sectors a call when a file is moved */

static char     small[RECENT][SECTOR_SIZE];
static char   (*recent)[SECTOR_SIZE] = small;
static unsigned slots = RECENT;
static uint32_t recent_lba[RECENT_MAX]; /* 0 for a slot holding nothing */
static uint32_t recent_used[RECENT_MAX], recent_clock;
static char    *sector = small[0];
static bool     held_table; /* whether head is the disk's */

static unsigned sectors_for(unsigned size);
static char lower(char c);
static int name_cmp(const char *a, const char *b);

static unsigned table_sectors(uint32_t size) {
    return sectors_for(size * (unsigned)sizeof(struct fs_file));
}

/* FNV-1a over the first n bytes of a name, case folded as names are
   matched. Never zero. */
static uint32_t hash_of(const char *name, size_t n) {
    uint32_t h = 0x811C9DC5u;

    for (size_t i = 0; i < n; i++) {
        h = (h ^ (uint8_t)lower(name[i])) * 0x01000193u;
    }
    return h != 0 ? h : 1;
}

/* The hash of the folder a name is in - "" for the root, else everything up
   to the slash before its last part. */
static uint32_t parent_hash(const char *name) {
    size_t n = strlen(name);

    if (n > 0 && name[n - 1] == '/') {
        n--;                        /* a folder's own slash */
    }
    while (n > 0 && name[n - 1] != '/') {
        n--;
    }
    return hash_of(name, n);
}

/* Points sector at the slot used longest ago, emptied, to fill with lba -
   or with nothing, 0, when it is only somewhere to put a sector together. */
static unsigned take_slot(uint32_t lba) {
    unsigned old = 0;

    for (unsigned i = 1; i < slots; i++) {
        if (recent_used[i] < recent_used[old]) {
            old = i;
        }
    }
    recent_lba[old] = lba;
    recent_used[old] = ++recent_clock;
    sector = recent[old];
    return old;
}

static void scratch(void) {
    take_slot(0);
}

static int load_sector(uint32_t lba) {
    for (unsigned i = 0; i < slots; i++) {
        if (recent_lba[i] == lba && lba != 0) {
            recent_used[i] = ++recent_clock;
            sector = recent[i];
            return 0;
        }
    }
    unsigned slot = take_slot(0);

    if (ata_read(lba, sector) < 0) {
        return FS_EIO;
    }
    recent_lba[slot] = lba;
    return 0;
}

/* Writes to the disk, and forgets what was kept of those sectors - unless
   it is what is being written, which is then already what the disk says. */
static int put(uint32_t lba, unsigned count, const void *data) {
    for (unsigned i = 0; i < slots; i++) {
        if (recent_lba[i] >= lba && recent_lba[i] < lba + count && data != recent[i]) {
            recent_lba[i] = 0;
        }
    }
    return ata_write_many(lba, count, data);
}

/* Forgets the sector in use: a write of it failed, so what the disk has is
   anyone's guess. */
static void forget(void) {
    for (unsigned i = 0; i < slots; i++) {
        if (sector == recent[i]) {
            recent_lba[i] = 0;
        }
    }
}

static int load_table(void) {
    if (held_table) {
        return 0;
    }
    if (load_sector(FS_LBA) < 0) {
        return FS_EIO;
    }
    memcpy(&head, sector, sizeof head);
    if (head.magic != FS_MAGIC) {
        return FS_EIO;
    }
    held_table = true;
    return 0;
}

static int save_header(void) {
    if (load_sector(FS_LBA) < 0) {
        return FS_EIO;
    }
    memset(sector, 0, SECTOR_SIZE);
    memcpy(sector, &head, sizeof head);
    if (put(FS_LBA, 1, sector) < 0) {
        forget();
        held_table = false;
        return FS_EIO;
    }
    return 0;
}

/* Entry index, copied out of its sector. */
static int read_entry(size_t index, struct fs_file *out) {
    if (index >= head.table_next ||
        load_sector(head.table_lba + (uint32_t)(index / PER_SECTOR)) < 0) {
        return FS_EIO;
    }
    *out = ((const struct fs_file *)sector)[index % PER_SECTOR];
    return 0;
}

/* Writes entry index - its name too, unless name is NULL. The other entries
   in its sector are read first and go back as they were. */
static int write_entry(size_t index, const char *name, uint32_t start, uint32_t size) {
    uint32_t lba = head.table_lba + (uint32_t)(index / PER_SECTOR);
    struct fs_file *entry;

    if (load_sector(lba) < 0) {
        return FS_EIO;
    }
    entry = &((struct fs_file *)sector)[index % PER_SECTOR];
    if (name != NULL) {
        memset(entry->name, 0, FS_NAME_LEN);
        memcpy(entry->name, name, strlen(name));
    }
    entry->start = start;
    entry->size = size;
    if (put(lba, 1, sector) < 0) {
        forget();
        return FS_EIO;
    }
    return 0;
}

/* Ends an operation. Its sectors are on the disk; the flush that makes the
   drive keep them waits for idle, sync or power off (ata_sync). */
static int done(int err) {
    return err;
}

/* ---- the index ----------------------------------------------------------- */

/* Visits the index sectors a hash's search goes through, from its own on,
   with the sector in `sector`. The visitor returns true to stop. */
static int probe(uint32_t hash, bool (*visit)(uint32_t lba, void *ctx), void *ctx) {
    for (uint32_t i = 0; i < head.index_sectors; i++) {
        uint32_t lba = head.index_lba + (hash + i) % head.index_sectors;

        if (load_sector(lba) < 0) {
            return FS_EIO;
        }
        if (visit(lba, ctx)) {
            return 0;
        }
    }
    return 0;
}

struct search {
    const char *name;
    uint32_t    hash;
    uint32_t    candidates[RECORDS];    /* entries whose hash matched */
    unsigned    count;
    bool        ended;                  /* a never-used record was met */
};

static bool gather(uint32_t lba, void *ctx) {
    struct search *s = ctx;
    const struct record *r = (const struct record *)sector;

    (void)lba;
    for (unsigned i = 0; i < RECORDS; i++) {
        if (r[i].entry == 0) {
            s->ended = true;
            break;
        }
        if (r[i].entry != GONE && r[i].hash == s->hash && s->count < RECORDS) {
            s->candidates[s->count++] = r[i].entry - 1;
        }
    }
    return s->ended || s->count > 0;
}

/* The entry named name, or NULL - one sector of the index, then the entry's
   own to be sure of the name, since two names can share a hash. The answer
   lasts until the next call. */
static struct slot *find(const char *name) {
    static struct slot found;
    struct search s = { .name = name, .hash = hash_of(name, strlen(name)) };
    struct fs_file entry;

    if (*name == '\0' || load_table() < 0) {
        return NULL;
    }
    for (;;) {
        s.count = 0;
        if (probe(s.hash, gather, &s) < 0) {
            return NULL;
        }
        for (unsigned i = 0; i < s.count; i++) {
            if (read_entry(s.candidates[i], &entry) == 0 && name_cmp(entry.name, name) == 0) {
                found = (struct slot){ s.candidates[i], entry.start, entry.size };
                return &found;
            }
        }
        /* The matches here were other names: the rare search that carries on
           is done the slow way, record by record, rather than being clever. */
        if (s.ended || s.count == 0) {
            break;
        }
        for (uint32_t i = 0; i < head.index_sectors * RECORDS; i++) {
            uint32_t at = (s.hash + i / RECORDS) % head.index_sectors;
            const struct record *r;

            if (load_sector(head.index_lba + at) < 0) {
                return NULL;
            }
            r = &((const struct record *)sector)[i % RECORDS];
            if (r->entry == 0) {
                return NULL;
            }
            if (r->entry != GONE && r->hash == s.hash) {
                uint32_t e = r->entry - 1;

                if (read_entry(e, &entry) == 0 && name_cmp(entry.name, name) == 0) {
                    found = (struct slot){ e, entry.start, entry.size };
                    return &found;
                }
            }
        }
        break;
    }
    return NULL;
}

static unsigned allocate(unsigned count, const struct slot *replacing);
static unsigned allocate_gap(unsigned count, const struct slot *replacing);

/* The index remade twice the size, from the table: what keeps its searches
   one sector long. Built in memory for the moment it takes, then written. */
static int index_grow(void) {
    uint32_t sectors = head.index_sectors * 2;
    char *table = mem_alloc((size_t)sectors * SECTOR_SIZE);
    uint32_t taken = 0, at;
    struct fs_file entry;

    if (table == NULL) {
        return FS_ENOSPC;
    }
    memset(table, 0, (size_t)sectors * SECTOR_SIZE);
    for (uint32_t e = 0; e < head.table_next; e++) {
        if (read_entry(e, &entry) < 0) {
            mem_free(table);
            return FS_EIO;
        }
        if (entry.name[0] == '\0') {
            continue;
        }
        /* Records fill a sector and leave its last few bytes over, just as
           they lie on the disk. */
        uint32_t h = hash_of(entry.name, strlen(entry.name));
        uint32_t i = (h % sectors) * RECORDS;
        struct record *r;

        for (;;) {
            r = (struct record *)(table + (size_t)(i / RECORDS) * SECTOR_SIZE) + i % RECORDS;
            if (r->entry == 0) {
                break;
            }
            i = (i + 1) % (sectors * RECORDS);
        }
        *r = (struct record){ h, parent_hash(entry.name), e + 1 };
        taken++;
    }
    if ((at = allocate(sectors, NULL)) == 0 ||
        put(at, sectors, table) < 0) {
        mem_free(table);
        return at == 0 ? FS_ENOSPC : FS_EIO;
    }
    mem_free(table);
    head.index_lba = at;
    head.index_sectors = sectors;
    head.index_taken = taken;
    return save_header();
}

struct placing {
    struct record record;
    uint32_t      remove;           /* the entry whose record goes, plus one */
};

static bool place(uint32_t lba, void *ctx) {
    struct placing *p = ctx;
    struct record *r = (struct record *)sector;

    for (unsigned i = 0; i < RECORDS; i++) {
        bool hit = p->remove != 0 ? r[i].entry == p->remove
                                  : r[i].entry == 0 || r[i].entry == GONE;

        if (p->remove != 0 && r[i].entry == 0) {
            return true;            /* not there */
        }
        if (hit) {
            if (p->remove == 0 && r[i].entry == 0) {
                head.index_taken++;
            }
            r[i] = p->remove != 0 ? (struct record){ 0, 0, GONE } : p->record;
            if (put(lba, 1, sector) < 0) {
                forget();
            }
            return true;
        }
    }
    return false;
}

/* Adds entry's record under name - which has to be in the table already -
   the index grown instead if it is half full: growing makes it again from
   the table, this entry and all. */
static int index_add(const char *name, uint32_t entry) {
    struct placing p = { { hash_of(name, strlen(name)), parent_hash(name), entry + 1 }, 0 };

    if ((head.index_taken + 1) * 2 > head.index_sectors * RECORDS) {
        return index_grow();
    }
    return probe(p.record.hash, place, &p);
}

static int index_remove(const char *name, uint32_t entry) {
    struct placing p = { { hash_of(name, strlen(name)), 0, 0 }, entry + 1 };

    return probe(p.record.hash, place, &p);
}

static unsigned sectors_for(unsigned size) {
    return (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
}

/* ---- paths --------------------------------------------------------------
 *
 * The table holds whole paths, so every call here first turns the path it
 * was given into one of those - joining it to the working directory, unless
 * it starts from the root - and then looks that up like any other name. */

static char cwd[FS_NAME_LEN];       /* "" at the root, else "docs/" */
static char full[FS_NAME_LEN];      /* where resolve builds the table's name */

/* Names are matched without regard to case, and stored with whatever case
   they were created with - so HOME.md and home.md are the same file, and it
   is listed the way it was written. */
static char lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}

static int name_cmp(const char *a, const char *b) {
    while (*a != '\0' && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return (int)(uint8_t)lower(*a) - (int)(uint8_t)lower(*b);
}

static bool starts_with(const char *name, const char *prefix) {
    while (*prefix != '\0') {
        if (lower(*name++) != lower(*prefix++)) {
            return false;
        }
    }
    return true;
}

static bool is_link(const struct slot *file) {
    return (file->size & FS_LINK) != 0;
}

/* Bytes of data an entry owns, which for a link is its target. */
static unsigned bytes_of(const struct slot *file) {
    return file->size & ~FS_LINK;
}

/* ---- following links -----------------------------------------------------
 *
 * A link is a file holding a path, and resolving a name walks it a part at
 * a time: each part that turns out to be a link is swapped for what it
 * holds, and the walk carries on from there - from the root if that starts
 * with '/', and otherwise from the folder the link is in. The last part is
 * only followed when the caller asks, since lstat, readlink and unlink are
 * about the link itself. */

#define WALK_MAX (FS_NAME_LEN + FS_LINK_LEN)   /* a path, links swapped in */

static char walk[WALK_MAX];         /* what is left of the path being resolved */
static int  resolve_err;            /* why resolve_any last gave NULL */

/* The whole path a name stands for, "" being the root - which is a folder
   with no entry of its own, so only the callers that can mean the root use
   this; the rest go through resolve, which turns "" into no name at all.
   Returns NULL, with resolve_err saying why, if the result would not fit, the
   path is empty, or it goes round more than FS_LINKS links. */
static const char *resolve_any(const char *path, bool follow) {
    unsigned links = 0;
    size_t n = 0;
    char *p = walk;

    resolve_err = FS_EINVAL;
    if (*path == '\0' || strlen(path) >= sizeof walk) {
        return NULL;
    }
    if (load_table() < 0) {
        resolve_err = FS_EIO;
        return NULL;
    }
    if (*path != '/') {
        n = strlen(cwd);            /* from where we are, not from the root */
        memcpy(full, cwd, n);
    }
    strcpy(walk, path);

    /* Part by part, so that "." and ".." mean what they do everywhere else:
       a script saying ./wallpaper/one.png names a file beside it. */
    while (*p != '\0') {
        size_t len = 0;

        while (p[len] != '\0' && p[len] != '/') {
            len++;
        }
        bool folder = p[len] == '/';

        if (len == 0) {
            p++;                    /* a leading slash, or "//" */
            continue;
        }
        if (len == 1 && p[0] == '.') {
            ;                       /* here: nothing to add */
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            if (n > 0) {            /* up one, and past the root is the root */
                for (n--; n > 0 && full[n - 1] != '/'; n--) {
                }
            }
        } else {
            if (n + len + 2 > FS_NAME_LEN) {
                return NULL;
            }
            memcpy(full + n, p, len);
            full[n + len] = '\0';

            struct slot *link = folder || follow ? find(full) : NULL;

            if (link != NULL && is_link(link)) {
                size_t size = bytes_of(link);
                char *rest = p + len;
                size_t left = strlen(rest);

                if (++links > FS_LINKS) {
                    resolve_err = FS_ELOOP;
                    return NULL;
                }
                if (size + left + 1 > sizeof walk || load_sector(link->start) < 0) {
                    resolve_err = size + left + 1 > sizeof walk ? FS_EINVAL : FS_EIO;
                    return NULL;
                }
                /* What the link holds, then whatever came after it. */
                memmove(walk + size, rest, left + 1);
                memcpy(walk, sector, size);
                p = walk;
                if (*p == '/') {
                    n = 0;          /* from the root */
                }
                continue;           /* and from the link's own folder if not */
            }
            n += len;
            if (folder) {
                full[n++] = '/';
            }
        }
        p += folder ? len + 1 : len;
    }
    full[n] = '\0';
    return full;                    /* "" is the root, which has no entry */
}

/* The same, for everything that needs an actual entry. An empty result means
   the root, and the root has no entry to find - and find("") would hand back
   the first free one, which is not the same thing at all. */
static const char *resolve(const char *path, bool follow) {
    const char *full = resolve_any(path, follow);

    return full != NULL && *full != '\0' ? full : NULL;
}

/* The same, ending in the slash that makes it a folder's name. */
static const char *dir_name(const char *path, bool follow) {
    size_t n;

    if (resolve(path, follow) == NULL) {
        return NULL;
    }
    n = strlen(full);
    if (full[n - 1] != '/') {
        if (n + 2 > FS_NAME_LEN) {
            return NULL;
        }
        full[n] = '/';
        full[n + 1] = '\0';
    }
    return full;
}

/* True if the folder holding name exists; the root always does. Needs the
   table loaded, and name has to be full, which it can cut and put back. */
static bool parent_exists(char *name) {
    size_t n = strlen(name);
    char keep;
    bool there;

    if (n > 0 && name[n - 1] == '/') {
        n--;                        /* a folder's own slash, not its parent's */
    }
    while (n > 0 && name[n - 1] != '/') {
        n--;
    }
    if (n == 0) {
        return true;
    }
    keep = name[n];
    name[n] = '\0';
    there = find(name) != NULL;
    name[n] = keep;
    return there;
}

/* A run of count free sectors, counting the sectors of the file being
   replaced as free. Returns 0 if there is no gap big enough.

   Most of the time it is where the file already is - a rewrite that fits,
   or a file at the end growing - or else past everything put on the disk so
   far, which the header remembers. Only once that reaches the end of the
   disk are the gaps looked for, and then where everything is comes out of
   the table for the length of the call, read a chunk at a time, so nothing
   about it has to be remembered in between. The header is the caller's to
   save. */
static unsigned allocate(unsigned count, const struct slot *replacing) {
    if (replacing != NULL) {
        unsigned have = sectors_for(replacing->size & ~FS_LINK);

        if (count <= have) {
            return replacing->start;
        }
        if (replacing->start + have == head.data_end &&
            replacing->start + count <= head.disk_sectors) {
            head.data_end = replacing->start + count;
            return replacing->start;
        }
    }
    if (head.data_end + count <= head.disk_sectors) {
        head.data_end += count;
        return head.data_end - count;
    }
    return allocate_gap(count, replacing);
}

static unsigned allocate_gap(unsigned count, const struct slot *replacing) {
    uint32_t n = head.table_next + 2, used = 0;
    struct run { uint32_t start, end; } *runs = mem_alloc(n * sizeof *runs);
    struct fs_file *chunk = mem_alloc(CHUNK * SECTOR_SIZE);
    unsigned start = DATA_LBA;
    bool moved = true;

    if (runs == NULL || chunk == NULL) {
        mem_free(runs);
        mem_free(chunk);
        return 0;
    }
    runs[used++] = (struct run){ head.table_lba, head.table_lba + table_sectors(head.table_size) };
    runs[used++] = (struct run){ head.index_lba, head.index_lba + head.index_sectors };
    for (uint32_t at = 0; at < table_sectors(head.table_next); at += CHUNK) {
        uint32_t k = table_sectors(head.table_next) - at < CHUNK ?
                     table_sectors(head.table_next) - at : CHUNK;

        if (ata_read_many(head.table_lba + at, k, chunk) < 0) {
            used = 0;
            break;
        }
        for (uint32_t i = 0; i < k * PER_SECTOR && at * PER_SECTOR + i < head.table_next; i++) {
            uint32_t bytes = chunk[i].size & ~FS_LINK;

            if (chunk[i].name[0] != '\0' && bytes > 0 &&
                (replacing == NULL || replacing->index != at * PER_SECTOR + i)) {
                runs[used++] = (struct run){ chunk[i].start, chunk[i].start + sectors_for(bytes) };
            }
        }
    }
    mem_free(chunk);

    /* Hop past every run the candidate overlaps until none do. */
    while (moved && used > 0) {
        moved = false;
        for (uint32_t i = 0; i < used; i++) {
            if (runs[i].start < start + count && start < runs[i].end) {
                start = runs[i].end;
                moved = true;
            }
        }
    }
    mem_free(runs);
    return used > 0 && start + count <= head.disk_sectors ? start : 0;
}

/* A free entry's number: one freed before, or a new one, the table grown if
   there is none - copied to a run TABLE_STEP entries longer, which the
   header then points at. Returns -1 if the disk has no room. The header is
   the caller's to save. */
static long free_entry(void) {
    uint32_t size = head.table_size + TABLE_STEP;
    unsigned have = table_sectors(head.table_size), count = table_sectors(size);
    char *chunk;
    unsigned at;

    if (head.free_head != 0) {
        struct fs_file entry;
        uint32_t index = head.free_head - 1;

        if (read_entry(index, &entry) < 0) {
            return -1;
        }
        head.free_head = entry.start;       /* a freed one keeps the next there */
        return (long)index;
    }
    if (head.table_next < head.table_size) {
        return (long)head.table_next++;
    }
    if ((at = allocate(count, NULL)) == 0 ||
        (chunk = mem_alloc(CHUNK * SECTOR_SIZE)) == NULL) {
        return -1;
    }
    for (unsigned i = 0; i < count; i += CHUNK) {
        unsigned n = count - i < CHUNK ? count - i : CHUNK;
        unsigned old = i < have ? (have - i < n ? have - i : n) : 0;

        memset(chunk, 0, CHUNK * SECTOR_SIZE);
        if ((old > 0 && ata_read_many(head.table_lba + i, old, chunk) < 0) ||
            put(at + i, n, chunk) < 0) {
            mem_free(chunk);
            return -1;
        }
    }
    mem_free(chunk);
    head.table_lba = at;
    head.table_size = size;
    return (long)head.table_next++;
}

/* Frees entry index onto the list of free ones. */
static int drop_entry(size_t index, const char *name) {
    int err = index_remove(name, (uint32_t)index);

    if (err == 0) {
        err = write_entry(index, "", head.free_head, 0);
    }
    if (err == 0) {
        head.free_head = (uint32_t)index + 1;
        err = save_header();
    }
    return err;
}

void fs_cache(bool slow) {
    char (*page)[SECTOR_SIZE] = slow ? mem_alloc(RECENT_MAX * SECTOR_SIZE) : NULL;

    if (slow && page == NULL) {
        return;
    }
    if (recent != small) {
        mem_free(recent);
    }
    recent = slow ? page : small;
    slots = slow ? RECENT_MAX : RECENT;
    memset(recent_lba, 0, sizeof recent_lba);
    sector = recent[0];
}

int fs_init(void) {
    if (load_table() < 0) {
        held_table = false;
        return FS_EIO;
    }
    return 0;
}

int fs_format(uint32_t disk_sectors) {
    unsigned table = table_sectors(TABLE_FIRST);
    char *zero = mem_alloc((size_t)(table + INDEX_FIRST) * SECTOR_SIZE);

    if (zero == NULL) {
        return FS_EIO;
    }
    memset(zero, 0, (size_t)(table + INDEX_FIRST) * SECTOR_SIZE);
    head = (struct header){
        .magic = FS_MAGIC, .disk_sectors = disk_sectors,
        .table_lba = DATA_LBA, .table_size = TABLE_FIRST,
        .index_lba = DATA_LBA + table, .index_sectors = INDEX_FIRST,
        .data_end = DATA_LBA + table + INDEX_FIRST,
    };
    held_table = true;
    if (put(DATA_LBA, table + INDEX_FIRST, zero) < 0) {
        mem_free(zero);
        return FS_EIO;
    }
    mem_free(zero);
    return done(save_header());
}

int fs_runs(uint32_t lba[2], unsigned count[2]) {
    if (load_table() < 0) {
        return FS_EIO;
    }
    lba[0] = head.table_lba;
    count[0] = table_sectors(head.table_next);
    lba[1] = head.index_lba;
    count[1] = head.index_sectors;
    return 0;
}

int fs_file(size_t index, struct fs_file *file) {
    if (load_table() < 0) {
        return FS_EIO;
    }
    if (index >= head.table_next || read_entry(index, file) < 0) {
        return FS_ENOENT;
    }
    return file->name[0] != '\0' ? 0 : FS_ENOENT;
}

int fs_list(const char *folder, size_t *cursor, struct fs_file *file, size_t *index) {
    uint32_t inside = hash_of(folder, strlen(folder));

    if (load_table() < 0) {
        return FS_EIO;
    }
    /* Through the index, record by record: the folder's hash says which
       entries might be in it, and only those are read. */
    for (; *cursor < (size_t)head.index_sectors * RECORDS; (*cursor)++) {
        const struct record *r;

        if (load_sector(head.index_lba + (uint32_t)(*cursor / RECORDS)) < 0) {
            return FS_EIO;
        }
        r = &((const struct record *)sector)[*cursor % RECORDS];
        if (r->entry == 0 || r->entry == GONE || r->parent != inside) {
            continue;
        }
        *index = r->entry - 1;
        if (fs_file(*index, file) == 0 && fs_inside(folder, file->name) != NULL) {
            (*cursor)++;
            return 0;
        }
    }
    return FS_ENOENT;
}

static int stat_at(const char *path, bool follow, struct fs_file *file) {
    if (resolve(path, follow) == NULL) {
        return resolve_err;
    }
    struct slot *found = find(full);
    if (found == NULL) {
        return FS_ENOENT;
    }
    memcpy(file->name, full, strlen(full) + 1);
    file->start = found->start;
    file->size = found->size;
    return 0;
}

int fs_stat(const char *path, struct fs_file *file) {
    return stat_at(path, true, file);
}

int fs_lstat(const char *path, struct fs_file *file) {
    return stat_at(path, false, file);
}

const char *fs_sector(uint32_t start, unsigned index) {
    return load_sector(start + index) == 0 ? sector : NULL;
}

int fs_read_many(uint32_t start, unsigned index, unsigned count, void *dest) {
    return ata_read_many(start + index, count, dest) == 0 ? 0 : FS_EIO;
}

/* Writes a file, or a link when flag is FS_LINK, under name - a whole path,
   already resolved into full. */
static int store(const char *name, const void *data, size_t size, uint32_t flag) {
    /* A trailing slash is how a folder is spelled, so it is not a file. */
    if (name[strlen(name) - 1] == '/') {
        return FS_EINVAL;
    }
    if (!parent_exists(full)) {
        return FS_ENOENT;
    }
    struct slot *file = find(name), old = { 0 };
    bool fresh = file == NULL;
    long index;

    if (!fresh) {
        old = *file;
        index = old.index;
    } else if ((index = free_entry()) < 0) {
        return FS_ENOSPC;
    }
    unsigned count = sectors_for((unsigned)size);
    unsigned start = allocate(count, fresh ? NULL : &old);
    if (start == 0) {
        return FS_ENOSPC;
    }

    /* Every whole sector goes straight from the caller's memory in one
       write; only the last, which has to be zero-padded to the end of its
       sector, goes through the buffer. The table stays in hand throughout. */
    unsigned whole = (unsigned)(size / SECTOR_SIZE);

    if (whole > 0 && put(start, whole, data) < 0) {
        return FS_EIO;
    }
    if (whole < count) {
        size_t done = (size_t)whole * SECTOR_SIZE;

        scratch();
        memset(sector, 0, SECTOR_SIZE);
        memcpy(sector, (const char *)data + done, size - done);
        if (put(start + whole, 1, sector) < 0) {
            return FS_EIO;
        }
    }

    int err = write_entry((size_t)index, name, start, (uint32_t)size | flag);

    if (err == 0 && fresh) {
        err = index_add(name, (uint32_t)index);
    }
    if (err == 0) {
        err = save_header();        /* where data ends, and the entries */
    }
    return done(err);
}

int fs_write(const char *path, const void *data, size_t size) {
    const char *name = resolve(path, true);

    return name == NULL ? resolve_err : store(name, data, size, 0);
}

int fs_symlink(const char *target, const char *path) {
    size_t size = strlen(target);
    const char *name = resolve(path, false);
    size_t n;

    if (name == NULL) {
        return resolve_err;
    }
    if (size == 0 || size >= FS_LINK_LEN) {
        return FS_EINVAL;
    }
    /* Taken already, whether by a file, a link, or a folder. */
    n = strlen(full);
    if (find(full) != NULL || n + 2 > FS_NAME_LEN) {
        return FS_EEXIST;
    }
    full[n] = '/';
    full[n + 1] = '\0';
    if (find(full) != NULL) {
        return FS_EEXIST;
    }
    full[n] = '\0';
    return store(full, target, size, FS_LINK);
}

int fs_readlink(const char *path, char *out, size_t max) {
    struct fs_file link = { .size = 0 };
    int err = fs_lstat(path, &link);
    size_t size;

    if (err < 0) {
        return err;
    }
    if ((link.size & FS_LINK) == 0) {
        return FS_EINVAL;
    }
    if (load_sector(link.start) < 0) {
        return FS_EIO;
    }
    size = (link.size & ~FS_LINK) < max ? (link.size & ~FS_LINK) : max;
    memcpy(out, sector, size);
    return (int)size;
}

int fs_remove(const char *path) {
    if (resolve(path, false) == NULL) {
        return resolve_err;         /* a link goes itself, not what it names */
    }
    struct slot *file = find(full);

    if (file == NULL) {
        /* Not a file of that name, so try it as a folder - which goes only
           once there is nothing left in it. */
        if (dir_name(path, false) == NULL) {
            return resolve_err;
        }
        file = find(full);
        if (file == NULL) {
            return FS_ENOENT;
        }
        uint32_t index = file->index;
        struct fs_file other;
        size_t cursor = 0, at;

        if (fs_list(full, &cursor, &other, &at) == 0) {
            return FS_ENOTEMPTY;
        }
        return done(drop_entry(index, full));
    }
    return done(drop_entry(file->index, full));
}

int fs_mkdir(const char *path) {
    size_t n;
    bool taken;

    if (dir_name(path, false) == NULL) {
        return resolve_err;
    }
    /* Taken as a folder, or as a file or a link spelled without the slash. */
    n = strlen(full);
    full[n - 1] = '\0';
    taken = find(full) != NULL;
    full[n - 1] = '/';
    if (taken || find(full) != NULL) {
        return FS_EEXIST;
    }
    if (!parent_exists(full)) {
        return FS_ENOENT;
    }
    long index = free_entry();
    if (index < 0) {
        return FS_ENOSPC;
    }
    /* No sectors at all: that is what lets an empty folder exist, and what
       leaves allocate() nothing to step over. */
    int err = write_entry((size_t)index, full, 0, 0);

    if (err == 0) {
        err = index_add(full, (uint32_t)index);
    }
    if (err == 0) {
        err = save_header();
    }
    return done(err);
}

int fs_chdir(const char *path) {
    size_t n;

    if (*path == '\0' || strcmp(path, "/") == 0) {
        cwd[0] = '\0';
        return 0;
    }
    if (strcmp(path, "..") == 0) {
        n = strlen(cwd);
        if (n > 0) {
            for (n--; n > 0 && cwd[n - 1] != '/'; n--) {
            }
            cwd[n] = '\0';
        }
        return 0;
    }
    if (dir_name(path, true) == NULL) {
        return resolve_err;
    }
    if (find(full) == NULL) {
        return FS_ENOENT;
    }
    n = strlen(full);
    memcpy(cwd, full, n + 1);
    return 0;
}

const char *fs_cwd(void) {
    return cwd;
}

const char *fs_inside(const char *folder, const char *name) {
    const char *rest = name + strlen(folder);

    if (!starts_with(name, folder) || *rest == '\0') {
        return NULL;                /* elsewhere, or the folder itself */
    }
    for (const char *p = rest; *p != '\0'; p++) {
        if (*p == '/' && p[1] != '\0') {
            return NULL;            /* deeper down than this folder */
        }
    }
    return rest;
}

const char *fs_in_cwd(const char *name) {
    return fs_inside(cwd, name);
}

int fs_folder(const char *path, char *out, size_t max) {
    const char *name = cwd;
    size_t n;

    if (*path != '\0' && strcmp(path, "/") != 0) {
        if (dir_name(path, true) == NULL) {
            return resolve_err;
        }
        if (find(full) == NULL) {
            return FS_ENOENT;
        }
        name = full;
    } else if (strcmp(path, "/") == 0) {
        name = "";
    }
    n = strlen(name);
    if (n + 1 > max) {
        return FS_EINVAL;
    }
    memcpy(out, name, n + 1);
    return 0;
}

int fs_get_stats(struct fs_stats *stats) {
    if (load_table() < 0) {
        return FS_EIO;
    }
    struct fs_file *chunk = mem_alloc(CHUNK * SECTOR_SIZE);
    uint32_t sectors = table_sectors(head.table_next);

    if (chunk == NULL) {
        return FS_EIO;
    }
    stats->total = head.disk_sectors;
    stats->used = DATA_LBA + table_sectors(head.table_size) + head.index_sectors;
    stats->files = 0;
    for (uint32_t at = 0; at < sectors; at += CHUNK) {
        uint32_t k = sectors - at < CHUNK ? sectors - at : CHUNK;

        if (ata_read_many(head.table_lba + at, k, chunk) < 0) {
            mem_free(chunk);
            return FS_EIO;
        }
        for (uint32_t i = 0; i < k * PER_SECTOR && at * PER_SECTOR + i < head.table_next; i++) {
            if (chunk[i].name[0] != '\0') {
                stats->used += sectors_for(chunk[i].size & ~FS_LINK);
                stats->files++;
            }
        }
    }
    mem_free(chunk);
    return 0;
}

const char *fs_error(int err) {
    static const char *const messages[] = {
        "disk error", "no such file or folder", "no space", "bad name",
        "already there", "not empty", "too many links",
    };
    return messages[-err - 1];
}

/* The table entry of the folder at path, one-based, or 0 for the root. A
   program opening "." to read it is asking for the working directory, and
   resolve has already turned that into a whole path. */
int fs_folder_at(const char *path, unsigned *index) {
    const char *full = resolve_any(path, true);
    char name[FS_NAME_LEN];
    size_t n;

    if (full == NULL) {
        return resolve_err;
    }
    if (*full == '\0') {
        *index = 0;                 /* the root */
        return 0;
    }
    n = strlen(full);
    if (n + 2 > FS_NAME_LEN) {
        return FS_EINVAL;
    }
    memcpy(name, full, n);
    if (name[n - 1] != '/') {
        name[n++] = '/';            /* a folder is spelled with one */
    }
    name[n] = '\0';
    if (load_table() < 0) {
        return FS_EIO;
    }
    struct slot *found = find(name);

    if (found == NULL) {
        return FS_ENOENT;
    }
    *index = found->index + 1;
    return 0;
}

/* Gives a file a different name, which is all that moving one is here: the
   sectors stay where they are and only the table changes. A folder is not
   moved, since every path inside it would have to change with it. */
int fs_rename(const char *from, const char *to) {
    char was[FS_NAME_LEN];
    const char *name = resolve(from, false);

    if (name == NULL || name[strlen(name) - 1] == '/') {
        return name == NULL ? resolve_err : FS_EINVAL;
    }
    memcpy(was, name, strlen(name) + 1);
    if (find(was) == NULL) {
        return FS_ENOENT;
    }

    name = resolve(to, false);
    if (name == NULL || name[strlen(name) - 1] == '/') {
        return name == NULL ? resolve_err : FS_EINVAL;
    }
    if (name_cmp(was, name) == 0) {
        return 0;                   /* already where it is being put */
    }
    if (find(name) != NULL) {
        return FS_EEXIST;
    }
    if (!parent_exists(full)) {
        return FS_ENOENT;
    }
    struct slot file = *find(was);
    int err = index_remove(was, file.index);

    if (err == 0) {
        err = write_entry(file.index, full, file.start, file.size);
    }
    if (err == 0) {
        err = index_add(full, file.index);
    }
    return done(err);
}

/* Writes into a file at offset, which may be its end - a program writing one
   a block at a time - or anywhere already in it, which is what a program
   that seeks about its own file does. Anything past the end extends it; a
   gap left behind reads as whatever was in those sectors, since nothing here
   goes out of its way to zero them.

   Files are one unbroken run of sectors, so a file that outgrows the gap it
   sits in is copied to a bigger one; the sectors it already filled are
   carried across a sector at a time, through the same one-sector buffer
   everything else here uses. */
int fs_write_at(const char *path, uint32_t offset, const void *data, size_t size) {
    const char *name = resolve(path, true);
    struct slot *file;
    unsigned have, need, start, end;

    if (name == NULL) {
        return resolve_err;
    }
    file = find(name);
    if (file == NULL) {
        return FS_ENOENT;
    }
    if (size == 0) {
        return 0;
    }
    end = offset + (unsigned)size;
    if (end < file->size) {
        end = file->size;           /* written into, not onto the end */
    }
    have = sectors_for(file->size);
    need = sectors_for(end);
    start = file->start;

    if (need > have) {
        unsigned room = allocate(need, file);

        if (room == 0) {
            return FS_ENOSPC;
        }
        /* The file moves whole, a run of sectors a call: what a write costs
           is the call, not the bytes. Past the old end, as a program that
           seeks beyond it and writes may, the whole sectors in between read
           as zeroes, as they do on Linux; the one the old end was in is
           zero past it already. */
        unsigned gap = offset / SECTOR_SIZE > have ? offset / SECTOR_SIZE : have;

        if ((room != start && have > 0) || gap > have) {
            char *run = mem_alloc(MOVE_RUN * SECTOR_SIZE);

            if (run == NULL) {
                return FS_ENOSPC;
            }
            for (unsigned i = room != start ? 0 : have; i < gap; i += MOVE_RUN) {
                unsigned n = gap - i < MOVE_RUN ? gap - i : MOVE_RUN;
                unsigned old = i < have ? (have - i < n ? have - i : n) : 0;

                memset(run, 0, (size_t)n * SECTOR_SIZE);
                if ((old > 0 && ata_read_many(start + i, old, run) < 0) ||
                    put(room + i, n, run) < 0) {
                    mem_free(run);
                    return FS_EIO;
                }
            }
            mem_free(run);
        }
        start = room;
    }

    /* The sectors it covers whole go straight from the data, in one call;
       one only partly covered - its first and last - is read back first, so
       the bytes already in it are kept. */
    for (size_t done = 0; done < size;) {
        unsigned at = (unsigned)((offset + done) / SECTOR_SIZE);
        unsigned into = (unsigned)((offset + done) % SECTOR_SIZE);
        size_t whole = into == 0 ? (size - done) / SECTOR_SIZE : 0;

        if (whole > 0) {
            if (put(start + at, (unsigned)whole, (const char *)data + done) < 0) {
                return FS_EIO;
            }
            done += whole * SECTOR_SIZE;
            continue;
        }
        size_t room = SECTOR_SIZE - into;
        size_t n = size - done < room ? size - done : room;

        scratch();
        memset(sector, 0, SECTOR_SIZE);
        if (at < have && ata_read(start + at, sector) < 0) {
            return FS_EIO;
        }
        memcpy(sector + into, (const char *)data + done, n);
        if (put(start + at, 1, sector) < 0) {
            return FS_EIO;
        }
        done += n;
    }

    if (load_table() < 0) {
        return FS_EIO;
    }
    file = find(name);
    if (file == NULL) {
        return FS_EIO;
    }
    int err = write_entry(file->index, NULL, start, end);

    return done(err == 0 ? save_header() : err);
}
