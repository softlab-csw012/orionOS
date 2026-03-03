#include "ksys_dispatch.h"
#include "ksys_abi.h"
#include "ksys_usercopy.h"
#include "../../fs/vfs.h"
#include "../../fs/note.h"

bool ksys_handle_file(registers_t* regs) {
    uint32_t eax = regs->eax;
    uint32_t ebx = regs->ebx;
    uint32_t ecx = regs->ecx;
    uint32_t edx = regs->edx;

    switch (eax) {
        case SYS_OPEN: {
            char path[MAX_PATH_LEN];
            if (ksys_copy_user_string(path, ebx, sizeof(path)) != 0) {
                regs->eax = (uint32_t)-1;
                return true;
            }
            uint32_t owner_pid = proc_current_pid();
            int fd = vfs_open(owner_pid, path);
            regs->eax = (fd >= 0) ? (uint32_t)fd : (uint32_t)-1;
            return true;
        }

        case SYS_OPEN_EX: {
            if (!ebx || ksys_validate_user_buffer(ebx, sizeof(sys_open_ex_t)) != 0) {
                regs->eax = (uint32_t)-1;
                return true;
            }
            sys_open_ex_t req = *(sys_open_ex_t*)(uintptr_t)ebx;
            if (!req.path_ptr) {
                regs->eax = (uint32_t)-1;
                return true;
            }
            char path[MAX_PATH_LEN];
            if (ksys_copy_user_string(path, req.path_ptr, sizeof(path)) != 0) {
                regs->eax = (uint32_t)-1;
                return true;
            }
            uint32_t owner_pid = proc_current_pid();
            int fd = vfs_open_ex(owner_pid, path, req.flags);
            regs->eax = (fd >= 0) ? (uint32_t)fd : (uint32_t)-1;
            return true;
        }

        case SYS_MKNOD: {
            if (!ebx || ksys_validate_user_buffer(ebx, sizeof(sys_mknod_t)) != 0) {
                regs->eax = 0;
                return true;
            }
            sys_mknod_t req = *(sys_mknod_t*)(uintptr_t)ebx;
            if (!req.path_ptr) {
                regs->eax = 0;
                return true;
            }
            char path[MAX_PATH_LEN];
            if (ksys_copy_user_string(path, req.path_ptr, sizeof(path)) != 0) {
                regs->eax = 0;
                return true;
            }
            regs->eax = vfs_mknod(path, (uint8_t)req.node_type, (uint16_t)req.major, (uint16_t)req.minor) ? 1u : 0u;
            return true;
        }

        case SYS_READ: {
            uint32_t owner_pid = proc_current_pid();
            if (ecx == 0 || !regs->edx) {
                regs->eax = 0;
                return true;
            }
            if (ksys_validate_user_buffer(regs->edx, ecx) != 0) {
                regs->eax = (uint32_t)-1;
                return true;
            }
            int r = vfs_read(owner_pid, ebx, (void*)(uintptr_t)regs->edx, ecx);
            if (r < 0) {
                if ((uint32_t)r == PIPE_READ_AGAIN) {
                    regs->eax = PIPE_READ_AGAIN;
                } else {
                    regs->eax = (uint32_t)-1;
                }
            } else {
                regs->eax = (uint32_t)r;
            }
            return true;
        }

        case SYS_WRITE: {
            uint32_t owner_pid = proc_current_pid();

            if (ecx == 0) {
                regs->eax = 0;
                return true;
            }
            if (!regs->edx || ksys_validate_user_buffer(regs->edx, ecx) != 0) {
                regs->eax = (uint32_t)-1;
                return true;
            }
            int r = vfs_write(owner_pid, ebx, (const void*)(uintptr_t)regs->edx, ecx);
            regs->eax = (r < 0) ? (uint32_t)-1 : (uint32_t)r;
            return true;
        }

        case SYS_CLOSE: {
            int rc = vfs_close(proc_current_pid(), ebx);
            regs->eax = (rc == 0) ? 0u : (uint32_t)-1;
            return true;
        }

        case SYS_PIPE: {
            if (!ebx || ksys_validate_user_buffer(ebx, sizeof(uint32_t) * 2u) != 0) {
                regs->eax = 0;
                return true;
            }
            uint32_t fds[2] = {0, 0};
            int rc = vfs_pipe_create(proc_current_pid(), fds);
            if (rc != 0) {
                regs->eax = 0;
                return true;
            }
            uint32_t* out = (uint32_t*)(uintptr_t)ebx;
            out[0] = fds[0];
            out[1] = fds[1];
            regs->eax = 1u;
            return true;
        }

        case SYS_PTY_OPEN: {
            if (!ebx || ksys_validate_user_buffer(ebx, sizeof(uint32_t) * 2u) != 0) {
                regs->eax = 0;
                return true;
            }
            uint32_t fds[2] = {0, 0};
            int rc = vfs_pty_create(proc_current_pid(), fds);
            if (rc != 0) {
                regs->eax = 0;
                return true;
            }
            uint32_t* out = (uint32_t*)(uintptr_t)ebx;
            out[0] = fds[0];
            out[1] = fds[1];
            regs->eax = 1u;
            return true;
        }

        case SYS_PTY_CTL: {
            int rc = vfs_pty_ctl(proc_current_pid(), ebx, ecx, edx);
            regs->eax = (uint32_t)rc;
            return true;
        }

        case SYS_CHDIR: {
            char path[MAX_PATH_LEN];
            if (!ebx || ksys_copy_user_string(path, ebx, sizeof(path)) != 0) {
                regs->eax = 0;
                return true;
            }
            regs->eax = vfs_chdir(path) ? 1u : 0u;
            return true;
        }

        case SYS_RM: {
            char path[MAX_PATH_LEN];
            if (!ebx || ksys_copy_user_string(path, ebx, sizeof(path)) != 0) {
                regs->eax = 0;
                return true;
            }
            regs->eax = vfs_remove(path) ? 1u : 0u;
            return true;
        }

        case SYS_NOTE: {
            char path[MAX_PATH_LEN];
            if (!ebx || ksys_copy_user_string(path, ebx, sizeof(path)) != 0) {
                regs->eax = 0;
                return true;
            }
            note(path);
            regs->eax = 1u;
            return true;
        }

        case SYS_DIR_LIST: {
            if (!ebx || ksys_validate_user_buffer(ebx, sizeof(sys_dir_list_t)) != 0) {
                regs->eax = (uint32_t)-1;
                return true;
            }
            sys_dir_list_t req = *(sys_dir_list_t*)(uintptr_t)ebx;
            if (!req.names_ptr || !req.is_dir_ptr || req.max_entries == 0 || req.name_len == 0) {
                regs->eax = 0;
                return true;
            }
            uint32_t max_entries = req.max_entries > 256 ? 256 : req.max_entries;
            uint32_t name_len = req.name_len > 64 ? 64 : req.name_len;
            uint32_t names_size = max_entries * name_len;
            if (ksys_validate_user_buffer(req.names_ptr, names_size) != 0 ||
                ksys_validate_user_buffer(req.is_dir_ptr, max_entries) != 0) {
                regs->eax = (uint32_t)-1;
                return true;
            }
            char path[MAX_PATH_LEN];
            const char* use_path = "";
            if (req.path_ptr) {
                if (ksys_copy_user_string(path, req.path_ptr, sizeof(path)) != 0) {
                    regs->eax = (uint32_t)-1;
                    return true;
                }
                if (path[0] != '\0') use_path = path;
            }
            int count = vfs_list_dir(use_path, (char*)(uintptr_t)req.names_ptr,
                                     (uint8_t*)(uintptr_t)req.is_dir_ptr, max_entries, name_len);
            regs->eax = (count < 0) ? (uint32_t)-1 : (uint32_t)count;
            return true;
        }

        default:
            return false;
    }
}
