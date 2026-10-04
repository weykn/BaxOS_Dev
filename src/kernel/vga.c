#include "vga.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#include "boot.h"
#include "debug.h"
#include "efi_kernel.h"
#include "mem.h"
#include "font.h"
#include "string.h"

#define GLYPH_W 8

/* The text sizes, named by how tall a character cell ends up. There is one
   set of shapes in the image, 8x16: the larger sizes draw every glyph pixel
   as a square block, and 8 draws each pair of scan lines as one, which costs
   nothing but the drawing. */
static const struct font {
    const char *name;
    uint8_t     height;     /* scan lines of the glyph it draws */
    uint8_t     scale;      /* pixels drawn per glyph pixel */
} fonts[] = {
    { "8",   8, 1 },
    { "16", 16, 1 },
    { "32", 16, 2 },
    { "48", 16, 3 },
    { "64", 16, 4 },
};

#define FONTS (sizeof fonts / sizeof fonts[0])
#define FONT_DEFAULT 1                  /* 8x16, the plain one */

/* The screen, as the firmware set it up and the loader passed it on. */
static volatile uint8_t *fb;
static unsigned fb_pitch;
static unsigned real_w, real_h;         /* the firmware's mode */

/* The screen everything is drawn in, which is the firmware's mode unless
   `scale` asked for a smaller one. Then each of its pixels is drawn as a
   block of the real ones - xmap[x] up to xmap[x + 1] across, ymap likewise
   down - nearest-neighbour, the same shape kept, black at the edges. NULL
   maps mean one pixel for one. It needs nothing of the hardware, so it
   works wherever the framebuffer does. */
static unsigned fb_width, fb_height;
static unsigned want_w, want_h;         /* `scale`, or 0 for none */
static uint16_t *xmap, *ymap;

/* The firmware's screen protocol, for changing mode later; NULL if it could
   not be found again, which only costs us the `mode` command. */
static struct efi_gop *gop;
static uint32_t gop_taken = 0xFFFFFFFF;     /* the mode the console was laid out for */

/* The console as a cell array - one 16-bit cell a character, the code point
   low and the colour attribute high - which the glyphs are drawn from.
   Scrolling, clearing and the title bar work on it rather than on pixels,
   and comparing against it is what keeps unchanged cells from being redrawn
   into a framebuffer that is slow to write.
 *
 * The whole screen is text: there is no row reserved for anything else.
 * It is firmware memory: a console can want anything from twelve to sixty
 * kilobytes depending on the mode and the font, which is far too much to set
 * aside for the largest case. */
static volatile uint16_t *screen;
static size_t screen_bytes;


static const struct font *glyphs = &fonts[FONT_DEFAULT];
static const uint8_t *shapes;                   /* its 256 glyphs */
static unsigned cell_w, cell_h;                 /* a character cell, in pixels */
static char     mode_name[16];                  /* "1024x768", for vga_mode */
static char     list_name[16];                  /* one entry of the mode list */

static unsigned width;          /* columns */
static size_t   cells;          /* on screen */
static size_t   cursor;         /* row * width + column */
static uint8_t  color = VGA_LIGHTGRAY;
static uint8_t  pen = VGA_LIGHTGRAY;    /* color before reverse video swaps it */
static bool     reversed;

/* What a full-screen program leans on: the rows that scroll (top up to, not
   including, bottom), a character written in the last column holding the
   cursor there until the next one (so the bottom-right cell can be written
   without scrolling), and a cursor it can hide while it redraws. */
static unsigned region_top, region_bottom;
static bool     wrap_pending;
static bool     cursor_shown = true;
static bool     inserting;      /* each character opens a gap for itself */
static bool     cr_on_lf = true;        /* "\n" goes back to column 0 too */

/* The sixteen colours as the framebuffer wants them: one pixel per uint32.
   Written for blue in the low byte, and swapped at startup if the screen
   turns out to want red there. */
static bool red_first;          /* the screen wants red in the low byte */

static uint32_t palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

unsigned vga_width(void) {
    return width;
}

unsigned vga_height(void) {
    return width == 0 ? 0 : (unsigned)(cells / width);
}

static uint16_t cell(char c) {
    return (uint16_t)((uint8_t)c | color << 8);
}

/* ---- drawing ------------------------------------------------------------ */

/* Scan line y of the framebuffer. */
static volatile uint32_t *row(unsigned y) {
    return (volatile uint32_t *)(fb + (size_t)y * fb_pitch);
}

/* Draws n pixels of the screen's scan line y, from x on. Everything that
   draws comes through here. */
static void emit(unsigned x, unsigned y, const uint32_t *px, unsigned n) {
    if (xmap == NULL) {
        volatile uint32_t *line = row(y) + x;

        for (unsigned i = 0; i < n; i++) {
            line[i] = px[i];
        }
        return;
    }
    for (unsigned ry = ymap[y]; ry < ymap[y + 1]; ry++) {
        volatile uint32_t *line = row(ry);

        for (unsigned i = 0; i < n; i++) {
            for (unsigned rx = xmap[x + i]; rx < xmap[x + i + 1]; rx++) {
                line[rx] = px[i];
            }
        }
    }
}

