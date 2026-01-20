.global _start
.extern main

_start:
    call main

    # sys_exit(0)
    movl $8, %eax     # SYS_EXIT = 8
    xorl %ebx, %ebx  # status = 0
    int  $0xA5
