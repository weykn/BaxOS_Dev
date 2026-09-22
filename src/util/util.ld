/* A utility is a raw image: no headers, no sections, no entry point to look
   up. Everything that is in the file is laid out from PROGRAM_BASE, with the
   entry stub first so that the first byte of the file is the first
   instruction. What follows the file - the zeroed data - is not in it: the
   kernel maps those pages empty as they are touched. */

ENTRY(_start)

SECTIONS
{
    . = 0x400000;               /* PROGRAM_BASE, where the kernel loads it */

    .text : {
        *(.text.start)          /* the entry, which has to be first */
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }

    .bss (NOLOAD) : {
        *(.bss .bss.* COMMON)
    }

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame*) }
}
