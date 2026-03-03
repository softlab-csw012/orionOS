#pragma once

#include "../cpu/isr.h"

void syscall_handler(registers_t* regs);
void sys_close_fds_for_pid(uint32_t pid);
void attach_default_stdio(uint32_t pid, uint32_t parent_pid);
