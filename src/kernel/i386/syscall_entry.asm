; Ring 3 on a 32-bit processor: entering a program, int 0x80 - its only way
; into the kernel, as on i386 Linux - leaving it again, and the exceptions
; it raises. The x86-64 counterpart is ../syscall_entry.asm, and this keeps
; to its shapes: the frames C reads are the same structures of 64-bit
; slots (struct user_regs, struct trap_regs), each register in the low half
; of one, so that syscall.c, thread.c and compat/ia32 read them alike.
;
; What x86-64 does not have to think about is the data segments: a program
; may leave anything in DS and ES, so every way in loads the kernel's and
; every way out puts the program's back. FS and GS are the program's own -
; GS its thread pointer, through the GDT entry tls_descriptor rewrites - and
; the kernel never uses them; each way out loads them again from the frame,
; which is what picks up the entry as this thread has it.
;
; In a struct user_regs, i386 has no R8-R15: R8 holds the program's GS and
; R9 its FS. In a struct trap_regs R10 and R11 hold its DS and ES as well.

BITS 32

SECTION .text
GLOBAL user_enter, user_resume, user_exit, trap_stubs, page_fault_entry, int80_entry
GLOBAL user_frame, user_cs, user_ss, user_flags, kernel_ds, thread_switch
GLOBAL tick_entry, stray_entry, full_return, full_rcx, full_r11
EXTERN page_fault, trap_signal, tss, ia32_dispatch

; The TSS's ESP0: the kernel stack a way in from ring 3 lands on, and zero
; while no program runs - which tells a kernel fault from a program's.
%define kernel_rsp (tss + 4)

; The flags a program may set for itself: carry, parity, adjust, zero,
; sign, trap, direction, overflow, alignment check and ID. Not IOPL, not
; nested task, not virtual-8086 mode, which an IRET to ring 3 would enter.
FLAGS_KEPT equ 0x00240DD5

; struct user_regs, in 8-byte slots
U_RAX equ 0
U_ECX equ 1 * 8                         ; the padding slot: i386 keeps ECX
U_RBP equ 6 * 8
U_RBX equ 7 * 8
U_FS  equ 9 * 8                         ; R9
U_GS  equ 10 * 8                        ; R8
U_RDX equ 11 * 8
U_RSI equ 12 * 8
U_RDI equ 13 * 8
U_FLAGS equ 14 * 8
U_RIP equ 15 * 8
U_RSP equ 16 * 8
U_SIZE equ 17 * 8

; struct trap_regs
T_ES  equ 4 * 8                         ; R11
T_DS  equ 5 * 8                         ; R10
T_FS  equ 6 * 8                         ; R9
T_GS  equ 7 * 8                         ; R8
T_RBP equ 8 * 8
T_RDX equ 9 * 8
T_RCX equ 10 * 8
T_RBX equ 11 * 8
T_RAX equ 12 * 8
T_RSI equ 13 * 8
T_RDI equ 14 * 8
T_ERR equ 15 * 8
T_RIP equ 16 * 8
T_CS  equ 17 * 8
T_FLAGS equ 18 * 8
T_RSP equ 19 * 8
T_SS  equ 20 * 8
T_SIZE equ 21 * 8

; One 64-bit slot of a frame on the stack: the value in the low half.
%macro SLOT 2
    mov dword [esp + %1], %2
    mov dword [esp + %1 + 4], 0
%endmacro

; int user_enter(uint64_t entry, uint64_t stack)
; Starts a program in ring 3, and returns its exit code once it calls
; user_exit. The caller's ESP0 is kept with the registers and put back on
; the way out, as on x86-64.
user_enter:
    push ebx
    push esi
    push edi
    push ebp
    push dword [kernel_rsp]
    mov [kernel_rsp], esp
    mov ecx, [esp + 24]                 ; entry
    mov edx, [esp + 32]                 ; stack
    push dword [user_ss]                ; the frame IRETD returns through
    push edx
    push dword [user_flags]
    push dword [user_cs]
    push ecx
    mov eax, [user_ss]
    mov ds, ax
    mov es, ax
    xor eax, eax                        ; a clean machine, as on Linux
    mov fs, ax
    mov gs, ax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    iretd

