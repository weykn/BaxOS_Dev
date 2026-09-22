#include "status.h"

#include "efi_kernel.h"
#include "fs.h"
#include "mem.h"
#include "string.h"
#include "vga.h"

#define BAR   VGA_ATTR(VGA_LIGHTGRAY, VGA_DARKGRAY)
#define PILL  VGA_ATTR(VGA_BLACK, VGA_LIGHTCYAN)
#define VALUE VGA_ATTR(VGA_WHITE, VGA_DARKGRAY)
#define SEP   VGA_ATTR(VGA_BLACK, VGA_DARKGRAY)
#define GLYPH_SEP "\xB3"            /* a thin vertical line, in code page 437 */

#define POWER VGA_ATTR(VGA_WHITE, VGA_RED)
#define POWER_LABEL " \x0F "       /* font.c draws the power symbol there */

static size_t left, right;          /* the stretch still free between the ends */

static unsigned last_time;          /* the RTC reading the bar was drawn for */
static unsigned mem_used, mem_total, disk_used, disk_total;    /* KiB */

/* Seconds past midnight, which is all the bar shows of the date. */
static unsigned rtc_now(void) {
    return efi_seconds();
}

static void put_left(const char *s, uint8_t attr) {
    while (*s != '\0' && left < right) {
        vga_title_cell((unsigned)left++, *s++, attr);
    }
}

/* Puts s just left of whatever is already at the right end. */
static void put_right(const char *s, uint8_t attr) {
    size_t length = strlen(s);

    if (length <= right - left) {
        right -= length;
        for (size_t i = 0; i < length; i++) {
            vga_title_cell((unsigned)(right + i), s[i], attr);
        }
    }
}

/* A module on the right: its label in its own colour, then its value, and a
   separator on its right unless it is the rightmost one.
 *
 * All of it goes on or none of it does. A large font leaves few columns, and
 * half a module - a label with no figure, or a separator with nothing beyond
 * it - reads as a glitch rather than as something that did not fit. */
static void module(const char *label, enum vga_color label_color, const char *value, bool sep) {
    if (strlen(label) + strlen(value) + (sep ? 4 : 3) > right - left) {
        return;
    }
    if (sep) {
        put_right(GLYPH_SEP, SEP);
    }
    put_right(" ", BAR);
    put_right(value, VALUE);
    put_right(" ", BAR);
    put_right(label, VGA_ATTR(label_color, VGA_DARKGRAY));
    put_right(" ", BAR);
}

static void draw(unsigned now) {
    char value[24];
    unsigned up = (unsigned)(efi_uptime_ms() / 1000);

    for (unsigned i = 0; i < vga_width(); i++) {
        vga_title_cell(i, ' ', BAR);
    }
    left = 0;
    right = vga_width();

    /* The power button in the corner, where a pointer thrown that way
       lands on it. */
    put_left(POWER_LABEL, POWER);
    put_left(" BaxOS ", PILL);
    put_left(" ", BAR);
    put_left(vga_mode(), BAR);

    /* The right end is built from the edge inwards. */
    ksprintf(value, " %02u:%02u:%02u ", now / 3600, now / 60 % 60, now % 60);
    put_right(value, PILL);
    ksprintf(value, "%u:%02u:%02u", up / 3600, up / 60 % 60, up % 60);
    module("up", VGA_PINK, value, false);
    ksprintf(value, "%uK/%uK", disk_used, disk_total);
    module("disk", VGA_YELLOW, value, true);
    ksprintf(value, "%uK/%uK", mem_used, mem_total);
    module("mem", VGA_LIGHTGREEN, value, true);

    vga_title();
}

void status_update(bool refresh) {
    unsigned now;

    vga_follow();
    now = rtc_now();

    if (now == last_time && !refresh) {
        return;
    }
    last_time = now;

    /* The figures are re-read whenever the bar is drawn, which is once a
       second. They used to be read only when the shell said a command had
       run - but the shell is a program now and has no way to say so, and
       a bar showing what memory was at boot is worse than useless. Neither
       reading touches the disk: one walks the file table, which is already
       in hand, and the other scans the kernel stack. */
    struct mem_stats m;
    struct fs_stats s;

    mem_get_stats(&m);
    mem_used = m.used_kib;
    mem_total = m.total_kib;
    if (fs_get_stats(&s) == 0) {
        /* Sectors to KiB. Rounded up, as everything that shows this figure
           rounds it up: half a kilobyte in use is a kilobyte that is not
           free. */
        disk_used = (s.used + 1) / 2;
        disk_total = s.total / 2;
    }
    draw(now);
}

bool status_power(unsigned column, unsigned *first) {
    *first = 0;
    return column < sizeof POWER_LABEL - 1;
}

void status_init(void) {
    last_time = rtc_now();
    status_update(true);
}
