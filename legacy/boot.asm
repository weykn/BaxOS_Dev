; BaxOS boot sector.
;
; The BIOS loads this sector to 0x7C00 in 16-bit real mode with DL = the boot
; drive. We read the file table, load kernel.bin from it to 0x10000, and jump
; from real mode straight into 64-bit long mode to run it.
;
; Low memory once the kernel runs, all of it below the kernel free for reuse
; by then:
;
;   0x00500  TSS and IDT, over the file table (kernel.ld, syscall.c)
;   0x00800  where the VGA BIOS keeps its fonts (vga.c)
;   0x01000  PML4, PDPT, PD, and the program window's page table
;   0x05000  the framebuffer's page directory (kernel.ld, vbe.c)
;   0x06000  free, up to the kernel - where programs are given their RAM
;   0x10000  kernel.bin and its .bss, then free again up to the top of
;            conventional memory, and past the BIOS's hole from 1 MiB up
;
; Two things are mapped: the first 2 MiB, identity-mapped for the kernel
; alone, and the 2 MiB program window at 0x400000, whose page table starts
; out empty - the kernel fills it a page at a time from the free RAM.

BITS 16
ORG 0x7C00

KERNEL     equ 0x10000              ; kernel.bin is loaded to and entered here
TABLE      equ 0x0500               ; the file table is read to here

; The file table, as laid out by src/kernel/fs.c - keep the two in step: one
; sector holding an 8-byte header, then 24-byte entries of
; { name[20], u16 start sector, u16 size in bytes }.
HEADER_SIZE equ 8
ENTRY_SIZE  equ 24
ENTRY_START equ 20
ENTRY_BYTES equ 22

PML4   equ 0x1000                   ; page tables, in free memory below the kernel
PDPT   equ 0x2000
PD     equ 0x3000
PT     equ 0x4000                   ; the window's; kernel.ld's program_pt
WINDOW equ 0x400000                 ; PROGRAM_BASE in syscall.h
FONTS  equ 0x0800                   ; kernel.ld's vga_fonts

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    cld

    ; ---- read the file table and find kernel.bin in it -------------------
    call read                               ; the DAP starts out aimed at the table
    mov di, TABLE + HEADER_SIZE             ; the first entry
.find:
    mov si, kernel_name
    mov cx, KERNEL_NAME_LEN
    push di
    repe cmpsb
    pop di
    je .found
    add di, ENTRY_SIZE
    cmp di, TABLE + 512
    jb .find
    mov si, msg_no_kernel
    jmp fail

.found:
    mov ax, [di + ENTRY_START]
    mov [dap_lba], ax
    mov ax, [di + ENTRY_BYTES]              ; mkfs caps this at 127 sectors, the
    add ax, 511                             ; most one BIOS read can move
    shr ax, 9
    mov [dap_count], ax
    mov word [dap_seg], KERNEL >> 4
    call read

    ; ---- fonts ------------------------------------------------------------
    ; Only the VGA BIOS knows where its fonts are, and vga.c draws every
    ; character from them: far pointers to 8x14, both halves of 8x8, and 8x16
    ; go to FONTS. This clobbers DL, so it has to come after the last read.
    mov si, font_ids
    mov di, FONTS
.font:
    lodsb
    mov bh, al
    mov ax, 0x1130
    push si
    push di
    int 0x10                                ; ES:BP = the font
    pop di
    pop si
    mov [di], bp
    mov [di + 2], es
    add di, 4
    cmp si, font_ids + 4
    jb .font
    push ds                                 ; ES back to 0 for the page tables
    pop es

    ; ---- A20 ------------------------------------------------------------
    ; Without it address line 20 reads as zero, so every second megabyte is a
    ; mirror of the one below it and RAM past 1 MiB cannot be used.
    mov ax, 0x2401
    int 0x15

    ; ---- page tables ------------------------------------------------------
    mov di, PML4
    mov cx, 4 * 4096 / 4
    xor eax, eax
    rep stosd
    ; Ring 3 has to be let through at every level for the window to work;
    ; the kernel's own 2 MiB page is where it is kept out.
    mov word [PML4], PDPT | 0x07                ; present | writable | user
    mov word [PDPT], PD | 0x07
    mov byte [PD], 0x83                         ; present | writable | 2 MiB page
    mov word [PD + (WINDOW >> 21) * 8], PT | 0x07

    ; ---- real mode -> long mode in one step ------------------------------
    cli
    lgdt [gdt_desc]
    mov eax, 1 << 5                         ; CR4.PAE
    mov cr4, eax
    mov eax, PML4
    mov cr3, eax
    mov ecx, 0xC0000080                     ; IA32_EFER
    rdmsr
    or ax, 1 << 8                           ; EFER.LME
    wrmsr
    mov eax, cr0
    or eax, 0x80000001                      ; CR0.PG | CR0.PE
    mov cr0, eax
    jmp dword 0x08:KERNEL

; Reads the sectors the DAP describes; never returns on failure.
read:
    mov si, dap
    mov ah, 0x42                            ; extended read, DL = boot drive
    int 0x13
    mov si, msg_disk
    jc fail
    ret

fail:                                       ; prints the string at SI and halts
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0E                            ; BIOS teletype
    int 0x10
    jmp fail
.halt:
    hlt
    jmp .halt

; ---------------------------------------------------------------------------
align 8
gdt:
    dq 0                                ; null
    dq 0x00AF9A000000FFFF               ; 0x08: 64-bit code
gdt_desc:
    dw gdt_desc - gdt - 1
    dd gdt

dap:
    db 0x10, 0                          ; DAP size
dap_count: dw 1
           dw 0                         ; buffer offset
dap_seg:   dw TABLE >> 4
dap_lba:   dq 1                         ; the file table, right after this sector

font_ids:      db 2, 3, 4, 6            ; 8x14, 8x8, 8x8's top half, 8x16
kernel_name:   db "kernel.bin", 0
KERNEL_NAME_LEN equ $ - kernel_name
msg_disk:      db "disk error", 0
msg_no_kernel: db "no kernel.bin", 0

times 510 - ($ - $$) db 0
dw 0xAA55
