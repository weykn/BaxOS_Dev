#pragma once

#include <stdint.h>

/* PCI configuration space, through the two ports every PC has for it. A
 * device is named by its configuration address: bus, device and function
 * shifted into place, as pci_find returns it. */

uint32_t pci_read(uint32_t at, unsigned offset);
void     pci_write(uint32_t at, unsigned offset, uint32_t value);

/* The first device on any bus from vendor with one of the count device ids,
   or 0 if there is none. */
uint32_t pci_find(uint16_t vendor, const uint16_t *devices, unsigned count);

/* The first function of class and subclass (0x01, 0x01 is an IDE
   controller), or 0. */
uint32_t pci_find_class(uint8_t class, uint8_t subclass);
