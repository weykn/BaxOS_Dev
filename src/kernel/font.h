#pragma once

#include <stdint.h>

/* The built-in character shapes, code page 437, 8x16 and one byte a scan
   line: all 256 glyphs in order. */
const uint8_t *font_glyphs(void);
