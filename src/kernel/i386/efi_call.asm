; Calls into a 32-bit firmware, on a stack of its own (efi_call.asm says
; why). Its convention is C's, so there is nothing to convert: EFI_CALL
; (efi.h) puts the function in efi_target and calls efi_jump as if it were
; that function, and the arguments - however many, however wide - are
; copied across to the firmware's stack as they are.
;
; Its memory is in the first gigabyte with every program's, so for the
; length of the call it sees its own view of that again (vm_firmware_view):
; a program's pages are where its own would be hidden.

BITS 32

SECTION .text
GLOBAL efi_jump, efi_target, efi_stack
EXTERN vm_firmware_view

EFI_STACK_BYTES equ 128 * 1024        ; EFI_STACK_PAGES in efi.c
ARG_WORDS       equ 16                ; the most any call takes, with room

efi_jump:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi
    pushfd
    push 1
    call vm_firmware_view
    add esp, 4
    mov ebx, [efi_stack]
    test ebx, ebx
    jz .here                            ; no firmware stack: after it is left
    lea eax, [ebx - EFI_STACK_BYTES]
    cmp esp, eax
    jbe .switch
    cmp esp, ebx
    jbe .here                           ; already on it
.switch:
    mov esp, ebx
.here:
    and esp, -16
    sub esp, ARG_WORDS * 4
    lea esi, [ebp + 8]
    mov edi, esp
    mov ecx, ARG_WORDS
    cld
    rep movsd
    mov eax, [efi_target]
    test ebx, ebx
    jz .call
    sti
.call:
    call eax
    cli
    lea esp, [ebp - 16]
    push edx                            ; what it answered
    push eax
    push 0
    call vm_firmware_view
    add esp, 4
    pop eax
    pop edx
    popfd                               ; interrupts as they were
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

SECTION .bss
efi_stack:  resd 2                      ; its top, or 0 (a uint64_t in efi.c)
efi_target: resd 1
