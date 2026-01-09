[BITS 32]
ORG 0x500000
global _start

section .text
_start:
    mov eax, 16 ; start orion-sysmgr
    int 0xA5

    mov eax, 1
    int 0xA5

    mov eax, 11
    int 0xA5
    test eax, 1
    jz .skip_clear

    mov eax, 3
    int 0xA5
.skip_clear:

    mov ebx, motd_path
    mov eax, 17
    int 0xA5
    
    mov eax, 8  ;exit
    int 0xA5

section .rodata
motd_path db "/system/config/motd.txt", 0
