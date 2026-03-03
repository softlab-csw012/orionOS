#pragma once

#include <stdbool.h>
#include "../../cpu/isr.h"

bool ksys_handle_proc(registers_t* regs);
bool ksys_handle_file(registers_t* regs);
bool ksys_handle_io(registers_t* regs);
bool ksys_handle_gui(registers_t* regs);
bool ksys_handle_misc(registers_t* regs);
