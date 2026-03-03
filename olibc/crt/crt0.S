.section .text
.global _start
.weak _start
.type _start, @function
.extern __libc_start_main
_start:
    xor %ebp, %ebp
    mov %rsp, %rdi
    call __libc_start_main

1:  jmp 1b
.size _start, .-_start