/* Draws one cell's glyph, foreground and background both. */
static void draw_cell(size_t i, uint16_t value) {
    const uint8_t *glyph = shapes + (value & 0xFF) * 16;
    unsigned step = 16 / glyphs->height;
    uint32_t fg = palette[value >> 8 & 0x0F];
    uint32_t bg = palette[value >> 12 & 0x0F];
    unsigned left = (unsigned)(i % width) * cell_w;
    unsigned top_y = (unsigned)(i / width) * cell_h;
    uint32_t line[GLYPH_W * 4];         /* one scan line, at the largest scale */

    for (unsigned y = 0; y < glyphs->height; y++) {
        uint8_t bits = glyph[y * step] | glyph[y * step + step - 1];

        /* Each scan line of the glyph is drawn scale times, and each of its
           pixels scale times across, which is what makes the cell bigger. */
        for (unsigned again = 0; again < glyphs->scale; again++) {
            unsigned screen_y = top_y + y * glyphs->scale + again;
            uint32_t *to = line;

            for (unsigned x = 0; x < GLYPH_W; x++) {
                bool ink = (bits & 0x80 >> x) != 0;

                for (unsigned wide = 0; wide < glyphs->scale; wide++) {
                    *to++ = ink ? fg : bg;
                }
            }
            emit(left, screen_y, line, cell_w);
        }
    }
}

/* Paints scan lines y0 up to y1 black. */
static void fill_rows(unsigned y0, unsigned y1, uint32_t rgb);

static void fill_background(unsigned y0, unsigned y1) {
    fill_rows(y0, y1, palette[VGA_BLACK]);
}

/* Paints scan lines y0 up to y1 in one colour. Clearing a whole screen this
   way is far cheaper than drawing a blank glyph in every cell. */
static void fill_rows(unsigned y0, unsigned y1, uint32_t rgb) {
    uint32_t run[64];

    for (unsigned i = 0; i < 64; i++) {
        run[i] = rgb;
    }
    for (unsigned y = y0; y < y1; y++) {
        for (unsigned x = 0; x < fb_width; x += 64) {
            emit(x, y, run, fb_width - x < 64 ? fb_width - x : 64);
        }
    }
}

/* There is no hardware cursor in a graphics mode, so it is drawn: the bottom
   two scan lines of the cell, scaled with the font, in the foreground
   colour. Putting the cell back the way it was erases it. */
static void cursor_draw(bool on) {
    if (cursor >= cells || (on && !cursor_shown)) {
        return;
    }
    if (!on) {
        draw_cell(cursor, screen[cursor]);
        return;
    }
    unsigned left = (unsigned)(cursor % width) * cell_w;
    unsigned top_y = (unsigned)(cursor / width) * cell_h;
    uint32_t line[GLYPH_W * 4];

    for (unsigned x = 0; x < cell_w; x++) {
        line[x] = palette[screen[cursor] >> 8 & 0x0F];
    }
    for (unsigned y = cell_h - 2 * glyphs->scale; y < cell_h; y++) {
        emit(left, top_y + y, line, cell_w);
    }
}

/* Writes a cell. Drawing a glyph costs a cell's worth of pixels, so one that
   already holds what it is being given is left alone - which is most of the
   screen, and most of a scroll. */
static void put(size_t i, uint16_t value) {
    if (screen[i] != value) {
        screen[i] = value;
        draw_cell(i, value);
    }
}

/* ---- modes and fonts ---------------------------------------------------- */

static void use_font(const struct font *f) {
    glyphs = f;
    shapes = font_glyphs();
    cell_w = GLYPH_W * f->scale;
    cell_h = f->height * f->scale;
}

/* Lays the console out for the screen and font now in use: sizes the grid,
   takes memory for its cells from the firmware, agrees with what is on
   screen, and clears it. False if the grid would be too small to use or the
   memory could not be had, having changed nothing. */
static bool layout(void) {
    unsigned columns = fb_width / cell_w;
    unsigned rows = fb_height / cell_h;
    size_t bytes = (size_t)columns * rows * sizeof(uint16_t);
    void *memory;

    if (columns < 40 || rows < 8) {
        return false;
    }
    if ((memory = mem_alloc(bytes)) == NULL) {
        return false;
    }
    if (screen != NULL) {
        mem_free((void *)screen);
    }
    screen = memory;
    screen_bytes = bytes;
    width = columns;
    cells = (size_t)columns * rows;
    region_top = 0;
    region_bottom = rows;
    wrap_pending = false;

    /* The screen holds whatever it held before, so agree with it: every cell
       blank and black, and the whole of it painted to match. Otherwise put()
       would skip cells it wrongly believed were already drawn. */
    for (size_t i = 0; i < cells; i++) {
        screen[i] = 0;
    }
    fill_background(0, fb_height);
    vga_clear();
    return true;
}

/* Parses "1024x768". Returns false on anything else, including sizes far too
   large to be real, which keeps the arithmetic that follows in range. */
static bool parse_size(const char *s, unsigned *w, unsigned *h) {
    unsigned value = 0;
    unsigned *out = w;

    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            value = value * 10 + (unsigned)(*s - '0');
            if (value > 8192) {
                return false;
            }
        } else if (*s == 'x' && out == w && value != 0) {
            *w = value;
            value = 0;
            out = h;
        } else if (*s == '\0' && out == h && value != 0) {
            *h = value;
            return true;
        } else {
            return false;
        }
    }
}

