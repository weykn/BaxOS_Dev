#pragma once

/* One look at the keyboard: the character typed - control characters and
   the sequences the arrows and the rest send included - or 0 if nothing
   has been. idle is called first, unless it is NULL, and a character it
   returns is taken as typed. */
char keyboard_poll_char(char (*idle)(void));
