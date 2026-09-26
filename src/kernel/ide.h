#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The first IDE disk, driven by the kernel itself once the firmware is gone:
 * programmed I/O through the legacy ports, polled, no interrupts. It is
 * what QEMU gives a machine by default, and what the firmware's disk driver
 * is replaced with - only if this finds the filesystem on it, which is what
 * decides whether the firmware can be let go at all.
 *
 * Sectors are counted from the start of the disk. Each call returns 0, or -1
 * if the drive reports an error or does not answer. */

int ide_read(uint64_t lba, unsigned count, void *buffer);
int ide_write(uint64_t lba, unsigned count, const void *buffer);
int ide_flush(void);

/* The first sector of the GPT partition whose sector at offset within it
   starts with magic, or 0 if there is no disk here or no such partition. */
uint64_t ide_find(uint32_t offset, uint32_t magic);
