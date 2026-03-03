[bits 32]
global _start

SYS_READ  equ 2
SYS_WRITE equ 3
SYS_EXIT  equ 1

section .data
msg db "input: ",0
msg_len equ $-msg

section .bss
buf resb 64

section .text

; write(fd, buf, len)
write:
    mov eax, SYS_WRITE
    int 0xA5
    iret

; read(fd, buf, len)
read:
    mov eax, SYS_READ
    int 0xA5
    iret

_start:

    ; print prompt
    mov ebx, 1          ; stdout
    mov ecx, msg
    mov edx, msg_len
    call write

    ; read stdin
    mov ebx, 0          ; stdin
    mov ecx, buf
    mov edx, 64
    call read
    mov esi, eax        ; bytes read 저장

    ; echo back
    mov ebx, 1
    mov ecx, buf
    mov edx, esi
    call write

    ; exit
    mov eax, SYS_EXIT
    xor ebx, ebx
    int 0xA5
