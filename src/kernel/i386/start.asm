; Kernel entry on a 32-bit processor. A loader - BOOTIA32.EFI or the BIOS
; one - calls _start in 32-bit protected mode with flat segments, the boot
; information as the one argument on the stack, as C passes it; paging may
; be on or off (kernel_main puts the kernel's own tables in either way).
;
; Linked first, so that _start is the first byte of the flat binary: the
; kernel is linked at 0 and relocates itself, wherever it was put.

BITS 32

SECTION .text.start
GLOBAL _start
GLOBAL stack_bottom, stack_top
EXTERN kernel_main
EXTERN __rela_start, __rela_end, __relr_start, __relr_end, _GLOBAL_OFFSET_TABLE_

R_386_RELATIVE equ 8

_start:
    mov edi, [esp + 4]              ; the boot information
    ; Nothing is relocated yet, so everything is reached from where this
    ; is, through the GOT's address - there being no RIP to ask.
    call .here
.here:
    pop ebx
    add ebx, _GLOBAL_OFFSET_TABLE_ + $$ - .here wrt ..gotpc
    lea esp, [ebx + stack_top wrt ..gotoff]
    lea eax, [ebx + __relr_end wrt ..gotoff]
    push eax
    lea eax, [ebx + __relr_start wrt ..gotoff]
    push eax
    lea esi, [ebx + __rela_start wrt ..gotoff]
    lea edx, [ebx + __rela_end wrt ..gotoff]
    lea ebx, [ebx + _start wrt ..gotoff]   ; the base: the image's first byte

    ; Each pointer the image holds was written for an image at 0: a REL
    ; entry (offset, type) adds the base to the word at base + offset - in
    ; the code too, where the assembly names an address.
.relocate:
    cmp esi, edx
    jae .relocated
    cmp byte [esi + 4], R_386_RELATIVE
    jne .next
    mov eax, [esi]
    add [ebx + eax], ebx
.next:
    add esi, 8
    jmp .relocate
.relocated:
    ; And RELR, as on x86-64 but in 32-bit words: an even one is an offset
    ; to relocate, an odd one a bitmap of the 31 words that follow.
    pop esi
    pop edx
    xor ebp, ebp                    ; where the next word to relocate is
.relr:
    cmp esi, edx
    jae .relrd
    mov eax, [esi]
    add esi, 4
    test al, 1
    jnz .bitmap
    lea ebp, [ebx + eax]
    add [ebp], ebx
    add ebp, 4
    jmp .relr
.bitmap:
    shr eax, 1
    mov ecx, ebp
.bit:
    test eax, eax
    jz .bits_done
    test al, 1
    jz .skip
    add [ecx], ebx
.skip:
    shr eax, 1
    add ecx, 4
    jmp .bit
.bits_done:
    add ebp, 31 * 4
    jmp .relr
.relrd:
    xor ebp, ebp                    ; the end of the frame-pointer chain
    and esp, -16
    sub esp, 12
    push edi
    call kernel_main

.hang:
    cli
    hlt
    jmp .hang

SECTION .bss
ALIGN 16
stack_bottom:
    ; As on x86-64 (start.asm): the kernel's, the firmware's calls' while
    ; there is no firmware stack, and its interrupt handlers'.
    resb 8192
stack_top:
