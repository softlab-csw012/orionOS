#ifndef ISR_H
#define ISR_H

#include <stddef.h>
#include <stdint.h>

/* ISRs reserved for CPU exceptions */
extern void isr0();
extern void isr1();
extern void isr2();
extern void isr3();
extern void isr4();
extern void isr5();
extern void isr6();
extern void isr7();
extern void isr8();
extern void isr9();
extern void isr10();
extern void isr11();
extern void isr12();
extern void isr13();
extern void isr14();
extern void isr15();
extern void isr16();
extern void isr17();
extern void isr18();
extern void isr19();
extern void isr20();
extern void isr21();
extern void isr22();
extern void isr23();
extern void isr24();
extern void isr25();
extern void isr26();
extern void isr27();
extern void isr28();
extern void isr29();
extern void isr30();
extern void isr31();
extern void isrA5();
/* IRQ definitions */
extern void irq0();
extern void irq1();
extern void irq2();
extern void irq3();
extern void irq4();
extern void irq5();
extern void irq6();
extern void irq7();
extern void irq8();
extern void irq9();
extern void irq10();
extern void irq11();
extern void irq12();
extern void irq13();
extern void irq14();
extern void irq15();

#define IRQ0 32
#define IRQ1 33
#define IRQ2 34
#define IRQ3 35
#define IRQ4 36
#define IRQ5 37
#define IRQ6 38
#define IRQ7 39
#define IRQ8 40
#define IRQ9 41
#define IRQ10 42
#define IRQ11 43
#define IRQ12 44
#define IRQ13 45
#define IRQ14 46
#define IRQ15 47

typedef struct __attribute__((packed)) {
   union { uint64_t r15; };
   union { uint64_t r14; };
   union { uint64_t r13; };
   union { uint64_t r12; };
   union { uint64_t r11; };
   union { uint64_t r10; };
   union { uint64_t r9; };
   union { uint64_t r8; };
   union { uint64_t rsi; uint32_t esi; };
   union { uint64_t rdi; uint32_t edi; };
   union { uint64_t rbp; uint32_t ebp; };
   union { uint64_t rdx; uint32_t edx; };
   union { uint64_t rcx; uint32_t ecx; };
   union { uint64_t rbx; uint32_t ebx; };
   union { uint64_t rax; uint32_t eax; };
   uint64_t int_no;
   uint64_t err_code;
   union { uint64_t rip; uint32_t eip; };
   uint64_t cs;
   union { uint64_t rflags; uint32_t eflags; };
   union { uint64_t rsp; uint32_t esp; };
   uint64_t ss;
} registers_t;

void isr_install();
void isr_handler(registers_t *r);
void isr_dispatch(registers_t *r);
void irq_install();
void irq_dispatch(registers_t *r);
void irq_set_ready(uint8_t ready);
uint8_t irq_is_ready(void);
void irq_enable(void);

typedef void (*isr_t)(registers_t*);
void register_interrupt_handler(uint8_t n, isr_t handler);

#endif
