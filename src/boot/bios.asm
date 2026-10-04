; The Tuxlet OS BIOS loader.
;
; The boot sector (mbr.asm) reads this to 0x8000 and runs it in real mode
; with DL the boot drive. It asks the BIOS what only the BIOS knows - the
; memory map and a linear framebuffer - maps the first 4 GiB one to one,
; goes to long mode and enters the kernel, which is built into the end of
; this file (incbin below) and runs where it lies.
;
; It stays: bios_call is how the kernel gets back into real mode to ask the
; BIOS for a sector or a key, until modules drive the disk and the
; keyboard. That is the BIOS counterpart of UEFI's boot services.
;
; Low memory, while the kernel runs:
;
;   0x00000  the BIOS's own: interrupt vectors, its data area
;   0x00500  the E820 map
;   0x00B00  the boot information, then bios_regs
;   0x01000  page tables, four directories of 2 MiB pages (the kernel
;            builds its own and gives these back)
;   0x07000  the real-mode stack, up to 0x7C00
;   0x08000  this loader, its IDT, then the kernel's KERNEL_BYTES
;   0x60000  64 KiB for INT 13h to read into
;
; Nothing of it is used once the kernel has let the BIOS go.

BITS 16
ORG 0x8000
DEFAULT ABS

KERNEL_BYTES equ 0x20000            ; boot.h
BOOT_MAGIC   equ 0x536F7861426     ; boot.h

E820       equ 0x0500
E820_MAX   equ 64
INFO       equ 0x0B00
REGS       equ 0x0C00
PML4       equ 0x1000
PDPT       equ 0x2000
PD         equ 0x3000               ; four of them, 0x3000-0x6FFF
STACK      equ 0x7C00
BUFFER     equ 0x60000
VBE_INFO   equ 0x1000               ; scratch, before the tables are built
MODE_INFO  equ 0x1200

; struct boot_info, as boot.h lays it out.
I_MAGIC    equ 0
I_SYSTEM   equ 8
I_FB       equ 24
I_WIDTH    equ 32
I_HEIGHT   equ 36
I_PITCH    equ 40
I_FORMAT   equ 44
I_STARTED  equ 88
I_E820     equ 96
I_COUNT    equ 104
I_DRIVE    equ 108
I_CALL     equ 112
I_REGS     equ 120
I_BUFFER   equ 128
I_SIZE     equ 136

; struct bios_regs
R_EAX equ 0
R_EBX equ 4
R_ECX equ 8
R_EDX equ 12
R_ESI equ 16
R_EDI equ 20
R_EBP equ 24
R_DS  equ 28
R_ES  equ 30
R_FLAGS equ 32
R_VECTOR equ 34

CODE64 equ 0x08
DATA   equ 0x10
CODE32 equ 0x18
CODE16 equ 0x20
DATA16 equ 0x28

start:
    mov [drive], dl                     ; before rdtsc takes EDX
    rdtsc                               ; uptime counts from here
    mov [started], eax
    mov [started + 4], edx
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, STACK
    cld

    ; ---- A20 ---------------------------------------------------------------
    mov ax, 0x2401
    int 0x15
    in al, 0x92                         ; and the fast way, for a BIOS without it
    test al, 2
    jnz .a20
    or al, 2
    and al, 0xFE
    out 0x92, al
.a20:

    ; ---- the memory map ----------------------------------------------------
    xor ebx, ebx
    mov di, E820
.e820:
    mov dword [di + 20], 1              ; ACPI 3 attributes: valid
    mov eax, 0xE820
    mov ecx, 24
    mov edx, 0x534D4150                 ; "SMAP"
    int 0x15
    jc .e820_done
    cmp eax, 0x534D4150
    jne .e820_done
    jcxz .e820_skip
    cmp dword [di + 8], 0               ; empty
    jne .e820_keep
    cmp dword [di + 12], 0
    je .e820_skip
