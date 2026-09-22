#pragma once

#include <stddef.h>
#include <stdint.h>

/* A PNG reader, for the wallpaper.
 *
 * It decodes as it reads and never holds the picture: rows are handed to the
 * caller one at a time, in order, and forgotten. A screen-sized wallpaper is
 * already megabytes, and the file it came from is bigger still - keeping
 * either whole would cost more memory than everything else here put together.
 *
 * Only what a photograph saved by anything modern actually uses is
 * understood: eight bits a channel, colour or colour with alpha, not
 * interlaced. */

/* Everything it needs from the world, so that the decoder itself knows
   nothing about files or memory. */
struct png_io {
    /* Reads up to len bytes; returns how many, 0 at the end of the file. */
    size_t (*read)(void *ctx, void *buffer, size_t len);
    /* Working memory, freed again before png_decode returns. */
    void *(*alloc)(void *ctx, size_t len);
    void  (*free)(void *ctx, void *memory);
    /* The size, before any row: what the picture will need to be fitted
       into whatever it is going onto. */
    void  (*size)(void *ctx, unsigned width, unsigned height);
    /* One row of the picture, top first: width pixels of channels bytes,
       red, green, blue and possibly alpha. */
    void  (*row)(void *ctx, unsigned y, const uint8_t *pixels, unsigned width,
                 unsigned channels);
    void *ctx;
};

#define PNG_EINVAL  (-1)    /* not a PNG, or not one this understands */
#define PNG_ENOSPC  (-2)    /* no memory to work in */
#define PNG_EDATA   (-3)    /* damaged: the compressed stream does not add up */

/* Decodes, calling io->row for each row. The size is reported before any row
   is. Returns 0 or a PNG_E* code. */
int png_decode(const struct png_io *io, unsigned *width, unsigned *height);
