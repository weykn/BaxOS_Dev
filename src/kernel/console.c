#include "console.h"

#include "bg.h"
#include "efi_kernel.h"
#include "keyboard.h"
#include "log.h"
#include "mouse.h"
#include "status.h"
#include "string.h"
#include "vga.h"

/* When the last key arrived, so that work put off until the machine is idle
   waits for it to be idle rather than merely between two keystrokes. */
#define QUIET_MS 400
static uint64_t last_key;

/* ---- the power menu ------------------------------------------------------
 *
 * Hangs under the power button in the status bar. Picking an item types its
 * command at the prompt, as if it had been typed, so the menu is only another
 * way of running a command - and does exactly what the command does. */

#define MENU_W 14

static const struct {
    const char *label;
    const char *command;
} menu[] = {
    { "Power off", "poweroff" },
    { "Reboot",    "reboot"   },
};

#define MENU_ITEMS (sizeof menu / sizeof menu[0])

#define MENU_ATTR VGA_ATTR(VGA_BLACK, VGA_LIGHTGRAY)
#define MENU_LIT  VGA_ATTR(VGA_WHITE, VGA_RED)

static bool     menu_open;
static unsigned menu_col;           /* its first column; rows 1 and down */
static int      menu_lit = -1;      /* the item under the pointer */
static uint16_t menu_under[MENU_ITEMS][MENU_W];

/* What a click picked up and has yet to be read. Room for a line's worth of
   backspaces as well as the longest command a menu types, since a menu
   clears the line before typing. */
#define PICKED 160

static char   picked[PICKED];
static size_t picked_len, picked_done;
static size_t typed_len;            /* what the line being read holds so far */

static void menu_draw_item(unsigned i) {
    uint8_t attr = (int)i == menu_lit ? MENU_LIT : MENU_ATTR;
    const char *label = menu[i].label;

    for (unsigned c = 0; c < MENU_W; c++) {
        size_t n = strlen(label);
        char ch = c >= 1 && c - 1 < n ? label[c - 1] : ' ';

        vga_put(menu_col + c, 1 + i, (uint16_t)((uint8_t)ch | attr << 8));
    }
}

static void menu_show(unsigned under) {
    unsigned width = vga_width();

    menu_col = under + MENU_W > width ? width - MENU_W : under;
    menu_lit = -1;
    for (unsigned i = 0; i < MENU_ITEMS; i++) {
        for (unsigned c = 0; c < MENU_W; c++) {
            menu_under[i][c] = vga_get(menu_col + c, 1 + i);
        }
        menu_draw_item(i);
    }
    menu_open = true;
}

static void menu_hide(void) {
    if (!menu_open) {
        return;
    }
    for (unsigned i = 0; i < MENU_ITEMS; i++) {
        for (unsigned c = 0; c < MENU_W; c++) {
            vga_put(menu_col + c, 1 + i, menu_under[i][c]);
        }
    }
    vga_cursor();
    menu_open = false;
}

/* The item at a cell, or -1. */
static int menu_item(unsigned column, unsigned row) {
    if (row < 1 || row > MENU_ITEMS || column < menu_col || column >= menu_col + MENU_W) {
        return -1;
    }
    return (int)row - 1;
}

/* Clears the line being typed and types command, then Enter. */
static void type_command(const char *command) {
    picked_len = picked_done = 0;
    for (size_t i = 0; i < typed_len && picked_len < PICKED - 1; i++) {
        picked[picked_len++] = '\b';
    }
    while (*command != '\0' && picked_len < PICKED - 1) {
        picked[picked_len++] = *command++;
    }
    picked[picked_len++] = '\n';
}

/* Keeps the status bar's clock going, and the pointer moving, while we wait
   for keys. There are no interrupts for either: they move when looked at.
   Clicking a name - in a listing, say - types it at the prompt, which saves
   copying a filename out by hand; those characters are handed back here as
   if typed. */
static char idle(void) {
    status_update(false);        /* which notices the firmware taking the
                                    screen back, and repaints */
    bg_check();                  /* ...and that loses the wallpaper */
    /* The syscalls of whatever ran last, to /log - but only once the
       keyboard has been quiet for a moment. Writing costs a tenth of a
       second, and doing it the instant a command finishes put that delay in
       front of whoever was already typing the next one. */
    if (efi_uptime_ms() - last_key > QUIET_MS) {
        log_flush();
    }

    if (mouse_present()) {
        unsigned column = 0, row = 0;
        bool on = mouse_poll(), at = vga_cell_at(mouse_x(), mouse_y(), &column, &row);
        int item = menu_open && at ? menu_item(column, row) : -1;

        if (on && menu_open && item != menu_lit) {
            int was = menu_lit;

            menu_lit = item;
            if (was >= 0) {
                menu_draw_item((unsigned)was);
            }
            if (item >= 0) {
                menu_draw_item((unsigned)item);
            }
        }
        if (mouse_clicked() && picked_done == picked_len) {
            unsigned first;

            if (menu_open) {
                menu_hide();        /* a click anywhere closes it */
                if (item >= 0) {
                    type_command(menu[item].command);
                }
            } else if (at && row == 0 && status_power(column, &first)) {
                menu_show(first);
            } else {
                picked_len = vga_word_at(mouse_x(), mouse_y(), picked, sizeof picked);
                picked_done = 0;
            }
        }
        /* Printing anything takes the arrow off the screen, so it is put
           back here rather than only when the mouse moves. */
        vga_pointer(mouse_x(), mouse_y());
    }
    return picked_done < picked_len ? picked[picked_done++] : 0;
}

