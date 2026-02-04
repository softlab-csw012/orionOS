; OrionOS MBR bootloader (stage 1)
; - Loads stage2 from LBA 1 into 0x0000:0x8000
; - Reads partition #1 start LBA from the existing partition table
; - Stores boot params at 0x0000:0x0500
;
; NOTE: This file is assembled to exactly 446 bytes and must be written
;       into the first 446 bytes of LBA0 (keep the partition table + 0x55AA).

[BITS 16]
[ORG 0x7C00]

%define BOOT_DRIVE_OFF 0x0500
%define PART_LBA_OFF   0x0504

%define STAGE2_LBA      1
%define STAGE2_SECTORS  64
%define STAGE2_SEG      0x0000
%define STAGE2_OFF      0x8000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    ; Save BIOS boot drive
    mov [BOOT_DRIVE_OFF], dl

    ; Partition #1 start LBA (MBR entry #0, LBA at +8)
    mov si, 0x7C00 + 0x1BE + 8
    mov eax, [si]
    mov [PART_LBA_OFF], eax

    ; Disk Address Packet at 0x0600
    mov si, 0x0600
    mov byte [si+0], 0x10       ; size
    mov byte [si+1], 0x00
    mov word [si+2], STAGE2_SECTORS
    mov word [si+4], STAGE2_OFF
    mov word [si+6], STAGE2_SEG
    mov dword [si+8], STAGE2_LBA
    mov dword [si+12], 0

    mov ah, 0x42                ; INT 13h Extensions - Extended Read
    int 0x13
    jc disk_error

    mov dl, [BOOT_DRIVE_OFF]
    jmp STAGE2_SEG:STAGE2_OFF

disk_error:
    cli
.hang:
    hlt
    jmp .hang

times 446-($-$$) db 0