uint64_t vga_framebuffer_end(void) {
    return (uint64_t)fb + (uint64_t)fb_pitch * real_h * 4;
}

/* Takes the framebuffer the firmware is using now. */
static void take_screen(uint64_t base, unsigned w, unsigned h, unsigned pitch,
                        unsigned format) {
    fb = (volatile uint8_t *)base;
    real_w = w;
    real_h = h;
    fb_pitch = pitch;
    ksprintf(mode_name, "%ux%u", w, h);

    /* The colours are written blue-first; a screen that wants red there gets
       the table turned round once, rather than every pixel turned round as
       it is drawn. */
    if (format == EFI_PIXEL_RGBX && !red_first) {
        red_first = true;
        for (unsigned i = 0; i < 16; i++) {
            uint32_t c = palette[i];
            palette[i] = (c & 0x00FF00) | (c >> 16 & 0xFF) | (c & 0xFF) << 16;
        }
    }
}

/* Sizes the screen drawn in against the real one, after either changes:
   the `scale` size if it fits, the real one otherwise - which is also what
   a map that cannot be had comes to. */
static void fit(void) {
    unsigned w = real_w, h = real_h;

    if (xmap != NULL) {
        mem_free(xmap);
        xmap = ymap = NULL;
    }
    if (want_w != 0 && want_w <= real_w && want_h <= real_h &&
        (want_w != real_w || want_h != real_h) &&
        (xmap = mem_alloc((want_w + want_h + 2) * sizeof(uint16_t))) != NULL) {
        /* As large as it goes without changing shape, in the middle. */
        unsigned out_w = real_w, out_h = real_h;

        w = want_w;
        h = want_h;
        if (real_w * h > real_h * w) {
            out_w = w * real_h / h;
        } else {
            out_h = h * real_w / w;
        }
        ymap = xmap + w + 1;
        for (unsigned i = 0; i <= w; i++) {
            xmap[i] = (uint16_t)((real_w - out_w) / 2 + i * out_w / w);
        }
        for (unsigned i = 0; i <= h; i++) {
            ymap[i] = (uint16_t)((real_h - out_h) / 2 + i * out_h / h);
        }
        for (unsigned y = 0; y < real_h; y++) {       /* the edges */
            volatile uint32_t *line = row(y);

            for (unsigned x = 0; x < real_w; x++) {
                line[x] = palette[VGA_BLACK];
            }
        }
    }
    fb_width = w;
    fb_height = h;
}

/* The firmware's mode i, if it is one we could draw in. */
static const struct efi_gop_info *gop_mode(unsigned i) {
    struct efi_gop_info *mode;
    efi_uintn size;

    if (gop == NULL || i >= gop->mode->max_mode ||
        EFI_ERROR(gop->query_mode(gop, i, &size, &mode)) ||
        mode->pixel_format > EFI_PIXEL_BGRX) {
        return NULL;
    }
    return mode;
}

unsigned vga_pixel_width(void) {
    return fb_width;
}

unsigned vga_pixel_height(void) {
    return fb_height;
}

static void repaint(void);

uint16_t vga_get(unsigned column, unsigned row) {
    size_t i = (size_t)row * width + column;

    return column < width && i < cells ? screen[i] : 0;
}

void vga_put(unsigned column, unsigned row, uint16_t value) {
    size_t i = (size_t)row * width + column;

    if (column < width && i < cells) {
        put(i, value);
    }
}

void vga_cursor(void) {
    cursor_draw(true);
}

/* Redraws every cell from the shadow, for when the screen has been taken
   away and given back. */
static void repaint(void) {
    for (size_t i = 0; i < cells; i++) {
        uint16_t value = screen[i];

        screen[i] = ~value;             /* so that put() sees a change */
        put(i, value);
    }
    cursor_draw(true);
}

/* The firmware's console owns the screen too, for as long as boot services
   are running, and it puts the mode back to its own whenever anything makes
   it look - leaving us drawing a picture of one size into a scanout of
   another. There is no way to tell it not to, so instead this notices and
   follows; everything that waits for a key calls it. */
void vga_firmware_gone(void) {
    gop = NULL;
}

void vga_follow(void) {
    unsigned was_w, was_h;

    if (gop == NULL || gop->mode->mode == gop_taken) {
        return;
    }
    was_w = width;
    was_h = (unsigned)(cells / (width != 0 ? width : 1));

    gop_taken = gop->mode->mode;
    take_screen(gop->mode->framebuffer, gop->mode->info->width,
                gop->mode->info->height, gop->mode->info->pixels_per_scanline * 4,
                gop->mode->info->pixel_format);
    fit();

    /* The same grid, so the cells still say what is on screen and only the
       pixels need putting back; a different one has to start over. */
    if (fb_width / cell_w == was_w && fb_height / cell_h == was_h) {
        repaint();
    } else {
        layout();
    }
}

