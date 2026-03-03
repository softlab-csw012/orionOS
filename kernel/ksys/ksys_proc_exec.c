#include "ksys_proc_split.h"
#include "ksys_abi.h"
#include "ksys_usercopy.h"
#include "../syscall.h"
#include "../bin.h"
#include "../tty.h"
#include "../io/console.h"
#include "../proc/proc.h"
#include "../proc/sysmgr.h"
#include "../io/fd.h"
#include "../ipc/gui_ipc.h"
#include "../../mm/mem.h"
#include "../../fs/fscmd.h"
#include "../../libc/string.h"

#define KERNEL_CS 0x08
#define KERNEL_DS 0x10

static bool has_process_suffix(const char* name, const char* suffix) {
    if (!name || !suffix) return false;
    size_t nlen = strlen(name);
    size_t slen = strlen(suffix);
    if (nlen < slen) return false;
    return strcasecmp(name + (nlen - slen), suffix) == 0;
}

static bool is_foreground_controller(const process_t* p) {
    if (!p) return false;
    return has_process_suffix(p->name, "shell") || has_process_suffix(p->name, "init.sys");
}

bool ksys_handle_proc_exec(registers_t* regs) {
    uint32_t eax = regs->eax;
    uint32_t ebx = regs->ebx;
    uint32_t ecx = regs->ecx;
    uint32_t edx = regs->edx;

    switch (eax) {
        case SYS_EXIT: {
            proc_exit(ebx);
            if (!proc_schedule(regs, false)) {
                regs->rip = (uintptr_t)bin_exit_trampoline;
                regs->cs = KERNEL_CS;
                regs->ss = KERNEL_DS;
            }
            return true;
        }

        case SYS_YIELD:
            (void)proc_schedule(regs, true);
            return true;

        case SYS_SPAWN_THREAD: {
            const char* name = ecx ? (const char*)(uintptr_t)ecx : NULL;
            process_t* child = ebx ? proc_create(name, (uintptr_t)ebx) : NULL;
            regs->eax = child ? child->pid : 0;
            return true;
        }

        case SYS_START_SYSMGR:
            regs->eax = proc_start_reaper() ? 1u : 0u;
            return true;

        case SYS_SPAWN: {
            char path[MAX_PATH_LEN];
            if (ksys_copy_user_string(path, ebx, sizeof(path)) != 0) {
                regs->eax = 0;
                return true;
            }
            int argc = (int)edx;
            if (argc < 0) {
                regs->eax = 0;
                return true;
            }
            char** argv = NULL;
            if (ksys_copy_user_argv(ecx, argc, MAX_ARGC, MAX_PATH_LEN, &argv) != 0) {
                regs->eax = 0;
                return true;
            }

            process_t* child = bin_create_process(path, (const char* const*)argv, argc, false);
            if (child) {
                fd_attach_default_stdio(child->pid, proc_current_pid());
                tty_set_foreground(child->pid);
            }
            regs->eax = child ? child->pid : 0;
            ksys_free_kernel_argv(argv, argc);
            return true;
        }

        case SYS_SPAWN_STDIO: {
            if (!ebx || ksys_validate_user_buffer(ebx, sizeof(sys_spawn_stdio_t)) != 0) {
                regs->eax = 0;
                return true;
            }

            sys_spawn_stdio_t req = *(sys_spawn_stdio_t*)(uintptr_t)ebx;
            if (!req.path_ptr) {
                regs->eax = 0;
                return true;
            }

            char path[MAX_PATH_LEN];
            if (ksys_copy_user_string(path, req.path_ptr, sizeof(path)) != 0) {
                regs->eax = 0;
                return true;
            }

            int argc = req.argc;
            if (argc < 0) {
                regs->eax = 0;
                return true;
            }

            char** argv = NULL;
            if (ksys_copy_user_argv(req.argv_ptr, argc, MAX_ARGC, MAX_PATH_LEN, &argv) != 0) {
                regs->eax = 0;
                return true;
            }

            process_t* child = bin_create_process(path, (const char* const*)argv, argc, false);
            ksys_free_kernel_argv(argv, argc);
            if (!child) {
                regs->eax = 0;
                return true;
            }

            uint32_t parent_pid = proc_current_pid();
            int32_t cin = -1;
            int32_t cout = -1;
            int32_t cerr = -1;

            int32_t p_stdin = fd_get_mapped_stdio_fd(parent_pid, DEV_STDIN);
            int32_t p_stdout = fd_get_mapped_stdio_fd(parent_pid, DEV_STDOUT);
            int32_t p_stderr = fd_get_mapped_stdio_fd(parent_pid, DEV_STDERR);

            cin = (p_stdin >= 0) ? fd_clone((uint32_t)p_stdin, parent_pid, child->pid)
                                 : fd_attach_device_for_pid(child->pid, "/dev/stdin");
            cout = (p_stdout >= 0) ? fd_clone((uint32_t)p_stdout, parent_pid, child->pid)
                                   : fd_attach_device_for_pid(child->pid, "/dev/stdout");
            cerr = (p_stderr >= 0) ? fd_clone((uint32_t)p_stderr, parent_pid, child->pid)
                                   : fd_attach_device_for_pid(child->pid, "/dev/stderr");
            if (cin < 0 || cout < 0 || cerr < 0) {
                proc_kill(child->pid, true);
                regs->eax = 0;
                return true;
            }

            if (req.stdin_fd >= 0) {
                int32_t n = fd_clone((uint32_t)req.stdin_fd, parent_pid, child->pid);
                if (n < 0) { proc_kill(child->pid, true); regs->eax = 0; return true; }
                syscall_fd_t* old = fd_get((uint32_t)cin, child->pid);
                if (old) fd_close(old);
                cin = n;
            }

            if (req.stdout_fd >= 0) {
                int32_t n = fd_clone((uint32_t)req.stdout_fd, parent_pid, child->pid);
                if (n < 0) { proc_kill(child->pid, true); regs->eax = 0; return true; }
                syscall_fd_t* old = fd_get((uint32_t)cout, child->pid);
                if (old) fd_close(old);
                cout = n;
            }

            if (req.stderr_fd >= 0) {
                int32_t n = fd_clone((uint32_t)req.stderr_fd, parent_pid, child->pid);
                if (n < 0) { proc_kill(child->pid, true); regs->eax = 0; return true; }
                syscall_fd_t* old = fd_get((uint32_t)cerr, child->pid);
                if (old) fd_close(old);
                cerr = n;
            }

            fd_proc_stdio_set(child->pid, cin, cout, cerr);

            regs->eax = child->pid;
            return true;
        }

        case SYS_ARGC: {
            process_t* cur = proc_current();
            regs->eax = cur ? (uint32_t)(cur->argc_saved >= 0 ? cur->argc_saved : 0) : 0u;
            return true;
        }

        case SYS_ARG_GET: {
            process_t* cur = proc_current();
            uint32_t idx = ebx;
            uint32_t dst = ecx;
            uint32_t dst_len = edx;
            if (!cur || !dst || dst_len == 0) {
                regs->eax = 0;
                return true;
            }
            if (idx >= (uint32_t)cur->argc_saved || idx >= PROC_ARGC_MAX) {
                regs->eax = 0;
                return true;
            }
            if (ksys_validate_user_buffer(dst, dst_len) != 0) {
                regs->eax = 0;
                return true;
            }
            const char* src = cur->argv_saved[idx];
            if (!src) {
                ((char*)(uintptr_t)dst)[0] = '\0';
                regs->eax = 1;
                return true;
            }
            uint32_t i = 0;
            for (; i + 1u < dst_len && src[i]; i++) {
                ((char*)(uintptr_t)dst)[i] = src[i];
            }
            ((char*)(uintptr_t)dst)[i] = '\0';
            regs->eax = 1;
            return true;
        }

        case SYS_WAIT: {
            uint32_t pid = ebx;
            uint32_t code = 0;
            if (pid == 0) {
                regs->eax = WAIT_NO_SUCH;
                return true;
            }
            if (proc_pid_exited(pid, &code)) {
                regs->eax = code;
                return true;
            }
            if (!proc_pid_alive(pid)) {
                regs->eax = WAIT_NO_SUCH;
                return true;
            }
            regs->eax = WAIT_RUNNING;
            return true;
        }

        case SYS_FORK: {
            uint32_t parent_pid = proc_current_pid();
            process_t* child = proc_fork(regs);
            if (child) {
                fd_attach_default_stdio(child->pid, parent_pid);
            }
            regs->eax = child ? child->pid : 0u;
            return true;
        }

        case SYS_EXEC: {
            char path[MAX_PATH_LEN];
            if (ksys_copy_user_string(path, ebx, sizeof(path)) != 0) {
                regs->eax = EXEC_ERR_FAULT;
                return true;
            }

            int argc = (int)edx;
            if (argc < 0) {
                regs->eax = EXEC_ERR_INVAL;
                return true;
            }
            char** argv = NULL;
            if (ksys_copy_user_argv(ecx, argc, MAX_ARGC, MAX_PATH_LEN, &argv) != 0) {
                regs->eax = EXEC_ERR_FAULT;
                return true;
            }

            uintptr_t entry = 0;
            uintptr_t image_base = 0;
            uint32_t image_size = 0;
            uintptr_t image_load_base = 0;
            if (!fscmd_exists(path)) {
                ksys_free_kernel_argv(argv, argc);
                regs->eax = EXEC_ERR_NOENT;
                return true;
            }
            if (!bin_load_image(path, &entry, &image_base, &image_size, &image_load_base)) {
                ksys_free_kernel_argv(argv, argc);
                regs->eax = EXEC_ERR_NOEXEC;
                return true;
            }

            process_t* cur = proc_current();
            if (!cur || cur->is_kernel) {
                ksys_free_kernel_argv(argv, argc);
                if (image_base) kfree((void*)(uintptr_t)image_base);
                regs->eax = EXEC_ERR_PERM;
                return true;
            }

            if (!proc_exec(cur, entry, image_base, image_size, image_load_base,
                           path, (const char* const*)argv, argc)) {
                ksys_free_kernel_argv(argv, argc);
                if (image_base) kfree((void*)(uintptr_t)image_base);
                regs->eax = EXEC_ERR_NOMEM;
                return true;
            }

            ksys_free_kernel_argv(argv, argc);
            sched_next_esp = cur->context_esp;
            regs->eax = 0;
            return true;
        }

        case SYS_GETPID:
            regs->eax = proc_current_pid();
            return true;

        case SYS_SET_FOREGROUND: {
            uint32_t caller = proc_current_pid();
            uint32_t fg = tty_get_foreground();
            uint32_t gui_server_pid = gui_ipc_server_pid_get();
            process_t* cur = proc_current();

            bool allowed = false;
            if (caller == 0 || caller == gui_server_pid || fg == 0 || caller == fg || is_foreground_controller(cur)) {
                allowed = true;
            }
            if (!allowed) {
                regs->eax = 0;
                return true;
            }
            tty_set_foreground(ebx);
            regs->eax = 1;
            return true;
        }

        default:
            return false;
    }
}
