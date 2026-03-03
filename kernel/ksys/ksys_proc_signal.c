#include "ksys_proc_split.h"
#include "ksys_abi.h"
#include "../proc/proc.h"
#include "../../fs/fscmd.h"
#include "../../libc/string.h"

static bool has_process_suffix(const char* name, const char* suffix) {
    if (!name || !suffix) return false;
    size_t nlen = strlen(name);
    size_t slen = strlen(suffix);
    if (nlen < slen) return false;
    return strcasecmp(name + (nlen - slen), suffix) == 0;
}

bool ksys_handle_proc_signal(registers_t* regs) {
    uint32_t eax = regs->eax;
    uint32_t ebx = regs->ebx;
    uint32_t ecx = regs->ecx;

    switch (eax) {
        case SYS_KILL: {
            uint32_t pid = ebx;
            bool force = (ecx != 0);
            process_t* cur = proc_current();
            if (cur && cur->uid != 0 && cur->pid != pid) {
                process_t* target = proc_lookup(pid);
                if (target && target->uid != cur->uid) {
                    regs->eax = (uint32_t)PROC_KILL_PERM;
                    return true;
                }
            }
            regs->eax = (uint32_t)proc_kill(pid, force);
            return true;
        }

        case SYS_SETUID: {
            process_t* cur = proc_current();
            if (!cur) {
                regs->eax = 0;
                return true;
            }
            uint32_t new_uid = ebx;
            bool allow = false;
            if (cur->uid == 0 || has_process_suffix(cur->name, "login") || new_uid == cur->uid) {
                allow = true;
            }
            if (!allow) {
                regs->eax = 0;
                return true;
            }
            cur->uid = new_uid;
            cur->gid = new_uid;
            if (current_fs == FS_FAT16 || current_fs == FS_FAT32) {
                fscmd_set_fat_mount_policy(cur->uid, cur->gid, FSCMD_MODE_OWNER_RW);
            }
            regs->eax = 1;
            return true;
        }

        case SYS_GETUID: {
            process_t* cur = proc_current();
            regs->eax = cur ? cur->uid : 0u;
            return true;
        }

        case SYS_GETGID: {
            process_t* cur = proc_current();
            regs->eax = cur ? cur->gid : 0u;
            return true;
        }

        default:
            return false;
    }
}
