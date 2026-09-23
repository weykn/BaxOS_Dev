#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct boot_info;

/* The text console, drawn into the framebuffer the firmware set up.
 *
 * There is no text mode here and no video hardware to program: UEFI hands
 * over a screen already working, and every character is drawn a pixel at a
 * time from shapes carried in the kernel image, scaled up by whole pixels -
 * so the size of the text changes without the size of the screen changing. */

/* Colours, as the VGA attribute byte numbers them: foreground in the low
   nibble of an attribute, background in the high one. */
enum vga_color {
    VGA_BLACK = 0, VGA_BLUE, VGA_GREEN, VGA_CYAN,
    VGA_RED, VGA_MAGENTA, VGA_BROWN, VGA_LIGHTGRAY,
    VGA_DARKGRAY, VGA_LIGHTBLUE, VGA_LIGHTGREEN, VGA_LIGHTCYAN,
    VGA_LIGHTRED, VGA_PINK, VGA_YELLOW, VGA_WHITE
};

#define VGA_ATTR(fg, bg) ((uint8_t)((fg) | (bg) << 4))

/* Columns on screen now: the framebuffer's width over the font's. Anything
   that draws a whole row - the title bar - has to ask rather than assume. */
unsigned vga_width(void);

/* Rows on screen, the title bar counted in, for anything that puts itself in
   the middle of it. */
unsigned vga_height(void);

/* Takes the screen the loader passed on. Returns 0, or -1 if a console will
   not fit in it, which leaves nothing able to show anything. */
int vga_start(struct boot_info *info);

/* Switches to "<width>x<height>", which has to be one the screen offers.
   Returns 0, or -1 leaving the mode in use alone. */
int vga_set_mode(const char *name);

/* Mode i the screen offers, counting from 0, or NULL past the last. The
   string is rebuilt on each call. */
const char *vga_mode_name(unsigned i);

/* The mode in use, as "<width>x<height>". */
const char *vga_mode(void);

/* Switches to the font whose cell is name pixels tall. Returns 0, or -1 if
   there is no such size, or it would leave too few rows on screen. */
int vga_set_font(const char *name);

/* Font size i, counting from 0, or NULL past the last. */
const char *vga_font_name(unsigned i);

/* The size in use. */
const char *vga_font(void);

/* Blanks the screen and puts the cursor in its corner. */
void vga_clear(void);

/* The screen in pixels, which is what a picture behind the text is drawn in. */
unsigned vga_pixel_width(void);
unsigned vga_pixel_height(void);

/* Packs a colour the way this screen's pixels want it. */
uint32_t vga_rgb(uint8_t r, uint8_t g, uint8_t b);

/* A picture behind the text: one pixel for each of the screen's, which the
   caller owns and keeps for as long as it is in use, or NULL for plain
   black. Wherever a cell's background is black, the picture shows through.
   The screen is repainted either way. */
void vga_background(const uint32_t *picture);

/* Whether a wallpaper is in use. It is dropped if the screen changes size
   under it, so whoever owns the pixels has to look. */
bool vga_has_background(void);

/* Reads and writes one cell anywhere on screen, title bar included, as a
   character in the low byte and an attribute in the high one. For things
   laid over the console for a moment - a menu - which save what they cover
   with vga_get and put it back with vga_put. */
uint16_t vga_get(unsigned column, unsigned row);
void vga_put(unsigned column, unsigned row, uint16_t value);

/* Draws the text cursor again. vga_put leaves it off, so nothing laid over
   its cell shows it through; whatever put the cell back calls this. */
void vga_cursor(void);

/* What the console's cells cost in memory, for the `mem` command. */
size_t vga_memory(void);

/* Puts the console back on screen if the firmware has taken the mode over.
   Everything that waits for a key calls this. */
void vga_follow(void);

/* Takes everything printed into buf, up to max bytes and NUL-terminated,
   rather than onto the screen, until vga_capture_end. Colour is left out.
   What a kernel command prints is read this way when the shell has sent its
   output somewhere other than the screen; it does not nest. */
void vga_capture(char *buf, size_t max);
void vga_capture_end(void);

void vga_set_color(enum vga_color fg, enum vga_color bg);
void vga_putc(char c);
void vga_puts(const char *s);

/* Minimal formatter: %s, %u and %x. %u takes an optional zero-padded
   one-digit width, as in %02u; %x prints a 64-bit value in lower-case hex,
   unpadded. */
void kprintf(const char *fmt, ...);

/* The same into buf, NUL-terminated. buf must be big enough. */
void ksprintf(char *buf, const char *fmt, ...);
