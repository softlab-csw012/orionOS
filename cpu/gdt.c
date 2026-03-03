#include "gdt.h"

#include <stdint.h>

static union {
    struct {
        gdt_entry_t null_seg;
        gdt_entry_t kernel_code;
        gdt_entry_t kernel_data;
        gdt_entry_t user_code;
        gdt_entry_t user_data;
        gdt_tss_entry_t tss;
    } entries;
    uint8_t raw[sizeof(gdt_entry_t) * 5 + sizeof(gdt_tss_entry_t)];
} gdt;

static struct gdt_ptr gp;

extern void gdt_flush(const struct gdt_ptr* gp);

static gdt_entry_t* gdt_entry_at(int num) {
    return (gdt_entry_t*)(gdt.raw + (size_t)num * sizeof(gdt_entry_t));
}

void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt_entry_t* entry = gdt_entry_at(num);

    entry->limit_low = (uint16_t)(limit & 0xFFFFu);
    entry->base_low = (uint16_t)(base & 0xFFFFu);
    entry->base_mid = (uint8_t)((base >> 16) & 0xFFu);
    entry->access = access;
    entry->granularity = (uint8_t)(((limit >> 16) & 0x0Fu) | (gran & 0xF0u));
    entry->base_high = (uint8_t)((base >> 24) & 0xFFu);
}

void gdt_set_tss64(int num, uintptr_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt_tss_entry_t* entry = (gdt_tss_entry_t*)(gdt.raw + (size_t)num * sizeof(gdt_entry_t));

    entry->limit_low = (uint16_t)(limit & 0xFFFFu);
    entry->base_low = (uint16_t)(base & 0xFFFFu);
    entry->base_mid = (uint8_t)((base >> 16) & 0xFFu);
    entry->access = access;
    entry->granularity = (uint8_t)(((limit >> 16) & 0x0Fu) | (gran & 0xF0u));
    entry->base_high = (uint8_t)((base >> 24) & 0xFFu);
    entry->base_upper = (uint32_t)(base >> 32);
    entry->reserved = 0;
}

void gdt_install(void) {
    gp.limit = (uint16_t)(sizeof(gdt.raw) - 1u);
    gp.base = (uint64_t)(uintptr_t)&gdt;

    gdt_set_gate(0, 0, 0, 0, 0);
    gdt_set_gate(1, 0, 0, 0x9A, 0x20);
    gdt_set_gate(2, 0, 0, 0x92, 0x00);
    gdt_set_gate(3, 0, 0, 0xFA, 0x20);
    gdt_set_gate(4, 0, 0, 0xF2, 0x00);
    gdt_set_tss64(5, 0, 0, 0, 0);

    gdt_flush(&gp);
}
