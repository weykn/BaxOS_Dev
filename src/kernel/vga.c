#include "vga.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#include "boot.h"
#include "debug.h"
#include "efi_kernel.h"
#include "font.h"
#include "string.h"

#define GLYPH_W 8

/* The text sizes, named by how tall a character cell ends up. There are two
   sets of shapes in the image; the larger sizes are the 8x16 one drawn with
   every glyph pixel as a square block, which costs nothing but the drawing. */
static const struct font {
    const char *name;
    uint8_t     height;     /* the shapes it draws from, in scan lines */
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
static unsigned fb_width, fb_height, fb_pitch;

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
 * One row past the cells on screen is scratch, where the title bar is built.
 * It is firmware memory: a console can want anything from twelve to sixty
 * kilobytes depending on the mode and the font, which is far too much to set
 * aside for the largest case. */
static volatile uint16_t *screen;
static size_t screen_bytes;

/* The wallpaper, owned by bg.c: one pixel for each of the screen's, or NULL
   for a plain black background. */
static const uint32_t *picture;

static const struct font *glyphs = &fonts[FONT_DEFAULT];
static const uint8_t *shapes;                   /* its 256 glyphs */
static unsigned cell_w, cell_h;                 /* a character cell, in pixels */
static char     mode_name[16];                  /* "1024x768", for vga_mode */
static char     list_name[16];                  /* one entry of the mode list */

static unsigned width;          /* columns */
static size_t   cells;          /* on screen, title bar included */
static size_t   cursor;         /* row * width + column */
static uint8_t  color = VGA_LIGHTGRAY;

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

/* The top-left pixel of cell i. */
static volatile uint8_t *cell_pixels(size_t i) {
    return fb + (i / width) * cell_h * (size_t)fb_pitch
              + (i % width) * cell_w * sizeof(uint32_t);
}

/* Draws one cell's glyph, foreground and background both. A cell whose
   background is black shows the wallpaper instead, where there is one: that
   is the whole of how the text comes to sit on a picture. */
static void draw_cell(size_t i, uint16_t value) {
    const uint8_t *glyph = shapes + (value & 0xFF) * glyphs->height;
    uint32_t fg = palette[value >> 8 & 0x0F];
    uint32_t bg = palette[value >> 12 & 0x0F];
    const uint32_t *behind = picture != NULL && (value >> 12 & 0x0F) == VGA_BLACK
                           ? picture : NULL;
    unsigned left = (unsigned)(i % width) * cell_w;
    unsigned top_y = (unsigned)(i / width) * cell_h;
    volatile uint8_t *top = cell_pixels(i);

    for (unsigned y = 0; y < glyphs->height; y++) {
        uint8_t bits = glyph[y];

        /* Each scan line of the glyph is drawn scale times, and each of its
           pixels scale times across, which is what makes the cell bigger. */
        for (unsigned again = 0; again < glyphs->scale; again++) {
            unsigned screen_y = top_y + y * glyphs->scale + again;
            volatile uint32_t *line =
                (volatile uint32_t *)(top + (y * glyphs->scale + again) * (size_t)fb_pitch);
            const uint32_t *from = behind != NULL
                                 ? behind + (size_t)screen_y * fb_width + left : NULL;

            for (unsigned x = 0; x < GLYPH_W; x++) {
                bool ink = (bits & 0x80 >> x) != 0;

                for (unsigned wide = 0; wide < glyphs->scale; wide++) {
                    uint32_t under = from != NULL ? *from++ : bg;

                    *line++ = ink ? fg : under;
                }
            }
        }
    }
}

/* Paints scan lines y0 up to y1 from the wallpaper, or in one colour when
   there is none - which is the same cheap sweep as before. */
static void fill_rows(unsigned y0, unsigned y1, uint32_t rgb);

static void fill_background(unsigned y0, unsigned y1) {
    if (picture == NULL) {
        fill_rows(y0, y1, palette[VGA_BLACK]);
        return;
    }
    for (unsigned y = y0; y < y1; y++) {
        volatile uint32_t *line = row(y);
        const uint32_t *from = picture + (size_t)y * fb_width;

        for (unsigned x = 0; x < fb_width; x++) {
            line[x] = from[x];
        }
    }
}

/* Paints scan lines y0 up to y1 in one colour. Clearing a whole screen this
   way is far cheaper than drawing a blank glyph in every cell. */
static void fill_rows(unsigned y0, unsigned y1, uint32_t rgb) {
    for (unsigned y = y0; y < y1; y++) {
        volatile uint32_t *line = row(y);

        for (unsigned x = 0; x < fb_width; x++) {
            line[x] = rgb;
        }
    }
}

/* There is no hardware cursor in a graphics mode, so it is drawn: the bottom
   two scan lines of the cell, scaled with the font, in the foreground
   colour. Putting the cell back the way it was erases it. */
static void cursor_draw(bool on) {
    if (cursor >= cells) {
        return;
    }
    if (!on) {
        draw_cell(cursor, screen[cursor]);
        return;
    }
    volatile uint8_t *top = cell_pixels(cursor);
    uint32_t fg = palette[screen[cursor] >> 8 & 0x0F];

    for (unsigned y = cell_h - 2 * glyphs->scale; y < cell_h; y++) {
        volatile uint32_t *line = (volatile uint32_t *)(top + y * (size_t)fb_pitch);

        for (unsigned x = 0; x < cell_w; x++) {
            line[x] = fg;
        }
    }
}

/* Writes a cell. Drawing a glyph costs a cell's worth of pixels, so one that
   already holds what it is being given is left alone - which is most of the
   title bar, every second, and most of a scroll. */
static void put(size_t i, uint16_t value) {
    if (screen[i] != value) {
        screen[i] = value;
        draw_cell(i, value);
    }
}

/* ---- modes and fonts ---------------------------------------------------- */

static void use_font(const struct font *f) {
    glyphs = f;
    shapes = font_glyphs(f->height);
    cell_w = GLYPH_W * f->scale;
    cell_h = f->height * f->scale;
}

/* Lays the console out for the screen and font now in use: sizes the grid,
   takes memory for its cells from the firmware, agrees with what is on
   screen, and clears it. False if the grid would be too small to use or the
   memory could not be had, having changed nothing. */
bool vga_has_background(void) {
    return picture != NULL;
}

uint32_t vga_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return red_first ? (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16
                     : (uint32_t)b | (uint32_t)g << 8 | (uint32_t)r << 16;
}

static bool layout(void) {
    unsigned columns = fb_width / cell_w;
    unsigned rows = fb_height / cell_h;
    size_t bytes = (size_t)columns * (rows + 1) * sizeof(uint16_t);
    struct efi_boot_services *bs = efi_boot()->system->boot;
    void *memory;

    if (columns < 40 || rows < 8) {
        return false;
    }
    if (EFI_ERROR(bs->allocate_pool(EFI_LOADER_DATA, bytes, &memory))) {
        return false;
    }
    if (screen != NULL) {
        bs->free_pool((void *)screen);
    }
    screen = memory;
    screen_bytes = bytes;
    width = columns;
    cells = (size_t)columns * rows;

    /* The screen holds whatever it held before, so agree with it: every cell
       blank and black, and the whole of it painted to match. Otherwise put()
       would skip cells it wrongly believed were already drawn. */
    for (size_t i = 0; i < cells + width; i++) {
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

/* Takes the framebuffer the firmware is using now. */
static void take_screen(uint64_t base, unsigned w, unsigned h, unsigned pitch,
                        unsigned format) {
    /* A wallpaper is exactly the size of the screen it was made for. If the
       screen has changed - which the firmware does on its own, and vga_follow
       then notices - it is dropped rather than read past the end of. */
    if (w != fb_width || h != fb_height) {
        picture = NULL;
    }
    fb = (volatile uint8_t *)base;
    fb_width = w;
    fb_height = h;
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

/* ---- the pointer ---------------------------------------------------------
 *
 * An arrow eight pixels wide and twelve tall: one bitmap for the white
 * inside, one for the black edge that keeps it visible against anything. It
 * is erased by redrawing the cells it covered out of the shadow, which is
 * why the shadow has to say what is on screen at all times. */

#define POINTER_W 8
#define POINTER_H 12

static const uint8_t pointer_edge[POINTER_H] = {
    0x80, 0xC0, 0xA0, 0x90, 0x88, 0x84, 0x82, 0x81, 0x8F, 0xAA, 0xCA, 0x06,
};
static const uint8_t pointer_fill[POINTER_H] = {
    0x00, 0x00, 0x40, 0x60, 0x70, 0x78, 0x7C, 0x7E, 0x70, 0x44, 0x04, 0x00,
};

static unsigned pointer_x, pointer_y;
static bool     pointer_on;

unsigned vga_pixel_width(void) {
    return fb_width;
}

unsigned vga_pixel_height(void) {
    return fb_height;
}

/* Puts back the cells the arrow was covering. */
static void pointer_erase(void) {
    unsigned first_row, last_row, first_col, last_col;

    if (!pointer_on || cell_w == 0) {
        return;
    }
    first_row = pointer_y / cell_h;
    last_row = (pointer_y + POINTER_H - 1) / cell_h;
    first_col = pointer_x / cell_w;
    last_col = (pointer_x + POINTER_W - 1) / cell_w;

    for (unsigned r = first_row; r <= last_row && r * width < cells; r++) {
        for (unsigned c = first_col; c <= last_col && c < width; c++) {
            size_t i = (size_t)r * width + c;

            if (i < cells) {
                draw_cell(i, screen[i]);
            }
        }
    }
    pointer_on = false;
}

static void pointer_draw(void) {
    for (unsigned y = 0; y < POINTER_H && pointer_y + y < fb_height; y++) {
        volatile uint32_t *line = row(pointer_y + y);

        for (unsigned x = 0; x < POINTER_W && pointer_x + x < fb_width; x++) {
            uint8_t bit = (uint8_t)(0x80 >> x);

            if (pointer_fill[y] & bit) {
                line[pointer_x + x] = palette[VGA_WHITE];
            } else if (pointer_edge[y] & bit) {
                line[pointer_x + x] = palette[VGA_BLACK];
            }
        }
    }
    pointer_on = true;
}

/* Cheap to call in a polling loop: if the arrow is already drawn where it
   belongs, there is nothing to do. */
void vga_pointer(unsigned x, unsigned y) {
    if (pointer_on && x == pointer_x && y == pointer_y) {
        return;
    }
    pointer_erase();
    pointer_x = x;
    pointer_y = y;
    pointer_draw();
}

void vga_pointer_off(void) {
    pointer_erase();
}

/* The run of non-blank characters under the pixel, which is what a click on
   a name in a listing should pick up. Returns its length. */
size_t vga_word_at(unsigned x, unsigned y, char *out, size_t max) {
    unsigned r, c, first, last;
    size_t n = 0;

    if (cell_w == 0 || y / cell_h == 0) {
        return 0;                   /* the title bar is not text to take */
    }
    r = y / cell_h;
    c = x / cell_w;
    if (c >= width || (size_t)r * width >= cells) {
        return 0;
    }
    size_t base = (size_t)r * width;
    if ((screen[base + c] & 0xFF) == ' ') {
        return 0;
    }
    for (first = c; first > 0 && (screen[base + first - 1] & 0xFF) != ' '; first--) {
    }
    for (last = c; last + 1 < width && (screen[base + last + 1] & 0xFF) != ' '; last++) {
    }
    for (unsigned i = first; i <= last && n + 1 < max; i++) {
        out[n++] = (char)(screen[base + i] & 0xFF);
    }
    out[n] = '\0';
    return n;
}

static void repaint(void);

void vga_background(const uint32_t *new_picture) {
    picture = new_picture;
    pointer_on = false;             /* the pixels under it are about to go */
    /* Only the scan lines below the last row of text: repainting the cells
       covers everything above, and a screen's worth of writes into a
       framebuffer is slow enough to be worth not doing twice. */
    fill_background((unsigned)(cells / width) * cell_h, fb_height);
    repaint();
}

bool vga_cell_at(unsigned x, unsigned y, unsigned *column, unsigned *row) {
    if (cell_w == 0 || x / cell_w >= width || (size_t)(y / cell_h) * width >= cells) {
        return false;
    }
    *column = x / cell_w;
    *row = y / cell_h;
    return true;
}

uint16_t vga_get(unsigned column, unsigned row) {
    size_t i = (size_t)row * width + column;

    return column < width && i < cells ? screen[i] : 0;
}

void vga_put(unsigned column, unsigned row, uint16_t value) {
    size_t i = (size_t)row * width + column;

    if (column < width && i < cells) {
        pointer_erase();            /* the glyph would draw over it */
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
    if (EFI_ERROR(info->system->boot->locate_protocol(&gop_guid, NULL, (void **)&gop))) {
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
        layout();
        return 0;
    }
    return -1;
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
    color = VGA_ATTR(fg, bg);
}

void vga_clear(void) {
    pointer_on = false;             /* whatever it covered is about to go */
    /* Everything below the title bar in one sweep, the leftover scan lines
       under the last full row included, and then the cells to match. */
    if ((color >> 4 & 0x0F) == VGA_BLACK) {
        fill_background(cell_h, fb_height);
    } else {
        fill_rows(cell_h, fb_height, palette[color >> 4 & 0x0F]);
    }
    for (size_t i = width; i < cells; i++) {
        screen[i] = cell(' ');
    }
    cursor = width;
    cursor_draw(true);
}

void vga_title_cell(unsigned column, char c, uint8_t attr) {
    if (column < width) {
        screen[cells + column] = (uint16_t)((uint8_t)c | attr << 8);
    }
}

void vga_title(void) {
    pointer_erase();                /* the bar would draw straight over it */
    for (size_t i = 0; i < width; i++) {
        put(i, screen[cells + i]);
    }
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

static enum { PLAIN, AFTER_ESC, IN_CSI } escape;
static unsigned params[PARAMS], param_count;

/* ANSI numbers colours in its own order, which is VGA's with red and blue
   swapped; bright is the same eight again with bit three set. */
static const uint8_t ansi_colors[8] = {
    VGA_BLACK, VGA_RED, VGA_GREEN, VGA_BROWN,
    VGA_BLUE, VGA_MAGENTA, VGA_CYAN, VGA_LIGHTGRAY,
};

static void set_graphics(void) {
    for (unsigned i = 0; i < param_count; i++) {
        unsigned n = params[i];

        if (n == 0) {
            color = VGA_ATTR(VGA_LIGHTGRAY, VGA_BLACK);
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
        }
    }
}

/* Blanks from the cursor to the end of the line, or the whole screen. */
static void erase(char what) {
    if (what == 'J' && (param_count == 0 || params[0] == 2)) {
        vga_clear();
        return;
    }
    size_t last = what == 'J' ? cells : cursor + (width - cursor % width);

    for (size_t i = cursor; i < last; i++) {
        put(i, cell(' '));
    }
}

/* Row 1 is the first row under the title bar, which is not a program's to
   draw on. */
static void move_cursor(void) {
    size_t row = param_count > 0 && params[0] > 0 ? params[0] : 1;
    size_t column = param_count > 1 && params[1] > 0 ? params[1] - 1 : 0;

    if (row * width + column < cells) {
        cursor = row * width + column;
    }
}

/* One character of a sequence. True if it was taken. */
/* Something to do with how far through its work a program says it is: the
   loading screen's bar. */
static void (*progress_hook)(unsigned done, unsigned total);

static bool escaped(char c) {
    if (escape == PLAIN) {
        if (c != 0x1B) {
            return false;
        }
        escape = AFTER_ESC;
        return true;
    }
    if (escape == AFTER_ESC) {
        /* Only CSI - "escape bracket" - is understood; anything else was a
           sequence this does not know, and is dropped with it. */
        escape = c == '[' ? IN_CSI : PLAIN;
        params[0] = param_count = 0;
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
    case 'q':
        /* Not a terminal's: this machine's own, for how far through its
           start-up script the shell is. It is a sequence rather than a
           syscall because the shell already has the console open, and
           because printing it costs a program nothing to leave in. */
        if (progress_hook != NULL) {
            progress_hook(params[0], param_count > 1 ? params[1] : 0);
        }
        break;
    default:
        break;                      /* something else: dropped */
    }
    return true;
}

/* ---- capture -------------------------------------------------------------
 *
 * Output can be taken into a buffer instead of onto the screen, which is how
 * the shell reads what a command printed: everything that shows on screen
 * comes through here, whether the kernel printed it or a program wrote it. */

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

/* Something to do before the next character reaches the screen - taking a
   loading screen down, so that what is printed lands on a clear one. It is
   asked once and then forgotten. */
static void (*print_hook)(void);

void vga_on_print(void (*hook)(void)) {
    print_hook = hook;
}

void vga_on_progress(void (*hook)(unsigned done, unsigned total)) {
    progress_hook = hook;
}

void vga_putc(char c) {
    if (capture != NULL) {
        if (capture_len + 1 < capture_max) {
            capture[capture_len++] = c;
            capture[capture_len] = '\0';
        }
        return;
    }
    /* An escape sequence is not something printed: a colour, or a line of
       progress, leaves whatever is on screen where it is - which is what
       lets the loading screen stay up while the script behind it reports. */
    if (escaped(c)) {
        return;
    }
    if (print_hook != NULL) {
        void (*hook)(void) = print_hook;

        print_hook = NULL;          /* before it runs, so it may print */
        hook();
    }
    dbg_screen(c);
    pointer_erase();
    cursor_draw(false);

    if (c == '\n') {
        cursor += width - cursor % width;
    } else if (c == '\b') {
        if (cursor > width) {
            put(--cursor, cell(' '));
        }
    } else {
        put(cursor++, cell(c));
    }

    /* Off the bottom: scroll everything under the title bar up a row. Reading
       a row ahead of the one being written keeps this a single pass. */
    if (cursor >= cells) {
        for (size_t i = width; i < cells; i++) {
            put(i, i < cells - width ? screen[i + width] : cell(' '));
        }
        cursor -= width;
    }
    cursor_draw(true);
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
