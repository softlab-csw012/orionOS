[bits 32]
[org 0x500000]

start:
    mov eax, 12
    mov ebx, con
    int 0xA5

    mov [fd], eax

    mov eax, 14
    mov ebx, [fd]
    mov edx, msg
    mov ecx, len
    int 0xA5

    mov eax, 15
    mov ebx, [fd]
    int 0xA5

loop:
    jmp loop

;mov eax, 8
;mov ebx, 0
;int 0xA5

con: db "console", 0

msg: db "Hello, world!", 10
len equ $ - msg

fd: dd 0