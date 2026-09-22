#pragma once

#include <stdbool.h>

/* The title bar as a status bar: screen mode, memory and disk use, uptime
   and the time of day. There are no timer interrupts, so it only moves when
   status_update is called - which the console does while it waits for a
   key, and not while a program runs. */

/* Starts counting uptime and draws the bar. Needs the filesystem up. */
void status_init(void);

/* Redraws the bar if the clock has moved on, or regardless if refresh is
   set. Memory and disk use are re-read whenever it is drawn. */
void status_update(bool refresh);

/* Whether a click on the bar's column landed on the power button, and the
   column the button starts at, for a menu to hang under it. */
bool status_power(unsigned column, unsigned *first);
