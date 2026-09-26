#include "fs.h"

#include <stdbool.h>

#include "ata.h"
#include "string.h"

/* On-disk layout: sector FS_LBA, right after the boot sector, is the file
 * table - an 8-byte header, then FS_MAX_FILES 56-byte entries. Everything
 * after it is file data, each file one contiguous run of sectors.
 *
 * Folders are only a spelling of the names in that table, so the layout is
 * exactly what it was before them: boot.asm still finds kernel.bin by
 * comparing the name at the front of an entry, and nothing on disk had to
 * move. tools/mkfs is built from this file too, to create the disk image. */

#define SECTOR_SIZE    FS_SECTOR
#define TABLE_SECTORS  15           /* what the table spans */
#define TABLE_BYTES    (TABLE_SECTORS * SECTOR_SIZE)
#define DATA_LBA       (FS_LBA + TABLE_SECTORS)

struct table {
    uint32_t       magic;
    uint32_t       disk_sectors;    /* every file must end before this */
    struct fs_file files[FS_MAX_FILES];
};

_Static_assert(sizeof(struct table) <= TABLE_BYTES,
               "the file table must fit the sectors set aside for it");

/* Two buffers, because a read goes through the firmware and costs about a
   millisecond: the table, which every path lookup wants, and one sector of
   file data. They were one buffer once, and the two then took turns throwing
   each other out - every name looked up cost the table being read back, all
   eight sectors of it, and every character of a script being run cost the
   sector it was in. Half a second of a boot went that way. */
static union {
    struct table table;
    char         data[TABLE_BYTES];
} buf;

static char     sector[SECTOR_SIZE];    /* one sector of file data */
static uint32_t held;       /* the LBA in sector; 0 means none */
static bool     held_table; /* whether buf holds the table */

static int load_sector(uint32_t lba) {
    if (held == lba) {
        return 0;
    }
    held = 0;
    if (ata_read(lba, sector) < 0) {
        return FS_EIO;
    }
    held = lba;
    return 0;
}

static int load_table(void) {
    if (held_table) {
        return 0;
    }
    /* One call for the whole table rather than one for each of its sectors:
       the firmware charges by the call, not by the byte. */
    if (ata_read_many(FS_LBA, TABLE_SECTORS, buf.data) < 0) {
        return FS_EIO;
    }
    held_table = true;
    return 0;
}

/* Writes back the part of the table one entry lies in - one of its sixteen
   sectors, or two when the entry straddles a boundary - and ends whatever
   operation was writing it: the table is saved last by everything here, so
   this is where the disk is made to catch up.

   The whole table used to go back for every fifty-six bytes that changed,
   and since a write of any kind ends in one of these, eight kilobytes of it
   was most of what writing a file cost. */
static int save_entry(const struct fs_file *file) {
    size_t at = (size_t)((const char *)file - buf.data);
    unsigned first = (unsigned)(at / SECTOR_SIZE);
    unsigned last = (unsigned)((at + sizeof *file - 1) / SECTOR_SIZE);

    if (ata_write_many(FS_LBA + first, last - first + 1,
                       buf.data + (size_t)first * SECTOR_SIZE) < 0) {
        held_table = false;
        return FS_EIO;
    }
    ata_sync();
    return 0;
}

/* All of it, which only formatting needs. */
static int save_table(void) {
    if (ata_write_many(FS_LBA, TABLE_SECTORS, buf.data) < 0) {
        held_table = false;
        return FS_EIO;
    }
    ata_sync();
    return 0;
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

/* The loaded table's entry for name, or NULL. Free entries have an empty
   name, so find("") returns the first free one. */
static struct fs_file *find(const char *name) {
    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        if (name_cmp(buf.table.files[i].name, name) == 0) {
            return &buf.table.files[i];
        }
    }
    return NULL;
}

static bool is_link(const struct fs_file *file) {
    return (file->size & FS_LINK) != 0;
}

