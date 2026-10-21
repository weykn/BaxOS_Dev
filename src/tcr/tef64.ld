/* TEF64, the Tuxlet executable format: a program as the bytes it runs as,
 * behind a 12-byte header -
 *
 *     "TEF64\0\0\0"   magic
 *     u32 size        what it needs of memory: the file, then zeroes (.bss)
 *
 * _start comes straight after the header, at byte 12. The kernel copies the
 * file to where position-independent programs go and jumps there; there is
 * nothing else to read. Nothing is relocated either, so code is built -fpie
 * and may not keep an address in its data: anything that would need fixing
 * up lands in .rela.dyn, and the link fails. */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS {
    . = 0;
    .tef : {
        BYTE(0x54) BYTE(0x45) BYTE(0x46) BYTE(0x36) BYTE(0x34) BYTE(0) BYTE(0) BYTE(0)
        LONG(_end)
        KEEP(*(.text._start))
    }
    .text   : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.* .got .got.plt) }
    .bss    : { *(.bss .bss.* COMMON) }
    _end = .;
    .rela.dyn : { *(.rela.*) }
    /DISCARD/ : { *(.dynamic .dynsym .dynstr .hash .gnu.hash .interp .note.* .comment .eh_frame*) }
}

ASSERT(_start == 12, "TEF64: _start must follow the header")
ASSERT(SIZEOF(.rela.dyn) == 0, "TEF64: an address in data would need relocating")
