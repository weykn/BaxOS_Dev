#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The pointer: a hypervisor's absolute mouse, the PS/2 mouse and the
   firmware's pointing devices, merged into one. There are no interrupts to
   drive it, so it moves when it is asked to - which the shell does while it
   waits for keys. */

/* Finds a pointing device and puts the pointer in the middle of the screen.
   Returns false if the machine has none, which leaves everything else
   working exactly as before. */
bool mouse_init(void);

/* Reads the device. True if the pointer moved or a button changed. */
bool mouse_poll(void);

unsigned mouse_x(void);
unsigned mouse_y(void);

/* Whether there is a pointing device at all. */
bool mouse_present(void);

/* True once for each press, cleared by asking. */
bool mouse_clicked(void);
