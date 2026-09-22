#include "mem.h"

#include "bg.h"
#include "boot.h"
#include "efi_kernel.h"
#include "syscall.h"
#include "vga.h"

/* Defined by kernel.ld and start.asm; only their addresses mean anything. */
extern char __kernel_start[], __bss_start[], __bss_end[], stack_bottom[], stack_top[];

static uint32_t span(const char *start, const char *end) {
    return (uint32_t)((uintptr_t)end - (uintptr_t)start);
}

void mem_get_stats(struct mem_stats *stats) {
    stats->total_kib = (uint32_t)efi_boot()->memory_kib;
    stats->image = span(__kernel_start, __bss_start);
    stats->stack = span(stack_bottom, stack_top);
    stats->data = span(__bss_start, __bss_end) - stats->stack;
    stats->page_tables = (uint32_t)program_tables();
    stats->console = (uint32_t)vga_memory();
    stats->wallpaper = bg_memory();
    stats->window = (uint32_t)program_memory();
    /* What the machine costs, which is everything above but the window: that
       is what whatever is running has borrowed, and it comes and goes with
       it. Leaving it in made this figure depend on who was asking - the
       title bar, drawn only while the machine waits for a key, could never
       agree with a program that measured memory while running. The window is
       reported on its own, by whatever wants to show it. */
    stats->used_kib = (stats->image + stats->data + stats->stack + stats->page_tables +
                       stats->console + stats->wallpaper + 1023) / 1024;

    /* The stack started out zeroed, so its deepest non-zero byte marks how
       far it has ever grown. */
    uint32_t untouched = 0;
    while (untouched < stats->stack && stack_bottom[untouched] == 0) {
        untouched++;
    }
    stats->stack_peak = stats->stack - untouched;
}
