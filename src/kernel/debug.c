#include "debug.h"

#ifdef DEBUG

#include <stdarg.h>
#include <stdint.h>

#include "io.h"

/* Bochs and QEMU both treat writes to this port as a character of debug
   output, and ignore it when nothing is listening. Real firmware leaves it
   alone, so a kernel built with this on still runs on a real machine - it
   just talks to nobody. */
#define DEBUG_PORT 0xE9

/* Deliberately its own small formatter rather than kprintf's: this file has
   to be removable without leaving anything behind, and it must work before
   there is a screen to print on. */
static void out(char c) {
    outb(DEBUG_PORT, (uint8_t)c);
}

static void put_uint(uint64_t value, unsigned base) {
    char digits[20];
    unsigned i = 0;

    do {
        digits[i++] = "0123456789abcdef"[value % base];
        value /= base;
    } while (value != 0);
    while (i > 0) {
        out(digits[--i]);
    }
}

void dbg_screen(char c) {
    out(c);
}

void dbg(const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    for (; *fmt != '\0'; fmt++) {
        if (*fmt != '%') {
            out(*fmt);
        } else if (*++fmt == 's') {
            for (const char *s = va_arg(args, const char *); *s != '\0'; s++) {
                out(*s);
            }
        } else if (*fmt == 'x') {
            out('0');
            out('x');
            put_uint(va_arg(args, uint64_t), 16);
        } else if (*fmt == 'u') {
            put_uint(va_arg(args, uint64_t), 10);
        } else {
            out(*fmt);
        }
    }
    va_end(args);
}

#endif
