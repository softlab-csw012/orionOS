#include "ksys_dispatch.h"
#include "ksys_abi.h"
#include "ksys_usercopy.h"
#include "ksys_security.h"
#include "ksys_rtc.h"
#include "../config.h"
#include "../cmd.h"
#include "../kernel.h"
#include "../proc/proc.h"
#include "../../cpu/timer.h"
#include "../../libc/string.h"

bool ksys_handle_misc(registers_t* regs) {
    uint32_t eax = regs->eax;
    uint32_t ebx = regs->ebx;
    uint32_t ecx = regs->ecx;

    switch (eax) {
        case SYS_GET_BOOT_FLAGS:
            regs->eax = orion_boot_flags();
            return true;

        case SYS_UPTIME_SECONDS:
            regs->eax = uptime_seconds();
            return true;

        case SYS_RTC_READ: {
            if (!ebx || ksys_validate_user_buffer(ebx, sizeof(sys_rtc_time_t)) != 0) {
                regs->eax = 0;
                return true;
            }
            ksys_rtc_time_t rt = {0};
            if (!ksys_rtc_read(&rt)) {
                regs->eax = 0;
                return true;
            }
            sys_rtc_time_t t = {
                .sec = rt.sec,
                .min = rt.min,
                .hour = rt.hour,
                .day = rt.day,
                .month = rt.month,
                .year = rt.year,
            };
            memcpy((void*)ebx, &t, sizeof(t));
            regs->eax = 1;
            return true;
        }

        case SYS_PROC_LIST: {
            if (!ebx || ecx == 0) {
                regs->eax = 0;
                return true;
            }
            uint32_t max = ecx > MAX_PROCS ? MAX_PROCS : ecx;
            uint32_t out_size = max * (uint32_t)sizeof(sys_proc_info_t);
            if (ksys_validate_user_buffer(ebx, out_size) != 0) {
                regs->eax = 0;
                return true;
            }
            regs->eax = (uint32_t)proc_list((proc_info_t*)ebx, (int)max);
            return true;
        }

        case SYS_SUPER_CMD: {
            process_t* cur = proc_current();
            char cmdline[MAX_PATH_LEN];
            if (!ebx || ksys_copy_user_string(cmdline, ebx, sizeof(cmdline)) != 0) {
                regs->eax = (uint32_t)-1;
                return true;
            }
            bool root = (cur && cur->uid == 0);
            bool user_allow = ksys_is_user_allowed_super_cmd(cmdline);
            if (!root && !user_allow) {
                regs->eax = (uint32_t)-2;
                return true;
            }
            if (!ksys_is_allowed_super_cmd(cmdline) && !user_allow) {
                regs->eax = (uint32_t)-3;
                return true;
            }
            char work[MAX_PATH_LEN];
            strncpy(work, cmdline, sizeof(work) - 1);
            work[sizeof(work) - 1] = '\0';
            bool prev_enable_shell = enable_shell;
            enable_shell = true;
            regs->eax = execute_single_command(cmdline, work) ? 1u : 0u;
            enable_shell = prev_enable_shell;
            return true;
        }

        case SYS_DF: {
            char work[8];
            strcpy(work, "df");
            regs->eax = execute_single_command("df", work) ? 1u : 0u;
            return true;
        }

        default:
            return false;
    }
}
