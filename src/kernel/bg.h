#pragma once

#include <stdint.h>

/* The wallpaper: a picture behind the text.
 *
 * It is kept as pixels the size of the screen, scaled to cover it, so that
 * drawing a character over it costs no more than drawing one over black. The
 * console asks for those pixels wherever a cell's background is black, which
 * is what makes the text look as though it floats on the picture. */

/* Why a wallpaper would not load. Kept clear of the filesystem's own codes,
   which bg_set passes through as they are, so that a missing file and a
   damaged one do not read as the same thing. */
#define BG_EFORMAT (-101)   /* not a picture this can read */
#define BG_EMEMORY (-102)   /* no room for it */
#define BG_EDATA   (-103)   /* damaged */

/* Loads path, scaled to the screen and dimmed towards black by alpha - 100
   for the picture as it is, 50 for half way to black, 0 for none of it.
   Returns 0, an FS_E* code if the file will not read, or a BG_E* code. */
int bg_set(const char *path, unsigned alpha);

/* Back to a plain black background. */
void bg_clear(void);

/* Loads the wallpaper again at the screen's new size. Nothing happens if
   there is none. */
void bg_refresh(void);

/* Puts the wallpaper back if the screen changed size and dropped it. Cheap
   to call in a waiting loop, which is where it is called from. */
void bg_check(void);

/* What the wallpaper costs, for the `mem` command. */
uint32_t bg_memory(void);
