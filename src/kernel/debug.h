#pragma once

/* Diagnostic output, for working on the kernel rather than for using it.
 *
 * It goes to the emulator's debug port, which QEMU can be told to write to a
 * file - so it costs one I/O instruction a character and shows up nowhere on
 * screen and nowhere on real hardware.
 *
 * To be rid of it entirely: drop -DDEBUG from the Makefile and every call
 * compiles to nothing; delete debug.c and debug.h and the only thing left to
 * remove is the dbg() lines themselves. */

#ifdef DEBUG
void dbg(const char *fmt, ...);     /* %s, %u and %x, as kprintf takes them */

/* One character of what went on screen, sent the same way: with the console
   mirrored to the port, a boot can be read back from a terminal without a
   screen to look at, which is the only way to see one from a script. */
void dbg_screen(char c);
#else
#define dbg(...) ((void)0)
#define dbg_screen(c) ((void)0)
#endif
