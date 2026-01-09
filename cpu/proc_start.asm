[bits 32]
global proc_start

proc_start:
    cli
    mov eax, [esp+4]
    mov esp, eax

    pop ebx
    mov ds, bx
    mov es, bx
    mov fs, bx
    mov gs, bx

    popa
    add esp, 8

    iret
