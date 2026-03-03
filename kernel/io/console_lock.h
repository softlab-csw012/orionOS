#pragma once

#include <stdint.h>

uintptr_t console_lock_acquire(void);
void console_lock_release(uintptr_t flags);
