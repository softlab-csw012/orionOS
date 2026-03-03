#pragma once

#include <stdint.h>

uint32_t console_lock_acquire(void);
void console_lock_release(uint32_t flags);
