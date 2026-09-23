#pragma once

#include <stdint.h>

/* Sizes in bytes unless named otherwise. */
struct mem_stats {
    uint32_t total_kib;     /* usable RAM, as the firmware counted it */
    uint32_t image;         /* kernel code and data, as loaded from kernel.bin */
    uint32_t data;          /* zero-filled kernel data (.bss), minus the stack */
    uint32_t stack;         /* the kernel stack */
    uint32_t stack_peak;    /* the most of the stack ever in use */
    uint32_t page_tables;   /* the fixed window's, hung off the firmware's */
    uint32_t console;       /* the console's cells, taken from the firmware */
    uint32_t disk_cache;    /* sectors kept so the next program need not read */
    uint32_t wallpaper;     /* the picture behind the text, if there is one */
    uint32_t window;        /* what the running program has, the tables
                               describing it counted in */
    uint32_t used_kib;      /* all of the above but the window, in KiB
                               rounded up: what the machine costs, whoever is
                               asking and whatever is running */
};

void mem_get_stats(struct mem_stats *stats);
