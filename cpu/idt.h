#ifndef IDT_H
#define IDT_H

#include <stddef.h>
#include <stdint.h>

#define KERNEL_CS 0x08

typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t flags;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed)) idt_gate_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idt_register_t;

#define IDT_ENTRIES 256
extern idt_gate_t idt[IDT_ENTRIES];
extern idt_register_t idt_reg;

void set_idt_gate(int n, uintptr_t handler);
void set_idt_gate_syscall(int n, uintptr_t handler);
void set_idt(void);

#endif