; int user_resume(const struct user_regs *regs, uint64_t rax)
; A program that is already running - a fork's child, a new thread - from a
; saved frame, every register as it says.
user_resume:
    push ebx
    push esi
    push edi
    push ebp
    push dword [kernel_rsp]
    mov [kernel_rsp], esp
    mov esi, [esp + 24]                 ; the frame
    mov eax, [esp + 28]                 ; what the call gives back
    push dword [user_ss]
    push dword [esi + U_RSP]
    mov ecx, [esi + U_FLAGS]
    and ecx, FLAGS_KEPT
    or ecx, [user_flags]
    push ecx
    push dword [user_cs]
    push dword [esi + U_RIP]
    mov ecx, [esi + U_GS]
    mov gs, cx
    mov ecx, [esi + U_FS]
    mov fs, cx
    mov ecx, [user_ss]
    mov es, cx
    mov ecx, [esi + U_ECX]
    mov ebx, [esi + U_RBX]
    mov edx, [esi + U_RDX]
    mov edi, [esi + U_RDI]
    mov ebp, [esi + U_RBP]
    push dword [user_ss]
    mov esi, [esi + U_RSI]              ; last: it was holding the frame
    pop ds
    iretd

; noreturn void user_exit(int code)
; Abandons the program, and whatever the kernel was doing for it, and
; returns from user_enter or user_resume.
user_exit:
    mov eax, [esp + 4]
    mov esp, [kernel_rsp]
    pop dword [kernel_rsp]
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret

; void thread_switch(uint64_t *save, uint64_t to)
thread_switch:
    mov eax, [esp + 4]
    mov edx, [esp + 8]
    push ebx
    push esi
    push edi
    push ebp
    mov [eax], esp
    mov dword [eax + 4], 0
    mov esp, edx
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret

; int 0x80: the number in EAX, the arguments in EBX, ECX, EDX, ESI, EDI and
; EBP, the answer back in EAX and everything else kept. The frame is a
; struct user_regs, built below the processor's own and the program's EBP;
; ia32_dispatch reads the call out of it, and execve or a signal may change
; any of it, so the processor's frame is written again from it on the way
; out.
int80_entry:
    push ebp
    mov ebp, esp                        ; the processor's frame at EBP + 4
    push 0
    push dword [ebp + 16]               ; its stack
    push 0
    push dword [ebp + 4]                ; where it was
    push 0
    push dword [ebp + 12]               ; its flags
    push 0
    push edi
    push 0
    push esi
    push 0
    push edx
    sub esp, 3 * 8                      ; R8 (GS), R9 (FS), R10
    push 0
    push ebx
    push 0
    push dword [ebp]                    ; its EBP
    sub esp, 4 * 8                      ; R12-R15
    push 0
    push ecx
    push 0
    push eax
    xor eax, eax
    mov [esp + U_GS + 4], eax
    mov [esp + U_FS + 4], eax
    mov ax, gs
    mov [esp + U_GS], eax
    mov ax, fs
    mov [esp + U_FS], eax
    mov eax, [ss:kernel_ds]
    mov ds, ax
    mov es, ax
    cld
    mov [user_frame], esp
    call ia32_dispatch                  ; the answer in EAX
    mov [esp + U_RAX], eax
    jmp frame_return

; Back to ring 3 from the struct user_regs at ESP, with the processor's
; frame and the program's EBP above it: what int80_entry ends with.
frame_return:
    lea ebp, [esp + U_SIZE]             ; the EBP slot; the frame above it
    mov eax, [esp + U_RIP]
    mov [ebp + 4], eax
    mov eax, [user_cs]
    mov [ebp + 8], eax
    mov eax, [esp + U_FLAGS]
    and eax, FLAGS_KEPT
    or eax, [user_flags]
    mov [ebp + 12], eax
    mov eax, [esp + U_RSP]
    mov [ebp + 16], eax
    mov eax, [user_ss]
    mov [ebp + 20], eax
    mov eax, [esp + U_GS]
    mov gs, ax
    mov eax, [esp + U_FS]
    mov fs, ax
    mov eax, [user_ss]
    mov ds, ax
    mov es, ax
    mov eax, [esp + U_RAX]
    mov ecx, [esp + U_ECX]
    mov ebx, [esp + U_RBX]
    mov edx, [esp + U_RDX]
    mov esi, [esp + U_RSI]
    mov edi, [esp + U_RDI]
    mov ebp, [esp + U_RBP]
    add esp, U_SIZE + 4
    iretd

; Every CPU exception but the page fault: one stub per vector, all the same
; size. Each puts a zero where the processor pushed no error code, then the
; vector, and trap_common saves the rest as a struct trap_regs and asks
; trap_signal what to do.
TRAP_STRIDE equ 32

ALIGN 32
trap_stubs:
%assign vector 0
%rep 32
    ALIGN TRAP_STRIDE
%if vector == 8 || (vector >= 10 && vector <= 14) || vector == 17 || vector == 21 || vector == 29 || vector == 30
%else
    push 0
%endif
    push vector
    jmp trap_common
%assign vector vector + 1
%endrep

