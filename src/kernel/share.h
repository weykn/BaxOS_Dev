#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pages of files that every process mapping them has in common.
 *
 * A library is the same bytes in every program that loads it, so a page of
 * one, read once, is mapped read-only into each of them; the first write to
 * it gives that process a copy of its own (vm.c). Each page is known by the
 * file's first sector and its page in the file, and counted, and goes back
 * when the last process lets go of it.
 *
 * A fork puts the parent's own pages here too, keyed by nothing, so parent
 * and child share them until one of them writes. */

/* The page for page index of the file at start, counted once more; 0 if no
   process has it. */
uint64_t shared_find(uint32_t start, uint32_t index);

/* Makes page - read from the file already - the one for start and index,
   counted once. False if there is no room to keep track of it. */
bool shared_add(uint32_t start, uint32_t index, uint64_t page);

/* A process's own page, from now on in common with a fork's child: counted
   once, for its holder, and with no file behind it. False without room. */
bool shared_adopt(uint64_t page);

/* The page back from the table to the one process that still has it, to
   write in as its own. False if anyone else has it too. */
bool shared_take(uint64_t page);

/* One more process has it mapped, or one fewer: the page goes back when no
   process has. */
void shared_hold(uint64_t page);
void shared_drop(uint64_t page);

/* Sectors lba .. lba + count have been written: a page read from them
   before is no longer what the file says, so nothing new gets it. */
void shared_written(uint32_t lba, unsigned count);

/* The pages held, and what keeping track of them costs. */
size_t shared_bytes(void);
