[bits 32]
global tss_flush

tss_flush:
    mov ax, 0x28
    ltr ax
    ret
