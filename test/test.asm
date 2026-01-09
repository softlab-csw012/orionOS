[BITS 32]
[ORG 0x500000]

MIN_NUM     equ 1
MAX_NUM     equ 50
MAX_DIGITS  equ 5

; ─────────────────────────────
; 시작
; ─────────────────────────────
start:
    mov eax, 3              ; clear screen
    int 0xA5
    mov [saved_esp], esp

    mov dword [try_count], 0
    call new_number

    mov eax, 2
    mov ebx, msg_title
    int 0xA5

; ─────────────────────────────
; 메인 루프
; ─────────────────────────────
game_loop:
    mov eax, 2
    mov ebx, msg_prompt
    int 0xA5

    call read_number        ; 입력 (에코 + 엔터)
    call atoi               ; EAX = 입력 숫자

    ; 숫자 범위 체크 (중요!)
    cmp eax, MIN_NUM
    jl game_loop
    cmp eax, MAX_NUM
    jg game_loop

    inc dword [try_count]

    movzx ebx, byte [secret_num]
    cmp eax, ebx
    je success
    jl hint_high
    jg hint_low

hint_high:
    mov eax, 2
    mov ebx, msg_high
    int 0xA5
    jmp fail

hint_low:
    mov eax, 2
    mov ebx, msg_low
    int 0xA5

fail:
    mov eax, 4
    mov ebx, 600
    mov ecx, 300
    int 0xA5
    jmp game_loop

; ─────────────────────────────
; 성공
; ─────────────────────────────
success:
    mov eax, 2
    mov ebx, msg_success
    int 0xA5

    mov eax, 2
    mov ebx, msg_try
    int 0xA5
    call print_try

    mov eax, 4
    mov ebx, 1200
    mov ecx, 200
    int 0xA5

exit:
    mov eax, 2
    mov ebx, msg_exit
    int 0xA5
    mov esp, [saved_esp]
    ret

; ─────────────────────────────
; 숫자 입력 (에코 + 엔터)
; ─────────────────────────────
read_number:
    mov dword [input_len], 0

.read_loop:
    mov eax, 6              ; getkey
    int 0xA5
    mov al, cl

    ; ESC
    cmp al, 27
    je exit

    ; Enter
    cmp al, 13
    je .done
    cmp al, 10
    je .done

    ; Backspace
    cmp al, 8
    je .backspace

    ; 숫자만 허용
    cmp al, '0'
    jl .read_loop
    cmp al, '9'
    jg .read_loop

    mov ecx, [input_len]
    cmp ecx, MAX_DIGITS
    jge .read_loop

    mov [input_buf + ecx], al
    inc dword [input_len]

    ; 에코 출력
    mov [echo_buf], al
    mov eax, 2
    mov ebx, echo_buf
    int 0xA5

    jmp .read_loop

.backspace:
    mov ecx, [input_len]
    cmp ecx, 0
    jle .read_loop
    dec dword [input_len]

    mov eax, 2
    mov ebx, msg_bs
    int 0xA5
    jmp .read_loop

.done:
    mov eax, 2
    mov ebx, msg_nl
    int 0xA5
    ret

; ─────────────────────────────
; atoi (문자열 → 숫자)
; 결과: EAX
; ─────────────────────────────
atoi:
    xor eax, eax
    xor ecx, ecx

.convert:
    cmp ecx, [input_len]
    jge .done
    movzx ebx, byte [input_buf + ecx]
    sub bl, '0'
    imul eax, eax, 10
    add eax, ebx
    inc ecx
    jmp .convert
.done:
    ret

; ─────────────────────────────
; 랜덤 숫자 생성 (정상 분포)
; ─────────────────────────────
new_number:
    rdtsc                  ; EDX:EAX
    xor edx, edx
    mov ecx, (MAX_NUM - MIN_NUM + 1)
    div ecx                ; EDX = EAX % range
    add edx, MIN_NUM
    mov [secret_num], dl
    ret

; ─────────────────────────────
; 시도 횟수 출력 (00~99)
; ─────────────────────────────
print_try:
    mov eax, [try_count]
    mov ebx, 10
    xor edx, edx
    div ebx

    add al, '0'
    add dl, '0'
    mov [try_buf], al
    mov [try_buf+1], dl

    mov eax, 2
    mov ebx, try_buf
    int 0xA5
    ret

; ─────────────────────────────
; 데이터
; ─────────────────────────────
secret_num db 0
try_count  dd 0
saved_esp  dd 0

input_buf  times MAX_DIGITS db 0
input_len  dd 0

echo_buf db 0,0
msg_bs   db 8,' ',8,0
msg_nl   db 10,0

msg_title   db "=== Guess the Number ===",10,0
msg_prompt db "GUESS (1-50): ",0
msg_high    db "HIGH!",10,0
msg_low     db "LOW!",10,0
msg_success db "SUCCESS!",10,0
msg_try     db "TRIES: ",0
msg_exit    db "EXIT",10,0

try_buf db "00",10,0