int vga_start(struct boot_info *info) {
    struct efi_guid gop_guid = EFI_GOP_GUID;

    /* The screen is already up - the loader saw to that - so this cannot
       fail the way setting a mode could. Finding the protocol again is only
       so that the mode can be changed later. */
    if (info->system == NULL ||
        EFI_ERROR(info->system->boot->locate_protocol(&gop_guid, NULL, (void **)&gop))) {
        gop = NULL;
    }
    /* The firmware's own record of the mode is fresher than the loader's. */
    if (gop != NULL) {
        gop_taken = gop->mode->mode;
        take_screen(gop->mode->framebuffer, gop->mode->info->width,
                    gop->mode->info->height,
                    gop->mode->info->pixels_per_scanline * 4,
                    gop->mode->info->pixel_format);
    } else {
        take_screen(info->framebuffer, info->width, info->height, info->pitch,
                    info->pixel_format);
    }
    fit();
    use_font(&fonts[FONT_DEFAULT]);
    return layout() ? 0 : -1;
}

int vga_set_mode(const char *name) {
    unsigned w, h;

    if (gop == NULL || !parse_size(name, &w, &h)) {
        return -1;
    }
    for (unsigned i = 0; i < gop->mode->max_mode; i++) {
        const struct efi_gop_info *mode = gop_mode(i);

        if (mode == NULL || mode->width != w || mode->height != h) {
            continue;
        }
        if (EFI_ERROR(gop->set_mode(gop, i))) {
            return -1;
        }
        /* set_mode fills in a fresh framebuffer, which may be somewhere else
           entirely and need not have the pitch the width suggests. */
        gop_taken = gop->mode->mode;
        take_screen(gop->mode->framebuffer, gop->mode->info->width,
                    gop->mode->info->height, gop->mode->info->pixels_per_scanline * 4,
                    gop->mode->info->pixel_format);
        fit();
        layout();
        return 0;
    }
    return -1;
}

int vga_set_scale(const char *name) {
    unsigned w = 0, h = 0;

    if (strcmp(name, "off") != 0 &&
        (!parse_size(name, &w, &h) || w > real_w || h > real_h)) {
        return -1;
    }
    unsigned was_w = want_w, was_h = want_h;

    want_w = w;
    want_h = h;
    fit();
    if (layout()) {
        return 0;
    }
    /* Too small for a console: back to what it was. */
    want_w = was_w;
    want_h = was_h;
    fit();
    layout();
    return -1;
}

const char *vga_scale(void) {
    if (xmap == NULL) {
        return "off";
    }
    ksprintf(list_name, "%ux%u", fb_width, fb_height);
    return list_name;
}

int vga_set_font(const char *name) {
    const struct font *was = glyphs;

    for (unsigned i = 0; i < FONTS; i++) {
        if (strcmp(fonts[i].name, name) != 0) {
            continue;
        }
        use_font(&fonts[i]);
        if (layout()) {
            return 0;
        }
        /* The grid it would give is unusable. layout() changes nothing when
           it says so, so putting the font back is the whole of the undo. */
        use_font(was);
        return -1;
    }
    return -1;
}

/* The sizes the screen itself offers, which is a better list than any we
   could guess at. */
const char *vga_mode_name(unsigned i) {
    const struct efi_gop_info *mode = gop_mode(i);

    if (mode == NULL) {
        return NULL;
    }
    ksprintf(list_name, "%ux%u", mode->width, mode->height);
    return list_name;
}

const char *vga_mode(void) {
    return mode_name;
}

const char *vga_font_name(unsigned i) {
    return i < FONTS ? fonts[i].name : NULL;
}

const char *vga_font(void) {
    return glyphs->name;
}

/* What the console's cells cost, for the `mem` command. */
size_t vga_memory(void) {
    return screen_bytes;
}

/* ---- the console -------------------------------------------------------- */

void vga_set_color(enum vga_color fg, enum vga_color bg) {
    pen = color = VGA_ATTR(fg, bg);
    reversed = false;
}

void vga_set_crlf(bool on) {
    cr_on_lf = on;
}

void vga_clear(void) {
    /* The whole screen in one sweep, the leftover scan lines under the last
       full row included, and then the cells to match. */
    if ((color >> 4 & 0x0F) == VGA_BLACK) {
        fill_background(0, fb_height);
    } else {
        fill_rows(0, fb_height, palette[color >> 4 & 0x0F]);
    }
    for (size_t i = 0; i < cells; i++) {
        screen[i] = cell(' ');
    }
    cursor = 0;
    cursor_draw(true);
}

/* ---- escape sequences ----------------------------------------------------
 *
 * A program has no way to reach into the console, so it says what it wants
 * the way every terminal has been told since the seventies: an escape, a
 * bracket, some numbers and a letter. Only the few that matter here are
 * understood - colours, clearing, and putting the cursor somewhere - which is
 * enough for a utility of ours to look like the shell drawing it, and enough
 * for a program off a Linux system to colour its output.
 *
 * Anything not understood is swallowed rather than printed, which is what a
 * terminal does and what keeps stray codes from littering the screen. */

#define PARAMS 4

static enum { PLAIN, AFTER_ESC, IN_CSI, CHARSET, IN_OSC } escape;

