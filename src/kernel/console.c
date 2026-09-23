#include "console.h"

#include "bg.h"
#include "efi_kernel.h"
#include "keyboard.h"
#include "log.h"
#include "string.h"
#include "vga.h"

/* When the last key arrived, so that work put off until the machine is idle
   waits for it to be idle rather than merely between two keystrokes. */
#define QUIET_MS 400
static uint64_t last_key;

/* What has to keep happening while nothing is running. There are no timer
   interrupts, so it happens when it is looked at - and waiting for a key is
   the only time anything is looking. */
static char idle(void) {
    vga_follow();                /* the firmware may have taken the screen
                                    back; this notices and repaints */
    bg_check();                  /* ...and that loses the wallpaper */
    /* The syscalls of whatever ran last, to /log - but only once the
       keyboard has been quiet for a moment. Writing costs a tenth of a
       second, and doing it the instant a command finishes put that delay in
       front of whoever was already typing the next one. */
    if (efi_uptime_ms() - last_key > QUIET_MS) {
        log_flush();
    }

    return 0;
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
                if (echo) {
                    vga_puts("\b \b");     /* back over it, blank it, back again */
                }
            }
            continue;
        }
        if (echo) {
            vga_putc(c);
        }
        buf[len++] = c;
        if (c == '\n') {
            break;
        }
    }
    return len;
}
