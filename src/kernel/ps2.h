#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The PS/2 keyboard.
 *
 * The firmware's own keyboard driver, which empties the port on a timer, is
 * stopped, and every byte is read here instead - the way an operating system
 * normally runs it, and what lets a key with no character of its own be
 * turned into the escape sequence a terminal sends for it. A keyboard on USB
 * is left to the firmware and is not affected. */

/* Takes the controller over, if the machine has one. Returns whether it
   does. */
bool ps2_init(void);

/* Reads everything waiting, keeping the keys for ps2_key. */
void ps2_poll(void);

/* The next typed character, or 0 if there is none. */
char ps2_key(void);
