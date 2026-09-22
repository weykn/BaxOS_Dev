#pragma once

#include <stddef.h>
#include <stdint.h>

/* A flat filesystem with folders: a one-sector file table, then each file
   stored as one contiguous run of sectors. The kernel itself lives in it, as
   kernel.bin.

   Folders cost nothing on disk and need no flag of their own. An entry's
   name is the whole path - "docs/notes.txt" - and a folder is an entry whose
   name ends in a slash and that owns no sectors, which is what lets an empty
   one exist. So a name ending in '/' only ever names a folder and one that
   does not only ever names a file.

   There is a working directory. Every path below is taken relative to it
   unless it starts with '/', which means from the root.

   To keep RAM use down, everything goes through a single buffer that holds
   either the table or one sector of file data, so file entries are handed
   out as copies. */

/* Where the filesystem starts on its partition, and how its table announces
   itself. The loader looks for this to tell which partition to hand over, so
   it lives here rather than inside fs.c. */
#define FS_LBA    1
#define FS_MAGIC  0x34465842u       /* "BXF4" */
#define FS_SECTOR 512               /* bytes in a sector, here and on disk */

#define FS_NAME_LEN  48     /* a whole path, including the NUL */
#define FS_MAX_FILES 109    /* table entries: files and folders together */
#define FS_REMAPS    8      /* folders standing in for other folders */
#define FS_REMAP_LEN 24     /* either side of one, including the NUL */

enum {
    FS_EIO       = -1,      /* the disk reported an error */
    FS_ENOENT    = -2,      /* no such file, folder, or containing folder */
    FS_ENOSPC    = -3,      /* no free entry, or no gap big enough */
    FS_EINVAL    = -4,      /* empty or over-long path */
    FS_EEXIST    = -5,      /* a folder of that name is already there */
    FS_ENOTEMPTY = -6,      /* a folder with anything still in it */
};

struct fs_file {
    char     name[FS_NAME_LEN];     /* the whole path; empty means free */
    uint32_t start;                 /* first sector (LBA); 0 for a folder */
    uint32_t size;                  /* in bytes; 0 for a folder */
};

struct fs_stats {
    unsigned total;         /* sectors on the disk */
    unsigned used;          /* sectors taken by the boot sector, table and files */
    unsigned files;         /* entries in use, folders included */
};

/* Checks the disk has a filesystem. Returns 0 or FS_EIO. */
int fs_init(void);

/* Writes an empty file table for a disk of disk_sectors sectors. */
int fs_format(uint32_t disk_sectors);

/* Copies out table entry index, whose name is a whole path. Returns 0,
   FS_ENOENT if it is free, or FS_EIO. */
int fs_file(size_t index, struct fs_file *file);

/* Copies out the file at path. Returns 0 or an FS_E* code. */
int fs_stat(const char *path, struct fs_file *file);

/* Reads sector index of the run starting at start. Returns its 512 bytes,
   which stay valid until the next fs call, or NULL on a disk error. */
const char *fs_sector(uint32_t start, unsigned index);

/* Reads count whole sectors of the run starting at start, from sector index
   within it, straight into dest. Returns 0 or FS_EIO. */
int fs_read_many(uint32_t start, unsigned index, unsigned count, void *dest);

/* Creates the file or replaces its contents. Returns 0 or an FS_E* code. */
int fs_write(const char *path, const void *data, size_t size);

/* Deletes a file, or a folder with nothing left in it. */
int fs_remove(const char *path);

/* Creates a folder. Returns 0 or an FS_E* code. */
int fs_mkdir(const char *path);

/* Moves to a folder. "" and "/" are the root, ".." is one level up. */
int fs_chdir(const char *path);

/* The working directory: "" at the root, else "docs/" - no leading slash,
   always a trailing one. */
const char *fs_cwd(void);

/* The part of a whole path that lies directly inside folder - which is ""
   for the root, else "docs/" - ending in '/' if it is itself a folder, or
   NULL if it lies deeper, elsewhere, or is the folder itself. */
const char *fs_inside(const char *folder, const char *name);

/* The same against the working directory: what `ls` shows. */
const char *fs_in_cwd(const char *name);

/* The table's name for the folder at path - "docs/", or "" for the root -
   copied into out. Returns 0, or an FS_E* code if there is no such folder.
   An empty path gives the working directory. */
int fs_folder(const char *path, char *out, size_t max);

int fs_get_stats(struct fs_stats *stats);

const char *fs_error(int err);

/* The table entry of the folder at path, one-based; 0 means the root. Takes
   "." and any relative path, like everything else here. */
int fs_folder_at(const char *path, unsigned *index);

/* Renames a file, which is how one is moved: the sectors stay put. Returns 0
   or an FS_E* code; folders cannot be renamed. */
int fs_rename(const char *from, const char *to);

/* Writes size bytes into a file at offset, which may be anywhere up to and
   including its end - past the end is refused. Returns 0 or an FS_E* code. */
int fs_write_at(const char *path, uint32_t offset, const void *data, size_t size);

/* ---- remaps -------------------------------------------------------------
 *
 * A folder standing in for another one. A program off a Linux system puts
 * its settings in /.config/<name>, a folder this disk has no use for; a
 * remap of /.config to /conf/pkg lands them beside the rest of the
 * configuration instead. Every path goes through the same resolution, so one
 * remap holds for reading, writing, listing and deleting alike. */

/* Makes from stand for to - both folders, spelled either way round the
   slashes. An empty or NULL to takes the remap off again. Returns 0,
   FS_EINVAL if a name is empty or too long, FS_ENOSPC if there is no free
   slot, or FS_ENOENT when taking off one that is not there. */
int fs_remap(const char *from, const char *to);

/* Remap index, one of FS_REMAPS, for listing them. Returns 0 and points
   *from and *to at it, or FS_ENOENT if the slot is free. */
int fs_remap_at(unsigned index, const char **from, const char **to);
