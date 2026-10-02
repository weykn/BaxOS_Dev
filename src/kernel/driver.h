#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The kernel's slots for drivers that live in modules. Each is empty until
 * its module registers, and back to empty when the module goes, which a
 * module refuses while the kernel still needs it (module.h). */

/* A disk the filesystem can be read from once the firmware is gone
   (storage/ide). Sectors count from the start of the disk; calls answer 0,
   or -1 on an error. */
struct disk_driver {
    int      (*read)(uint64_t lba, unsigned count, void *buffer);
    int      (*write)(uint64_t lba, unsigned count, const void *buffer);
    int      (*flush)(void);
    /* The first sector of the partition whose sector at offset within it
       starts with magic, or 0 if there is no disk or no such partition. */
    uint64_t (*find)(uint32_t offset, uint32_t magic);
};

/* What the disk said, kept in memory (storage/cache): every read of the
   disk goes through it, and every write past it. read fills count sectors
   from lba into out, from what it holds or else from the disk, which it
   reads through disk; written is told what was written, to keep what it
   holds true. shrink gives back at least bytes of memory, if it holds that
   much - the kernel asks it before an allocation fails - and answers how
   much it gave; memory is what it holds now. */
struct disk_cache {
    int    (*read)(uint32_t lba, unsigned count, void *out,
                   int (*disk)(uint32_t lba, unsigned count, void *out));
    void   (*written)(uint32_t lba, unsigned count, const void *data);
    size_t (*shrink)(size_t bytes);
    size_t (*memory)(void);
};

/* A keyboard read without the firmware (input/ps2). */
struct keyboard_driver {
    bool (*start)(void);            /* takes it over; false if there is none */
    char (*key)(void);              /* the next character typed, or 0 */
};

/* A record of the syscalls programs make (debug/trace). begin answers
   what end is handed back, or NULL for nothing to finish; program names
   whose calls these are from now on, and answers the name before, which
   is put back when that program ends (NULL is the kernel's own); flush
   writes out what is held, and is called when the machine is idle. */
#define TRACE_NAME 20               /* a program's name as it keeps it */

struct tracer {
    void       *(*begin)(uint32_t number, uint64_t a, uint64_t b, uint64_t c);
    void        (*end)(void *entry, uint64_t result);
    const char *(*program)(const char *name);
    void        (*flush)(void);
};

extern const struct disk_driver    *disk_driver;
extern const struct disk_cache     *disk_cache;
extern const struct tracer         *tracer;
extern const struct keyboard_driver *keyboard_driver;

/* Called by a module as it starts, and with NULL as it goes. */
void disk_register(const struct disk_driver *d);
void disk_cache_register(const struct disk_cache *c);
void keyboard_register(const struct keyboard_driver *k);
void tracer_register(const struct tracer *t);
