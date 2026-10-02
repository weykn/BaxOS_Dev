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
