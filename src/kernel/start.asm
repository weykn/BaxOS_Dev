; Kernel entry. The UEFI loader jumps here with the machine already in long
; mode, everything the firmware set up still mapped, and RDI pointing at the
; boot information - which is also the first argument kernel_main takes, so it
; is simply left where it is.
;
; This object is linked first so that _start lands at the very start of the
; flat binary, which is the address the loader jumps to - wherever the
; firmware had room, since the kernel is linked at 0 and relocates itself.

BITS 64
DEFAULT REL

SECTION .text.start
GLOBAL _start
GLOBAL stack_bottom, stack_top
EXTERN kernel_main
EXTERN __rela_start, __rela_end

R_X86_64_RELATIVE equ 8

_start:
    ; The loader zeroed this whole region before copying the image in, so
    ; .bss - which a flat binary does not carry - is already clear.
    lea rsp, [stack_top]

    ; Every pointer the image holds - a table of commands, a string in an
    ; array - was written for an image at 0. Each is one RELATIVE entry:
    ; put base + addend at base + offset. Nothing else is left by a static
    ; position-independent link.
    lea r8, [_start]                ; the base: _start is the image's first byte
    lea rsi, [__rela_start]
    lea rdx, [__rela_end]
.relocate:
    cmp rsi, rdx
    jae .relocated
    cmp dword [rsi + 8], R_X86_64_RELATIVE
    jne .next
    mov rax, [rsi + 16]             ; the addend
    add rax, r8
    mov rcx, [rsi]                  ; the offset
    mov [r8 + rcx], rax
.next:
    add rsi, 24
    jmp .relocate
.relocated:
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
