#include "ksys_dispatch.h"
#include "ksys_abi.h"
#include "ksys_usercopy.h"
#include "../io/console_lock.h"
#include "../ipc/gui_ipc.h"
#include "../proc/proc.h"
#include "../../libc/string.h"

static bool gui_queue_push(const sys_gui_msg_t* msg) {
    return gui_ipc_queue_push((const gui_ipc_msg_t*)msg);
}

static bool gui_queue_pop(sys_gui_msg_t* out) {
    return gui_ipc_queue_pop((gui_ipc_msg_t*)out);
}

static bool gui_client_queue_push(uint32_t pid, const sys_gui_msg_t* msg) {
    return gui_ipc_client_push(pid, (const gui_ipc_msg_t*)msg);
}

static bool gui_client_queue_pop(uint32_t pid, sys_gui_msg_t* out) {
    return gui_ipc_client_pop(pid, (gui_ipc_msg_t*)out);
}

bool ksys_handle_gui(registers_t* regs) {
    uint32_t eax = regs->eax;
    uint32_t ebx = regs->ebx;
    uint32_t ecx = regs->ecx;

    switch (eax) {
        case SYS_GUI_BIND: {
            uint32_t pid = proc_current_pid();
            uint32_t gui_server_pid = gui_ipc_server_pid_get();
            if (gui_server_pid != 0 && gui_server_pid != pid && proc_pid_alive(gui_server_pid)) {
                regs->eax = 0;
                return true;
            }
            gui_ipc_server_pid_set(pid);
            regs->eax = 1;
            return true;
        }

        case SYS_GUI_SEND: {
            uint32_t gui_server_pid = gui_ipc_server_pid_get();
            if (!gui_server_pid || !ebx || ksys_validate_user_buffer(ebx, sizeof(sys_gui_msg_t)) != 0) {
                regs->eax = 0;
                return true;
            }
            sys_gui_msg_t msg = *(sys_gui_msg_t*)ebx;
            msg.sender_pid = proc_current_pid();
            uint32_t flags = console_lock_acquire();
            bool ok = gui_queue_push(&msg);
            console_lock_release(flags);
            regs->eax = ok ? 1u : 0u;
            return true;
        }

        case SYS_GUI_RECV: {
            uint32_t gui_server_pid = gui_ipc_server_pid_get();
            if (proc_current_pid() != gui_server_pid || !ebx ||
                ksys_validate_user_buffer(ebx, sizeof(sys_gui_msg_t)) != 0) {
                regs->eax = 0;
                return true;
            }
            sys_gui_msg_t msg;
            uint32_t flags = console_lock_acquire();
            bool ok = gui_queue_pop(&msg);
            console_lock_release(flags);
            if (!ok) {
                regs->eax = 0;
                return true;
            }
            memcpy((void*)ebx, &msg, sizeof(msg));
            regs->eax = 1;
            return true;
        }

        case SYS_GUI_SEND_TO: {
            uint32_t sender_pid = proc_current_pid();
            uint32_t gui_server_pid = gui_ipc_server_pid_get();
            if (!gui_server_pid || sender_pid != gui_server_pid || !ebx || !ecx ||
                !proc_pid_alive(ebx) ||
                ksys_validate_user_buffer(ecx, sizeof(sys_gui_msg_t)) != 0) {
                regs->eax = 0;
                return true;
            }
            sys_gui_msg_t msg = *(sys_gui_msg_t*)ecx;
            msg.sender_pid = sender_pid;
            uint32_t flags = console_lock_acquire();
            bool ok = gui_client_queue_push(ebx, &msg);
            console_lock_release(flags);
            regs->eax = ok ? 1u : 0u;
            return true;
        }

        case SYS_GUI_RECV_EVENT: {
            uint32_t pid = proc_current_pid();
            uint32_t gui_server_pid = gui_ipc_server_pid_get();
            if (pid == 0 || pid == gui_server_pid || !ebx ||
                ksys_validate_user_buffer(ebx, sizeof(sys_gui_msg_t)) != 0) {
                regs->eax = 0;
                return true;
            }
            sys_gui_msg_t msg;
            uint32_t flags = console_lock_acquire();
            bool ok = gui_client_queue_pop(pid, &msg);
            console_lock_release(flags);
            if (!ok) {
                regs->eax = 0;
                return true;
            }
            memcpy((void*)ebx, &msg, sizeof(msg));
            regs->eax = 1;
            return true;
        }

        default:
            return false;
    }
}
