[BITS 64]
global _start

section .text
_start:
    mov eax, 19
    int 0xA5

    mov eax, 259
    int 0xA5
    test eax, 1
    jz .skip_clear

    mov eax, 64
    int 0xA5
.skip_clear:

    lea rbx, [rel shell_path]
    xor rcx, rcx
    xor rdx, rdx
    mov eax, 9
    int 0xA5

.hang:
    jmp .hang

section .rodata
shell_path db "/cmd/login", 0
