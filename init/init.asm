[BITS 32]
global _start

section .text
_start:
    jmp short .getbase
.base:
    pop ebp
    jmp .realstart

.getbase:
    call .base

.realstart:
    mov eax, 19 ; start orion-sysmgr
    int 0xA5

    mov eax, 259 ; get boot flags
    int 0xA5
    test eax, 1
    jz .skip_clear

    mov eax, 64 ; clear screen
    int 0xA5
.skip_clear:

    lea ebx, [ebp + shell_path - .base]
    xor ecx, ecx
    xor edx, edx
    mov eax, 9
    int 0xA5

.hang:
    jmp .hang

section .rodata
shell_path db "/cmd/login", 0
