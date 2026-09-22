/* The UEFI loader takes this memory from the firmware, copies the image to
   the front of it and jumps to its first byte, so .text.start (holding
   _start) has to come first and the whole image has to be linked for that
   address - there is no relocation at runtime.

   KERNEL_BASE in boot.h is the same number, and the loader asks the firmware
   for KERNEL_BYTES there: image, .bss and stack all live inside it. */

ENTRY(_start)

SECTIONS
{
    . = 0x00200000;
    __kernel_start = .;

    .text : {
        KEEP(*(.text.start))
        *(.text*)
    }

    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }

    . = ALIGN(16);
    __bss_start = .;
    .bss : {
        /* Widest alignment first: the page tables and the stack want whole
           pages, and left where they fell they pushed three or four
           kilobytes of padding in front of themselves. */
        *(SORT_BY_ALIGNMENT(COMMON))
        *(SORT_BY_ALIGNMENT(.bss*))
    }
    . = ALIGN(16);
    __bss_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
