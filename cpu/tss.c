#include "tss.h"
#include "gdt.h"
#include "../libc/string.h"

static tss_entry_t tss_entry;

extern void tss_flush(void);

static void tss_write(uintptr_t kernel_stack) {
    uintptr_t base = (uintptr_t)&tss_entry;
    uint32_t limit = (uint32_t)(sizeof(tss_entry) - 1u);

    gdt_set_tss64(5, base, limit, 0x89, 0x00);

    memset(&tss_entry, 0, sizeof(tss_entry));
    tss_entry.rsp0 = (uint64_t)kernel_stack;
    tss_entry.iomap_base = (uint16_t)sizeof(tss_entry);
}

void tss_install(uintptr_t kernel_stack) {
    tss_write(kernel_stack);
    tss_flush();
}

void tss_set_kernel_stack(uintptr_t kernel_stack) {
    tss_entry.rsp0 = (uint64_t)kernel_stack;
}
