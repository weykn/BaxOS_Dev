/* TEF, the Tuxlet executable format: a program as the bytes it runs as,
 * behind an 8-byte header -
 *
 *     "TEF"           magic
 *     u8 class        1 for 32-bit code, 2 for 64-bit, as ELF numbers them
 *     u32 size        what it needs of memory: the file, then zeroes (.bss)
 *
 * _start comes straight after the header, at byte 8. The kernel copies the
 * file to where position-independent programs go and jumps there; there is
 * nothing else to read. Nothing is relocated either, so code is built -fpie
 * and may not keep an address in its data: anything that would need fixing
 * up lands in .rela.dyn, and the link fails. The Makefile passes TEF_CLASS
 * and the output format (-m32 builds elf32-i386). */

ENTRY(_start)

SECTIONS {
    . = 0;
    .tef : {
        BYTE(0x54) BYTE(0x45) BYTE(0x46) BYTE(TEF_CLASS)
        LONG(_end)
        KEEP(*(.text._start))
    }
    .text   : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.* .got .got.plt) }
    .bss    : { *(.bss .bss.* COMMON) }
    _end = .;
    .rela.dyn : { *(.rela.* .rel.*) }
    /DISCARD/ : { *(.dynamic .dynsym .dynstr .hash .gnu.hash .interp .note.* .comment .eh_frame*) }
}

ASSERT(_start == 8, "TEF: _start must follow the header")
ASSERT(SIZEOF(.rela.dyn) == 0, "TEF: an address in data would need relocating")
