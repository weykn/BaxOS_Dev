#pragma once

#include <stdint.h>

#include "boot.h"

/* The firmware, as the rest of the kernel sees it. ata.h and keyboard.h are
   answered by efi.c too, so nothing above this layer knows how the machine
   was started. */

void efi_init(struct boot_info *info);

/* What the loader found, for the framebuffer and the memory figure. */
const struct boot_info *efi_boot(void);

/* Asks the firmware to bind drivers to everything it knows about, which is
   what makes a mouse on USB turn up. Slow - it is left until the shell is
   idle rather than done on the way up. */
void efi_connect_devices(void);

/* Seconds past midnight, by the firmware's clock. */
unsigned efi_seconds(void);

/* The same clock as seconds since the start of 1970, which is what a program
   asking the time expects. */
uint64_t efi_epoch(void);

/* Milliseconds since the loader was entered. The first call works out how
   fast the processor's counter runs, which takes a hundredth of a second;
   the reading it returns is taken before that, so it costs the answer
   nothing. */
uint64_t efi_uptime_ms(void);

/* Asks the machine to switch itself off; returns only if it would not. */
void efi_power_off(void);

/* Asks the machine to start again from the firmware; returns only if it
   would not. */
void efi_restart(void);
