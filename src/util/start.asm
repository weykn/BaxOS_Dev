; Where a raw utility starts.
;
; There is no header, no loader and nothing to relocate: the kernel puts the
; file in memory at PROGRAM_BASE and jumps to its first byte, so this object
; is linked first and this instruction is that byte.
;
; The stack is the one the kernel built - the same one a Linux program gets -
; so the count of arguments is on top of it and the arguments themselves
; follow, which is exactly what C's main wants.

BITS 64
DEFAULT REL

SECTION .text.start
GLOBAL _start
EXTERN main

_start:
    mov rdi, [rsp]              ; argc
    lea rsi, [rsp + 8]          ; argv
    and rsp, -16                ; a C call wants the stack aligned
    call main

    mov edi, eax                ; what main returned is the exit code
    mov eax, 60                 ; SYS_EXIT
    syscall
.hang:
    hlt                         ; the kernel does not return from exit
    jmp .hang
