#pragma once

#include <stdint.h>

/* The built-in character shapes, code page 437, 8 pixels wide and one byte a
   scan line. height is 8 or 16; the table holds all 256 glyphs in order. */
const uint8_t *font_glyphs(unsigned height);