/* ---- what a byte draws ------------------------------------------------------
 *
 * Text arrives as UTF-8, as it does on Linux's console, and the font is
 * code page 437: a character is drawn as the glyph of the same shape, or as
 * a question mark where the font has none. A program drawing lines the
 * VT100's way - "ESC ( 0" or SO, then letters - gets the same glyphs. */

static bool     charset_g1;         /* the escape just seen names G1, not G0 */
static bool     graphics[2];        /* G0 and G1: the VT100's line drawing set */
static bool     shifted;            /* SO: G1 is the one in use */
static uint32_t utf8;               /* a character being gathered */
static unsigned utf8_left;          /* its bytes still to come */

/* Code page 437's upper half, as the Unicode each glyph is. */
static const uint16_t cp437[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

/* Characters drawn with a glyph meant for another: the rounded and heavy
   lines as plain ones, arrows and bullets from the font's low codes. */
static const struct { uint16_t from; uint8_t to; } alike[] = {
    { 0x256D, 0xDA }, { 0x256E, 0xBF }, { 0x256F, 0xD9 }, { 0x2570, 0xC0 },
    { 0x2501, 0xC4 }, { 0x2503, 0xB3 }, { 0x2022, 0x07 }, { 0x25CF, 0x07 },
    { 0x2190, 0x1B }, { 0x2191, 0x18 }, { 0x2192, 0x1A }, { 0x2193, 0x19 },
    { 0x25B6, 0x10 }, { 0x25C0, 0x11 }, { 0x2666, 0x04 }, { 0x25C6, 0x04 },
    { 0x2026, 0xFA }, { 0x2713, 0xFB },
};

/* The VT100's line drawing set, '_' to '~', as code page 437. */
static const uint8_t vt100_lines[32] = {
    ' ', 0x04, 0xB1, '?', '?', '?', '?', 0xF8, 0xF1, 0xB0, 0xCE, 0xD9, 0xBF, 0xDA,
    0xC0, 0xC5, '-', '-', 0xC4, '-', '_', 0xC3, 0xB4, 0xC1, 0xC2, 0xB3, 0xF3, 0xF2,
    0xE3, 0xD8, 0x9C, 0xFE,
};

static uint8_t glyph_for(uint32_t u) {
    if (u < 0x80) {
        return (uint8_t)u;
    }
    for (unsigned i = 0; i < 128; i++) {
        if (cp437[i] == u) {
            return (uint8_t)(0x80 + i);
        }
    }
    for (unsigned i = 0; i < sizeof alike / sizeof alike[0]; i++) {
        if (alike[i].from == u) {
            return alike[i].to;
        }
    }
    return '?';
}

void vga_text_reset(void) {
    graphics[0] = graphics[1] = shifted = false;
    utf8_left = 0;
}

/* The glyph a byte of text draws, or 0 while a character is still being
   gathered. */
static uint8_t glyph(uint8_t c) {
    if (c < 0x80) {
        utf8_left = 0;
        return graphics[shifted] && c >= '_' && c <= '~' ? vt100_lines[c - '_'] : c;
    }
    if (c >= 0xC0) {                /* the first of a character's bytes */
        utf8_left = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
        utf8 = c & (0x3Fu >> utf8_left);
        return 0;
    }
    if (utf8_left == 0) {
        return '?';                 /* a stray continuation */
    }
    utf8 = utf8 << 6 | (c & 0x3F);
    return --utf8_left > 0 ? 0 : glyph_for(utf8);
}
static unsigned osc_left;       /* of a palette entry: "P" and seven digits */
static size_t   saved_cursor;
static uint8_t  saved_pen;
static unsigned params[PARAMS], param_count;
static bool     private;        /* a sequence about the terminal, not the screen */

/* ANSI numbers colours in its own order, which is VGA's with red and blue
   swapped; bright is the same eight again with bit three set. */
static const uint8_t ansi_colors[8] = {
    VGA_BLACK, VGA_RED, VGA_GREEN, VGA_BROWN,
    VGA_BLUE, VGA_MAGENTA, VGA_CYAN, VGA_LIGHTGRAY,
};

static void set_graphics(void) {
    color = pen;
    if (param_count == 0) {
        param_count = 1;            /* "escape [ m" is "escape [ 0 m" */
        params[0] = 0;
    }
    for (unsigned i = 0; i < param_count; i++) {
        unsigned n = params[i];

        if (n == 0) {
            color = VGA_ATTR(VGA_LIGHTGRAY, VGA_BLACK);
            reversed = false;
        } else if (n == 39) {
            color = (uint8_t)((color & 0xF0) | VGA_LIGHTGRAY);
        } else if (n == 49) {
            color = (uint8_t)(color & 0x0F);
        } else if (n == 1) {
            color = (uint8_t)(color | 0x08);            /* bright */
        } else if (n == 22) {
            color = (uint8_t)(color & ~0x08);
        } else if (n >= 30 && n <= 37) {
            color = (uint8_t)((color & 0xF8) | ansi_colors[n - 30]);
        } else if (n >= 90 && n <= 97) {
            color = (uint8_t)((color & 0xF0) | ansi_colors[n - 90] | 0x08);
        } else if (n >= 40 && n <= 47) {
            color = (uint8_t)((color & 0x0F) | ansi_colors[n - 40] << 4);
        } else if (n >= 100 && n <= 107) {
            color = (uint8_t)((color & 0x0F) | (ansi_colors[n - 100] | 0x08) << 4);
        } else if (n == 7 || n == 27) {
            /* Reverse video: the attribute byte with its two halves the other
               way round, kept apart so that colours set under it still land
               where they are meant to. */
            reversed = n == 7;
        }
    }
    pen = color;
    if (reversed) {
        color = (uint8_t)((pen >> 4 & 0x0F) | (pen & 0x0F) << 4);
    }
}

/* Blanks from the cursor to the end of the line, or the whole screen. */
static void erase(char what) {
    if (what == 'J' && (param_count == 0 || params[0] == 2)) {
        vga_clear();
        return;
    }
    size_t first = cursor, last = cursor + (width - cursor % width);

    if (what == 'J') {
        last = cells;
    } else if (param_count > 0 && params[0] == 1) {
        first = cursor - cursor % width;
        last = cursor + 1;
    } else if (param_count > 0 && params[0] == 2) {
        first = cursor - cursor % width;
    }
    for (size_t i = first; i < last; i++) {
        put(i, cell(' '));
    }
}

/* The first parameter, or one when there is none: what every sequence that
   takes a count means by leaving it out. */
static size_t count_param(void) {
    return param_count > 0 && params[0] > 0 ? params[0] : 1;
}

/* Moves rows top up to bottom by one: up, the top one going and a blank one
   coming in at the bottom, or down, the other way round. */
static void scroll(unsigned top, unsigned bottom, bool up) {
    size_t first = (size_t)top * width, last = (size_t)bottom * width;

    if (up) {
        for (size_t i = first; i < last; i++) {
            put(i, i + width < last ? screen[i + width] : cell(' '));
        }
    } else {
        for (size_t i = last; i-- > first;) {
            put(i, i >= first + width ? screen[i - width] : cell(' '));
        }
    }
}

/* Down a row, scrolling the region when the cursor is on its last one. */
static void line_feed(void) {
    unsigned row = (unsigned)(cursor / width);

    if (row + 1 == region_bottom) {
        scroll(region_top, region_bottom, true);
    } else if (row + 1 < vga_height()) {
        cursor += width;
    }
}

/* Up a row, scrolling the region the other way on its first. */
static void reverse_feed(void) {
    unsigned row = (unsigned)(cursor / width);

    if (row == region_top) {
        scroll(region_top, region_bottom, false);
    } else if (row > 0) {
        cursor -= width;
    }
}

/* Moves the cursor about the screen without printing anything. Row 0 is the
   title bar, which is not a program's to draw on. */
static void move_by(char what) {
    size_t row = cursor / width, column = cursor % width;
    size_t n = count_param();

    switch (what) {
    case 'A':
        row = row > n ? row - n : 0;
        break;
    case 'B':
        row = row + n < vga_height() ? row + n : vga_height() - 1;
        break;
    case 'C':
        column = column + n < width ? column + n : width - 1;
        break;
    case 'D':
        column = column > n ? column - n : 0;
        break;
    case 'G':
        column = n - 1 < width ? n - 1 : width - 1;
        break;
    default:
        return;
    }
    cursor = row * width + column;
}

/* Opens a gap in the line at the cursor, or closes one: what a terminal does
   for a program editing a line in the middle of it. The rest of the line
   moves, and nothing beyond the line is touched. */
static void shift_line(bool open, size_t n) {
    size_t start = cursor, end = cursor - cursor % width + width;

    if (n > end - start) {
        n = end - start;
    }
    if (open) {
        for (size_t i = end; i-- > start + n;) {
            put(i, screen[i - n]);
        }
        for (size_t i = start; i < start + n; i++) {
            put(i, cell(' '));
        }
    } else {
        for (size_t i = start; i < end; i++) {
            put(i, i + n < end ? screen[i + n] : cell(' '));
        }
    }
}

/* Rows and columns are counted from one, as a terminal counts them. */
static void move_cursor(void) {
    size_t row = param_count > 0 && params[0] > 0 ? params[0] - 1 : 0;
    size_t column = param_count > 1 && params[1] > 0 ? params[1] - 1 : 0;

    if (row * width + column < cells) {
        cursor = row * width + column;
    }
}

/* Opens n blank rows at the cursor's, pushing the rest of the region down,
   or closes them, pulling it up. Outside the region it does nothing. */
static void shift_rows(bool open) {
    unsigned row = (unsigned)(cursor / width);
    size_t n = count_param();

    if (row < region_top || row >= region_bottom) {
        return;
    }
    if (n > region_bottom - row) {
        n = region_bottom - row;
    }
    while (n-- > 0) {
        scroll(row, region_bottom, !open);
    }
    cursor -= cursor % width;
}

/* Blanks n characters from the cursor on, without moving it or the rest of
   the line. */
static void erase_chars(void) {
    size_t end = cursor - cursor % width + width;
    size_t n = count_param();

    for (size_t i = cursor; i < end && n > 0; i++, n--) {
        put(i, cell(' '));
    }
}

static void set_region(void) {
    unsigned rows = vga_height();
    unsigned top = param_count > 0 && params[0] > 0 ? params[0] - 1 : 0;
    unsigned bottom = param_count > 1 && params[1] > 0 && params[1] <= rows ? params[1] : rows;

    if (top + 1 < bottom) {
        region_top = top;
        region_bottom = bottom;
        cursor = 0;
    }
}

static void to_row(void) {
    size_t row = count_param() - 1;

    if (row < vga_height()) {
        cursor = row * width + cursor % width;
    }
}

/* One character of a sequence. True if it was taken. */

static bool escaped(char c) {
    if (escape == PLAIN) {
        if (c != 0x1B) {
            return false;
        }
        escape = AFTER_ESC;
        return true;
    }
    if (escape == AFTER_ESC) {
        /* CSI - "escape bracket" - is most of them; the rest are a letter
           straight after the escape. One this does not know is dropped. */
        escape = c == '[' ? IN_CSI : c == '(' || c == ')' ? CHARSET
               : c == ']' ? IN_OSC : PLAIN;
        charset_g1 = c == ')';
        params[0] = param_count = 0;
        private = false;
        osc_left = 0;
        if (c == '7') {
            saved_cursor = cursor;
            saved_pen = pen;
        } else if (c == '8') {
            cursor = saved_cursor < cells ? saved_cursor : 0;
            pen = saved_pen;
            reversed = false;
            color = pen;
            wrap_pending = false;
        } else if (c == 'M') {
            reverse_feed();
            wrap_pending = false;
        } else if (c == 'D') {
            line_feed();
            wrap_pending = false;
        } else if (c == 'E') {
            cursor -= cursor % width;
            line_feed();
            wrap_pending = false;
        } else if (c == 'c') {
            vga_text_reset();
            pen = color = VGA_ATTR(VGA_LIGHTGRAY, VGA_BLACK);
            reversed = inserting = false;
            cursor_shown = true;
            region_top = 0;
            region_bottom = vga_height();
            vga_clear();
        }
        return true;
    }
    if (escape == CHARSET) {
        escape = PLAIN;             /* '0' is the line drawing set; 'B', or
                                       anything else, plain text */
        graphics[charset_g1] = c == '0';
        return true;
    }
    if (escape == IN_OSC) {
        /* The Linux console's palette ("P" and seven digits, or "R" to put it
           back), or anything else up to a bell. None of it is kept. */
        if (osc_left > 0) {
            if (--osc_left == 0) {
                escape = PLAIN;
            }
        } else if (c == 'P') {
            osc_left = 7;
        } else if (c == 'R' || c == 0x07 || c == 0x1B) {
            escape = c == 0x1B ? AFTER_ESC : PLAIN;
        }
        return true;
    }
    if (c == '?' || c == '<' || c == '=' || c == '>') {
        /* A terminal's own settings rather than anything drawn - bracketed
           paste, which a line editor turns on, is "escape [ ? 2 0 0 4 h". None of
           them mean anything to this screen, and the whole sequence is
           swallowed rather than half-read. */
        private = true;
        return true;
    }
    if (c >= '0' && c <= '9') {
        if (param_count == 0) {
            param_count = 1;
            params[0] = 0;
        }
        if (param_count <= PARAMS) {
            params[param_count - 1] = params[param_count - 1] * 10 + (unsigned)(c - '0');
        }
        return true;
    }
    if (c == ';') {
        if (param_count < PARAMS) {
            params[param_count++] = 0;
        }
        return true;
    }
    escape = PLAIN;                 /* the final letter ends it either way */
    wrap_pending = false;
    if (private) {
        /* Of the terminal's own settings, only showing the cursor matters. */
        private = false;
        if ((c == 'h' || c == 'l') && param_count > 0 && params[0] == 25) {
            cursor_draw(false);
            cursor_shown = c == 'h';
            cursor_draw(true);
        }
        return true;
    }
    switch (c) {
    case 'm':
        set_graphics();
        break;
    case 'J':
    case 'K':
        erase(c);
        break;
    case 'H':
    case 'f':
        move_cursor();
        break;
    case 'A':
    case 'B':
    case 'C':
    case 'D':
    case 'G':
        move_by(c);
        break;
    case '@':
        shift_line(true, count_param());
        break;
    case 'P':
        shift_line(false, count_param());
        break;
    case 'L':
    case 'M':
        shift_rows(c == 'L');
        break;
    case 'X':
        erase_chars();
        break;
    case 'd':
        to_row();
        break;
    case 'r':
        set_region();
        break;
    case 'h':
    case 'l':
        if (param_count > 0 && params[0] == 4) {
            inserting = c == 'h';
        }
        break;
    default:
        break;                      /* something else: dropped */
    }
    return true;
}

/* ---- capture -------------------------------------------------------------
 *
 * Output can be taken into a buffer instead of onto the screen. A kernel
 * command prints rather than writing to a descriptor - it is kernel code, and
 * there is nothing else for it to print on - so this is how what one prints
 * reaches a pipe or a file when the shell has redirected it.
 *
 * Colour is dropped on the way: nothing is being coloured, and a sequence
 * meant for a screen is noise in a file. */

static char  *capture;
static size_t capture_max, capture_len;

void vga_capture(char *buf, size_t max) {
    capture = buf;
    capture_max = max;
    capture_len = 0;
    buf[0] = '\0';
}

void vga_capture_end(void) {
    capture = NULL;
}

void vga_putc(char c) {
    if (capture != NULL) {
        if (!escaped(c) && capture_len + 1 < capture_max) {
            capture[capture_len++] = c;
            capture[capture_len] = '\0';
        }
        return;
    }
    /* An escape sequence is not something printed: a colour, or a line of
       progress, leaves whatever is on screen where it is - which is what
       lets the loading screen stay up while the script behind it reports. */
    size_t was = cursor;

    if (escaped(c)) {
        if (cursor != was) {
            /* A move: the cursor drawn where it was has to go with it. */
            size_t now = cursor;

            cursor = was;
            cursor_draw(false);
            cursor = now;
            cursor_draw(true);
        }
        return;
    }
    dbg_screen(c);
    cursor_draw(false);

    if (c == '\n') {
        /* Down a line, and back to the start of it unless a program has
           said it will send the "\r" itself. */
        if (cr_on_lf) {
            cursor -= cursor % width;
        }
        wrap_pending = false;
        line_feed();
    } else if (c == '\r') {
        cursor -= cursor % width;
        wrap_pending = false;
    } else if (c == '\b') {
        /* A move, not an erase: that is what a terminal does with it, and
           what a program editing a line in place counts on. Rubbing a
           character out is "\b \b", which is what the console echoes. */
        if (cursor % width > 0 && !wrap_pending) {
            cursor--;
        }
        wrap_pending = false;
    } else if (c == '\t') {
        /* To the next stop, moving rather than writing, and no further
           than the last column. */
        size_t end = cursor - cursor % width + width - 1;

        cursor = (cursor / 8 + 1) * 8 < end ? (cursor / 8 + 1) * 8 : end;
        wrap_pending = false;
    } else if (c == 0x0E || c == 0x0F) {
        shifted = c == 0x0E;        /* SO and SI: G1 in use, and G0 again */
    } else if ((unsigned char)c < 0x20 || c == 0x7F) {
        /* A control character this screen has no answer for - the bell most
           of all, which a line editor rings whenever an edit does nothing, a
           backspace at the start of a line among them. The font has a glyph
           at every code, so printing one drew a stray dot on the line being
           typed. A terminal shows nothing for these, and nor does this. */
        cursor_draw(true);
        return;
    } else {
        /* A character in the last column leaves the cursor on it, and the
           next one goes to the start of the line below. */
        if (wrap_pending) {
            cursor -= cursor % width;
            line_feed();
            wrap_pending = false;
        }
        uint8_t g = glyph((uint8_t)c);

        if (g == 0) {
            cursor_draw(true);
            return;                 /* more of the character to come */
        }
        if (inserting) {
            shift_line(true, 1);
        }
        put(cursor, cell((char)g));
        if (cursor % width == width - 1) {
            wrap_pending = true;
        } else {
            cursor++;
        }
    }
    cursor_draw(true);
}

/* Where the next character goes, which is the line below if one has just
   filled the last column: the wrap is made now, so the answer is a place on
   screen. */
size_t vga_at(void) {
    if (wrap_pending) {
        cursor_draw(false);
        cursor -= cursor % width;
        line_feed();
        wrap_pending = false;
        cursor_draw(true);
    }
    return cursor;
}

void vga_puts(const char *s) {
    while (*s != '\0') {
        vga_putc(*s++);
    }
}

/* ---- formatting ------------------------------------------------------- */

static char *out_buf;           /* where ksprintf is writing; NULL means the screen */

static void out(char c) {
    if (out_buf != NULL) {
        *out_buf++ = c;
    } else {
        vga_putc(c);
    }
}

static void put_uint(unsigned value, size_t pad) {
    char digits[10];
    size_t i = 0;

    do {
        digits[i++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (i < pad) {
        digits[i++] = '0';
    }
    while (i > 0) {
        out(digits[--i]);
    }
}

static void put_hex(uint64_t value) {
    char digits[16];
    size_t i = 0;

    do {
        digits[i++] = "0123456789abcdef"[value % 16];
        value /= 16;
    } while (value != 0);
    while (i > 0) {
        out(digits[--i]);
    }
}

static void format(const char *fmt, va_list args) {
    for (; *fmt != '\0'; fmt++) {
        if (*fmt != '%') {
            out(*fmt);
        } else if (*++fmt == 's') {
            for (const char *s = va_arg(args, const char *); *s != '\0'; s++) {
                out(*s);
            }
        } else if (*fmt == 'x') {
            put_hex(va_arg(args, uint64_t));
        } else {
            size_t pad = 0;
            if (*fmt == '0') {                  /* %0Nu */
                pad = (size_t)(fmt[1] - '0');
                fmt += 2;
            }
            put_uint(va_arg(args, unsigned), pad);
        }
    }
}

void kprintf(const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    format(fmt, args);
    va_end(args);
}

void ksprintf(char *buf, const char *fmt, ...) {
    va_list args;

    out_buf = buf;
    va_start(args, fmt);
    format(fmt, args);
    va_end(args);
    *out_buf = '\0';
    out_buf = NULL;
}
