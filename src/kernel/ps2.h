#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The PS/2 controller, keyboard and mouse both.
 *
 * The two share one controller and one data port, and whoever reads a byte
 * gets it whichever device it came from. So the firmware's keyboard driver,
 * which empties the port on a timer, is stopped, and every byte is read here
 * and sent where it belongs - the way an operating system normally runs it.
 * A keyboard on USB is left to the firmware and is not affected. */

/* Takes the controller over, if the machine has one. Returns whether it
   does. */
bool ps2_init(void);

/* Sends a byte to the mouse port and returns its first answer, or -1 if
   none came. Only for setting the mouse up: keys arriving meanwhile are
   dropped. */
int ps2_aux_send(uint8_t value);

/* Waits for the next byte from the mouse port, or returns -1. */
int ps2_aux_read(void);

/* Reads everything waiting. Mouse bytes go to mouse_ps2_byte, keys are kept
   for ps2_key. */
void ps2_poll(void);

/* The next typed character, or 0 if there is none. */
char ps2_key(void);

/* Defined by mouse.c: one byte of a mouse report. */
void mouse_ps2_byte(uint8_t byte);
