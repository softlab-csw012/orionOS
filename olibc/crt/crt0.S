.section .text
.global _start
.weak _start
.type _start, @function
.extern __libc_start_main
_start:
    xor %ebp, %ebp          # ABI 요구 (백트레이스 종료 표시)
    push %esp               # stack_end
    call __libc_start_main

1:  jmp 1b
.size _start, .-_start
