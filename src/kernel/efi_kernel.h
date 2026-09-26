#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "boot.h"

/* The firmware, as the rest of the kernel sees it. ata.h and keyboard.h are
   answered by efi.c too, so nothing above this layer knows how the machine
   was started. */

void efi_init(struct boot_info *info);

/* What the loader found, for the framebuffer and the memory figure. */
const struct boot_info *efi_boot(void);

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

/* RAM free right now, as the firmware's memory map has it, in KiB. Only
   while the firmware is still running; mem_free_kib knows which to ask. */
uint64_t efi_free_kib(void);

/* Lets the firmware go, as an operating system does once it is up: its
   drivers and everything it kept for itself become the kernel's memory. It
   only happens if the kernel can do without it - a PS/2 keyboard and an IDE
   disk holding the filesystem - and otherwise nothing changes and the
   firmware's drivers go on being used. Returns whether it went. */
bool efi_leave(void);

/* Asks the machine to switch itself off; returns only if it would not. */
void efi_power_off(void);

/* Asks the machine to start again from the firmware; returns only if it
   would not. */
void efi_restart(void);
