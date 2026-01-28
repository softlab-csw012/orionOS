;─────────────────────────────────────────────
; kernel_entry.asm — Limine 64-bit 커널 진입점
;─────────────────────────────────────────────
[bits 64]
default rel
extern kernel_main

extern __bss_start
extern __bss_end

section .text
align 16
global _start

_start:
    cli

    ; BSS 초기화
    mov rdi, __bss_start
    mov rcx, __bss_end
    sub rcx, rdi
    xor rax, rax
    rep stosb

    ; 스택 설정
    mov rsp, stack_top
    and rsp, -16

    call kernel_main

.hang:
    hlt
    jmp .hang

section .bss
align 16
stack_bottom:
    resb 65536           ; 64KB 스택
stack_top:
