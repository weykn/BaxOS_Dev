#include "pci.h"

#include "io.h"
#include "mem.h"

uint32_t pci_read(uint32_t at, unsigned offset) {
    outl(0xCF8, 0x80000000u | at | (offset & 0xFC));
    return inl(0xCFC);
}

void pci_write(uint32_t at, unsigned offset, uint32_t value) {
    outl(0xCF8, 0x80000000u | at | (offset & 0xFC));
    outl(0xCFC, value);
}

/* Every bus and device, function 0 of each: onboard cards and the ones
   behind a bridge alike are there. */
uint32_t pci_find(uint16_t vendor, const uint16_t *devices, unsigned count) {
    for (uint32_t at = 1u << 11; at < 1u << 24; at += 1u << 11) {
        uint32_t id = pci_read(at, 0);

        if ((id & 0xFFFF) != vendor) {
            continue;
        }
        for (unsigned i = 0; i < count; i++) {
            if (id >> 16 == devices[i]) {
                return at;
            }
        }
    }
    return 0;
}

/* The first function of any device whose class and subclass are these -
   every function, since a chipset's IDE controller is one of several on the
   same device. */
uint32_t pci_find_class(uint8_t class, uint8_t subclass) {
    return pci_next_class(class, subclass, 0);
}

uint32_t pci_next_class(uint8_t class, uint8_t subclass, uint32_t after) {
    for (uint32_t at = (after != 0 ? after : 0) + (1u << 8); at < 1u << 24; at += 1u << 8) {
        if ((pci_read(at, 0) & 0xFFFF) == 0xFFFF) {
            continue;
        }
        uint32_t code = pci_read(at, 8) >> 16;

        if (code == ((uint32_t)class << 8 | subclass)) {
            return at;
        }
    }
    return 0;
}

uint64_t pci_memory(uint32_t at, unsigned n) {
    uint32_t bar = pci_read(at, 0x10 + 4 * n);
    uint64_t base = bar & ~0xFull;

    if (bar & 1) {
        return 0;
    }
    if ((bar & 6) == 4) {
        base |= (uint64_t)pci_read(at, 0x14 + 4 * n) << 32;
    }
    pci_write(at, 0x04, (pci_read(at, 0x04) & 0xFFFF) | 0x6);   /* memory, bus master */
    if (base != 0) {
        mem_map_io(base);
    }
    return base;
}
