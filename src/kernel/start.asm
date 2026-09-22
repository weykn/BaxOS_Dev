; Kernel entry. The UEFI loader jumps here with the machine already in long
; mode, everything the firmware set up still mapped, and RDI pointing at the
; boot information - which is also the first argument kernel_main takes, so it
; is simply left where it is.
;
; This object is linked first so that _start lands at the very start of the
; flat binary, which is the address the loader jumps to.

BITS 64
DEFAULT REL

SECTION .text.start
GLOBAL _start
GLOBAL stack_bottom, stack_top
EXTERN kernel_main

_start:
    ; The loader zeroed this whole region before copying the image in, so
    ; .bss - which a flat binary does not carry - is already clear.
    mov rsp, stack_top
    xor rbp, rbp                ; terminate the frame-pointer chain
    call kernel_main

.hang:
    cli
    hlt
    jmp .hang

SECTION .bss
ALIGN 16
stack_bottom:
    ; This stack is not only ours: every call into the firmware runs on it,
    ; and so do the firmware's interrupt handlers, through the TSS, whenever
    ; one arrives while a ring 3 program is running. Nothing checks for
    ; running off the end of it.
    ;
    ; The deepest the kernel itself can go is a program started by a program
    ; started by a program, as far as SPAWN_DEPTH allows, with a firmware
    ; call at the bottom of it. Measured, that is a little over five
    ; kilobytes - `mem` shows it as the peak - and what a spawn keeps for
    ; itself is borrowed rather than stacked, which is what holds it there.
    ; This is that with half as much again to spare.
    resb 8192
stack_top:
