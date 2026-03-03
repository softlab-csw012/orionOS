[bits 64]

[extern isr_handler]
[extern irq_handler]
[extern sched_next_esp]

global switch_to

%macro PUSH_GPRS 0
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rdi
    push rsi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
%endmacro

%macro POP_GPRS 0
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rsi
    pop rdi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax
%endmacro

%macro MAYBE_SWITCH_FRAME 0
    mov rax, [rel sched_next_esp]
    test rax, rax
    jz %%no_switch
    mov rsp, rax
    mov qword [rel sched_next_esp], 0
%%no_switch:
%endmacro

switch_to:
    mov rsp, rdi
    POP_GPRS
    add rsp, 16
    iretq

isr_common_stub:
    cld
    PUSH_GPRS
    mov rdi, rsp
    call isr_handler
    MAYBE_SWITCH_FRAME
    POP_GPRS
    add rsp, 16
    iretq

irq_common_stub:
    cld
    PUSH_GPRS
    mov rdi, rsp
    call irq_handler
    MAYBE_SWITCH_FRAME
    POP_GPRS
    add rsp, 16
    iretq

%macro ISR_NOERR 2
%1:
    push qword 0
    push qword %2
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 2
%1:
    push qword %2
    jmp isr_common_stub
%endmacro

%macro IRQ_STUB 2
%1:
    push qword 0
    push qword %2
    jmp irq_common_stub
%endmacro

global isr0
global isr1
global isr2
global isr3
global isr4
global isr5
global isr6
global isr7
global isr8
global isr9
global isr10
global isr11
global isr12
global isr13
global isr14
global isr15
global isr16
global isr17
global isr18
global isr19
global isr20
global isr21
global isr22
global isr23
global isr24
global isr25
global isr26
global isr27
global isr28
global isr29
global isr30
global isr31
global isrA5
global irq0
global irq1
global irq2
global irq3
global irq4
global irq5
global irq6
global irq7
global irq8
global irq9
global irq10
global irq11
global irq12
global irq13
global irq14
global irq15

ISR_NOERR isr0, 0
ISR_NOERR isr1, 1
ISR_NOERR isr2, 2
ISR_NOERR isr3, 3
ISR_NOERR isr4, 4
ISR_NOERR isr5, 5
ISR_NOERR isr6, 6
ISR_NOERR isr7, 7
ISR_ERR   isr8, 8
ISR_NOERR isr9, 9
ISR_ERR   isr10, 10
ISR_ERR   isr11, 11
ISR_ERR   isr12, 12
ISR_ERR   isr13, 13
ISR_ERR   isr14, 14
ISR_NOERR isr15, 15
ISR_NOERR isr16, 16
ISR_ERR   isr17, 17
ISR_NOERR isr18, 18
ISR_NOERR isr19, 19
ISR_NOERR isr20, 20
ISR_ERR   isr21, 21
ISR_NOERR isr22, 22
ISR_NOERR isr23, 23
ISR_NOERR isr24, 24
ISR_NOERR isr25, 25
ISR_NOERR isr26, 26
ISR_NOERR isr27, 27
ISR_NOERR isr28, 28
ISR_NOERR isr29, 29
ISR_ERR   isr30, 30
ISR_NOERR isr31, 31
ISR_NOERR isrA5, 0xA5

IRQ_STUB irq0, 32
IRQ_STUB irq1, 33
IRQ_STUB irq2, 34
IRQ_STUB irq3, 35
IRQ_STUB irq4, 36
IRQ_STUB irq5, 37
IRQ_STUB irq6, 38
IRQ_STUB irq7, 39
IRQ_STUB irq8, 40
IRQ_STUB irq9, 41
IRQ_STUB irq10, 42
IRQ_STUB irq11, 43
IRQ_STUB irq12, 44
IRQ_STUB irq13, 45
IRQ_STUB irq14, 46
IRQ_STUB irq15, 47