.e820_keep:
    inc word [e820_count]
    add di, 24
    cmp word [e820_count], E820_MAX
    jae .e820_done
.e820_skip:
    test ebx, ebx
    jnz .e820
.e820_done:
    cmp word [e820_count], 0
    jne .vbe
    mov si, msg_memory
    jmp fail

    ; ---- the screen --------------------------------------------------------
    ; Of the modes the card offers with a linear framebuffer at 32 bits a
    ; pixel, the largest no wider than 1920.
.vbe:
    mov dword [VBE_INFO], "VBE2"
    mov ax, 0x4F00
    mov di, VBE_INFO
    int 0x10
    cmp ax, 0x004F
    jne .no_screen
    mov si, [VBE_INFO + 14]             ; the mode list, a far pointer
    mov fs, [VBE_INFO + 16]
.mode:
    mov cx, [fs:si]
    add si, 2
    cmp cx, 0xFFFF
    je .chosen
    push si
    push fs
    push cx
    mov ax, 0x4F01
    mov di, MODE_INFO
    int 0x10
    pop cx
    pop fs
    pop si
    cmp ax, 0x004F
    jne .mode
    mov ax, [MODE_INFO]                 ; attributes
    and ax, 0x91                        ; supported, graphics, linear
    cmp ax, 0x91
    jne .mode
    cmp byte [MODE_INFO + 25], 32       ; bits a pixel
    jne .mode
    cmp byte [MODE_INFO + 27], 6        ; direct colour
    jne .mode
    mov ax, [MODE_INFO + 18]            ; width
    cmp ax, 1920
    ja .mode
    movzx eax, ax
    movzx edx, word [MODE_INFO + 20]
    imul eax, edx
    cmp eax, [best_area]
    jbe .mode
    mov [best_area], eax
    mov [best_mode], cx
    mov ax, [MODE_INFO + 18]
    mov [info + I_WIDTH], ax
    mov ax, [MODE_INFO + 20]
    mov [info + I_HEIGHT], ax
    mov ax, [MODE_INFO + 16]            ; bytes a scan line
    cmp word [VBE_INFO + 4], 0x0300
    jb .pitch
    mov ax, [MODE_INFO + 50]            ; VBE 3 has it for linear modes apart
.pitch:
    mov [info + I_PITCH], ax
    mov eax, [MODE_INFO + 40]
    mov [info + I_FB], eax
    xor eax, eax                        ; red in the low byte: RGBX
    cmp byte [MODE_INFO + 32], 16
    jne .format
    inc eax                             ; red third: BGRX
.format:
    mov [info + I_FORMAT], eax
    jmp .mode
.chosen:
    mov bx, [best_mode]
    test bx, bx
    jz .no_screen
    or bx, 0x4000                       ; the linear framebuffer
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    je .tables
.no_screen:
    mov si, msg_screen
    jmp fail

    ; ---- page tables: 4 GiB one to one -------------------------------------
.tables:
    mov di, PML4
    mov cx, (PD + 4 * 4096 - PML4) / 4
    xor eax, eax
    rep stosd
    mov dword [PML4], PDPT | 3
    mov dword [PDPT], PD | 3
    mov dword [PDPT + 8], (PD + 0x1000) | 3
    mov dword [PDPT + 16], (PD + 0x2000) | 3
    mov dword [PDPT + 24], (PD + 0x3000) | 3
    mov di, PD
    mov eax, 0x83                       ; present, writable, 2 MiB
    xor edx, edx
