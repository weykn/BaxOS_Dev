; Ring 3 support: entering a user program, the syscall entry point, leaving
; the program again, and the CPU exceptions it can raise along the way. The C
; side is syscall.c.

BITS 64
DEFAULT REL

SECTION .text
GLOBAL user_enter, user_resume, user_exit, syscall_entry, trap_stubs, page_fault_entry
GLOBAL user_frame, user_cs, user_ss, user_flags
EXTERN syscall_dispatch, page_fault, trap_report, tss

; A program's RFLAGS, in user_flags below: the always-one bit, and IF while
; the firmware is running. Interrupts have to stay on in ring 3 then: the
; firmware's timers are what drive its keyboard and its USB polling, so a
; program running with them off freezes the whole machine rather than just
; itself. Once the firmware is gone (efi_leave) they go off for good: the
; kernel polls everything, and none of the firmware's handlers are left.

; Ring 3 is entered and returned to with IRETQ rather than SYSRET.
;
; SYSRET is the faster of the two and was what this used, but on AMD it sets
; the stack selector without the two bits that say ring 3 - so a program ran
; with SS = 0x68 where it should have been 0x6B. Nothing noticed until the
; first interrupt: the processor pushes that SS, and IRETQ refuses to return
; to ring 3 with a stack selector that says ring 0. Every program died on its
; first page fault, on every AMD machine, while emulation let it pass.
;
; IRETQ takes the selectors from the frame it is given, so there is nothing
; to get wrong. gdt_init fills these in with the ring bits already on.
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

    push qword [user_ss]                ; the frame IRETQ returns through
    push rsi                            ; the program's stack
    push qword [user_flags]
    push qword [user_cs]
    push rdi                            ; where it starts

    ; Hand over a clean machine. Linux does the same, and a libc reads more
    ; of it than one would think: glibc's _start takes RDX to be a function
    ; to run at exit, and calls whatever is in it.
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    iretq

; int user_resume(const struct user_regs *regs, uint64_t rax)
; The same again for a program that is already running: every register comes
; back from a saved syscall frame rather than being cleared, so what starts is
; the caller at the instruction after its syscall - which is what fork's child
; is. Returns its exit code, as user_enter does.
user_resume:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    push qword [kernel_rsp]
    sub rsp, 8
    mov [kernel_rsp], rsp
    mov r15, rdi                        ; the frame, while the registers load
    mov rax, rsi                        ; what the syscall gives back

    push qword [user_ss]                ; the frame IRETQ returns through
    push qword [r15 + 16*8]             ; the program's own stack
    push qword [r15 + 14*8]             ; its flags
    push qword [user_cs]
    push qword [r15 + 15*8]             ; and where it was

    mov rcx, [r15 + 1*8]
    mov r11, [r15 + 0*8]
    mov r14, [r15 + 3*8]
    mov r13, [r15 + 4*8]
    mov r12, [r15 + 5*8]
    mov rbp, [r15 + 6*8]
    mov rbx, [r15 + 7*8]
    mov r10, [r15 + 8*8]
    mov r9,  [r15 + 9*8]
    mov r8,  [r15 + 10*8]
    mov rdx, [r15 + 11*8]
    mov rsi, [r15 + 12*8]
    mov rdi, [r15 + 13*8]
    mov r15, [r15 + 2*8]                ; last: it was holding the frame
    iretq

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
; Every register the program had is kept, not only the ones Linux promises
; back: fork starts its child from exactly this frame, and a register missing
; from it would be a register the child loses. The C call in the middle keeps
; RBX, RBP and R12-R15 of its own accord, so those six cost nothing but the
; pushes - and being in the frame is what lets user_resume put them back.
syscall_entry:
    mov [user_rsp], rsp
    mov rsp, [kernel_rsp]               ; just below user_enter's frame
    push qword [user_rsp]
    push rcx                            ; the program's RIP
    push r11                            ; three pushes leave RSP 16-byte aligned
    ; Linux gives back every register but RAX, RCX and R11, and libcs keep
    ; values in the argument registers across a syscall on the strength of
    ; it - glibc its TLS offsets in R8.
    push rdi
    push rsi
    push rdx
    push r8
    push r9
    push r10
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov rcx, r10                        ; the 4th argument, where C wants it
    sub rsp, 8                          ; and back to 16 once the number is on
    push rax
    mov [user_frame], rsp               ; where a handler finds all of it
    call syscall_dispatch
    add rsp, 16
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rsi
    pop rdi
    ; What is left is the program's RFLAGS, its RIP and its stack. IRETQ wants
    ; those in its own order with the selectors between them, so the frame is
    ; built just below them and they are copied across.
    mov rcx, [rsp + 8]                  ; where it was
    mov r11, [rsp]                      ; its flags
    sub rsp, 40
    mov [rsp], rcx
    mov rcx, [user_cs]
    mov [rsp + 8], rcx
    mov [rsp + 16], r11
    mov rcx, [rsp + 56]                 ; its stack, past the frame just made
    mov [rsp + 24], rcx
    mov rcx, [user_ss]
    mov [rsp + 32], rcx
    xor ecx, ecx                        ; a syscall may clobber it, and did
    iretq

; Every CPU exception but the page fault lands here: one stub per vector, all
; the same size, so that the C side can install them from a single symbol.
; Each says which vector it was and then ends the program, as Linux would with
; a signal - an illegal instruction and a general protection fault are worth
; telling apart, and used to be indistinguishable.
TRAP_STRIDE equ 16

ALIGN 16
trap_stubs:
%assign vector 0
%rep 32
    ALIGN TRAP_STRIDE
    push rdi
    mov edi, vector
    mov rsi, rsp                    ; the saved RDI, then the CPU's own frame
    jmp trap_common
%assign vector vector + 1
%endrep

trap_common:
    cld                                 ; the program may have left DF set
    ; With no program running there is no frame to go back to, and the fault
    ; is the kernel's own. Stopping leaves whatever it was on screen to read,
    ; where returning would run off a stack that was never set up.
    cmp qword [kernel_rsp], 0
    je .stop
    call trap_report                    ; EDI is still the vector
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

SECTION .data
user_flags: dq 0x202

SECTION .bss
user_rsp:   resq 1
user_frame: resq 1
user_cs:    resq 1                      ; ring 3's selectors, set by gdt_init
user_ss:    resq 1
