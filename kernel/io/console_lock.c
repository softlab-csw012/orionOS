#include "console_lock.h"

#define EFLAGS_IF 0x200u

uintptr_t console_lock_acquire(void) {
    uintptr_t flags = 0;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

void console_lock_release(uintptr_t flags) {
    if (flags & EFLAGS_IF) {
        __asm__ volatile("sti" ::: "memory");
    }
}
