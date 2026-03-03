// idt.c
#include "idt.h"

idt_gate_t idt[IDT_ENTRIES];
idt_register_t idt_reg;

static void set_idt_gate_common(int n, uintptr_t handler, uint8_t flags) {
    idt[n].offset_low = (uint16_t)(handler & 0xFFFFu);
    idt[n].selector = KERNEL_CS;
    idt[n].ist = 0;
    idt[n].flags = flags;
    idt[n].offset_mid = (uint16_t)((handler >> 16) & 0xFFFFu);
    idt[n].offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFFu);
    idt[n].reserved = 0;
}

void set_idt_gate(int n, uintptr_t handler) {
    set_idt_gate_common(n, handler, 0x8E);
}

void set_idt_gate_syscall(int n, uintptr_t handler) {
    set_idt_gate_common(n, handler, 0xEE);
}

void set_idt() {
    idt_reg.base = (uint64_t)(uintptr_t)&idt;
    idt_reg.limit = (uint16_t)(IDT_ENTRIES * sizeof(idt_gate_t) - 1u);
    asm volatile("lidt %0" : : "m"(idt_reg));
}
