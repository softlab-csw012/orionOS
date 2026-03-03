#include "syscall.h"
#include "kernel.h"
#include "io/console.h"
#include "io/fd.h"
#include "proc/proc.h"
#include "ksys/ksys_abi.h"
#include "ksys/ksys_dispatch.h"

void attach_default_stdio(uint32_t pid, uint32_t parent_pid) {
    fd_attach_default_stdio(pid, parent_pid);
}

void sys_close_fds_for_pid(uint32_t pid) {
    fd_close_all_for_pid(pid);
}

static bool syscall_from_user(const registers_t* regs) {
    if (!regs) {
        return false;
    }
    if ((regs->cs & 0x3u) != 0x3u) {
        return false;
    }
    if (!proc_current_is_user()) {
        return false;
    }
    return true;
}

void syscall_handler(registers_t* regs) {
    if (!regs) {
        return;
    }
    if (!syscall_from_user(regs)) {
        regs->eax = (uint32_t)-1;
        return;
    }
    if (regs->eax == 0 || regs->eax > SYS_SUPER_CMD) {
        regs->eax = (uint32_t)-1;
        return;
    }

    if (ksys_handle_proc(regs)) return;
    if (ksys_handle_file(regs)) return;
    if (ksys_handle_io(regs)) return;
    if (ksys_handle_gui(regs)) return;
    if (ksys_handle_misc(regs)) return;

    regs->eax = (uint32_t)-1;
}
