#include "tss.h"
#include "gdt.h"
#include "../libc/string.h"

static tss_entry_t tss_entry;

extern void tss_flush(void);

static void tss_write(uint32_t kernel_stack) {
    uint32_t base = (uint32_t)&tss_entry;
    uint32_t limit = sizeof(tss_entry) - 1u;

    gdt_set_gate(5, base, limit, 0x89, 0x00);

    memset(&tss_entry, 0, sizeof(tss_entry));
    tss_entry.ss0 = 0x10;
    tss_entry.esp0 = kernel_stack;
    tss_entry.iomap_base = sizeof(tss_entry);
}

void tss_install(uint32_t kernel_stack) {
    tss_write(kernel_stack);
    tss_flush();
}

void tss_set_kernel_stack(uint32_t kernel_stack) {
    tss_entry.esp0 = kernel_stack;
}
