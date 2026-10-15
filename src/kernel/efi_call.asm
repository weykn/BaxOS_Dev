; Calls into the firmware, on a stack of its own.
;
; UEFI promises its drivers 128 KiB of stack, and a real machine's take it:
; letting go of a USB controller tears down every device behind it, a disk
; read goes through half a dozen layers of drivers. The kernel's own stack is
; 8 KiB. So while the firmware is there, every call into it switches to
; efi_stack (efi.c, given back with the rest of the firmware's memory once
; it is left), and only there are its interrupts let in: its timer handlers
; run on the same big stack, never on the kernel's.

BITS 64
DEFAULT REL

SECTION .text
GLOBAL efi_call, efi_stack

EFI_STACK_BYTES equ 128 * 1024        ; EFI_STACK_PAGES in efi.c

; uint64_t efi_call(const void *fn, uint64_t a, uint64_t b, uint64_t c,
;                   uint64_t d, uint64_t e)
; fn with the five arguments in Microsoft's convention: a-d in RCX, RDX, R8,
; R9, e on the stack above the 32 bytes the callee may use as its own.
efi_call:
    push rbp
    mov rbp, rsp
    push rbx
    pushfq
    mov rbx, [efi_stack]
    test rbx, rbx
    jz .here                            ; no firmware stack: after it is left
    lea rax, [rbx - EFI_STACK_BYTES]
    cmp rsp, rax
    jbe .switch
    cmp rsp, rbx
    jbe .here                           ; already on it
.switch:
    mov rsp, rbx
.here:
    and rsp, -16
    sub rsp, 48
    mov [rsp + 32], r9
    mov rax, rdi
    mov r9, r8
    mov r8, rcx
    mov rcx, rsi
    test rbx, rbx
    jz .call
    sti
.call:
    call rax
    cli
    lea rsp, [rbp - 16]
    popfq                               ; interrupts as they were
    pop rbx
    pop rbp
    ret

SECTION .bss
efi_stack: resq 1                       ; its top, or 0
