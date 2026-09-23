#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The screen and the keyboard, as the one terminal this machine has.
 *
 * A program reads from it through fd 0 and writes to it through fd 1, and
 * what happens in between - a line gathered before the program sees it, the
 * typing echoed, backspace erasing - is here rather than in the program.
 * That is what lets the shell be an ordinary program: it asks for a line and
 * gets one.
 *
 * Waiting for a key is also the only time the machine is idle, so everything
 * that has to keep moving without interrupts moves here: the clock in the
 * status bar, the wallpaper, and writing out what the last program did. */

/* A plain terminal again: lines gathered here, typing echoed, Ctrl-D ending
   the input. Run before each program, so one that left the terminal raw
   cannot leave it that way for the next. */
void console_reset(void);

/* Reads up to count bytes of what is typed, as the settings say to: a line
   at a time when cooked, one key at a time when raw. Returns how many. */
uint64_t console_read(char *buf, uint64_t count);

/* Whether a key is waiting. Looking takes it off the keyboard, so it is kept
   for the next read. */
bool console_ready(void);

/* The settings, as Linux's tcgetattr and tcsetattr pass them. size is how
   much of them the program asked for or handed over. */
void console_get(void *out, size_t size);
void console_set(const void *in, size_t size);
