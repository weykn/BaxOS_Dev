#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Keys and the mouse as Linux's evdev hands them to a program: one event at
 * a time - a key going down or up, the mouse moving - each with the time it
 * happened, read from /dev/input/event0 (the keyboard) and event1 (the
 * mouse). The keyboard drivers report them as they read the hardware, which
 * they do whenever the console looks for a key.
 *
 * Nothing is kept until a program opens one: the queue is bought then and
 * goes when the program that started it all ends. */

#define INPUT_KEYBOARD 0
#define INPUT_MOUSE    1
#define INPUTS         2

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_REP 0x14

#define REL_X     0x00
#define REL_Y     0x01
#define REL_WHEEL 0x08
#define BTN_LEFT  0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112

#define INPUT_KEY_BYTES 48          /* every key and button up to 0x17F, a bit each */

/* What a driver calls: a key (EV_KEY, Linux's KEY_ code, 1 down, 0 up), a
   movement (EV_REL), or the end of a batch (EV_SYN, 0, 0). A key that goes
   down while it is down is a repeat, and is passed on as one. */
void input_report(unsigned device, unsigned type, unsigned code, int value);

/* Opening one: false if its queue cannot be had. */
bool input_open(unsigned device);

/* Whether an event is waiting, looking at the hardware first. */
bool input_ready(unsigned device);

/* Up to count bytes of whole events, as struct input_event; 0 if none. */
uint64_t input_read(unsigned device, void *buf, uint64_t count);

/* The clock event times are on (EVIOCSCLOCKID): from boot, or from 1970. */
void input_clock(unsigned device, bool monotonic);

/* Which keys are down now, a bit each, for EVIOCGKEY. */
const uint8_t *input_keys(unsigned device);

/* The queues go back, once nothing is running. */
void input_release(void);
