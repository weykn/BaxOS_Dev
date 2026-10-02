
global _start

section .data
    message: db "Hello World", 0xA
    length: equ $ - message

section .text

_start:
    mov rax, 1
    mov rdi, 1
    lea rsi, [rel message]
    mov rdx, length
    syscall

    mov rax, 60
    xor rdi, rdi
    syscall
