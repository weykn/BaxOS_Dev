; The Tuxlet OS boot sector, for a PC that starts from a BIOS.
;
; The disk is GPT, and this is the code half of its protective MBR: the
; BIOS runs the first 440 bytes of sector 0 whatever the partition table
; after them says. All it does is read the BIOS loader - src/boot/bios.asm,
; which carries the kernel - from the BIOS boot partition to 0x8000 and run
; it, with DL still the drive. Where that partition is, the build writes
; into BLOB_LBA and BLOB_SECTORS as it assembles this, so nothing here
; reads the GPT.

BITS 16
ORG 0x7C00

LOADER equ 0x8000
CHUNK  equ 64                       ; sectors a read: 32 KiB, which any BIOS takes

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld
    jmp 0:.flat                     ; some BIOSes enter at 07C0:0000
.flat:
    mov [dap_drive], dl

    mov ah, 0x41                    ; extended reads, by LBA
    mov bx, 0x55AA
    int 0x13
    jc fail
    cmp bx, 0xAA55
    jne fail

.next:
    mov cx, [blob_sectors]
    jcxz .done
    cmp cx, CHUNK
    jbe .count
    mov cx, CHUNK
.count:
    mov [dap_count], cx
    mov si, dap
    mov dl, [dap_drive]
    mov ah, 0x42
    int 0x13
    jc fail
    mov cx, [dap_count]
    sub [blob_sectors], cx
    add [dap_lba], ecx              ; the high half of cx is zero: jcxz above
    shl cx, 5                       ; sectors to paragraphs
    add [dap_seg], cx
    jmp .next
.done:
    mov dl, [dap_drive]
    jmp 0:LOADER

fail:
    mov si, msg
.say:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0E
    xor bx, bx
    int 0x10
    jmp .say
.halt:
    hlt
    jmp .halt

dap:        db 0x10, 0
dap_count:  dw 0
            dw 0                    ; offset
dap_seg:    dw LOADER >> 4
dap_lba:    dq BLOB_LBA
dap_drive:  db 0
msg:        db "Tuxlet OS: disk error", 0

; Given by the build: how many sectors the loader is (its first is dap_lba's).
blob_sectors: dw BLOB_SECTORS
times 440 - ($ - $$) db 0
