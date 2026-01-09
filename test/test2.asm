[bits 32]
[org 0x500000]
global start

start:
    mov eax, 2
    mov ebx, test
    int 0xA5

    ;mov eax, 5 
    ;int 0xA5

    mov eax, 4
    mov ebx, 600
    mov ecx, 30000
    int 0xA5
    
    hlt
    ;mov eax, 8
    ;int 0xA5

;loop:
;    jmp loop

test: db "123", 10, 0