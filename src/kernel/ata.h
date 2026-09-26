#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* The disk we boot from, in 512-byte sectors. All of these return 0, or -1
   if the drive reports an error.
 *
 * Reading and writing many sectors at a time matters more here than anywhere
 * else in the kernel: each call is a round trip to the firmware, and the
 * firmware charges by the call rather than by the byte. */
int ata_read(uint32_t lba, void *buffer);
int ata_write(uint32_t lba, const void *buffer);

/* count sectors in one go, straight to or from buffer. */
int ata_read_many(uint32_t lba, unsigned count, void *buffer);

/* The disk's recent sectors, kept so that the next program to start need not
   read the same library again. Sets how much it may hold, rounded down to
   whole 128 KiB lines - 0 turns it off - dropping what it held. Returns the
   size it has now, which is 0 if there was no memory for even the list. */
size_t ata_cache_size(size_t bytes);

/* What it has taken from the firmware, what it may take, and how much of
   that is holding something. */
size_t ata_cache_memory(void);
size_t ata_cache_room(void);
size_t ata_cache_held(void);

/* Reads a run of sectors into it without copying them anywhere, and returns
   how many bytes it took. */
size_t ata_cache_read(uint32_t lba, unsigned count);
int ata_write_many(uint32_t lba, unsigned count, const void *buffer);

/* Pushes what has been written through the firmware's cache, so that it is
   on the disk rather than on its way there. A write is not durable until
   this returns, and it is what the whole operation costs - so it is called
   once at the end of one, not after every sector of it. */
void ata_sync(void);
