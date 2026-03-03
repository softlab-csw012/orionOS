#pragma once

#include "ksys_dispatch.h"

bool ksys_handle_proc_exec(registers_t* regs);
bool ksys_handle_proc_signal(registers_t* regs);