; The vector, the error code and the processor's frame are on the stack:
; this saves everything as a struct trap_regs below them (ESP, SS and the
; segments through SS, the one segment that is surely the kernel's), with
; EBP pointing at them: EBP + 4 the vector, + 8 the code, + 12 EIP.
%macro SAVE_TRAP 0
    push ebp
    mov ebp, esp
    sub esp, T_SIZE
    SLOT T_RAX, eax
    SLOT T_RBX, ebx
    SLOT T_RCX, ecx
    SLOT T_RDX, edx
    SLOT T_RSI, esi
    SLOT T_RDI, edi
    mov eax, [ebp]
    SLOT T_RBP, eax
    mov eax, [ebp + 8]
    SLOT T_ERR, eax
    mov eax, [ebp + 12]
    SLOT T_RIP, eax
    mov eax, [ebp + 16]
    SLOT T_CS, eax
    mov eax, [ebp + 20]
    SLOT T_FLAGS, eax
    test byte [ebp + 16], 3
    jz %%kernel
    mov eax, [ebp + 24]                 ; a program's: its stack is in the frame
    SLOT T_RSP, eax
    mov eax, [ebp + 28]
    SLOT T_SS, eax
    jmp %%saved
%%kernel:
    lea eax, [ebp + 24]                 ; the kernel's: where it was is here
    SLOT T_RSP, eax
    xor eax, eax
    mov ax, ss
    SLOT T_SS, eax
%%saved:
    xor eax, eax
    SLOT 0, eax                         ; R15-R12
    SLOT 8, eax
    SLOT 16, eax
    SLOT 24, eax
    mov ax, es
    SLOT T_ES, eax
    mov ax, ds
    SLOT T_DS, eax
    mov ax, fs
    SLOT T_FS, eax
    mov ax, gs
    SLOT T_GS, eax
    mov eax, [ss:kernel_ds]
    mov ds, ax
    mov es, ax
    cld
%endmacro

; And back, through the processor's frame rewritten from the saved one:
; a handler may have been set to run, with a stack of its own.
%macro RESTORE_TRAP 0
    mov eax, [esp + T_RIP]
    mov [ebp + 12], eax
    mov eax, [esp + T_FLAGS]
    test byte [ebp + 16], 3
    jz %%kernel
    and eax, FLAGS_KEPT
    or eax, [user_flags]
    mov [ebp + 20], eax
    mov eax, [esp + T_RSP]
    mov [ebp + 24], eax
    jmp %%back
%%kernel:
    mov [ebp + 20], eax
%%back:
    mov eax, [esp + T_GS]
    mov gs, ax
    mov eax, [esp + T_FS]
    mov fs, ax
    mov eax, [esp + T_ES]
    mov es, ax
    mov eax, [esp + T_DS]
    mov ds, ax
    mov eax, [esp + T_RAX]
    mov ebx, [esp + T_RBX]
    mov ecx, [esp + T_RCX]
    mov edx, [esp + T_RDX]
    mov esi, [esp + T_RSI]
    mov edi, [esp + T_RDI]
    mov ebp, [esp + T_RBP]
    add esp, T_SIZE + 12                ; the frame, EBP, the vector, the code
    iretd
%endmacro

trap_common:
    SAVE_TRAP
    ; With no program running the fault is the kernel's own, and there is
    ; nothing to go back to: stopping leaves the screen to be read.
    cmp dword [kernel_rsp], 0
    je .stop
    mov eax, esp
    push eax                            ; the registers
    push dword [ebp + 4]                ; the vector
    call trap_signal                    ; returns only to run a handler
    add esp, 8
    RESTORE_TRAP
.stop:
    cli
    hlt
    jmp .stop

; Interrupts, which only arrive while a program runs.
tick_entry:
    push 0
    push 0x101
    jmp trap_common

stray_entry:
    push 0
    push 0x100
    jmp trap_common

; A page fault: page_fault(address, registers), which maps the page in or
; hands the program its SIGSEGV - from the kernel's own code too, reaching
; into a program's memory.
page_fault_entry:
    push 14
    SAVE_TRAP
    mov eax, esp
    push eax
    push 0                              ; the address, a uint64_t
    mov eax, cr2
    push eax
    call page_fault
    add esp, 12
    RESTORE_TRAP

SECTION .data
; A program's EFLAGS: interrupts off from the start, as for a BIOS. A 32-bit
; firmware's interrupt handlers are written for ring 0 only - one that
; arrives in ring 3 goes back with the firmware's own code segment and a
; firmware address - so they run only while the kernel calls into it
; (efi_call.asm), and programs get interrupts once sched/timer gives the
; kernel's own (tick_register).
user_flags: dq 0x002

SECTION .bss
full_rcx:   resq 1                      ; x86-64's: rt_sigreturn there only
full_r11:   resq 1
full_return: resb 1
alignb 8
user_frame: resd 2
user_cs:    resq 1                      ; ring 3's selectors, set by gdt_init
user_ss:    resq 1
kernel_ds:  resq 1                      ; and the kernel's data segment
