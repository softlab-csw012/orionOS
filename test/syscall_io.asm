[BITS 32]
[ORG 0x500000]
global start

%define SYS_KPRINT 2
%define SYS_EXIT   8
%define SYS_OPEN   12
%define SYS_READ   13
%define SYS_WRITE  14
%define SYS_CLOSE  15

start:
    mov eax, SYS_KPRINT
    mov ebx, msg_intro
    int 0xA5

    mov eax, SYS_OPEN
    mov ebx, path
    int 0xA5
    cmp eax, -1
    je open_fail
    mov [fd], eax

    mov eax, SYS_WRITE
    mov ebx, [fd]
    mov edx, write_buf
    mov ecx, write_len
    int 0xA5
    cmp eax, -1
    je write_fail

    mov eax, SYS_READ
    mov ebx, [fd]
    mov edx, read_buf
    mov ecx, read_len
    int 0xA5
    cmp eax, -1
    je read_fail

    mov [read_count], eax
    mov ecx, eax
    mov byte [read_buf + ecx], 0

    mov eax, SYS_KPRINT
    mov ebx, msg_read
    int 0xA5

    mov eax, SYS_KPRINT
    mov ebx, read_buf
    int 0xA5

    mov eax, SYS_KPRINT
    mov ebx, msg_nl
    int 0xA5

    mov eax, SYS_CLOSE
    mov ebx, [fd]
    int 0xA5

    mov eax, SYS_KPRINT
    mov ebx, msg_ok
    int 0xA5
    jmp done

open_fail:
    mov eax, SYS_KPRINT
    mov ebx, msg_open_fail
    int 0xA5
    jmp done

write_fail:
    mov eax, SYS_KPRINT
    mov ebx, msg_write_fail
    int 0xA5
    jmp done

read_fail:
    mov eax, SYS_KPRINT
    mov ebx, msg_read_fail
    int 0xA5
    jmp done

done:
    mov eax, SYS_EXIT
    xor ebx, ebx
    int 0xA5
    jmp $

path: db "/home/sysio.txt", 0

msg_intro:      db "[sysio] open/write/read test", 10, 0
msg_read:       db "[sysio] read back: ", 0
msg_ok:         db "[sysio] done", 10, 0
msg_open_fail:  db "[sysio] open failed", 10, 0
msg_write_fail: db "[sysio] write failed", 10, 0
msg_read_fail:  db "[sysio] read failed", 10, 0
msg_nl:         db 10, 0

write_buf: db "hello from user syscall io!", 10
write_len equ $ - write_buf

read_buf: times 128 db 0
read_len equ 127

fd: dd 0
read_count: dd 0
