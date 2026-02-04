;─────────────────────────────────────────────
; kernel_entry.asm — BOOTX Multiboot2 커널 진입점 (+ BSS 초기화)
;─────────────────────────────────────────────
[bits 32]
[extern kernel_main]

; BSS 경계 심볼 (링커 스크립트에서 제공됨)
extern __bss_start
extern __bss_end

section .multiboot_header
align 8
    MULTIBOOT2_HEADER_MAGIC  equ 0xe85250d6
    MULTIBOOT2_ARCHITECTURE  equ 0
    MULTIBOOT2_HEADER_LENGTH equ header_end - header_start
    MULTIBOOT2_CHECKSUM      equ -(MULTIBOOT2_HEADER_MAGIC + MULTIBOOT2_ARCHITECTURE + MULTIBOOT2_HEADER_LENGTH)

header_start:
    dd MULTIBOOT2_HEADER_MAGIC
    dd MULTIBOOT2_ARCHITECTURE
    dd MULTIBOOT2_HEADER_LENGTH
    dd MULTIBOOT2_CHECKSUM
    
    ; Framebuffer request tag (VBE)
    align 8
framebuffer_tag:
    dw 5                      ; type = framebuffer
    dw 0                      ; flags
    dd framebuffer_tag_end - framebuffer_tag
    dd 0                      ; width (no preference)
    dd 0                      ; height (no preference)
    dd 0                      ; bpp (no preference)
framebuffer_tag_end:

    ; 필수 end 태그
    align 8
    dw 0
    dw 0
    dd 8
header_end:

;─────────────────────────────────────────────
; 코드 시작
;─────────────────────────────────────────────
section .text
align 16
global _start

_start:
    cli

    ; multiboot 인자 백업
    mov esp, eax        ; magic

    ; BSS 초기화
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    shr ecx, 2
    xor eax, eax
    rep stosd

    ; 커널 스택
    mov esp, stack_top

    ; multiboot 인자 복원
    mov eax, esp

    push ebx
    push eax
    call kernel_main

.hang:
    hlt
    jmp .hang

;─────────────────────────────────────────────
; 스택 + BSS 공간
;─────────────────────────────────────────────
section .bss
align 16
stack_bottom:
    resb 16384            ; 16KB 스택
stack_top: