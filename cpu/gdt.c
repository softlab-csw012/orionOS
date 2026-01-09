#include "gdt.h"

// GDT 엔트리 6개: null, code, data, user_code, user_data, tss
static struct gdt_entry gdt[6];
static struct gdt_ptr gp;

// ASM 함수 (gdt_flush) 선언
extern void gdt_flush(uint32_t);

void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;

    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity = ((limit >> 16) & 0x0F);

    gdt[num].granularity |= (gran & 0xF0);
    gdt[num].access       = access;
}

void gdt_install(void) {
    gp.limit = (sizeof(struct gdt_entry) * 6) - 1;
    gp.base  = (uint32_t)&gdt;

    // Null descriptor
    gdt_set_gate(0, 0, 0, 0, 0);

    // Kernel code segment: base=0, limit=4GB, access=0x9A, gran=0xCF
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);

    // Kernel data segment: base=0, limit=4GB, access=0x92, gran=0xCF
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    // User code segment (ring 3)
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);

    // User data segment (ring 3)
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    // TSS entry (filled by tss_install)
    gdt_set_gate(5, 0, 0, 0, 0);

    // GDTR 로드
    gdt_flush((uint32_t)&gp);
}
