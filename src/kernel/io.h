#pragma once

#include <stddef.h>
#include <stdint.h>

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* 32-bit ports, for PCI configuration space. */
static inline uint32_t inl(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outl(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

/* Reads a CMOS register: the RTC's, or one the BIOS filled in. */
static inline uint8_t cmos_read(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

/* Move count 16-bit words between a port and memory. */
static inline void insw(uint16_t port, void *buffer, size_t count) {
    __asm__ volatile("rep insw" : "+D"(buffer), "+c"(count) : "d"(port) : "memory");
}

static inline void outsw(uint16_t port, const void *buffer, size_t count) {
    __asm__ volatile("rep outsw" : "+S"(buffer), "+c"(count) : "d"(port) : "memory");
}

static inline void halt_forever(void) {
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
