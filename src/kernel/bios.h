#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "boot.h"

/* The machine as a PC BIOS started it (src/boot/bios.asm), answering what
 * efi.c answers from UEFI when the loader was that one instead. The BIOS
 * is reached by dropping back to real mode for one interrupt at a time:
 * INT 13h for the disk and INT 16h for the keyboard, until modules drive
 * them. The clock, power and reset need no BIOS at all. */

/* Takes memory from the E820 map, the kernel's own page tables, and finds
   the filesystem's partition on the boot drive. info is efi.c's copy, and
   its memory figures are filled in here. */
void bios_init(struct boot_info *info);

/* Sectors of the filesystem's partition, by INT 13h; 0 or -1. */
int bios_read(uint32_t lba, unsigned count, void *buffer);
int bios_write(uint32_t lba, unsigned count, const void *buffer);

/* A key from INT 16h, as keyboard_poll_char hands one on, or 0. */
char bios_key(void);

/* The CMOS clock: seconds past midnight, and since 1970. */
unsigned bios_seconds(void);
uint64_t bios_epoch(void);

/* Processor counter ticks in a second, timed against the PIT. */
uint64_t bios_ticks_per_second(void);

/* No more calls into the BIOS: its buffer becomes free memory. */
void bios_leave(void);

/* ACPI S5, and a reset; each returns only if the machine would not. */
void bios_power_off(void);
void bios_restart(void);
