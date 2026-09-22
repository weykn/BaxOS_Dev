; Ring 3 support: entering a user program, the syscall entry point, leaving
; the program again, and the CPU exceptions it can raise along the way. The C
; side is syscall.c.

BITS 64
DEFAULT REL

SECTION .text
GLOBAL user_enter, user_exit, syscall_entry, fault_entry, page_fault_entry
EXTERN syscall_dispatch, page_fault, tss

; The always-one bit, and IF. Interrupts have to stay on in ring 3: the
; firmware's timers are what drive its keyboard and its USB polling, so a
; program running with them off freezes the whole machine rather than just
; itself - and a program that never returns could then only be powered off.
USER_RFLAGS equ 0x202
KILLED      equ 139                     ; PROGRAM_KILLED in syscall.h

; The kernel stack pointer while a program runs. It is the TSS's RSP0, so an
; exception from ring 3 switches to the very stack the syscalls use. It is
; zero when no program is running, which is what tells a kernel fault from a
; program's.
%define kernel_rsp (tss + 4)

; int user_enter(uint64_t entry, uint64_t stack)
; Starts a program in ring 3; returns its exit code once it calls user_exit.
;
; A program can start another - the shell is itself a program - so the frame
; this leaves is one of a stack of them: the caller's RSP0 is kept alongside
; the registers and put back on the way out, and the outermost one puts back
; the zero that says no program is running.
user_enter:
    push rbx                            ; what a C caller expects kept
    push rbp
    push r12
    push r13
    push r14
    push r15
    push qword [kernel_rsp]             ; whoever we are running inside
    sub rsp, 8                          ; syscall_entry wants RSP0 8 off 16
    mov [kernel_rsp], rsp
    mov rcx, rdi                        ; sysret jumps to RCX ...
    mov r11, USER_RFLAGS                ; ... with RFLAGS = R11
    mov rsp, rsi

    ; Hand over a clean machine. Linux does the same, and a libc reads more
    ; of it than one would think: glibc's _start takes RDX to be a function
    ; to run at exit, and calls whatever is in it.
    xor eax, eax
    xor ebx, ebx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    o64 sysret

; noreturn void user_exit(int code)
; Called from a syscall handler or an exception: abandons the program, and
; whatever the kernel was doing for it, and returns from user_enter.
user_exit:
    mov eax, edi
    mov rsp, [kernel_rsp]
    add rsp, 8
    pop qword [kernel_rsp]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

; SYSCALL lands here in ring 0 but still on the user stack, with RCX = the
; program's RIP and R11 = its RFLAGS. The number is in RAX, the arguments in
; RDI, RSI and RDX, and the result goes back in RAX.
; Linux passes six arguments in RDI, RSI, RDX, R10, R8 and R9. A C function
; takes its fourth in RCX rather than R10 - RCX cannot be used, because the
; syscall instruction puts the return address there - so that one is moved
; across, and the call number goes on the stack as a seventh.
syscall_entry:
    mov [user_rsp], rsp
    mov rsp, [kernel_rsp]               ; just below user_enter's frame
    push qword [user_rsp]
    push rcx                            ; the program's RIP
    push r11                            ; three pushes leave RSP 16-byte aligned
    ; Linux gives back every register but RAX, RCX and R11, and libcs keep
    ; values in the argument registers across a syscall on the strength of
    ; it - glibc its TLS offsets in R8. Six more pushes keep the alignment.
    push rdi
    push rsi
    push rdx
    push r8
    push r9
    push r10
    mov rcx, r10                        ; the 4th argument, where C wants it
    sub rsp, 8                          ; and back to 16 once the number is on
    push rax
    call syscall_dispatch
    add rsp, 16
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rsi
    pop rdi
    pop r11
    pop rcx
    pop rsp
    o64 sysret

; Every CPU exception but the page fault lands here. None of them can be
; resumed, so the program is ended, as Linux would with a signal; the
; exception's frame is abandoned along with the rest of the stack.
fault_entry:
    cld                                 ; the program may have left DF set
    ; With no program running there is no frame to go back to, and the fault
    ; is the kernel's own. Stopping leaves whatever it was on screen to read,
    ; where returning would run off a stack that was never set up.
    cmp qword [kernel_rsp], 0
    je .stop
    mov edi, KILLED
    jmp user_exit
.stop:
    cli
    hlt
    jmp .stop

; A page fault is normally a program touching a page of its window for the
; first time: page_fault(address) maps one in and returns, and the access is
; retried. For anything else it ends the program itself. It can strike in
; the middle of kernel code too, so everything a C call may clobber is kept.
page_fault_entry:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    sub rsp, 8                          ; the CPU's frame and nine pushes leave RSP 8 off
    cld
    mov rdi, cr2                        ; the address that faulted
    mov rsi, [rsp + 88]                 ; and the instruction, past the error code
    call page_fault
    add rsp, 8
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    add rsp, 8                          ; the error code
    iretq

SECTION .bss
user_rsp: resq 1
