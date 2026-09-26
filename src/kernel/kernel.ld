/* The UEFI loader takes KERNEL_BYTES (boot.h) wherever the firmware has
   them, copies the image to the front and jumps to its first byte, so
   .text.start (holding _start) has to come first. The image is linked at 0
   as a position-independent executable and carries its relocations with it:
   _start applies them before anything else runs. Image, .bss and stack all
   live inside the KERNEL_BYTES. */

ENTRY(_start)

SECTIONS
{
    . = 0;
    __kernel_start = .;

    .text : {
        KEEP(*(.text.start))
        *(.text*)
    }

    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }

    /* What a position-independent link leaves for a loader, kept in the
       image because the kernel is its own loader. Only the relocations are
       read; the rest is the linker's paperwork. */
    .dynamic : { *(.dynamic) }
    .rela   : {
        __rela_start = .;
        *(.rela*)
        __rela_end = .;
    }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    .hash   : { *(.hash .gnu.hash) }
    .got    : { *(.got .got.plt) }

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

    /* KERNEL_BYTES in boot.h: all of it has to fit what the loader takes. */
    ASSERT(__bss_end <= 0x20000, "the kernel outgrew KERNEL_BYTES")

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
