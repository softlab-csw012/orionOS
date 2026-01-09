[bits 32]
global gdt_flush

gdt_flush:
    mov eax, [esp+4]      ; ← 함수 인자 읽기 (&gp)
    lgdt [eax]            ; GDTR에 로드

    mov ax, 0x10          ; 데이터 세그먼트 선택자 (index 2)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    jmp 0x08:.flush_done  ; 코드 세그먼트 갱신 (index 1)
.flush_done:
    ret
