;─────────────────────────────────────────────
; kernel_entry.asm — Limine 64-bit 커널 진입점 (+ BSS 초기화)
;─────────────────────────────────────────────
[bits 64]
[extern kernel_main]

; BSS 경계 심볼 (링커 스크립트에서 제공됨)
extern __bss_start
extern __bss_end

section .limine_requests_start
align 8
    dq 0xf6b8f4b39de7d1ae
    dq 0xfab91a6940fcb9cf
    dq 0x785c6ed015d3e316
    dq 0x181e920a7852b9d9

section .limine_requests
align 8
base_revision:
    dq 0xf9562b2d5c95a6c8
    dq 0x6a7b384944536bdc
    dq 5

memmap_request:
    dq 0xc7b1dd30df4c8b88
    dq 0x0a82e883a194f07b
    dq 0x67cf3d9d378a806f
    dq 0xe304acdfc50c3c62
    dq 0
global limine_memmap_response
limine_memmap_response:
    dq 0

module_request:
    dq 0xc7b1dd30df4c8b88
    dq 0x0a82e883a194f07b
    dq 0x3e7e279702be32af
    dq 0xca1c4f3bd1280cee
    dq 0
global limine_module_response
limine_module_response:
    dq 0
    dq 0
    dq 0

executable_address_request:
    dq 0xc7b1dd30df4c8b88
    dq 0x0a82e883a194f07b
    dq 0x71ba76863cc55f63
    dq 0xb2644a48c516a487
    dq 0
global limine_executable_address_response
limine_executable_address_response:
    dq 0

framebuffer_request:
    dq 0xc7b1dd30df4c8b88
    dq 0x0a82e883a194f07b
    dq 0x9d5827dcd881dd75
    dq 0xa3148604f6fab11b
    dq 0
global limine_framebuffer_response
limine_framebuffer_response:
    dq 0

section .limine_requests_end
align 8
    dq 0xadc0e0531bb10d03
    dq 0x9572709f31764c62

;─────────────────────────────────────────────
; 코드 시작
;─────────────────────────────────────────────
section .text
align 16
global _start

_start:
    cli
    mov dx, 0x00e9
    mov al, 'A'
    out dx, al

    ; Stage 0: entered kernel image
    mov rax, [rel limine_framebuffer_response]
    test rax, rax
    jz .no_fb_stage0
    mov rax, [rax + 16]
    test rax, rax
    jz .no_fb_stage0
    mov rax, [rax]
    test rax, rax
    jz .no_fb_stage0
    mov rdi, [rax + 0]
    test rdi, rdi
    jz .no_fb_stage0
    mov ecx, 128
    mov eax, 0x00ff0000
.stage0_fill:
    mov dword [rdi], eax
    add rdi, 4
    loop .stage0_fill
.no_fb_stage0:
    mov dx, 0x00e9
    mov al, 'B'
    out dx, al

    ; BSS 초기화
    lea rdi, [rel __bss_start]
    lea rcx, [rel __bss_end]
    sub rcx, rdi
    shr rcx, 3
    xor rax, rax
    rep stosq
    mov dx, 0x00e9
    mov al, 'C'
    out dx, al

    ; 커널 스택
    lea rsp, [rel stack_top]
    mov dx, 0x00e9
    mov al, 'D'
    out dx, al

    xor edi, edi
    xor esi, esi
    call kernel_main

.hang:
    hlt
    jmp .hang

;─────────────────────────────────────────────
; 스택 + BSS 공간
;─────────────────────────────────────────────
section .bss
align 16
global stack_bottom
stack_bottom:
    resb 32768            ; 32KB 스택
global stack_top
stack_top:
