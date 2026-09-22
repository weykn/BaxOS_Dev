#include "keyboard.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "io.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64

#define STATUS_OUTPUT_FULL 0x01

#define SC_RELEASE  0x80    /* set in the scancode when a key goes up */
#define SC_EXTENDED 0xE0    /* prefix byte for the grey keys */

#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36

/* Scancode set 1, which is what the PS/2 controller hands us by default, up
   to the space bar. Anything left at 0 has no character and gets dropped. */
static const char unshifted[0x3A] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=', [0x0E] = '\b',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']', [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
    [0x27] = ';', [0x28] = '\'', [0x29] = '`', [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x37] = '*', [0x39] = ' ',
};

static const char shifted[0x3A] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+', [0x0E] = '\b',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T',
    [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P',
    [0x1A] = '{', [0x1B] = '}', [0x1C] = '\n',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G',
    [0x23] = 'H', [0x24] = 'J', [0x25] = 'K', [0x26] = 'L',
    [0x27] = ':', [0x28] = '"', [0x29] = '~', [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B',
    [0x31] = 'N', [0x32] = 'M', [0x33] = '<', [0x34] = '>', [0x35] = '?',
    [0x37] = '*', [0x39] = ' ',
};

static bool shift_held;

static uint8_t read_scancode(void (*idle)(void)) {
    while ((inb(PS2_STATUS) & STATUS_OUTPUT_FULL) == 0) {
        if (idle != NULL) {
            idle();
        }
        __asm__ volatile("pause");
    }
    return inb(PS2_DATA);
}

char keyboard_read_char(void (*idle)(void)) {
    for (;;) {
        uint8_t code = read_scancode(idle);
        uint8_t key = code & ~SC_RELEASE;

        /* The grey keys announce themselves with a prefix byte. We have no use
           for any of them, so drop the pair. */
        if (code == SC_EXTENDED) {
            (void)read_scancode(NULL);
        } else if (key == SC_LSHIFT || key == SC_RSHIFT) {
            shift_held = !(code & SC_RELEASE);
        } else if (code < sizeof unshifted) {
            char c = shift_held ? shifted[code] : unshifted[code];
            if (c != 0) {
                return c;
            }
        }
    }
}
