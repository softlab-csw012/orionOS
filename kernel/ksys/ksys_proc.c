#include "ksys_proc_split.h"

bool ksys_handle_proc(registers_t* regs) {
    if (ksys_handle_proc_exec(regs)) return true;
    if (ksys_handle_proc_signal(regs)) return true;
    return false;
}