/* ---- the settings --------------------------------------------------------
 *
 * A libc asks what the terminal is doing before its program says anything:
 * a shell told that its input is not a terminal reads it as it would a
 * script, silently and with no prompt. So the screen answers, and keeps the
 * settings it is handed. What matters of them is whether a line is gathered
 * here before the program sees it, and whether typing shows on the way.
 *
 * The shape is the kernel's own struct termios, which is what a libc puts on
 * the wire - not its own, which carries the line speeds as well. */

#define NCCS 19

struct termios {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t  line, cc[NCCS];
    uint32_t ispeed, ospeed;
};

_Static_assert(sizeof(struct termios) == 44, "struct termios2 is what Linux's is");

#define ICRNL  0x0100       /* a typed return arrives as a newline */
#define IXON   0x0400
#define OPOST  0x0001
#define ONLCR  0x0004
#define B38400 0x000F
#define CS8    0x0030
#define CREAD  0x0080
#define ISIG   0x0001
#define ICANON 0x0002       /* a line at a time, gathered here */
#define ECHO   0x0008       /* what is typed shows as it is typed */
#define ECHOE  0x0010
#define ECHOK  0x0020
#define IEXTEN 0x8000

#define VERASE 2
#define VEOF   4
#define VMIN   6

static struct termios settings;

/* A key taken off the keyboard before anything asked for it, which is what
   answering "is there anything to read?" costs: the look cannot be undone,
   so what it found waits here for the next read. */
static char peeked;

static char take_key(void) {
    char c = peeked;

    if (c != 0) {
        peeked = 0;
    } else {
        c = keyboard_read_char(idle);
    }
    last_key = efi_uptime_ms();
    menu_hide();                    /* typing closes it, before it is echoed */
    return c;
}

bool console_ready(void) {
    if (peeked == 0) {
        peeked = keyboard_poll_char(idle);
        if (peeked != 0) {
            last_key = efi_uptime_ms();
        }
    }
    return peeked != 0;
}

void console_reset(void) {
    memset(&settings, 0, sizeof settings);
    peeked = 0;
    picked_len = picked_done = typed_len = 0;
    settings.iflag = ICRNL | IXON;
    settings.oflag = OPOST | ONLCR;
    settings.cflag = B38400 | CS8 | CREAD;
    settings.lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
    settings.cc[VERASE] = '\b';     /* what the keyboard sends for it */
    settings.cc[VEOF] = 4;
    settings.cc[VMIN] = 1;
}

void console_get(void *out, size_t size) {
    memcpy(out, &settings, size > sizeof settings ? sizeof settings : size);
}

void console_set(const void *in, size_t size) {
    memcpy(&settings, in, size > sizeof settings ? sizeof settings : size);
}

/* A line typed at the keyboard, echoed as it is typed, ending in the
   newline. A line longer than the buffer comes back in pieces, as it would
   on Linux. With ICANON turned off - which is what a program doing its own
   line editing does - a character comes back the moment it is typed and
   nothing is echoed unless ECHO says so. */
uint64_t console_read(char *buf, uint64_t count) {
    bool cooked = (settings.lflag & ICANON) != 0;
    bool echo = (settings.lflag & ECHO) != 0;
    uint64_t len = 0;

    typed_len = 0;
    while (len < count) {
        char c = take_key();

        if (!cooked) {
            buf[len++] = c;
            if (echo) {
                vga_putc(c);
            }
            break;                  /* one character is a read of its own */
        }
        if (c == (char)settings.cc[VEOF]) {
            break;                  /* what has been typed, and nothing more -
                                       none of it at all is end of input */
        }
        if (c == (char)settings.cc[VERASE]) {
            if (len > 0) {
                len--;
                typed_len = len;
                if (echo) {
                    vga_putc('\b');
                }
            }
            continue;
        }
        if (echo) {
            vga_putc(c);
        }
        buf[len++] = c;
        typed_len = len;
        if (c == '\n') {
            break;
        }
    }
    return len;
}