.pd:
    mov [di], eax
    mov [di + 4], edx
    add eax, 0x200000
    adc edx, 0
    add di, 8
    cmp di, PD + 4 * 4096
    jb .pd

    ; ---- the boot information ---------------------------------------------
    mov eax, BOOT_MAGIC & 0xFFFFFFFF
    mov [info + I_MAGIC], eax
    mov dword [info + I_MAGIC + 4], BOOT_MAGIC >> 32
    mov eax, [started]
    mov [info + I_STARTED], eax
    mov eax, [started + 4]
    mov [info + I_STARTED + 4], eax
    mov dword [info + I_E820], E820
    mov ax, [e820_count]
    mov [info + I_COUNT], ax
    mov al, [drive]
    mov [info + I_DRIVE], al
    mov dword [info + I_CALL], bios_call
    mov dword [info + I_REGS], REGS
    mov dword [info + I_BUFFER], BUFFER
    mov di, INFO                        ; where the kernel is told to look
    mov si, info
    mov cx, I_SIZE
    rep movsb

    ; ---- long mode ---------------------------------------------------------
    cli
    lgdt [gdt_desc]
    mov eax, 1 << 5                     ; PAE
    mov cr4, eax
    mov eax, PML4
    mov cr3, eax
    mov ecx, 0xC0000080                 ; EFER
    rdmsr
    or eax, 1 << 8                      ; LME
    wrmsr
    mov eax, cr0
    or eax, 0x80000001                  ; PG, PE
    mov cr0, eax
    jmp CODE64:enter

fail:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0E
    xor bx, bx
    int 0x10
    jmp fail
.halt:
    hlt
    jmp .halt

BITS 64
enter:
    mov ax, DATA
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax
    lidt [idt_desc]                     ; empty: the kernel fills the traps in
    mov rsp, STACK
    ; The kernel's .bss, which the image does not carry.
    mov rdi, kernel_end
    mov rcx, kernel + KERNEL_BYTES
    sub rcx, rdi
    xor eax, eax
    rep stosb
    mov rdi, INFO
    jmp kernel

; ---- bios_call ---------------------------------------------------------------
;
; Called by the kernel (SysV, no arguments) to run INT bios_regs.vector with
; the registers in bios_regs, in real mode, and back. Everything long mode
; had is put back as it was: the tables, the segments, EFER and the FS and
; GS bases a program may be using. Interrupts are on only for the INT
; itself - the BIOS needs its timer and its disk and keyboard interrupts -
; and the kernel's IDT is out of the way while they are.

bios_call:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov [saved_rsp], rsp
    sgdt [saved_gdt]
    sidt [saved_idt]
    mov rax, cr0
    mov [saved_cr0], rax
    mov rax, cr3
    mov [saved_cr3], rax
    mov rax, cr4
    mov [saved_cr4], rax
    mov ecx, 0xC0000080
    rdmsr
    mov [saved_efer], eax
    mov [saved_efer + 4], edx
    mov ecx, 0xC0000100                 ; FS base
    rdmsr
    mov [saved_fs], eax
    mov [saved_fs + 4], edx
    mov ecx, 0xC0000101                 ; GS base
    rdmsr
    mov [saved_gs], eax
    mov [saved_gs + 4], edx
    mov ecx, 0xC0000102                 ; the other GS base, swapgs's
    rdmsr
    mov [saved_kgs], eax
    mov [saved_kgs + 4], edx
    mov ax, cs
    mov [saved_cs], ax
    mov ax, ds
    mov [saved_ds], ax
    mov ax, es
    mov [saved_es], ax
    mov ax, ss
    mov [saved_ss], ax

    lgdt [gdt_desc]
    push CODE32
    lea rax, [.compat]
    push rax
    retfq
BITS 32
.compat:
    mov ax, DATA
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, cr0
    and eax, 0x7FFEFFFF                 ; paging, and write protection, off
    mov cr0, eax
    mov ecx, 0xC0000080
    rdmsr
    and eax, ~(1 << 8)                  ; and long mode with it
    wrmsr
    jmp CODE16:.prot16
BITS 16
.prot16:
    mov ax, DATA16
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov eax, cr0
    and eax, ~1
    mov cr0, eax
    jmp 0:.real