/* Bytes of data an entry owns, which for a link is its target. */
static unsigned bytes_of(const struct fs_file *file) {
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

            struct fs_file *link = folder || follow ? find(full) : NULL;

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

/* The first run of count free sectors, counting the sectors of the file being
   replaced as free. Returns 0 if there is no gap big enough. */
static unsigned allocate(unsigned count, const struct fs_file *replacing) {
    unsigned start = DATA_LBA;
    bool moved = true;

    /* Hop past every file the candidate run overlaps until none do. */
    while (moved) {
        moved = false;
        for (size_t i = 0; i < FS_MAX_FILES; i++) {
            const struct fs_file *file = &buf.table.files[i];
            unsigned end = file->start + sectors_for(bytes_of(file));

            if (file != replacing && file->name[0] != '\0' &&
                file->start < start + count && start < end) {
                start = end;
                moved = true;
            }
        }
    }
    return start + count <= buf.table.disk_sectors ? start : 0;
}

int fs_init(void) {
    if (load_table() < 0 || buf.table.magic != FS_MAGIC) {
        held = 0;
        held_table = false;
        return FS_EIO;
    }
    return 0;
}

int fs_format(uint32_t disk_sectors) {
    memset(&buf, 0, sizeof buf);
    buf.table.magic = FS_MAGIC;
    buf.table.disk_sectors = disk_sectors;
    held = 0;
    held_table = true;
    return save_table();
}

int fs_file(size_t index, struct fs_file *file) {
    if (load_table() < 0) {
        return FS_EIO;
    }
    *file = buf.table.files[index];
    return file->name[0] != '\0' ? 0 : FS_ENOENT;
}

static int stat_at(const char *path, bool follow, struct fs_file *file) {
    if (resolve(path, follow) == NULL) {
        return resolve_err;
    }
    struct fs_file *found = find(full);
    if (found == NULL) {
        return FS_ENOENT;
    }
    *file = *found;
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
    struct fs_file *file = find(name);
    if (file == NULL) {
        file = find("");
    }
    if (file == NULL) {
        return FS_ENOSPC;
    }
    size_t index = (size_t)(file - buf.table.files);
    unsigned count = sectors_for((unsigned)size);
    unsigned start = allocate(count, file);
    if (start == 0) {
        return FS_ENOSPC;
    }

    /* Every whole sector goes straight from the caller's memory in one
       write; only the last, which has to be zero-padded to the end of its
       sector, goes through the buffer. The table stays in hand throughout. */
    unsigned whole = (unsigned)(size / SECTOR_SIZE);

    if (whole > 0 && ata_write_many(start, whole, data) < 0) {
        return FS_EIO;
    }
    if (whole < count) {
        size_t done = (size_t)whole * SECTOR_SIZE;

        held = 0;
        memset(sector, 0, SECTOR_SIZE);
        memcpy(sector, (const char *)data + done, size - done);
        if (ata_write(start + whole, sector) < 0) {
            return FS_EIO;
        }
    }

    if (load_table() < 0) {
        return FS_EIO;
    }
    file = &buf.table.files[index];
    memcpy(file->name, name, strlen(name) + 1);
    file->start = start;
    file->size = (uint32_t)size | flag;
    return save_entry(file);
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
    if (!is_link(&link)) {
        return FS_EINVAL;
    }
    if (load_sector(link.start) < 0) {
        return FS_EIO;
    }
    size = bytes_of(&link) < max ? bytes_of(&link) : max;
    memcpy(out, sector, size);
    return (int)size;
}

int fs_remove(const char *path) {
    if (resolve(path, false) == NULL) {
        return resolve_err;         /* a link goes itself, not what it names */
    }
    struct fs_file *file = find(full);

    if (file == NULL) {
        /* Not a file of that name, so try it as a folder - which goes only
           once there is nothing left under it. */
        if (dir_name(path, false) == NULL) {
            return resolve_err;
        }
        file = find(full);
        if (file == NULL) {
            return FS_ENOENT;
        }
        for (size_t i = 0; i < FS_MAX_FILES; i++) {
            struct fs_file *other = &buf.table.files[i];

            if (other != file && other->name[0] != '\0' && starts_with(other->name, full)) {
                return FS_ENOTEMPTY;
            }
        }
    }
    memset(file, 0, sizeof *file);
    return save_entry(file);
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
    struct fs_file *entry = find("");
    if (entry == NULL) {
        return FS_ENOSPC;
    }
    /* No sectors at all: that is what lets an empty folder exist, and what
       leaves allocate() nothing to step over. */
    memset(entry, 0, sizeof *entry);
    memcpy(entry->name, full, strlen(full) + 1);
    return save_entry(entry);
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
    stats->total = buf.table.disk_sectors;
    stats->used = DATA_LBA;
    stats->files = 0;
    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        if (buf.table.files[i].name[0] != '\0') {
            stats->used += sectors_for(bytes_of(&buf.table.files[i]));
            stats->files++;
        }
    }
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
    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        if (name_cmp(buf.table.files[i].name, name) == 0) {
            *index = (unsigned)i + 1;
            return 0;
        }
    }
    return FS_ENOENT;
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
    struct fs_file *file = find(was);

    memcpy(file->name, full, strlen(full) + 1);
    return save_entry(file);
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
    struct fs_file *file;
    unsigned have, need, start, end;

    if (name == NULL) {
        return resolve_err;
    }
    file = find(name);
    if (file == NULL) {
        return FS_ENOENT;
    }
    if (offset > file->size) {
        return FS_EINVAL;           /* no writing past the end of one */
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
        if (room != start && have > 0) {
            for (unsigned i = 0; i < have; i++) {
                held = 0;
                if (ata_read(start + i, sector) < 0 ||
                    ata_write(room + i, sector) < 0) {
                    return FS_EIO;
                }
            }
        }
        start = room;
    }

    /* Sector by sector. One only partly covered by the write is read back
       first, so the bytes already in it are kept. */
    for (size_t done = 0; done < size;) {
        unsigned at = (unsigned)((offset + done) / SECTOR_SIZE);
        unsigned into = (unsigned)((offset + done) % SECTOR_SIZE);
        size_t room = SECTOR_SIZE - into;
        size_t n = size - done < room ? size - done : room;

        held = 0;
        memset(sector, 0, SECTOR_SIZE);
        if (n < SECTOR_SIZE && at < have && ata_read(start + at, sector) < 0) {
            return FS_EIO;
        }
        memcpy(sector + into, (const char *)data + done, n);
        if (ata_write(start + at, sector) < 0) {
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
    file->start = start;
    file->size = end;
    return save_entry(file);
}
