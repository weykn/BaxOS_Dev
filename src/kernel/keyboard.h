#pragma once

/* Blocks until a key is pressed, then returns its character. Keys without one
   (shift, tab, escape, the function keys, arrows) are swallowed rather than
   returned, so callers only ever see printable characters plus '\n' and '\b'.
   While it waits it calls idle over and over, unless that is NULL; if idle
   returns a character, that is taken as typed. */
char keyboard_read_char(char (*idle)(void));

/* One look at the keyboard rather than a wait: the character typed, or 0 if
   nothing has been. idle is called once, as above. */
char keyboard_poll_char(char (*idle)(void));