.real:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov sp, STACK
    lidt [ivt_desc]
    mov al, [REGS + R_VECTOR]
    mov [.int + 1], al
    jmp .loaded                         ; past the modified byte
.loaded:
    mov eax, [REGS + R_EAX]
    mov ebx, [REGS + R_EBX]
    mov ecx, [REGS + R_ECX]
    mov edx, [REGS + R_EDX]
    mov esi, [REGS + R_ESI]
    mov edi, [REGS + R_EDI]
    mov ebp, [REGS + R_EBP]
    push word [REGS + R_DS]
    mov es, [REGS + R_ES]
    pop ds
    sti
.int:
    int 0x13
    cli
    pushf
    push ds
    push word 0
    pop ds
    mov [REGS + R_EAX], eax
    mov [REGS + R_EBX], ebx
    mov [REGS + R_ECX], ecx
    mov [REGS + R_EDX], edx
    mov [REGS + R_ESI], esi
    mov [REGS + R_EDI], edi
    mov [REGS + R_EBP], ebp
    mov [REGS + R_ES], es
    pop word [REGS + R_DS]
    pop word [REGS + R_FLAGS]
    cld

    o32 lgdt [gdt_desc]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword CODE32:.prot32
BITS 32
.prot32:
    mov ax, DATA
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, [saved_cr4]
    mov cr4, eax
    mov eax, [saved_cr3]
    mov cr3, eax
    mov ecx, 0xC0000080
    mov eax, [saved_efer]
    mov edx, [saved_efer + 4]
    wrmsr
    mov eax, [saved_cr0]
    mov cr0, eax
    jmp CODE64:.long
BITS 64
.long:
    lgdt [saved_gdt]
    lidt [saved_idt]
    mov ax, [saved_ds]
    mov ds, ax
    mov ax, [saved_es]
    mov es, ax
    mov ax, [saved_ss]
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax
    mov ecx, 0xC0000100
    mov eax, [saved_fs]
    mov edx, [saved_fs + 4]
    wrmsr
    mov ecx, 0xC0000101
    mov eax, [saved_gs]
    mov edx, [saved_gs + 4]
    wrmsr
    mov ecx, 0xC0000102
    mov eax, [saved_kgs]
    mov edx, [saved_kgs + 4]
    wrmsr
    mov rsp, [saved_rsp]
    movzx eax, word [saved_cs]
    push rax
    lea rax, [.back]
    push rax
    retfq
.back:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

; ---------------------------------------------------------------------------

align 8
gdt:
    dq 0
    dq 0x00AF9A000000FFFF               ; 0x08: 64-bit code
    dq 0x00CF92000000FFFF               ; 0x10: flat data
    dq 0x00CF9A000000FFFF               ; 0x18: 32-bit code
    dq 0x00009A000000FFFF               ; 0x20: 16-bit code
    dq 0x000092000000FFFF               ; 0x28: 16-bit data
gdt_desc:
    dw gdt_desc - gdt - 1
    dq gdt
idt_desc:
    dw 4096 - 1
    dq idt
ivt_desc:
    dw 0x3FF
    dq 0

started:    dq 0
best_area:  dd 0
best_mode:  dw 0
e820_count: dw 0
drive:      db 0

align 8
saved_rsp:  dq 0
saved_cr0:  dq 0
saved_cr3:  dq 0
saved_cr4:  dq 0
saved_efer: dq 0
saved_fs:   dq 0
saved_gs:   dq 0
saved_kgs:  dq 0
saved_gdt:  dw 0
            dq 0
saved_idt:  dw 0
            dq 0
saved_cs:   dw 0
saved_ds:   dw 0
saved_es:   dw 0
saved_ss:   dw 0

info:       times I_SIZE db 0

msg_memory: db "Tuxlet OS: no memory map", 0
msg_screen: db "Tuxlet OS: no 32-bit VBE mode", 0

align 4096
idt:        times 4096 db 0
kernel:
    incbin KERNEL_BIN
kernel_end:
