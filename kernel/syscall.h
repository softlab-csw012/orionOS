#pragma once

#include "../cpu/isr.h"

void syscall_handler(registers_t* regs);
void sys_close_fds_for_pid(uint32_t pid);
