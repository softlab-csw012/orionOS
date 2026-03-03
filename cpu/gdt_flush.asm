[bits 64]
global gdt_flush

gdt_flush:
    lgdt [rdi]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    push qword 0x08
    lea rax, [rel .flush_done]
    push rax
    retfq
.flush_done:
    ret
