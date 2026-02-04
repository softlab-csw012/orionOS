#include "sysmgr.h"
#include "proc.h"
#include "timer_task.h"
#include "workqueue.h"
#include "../bin.h"
#include "../kernel.h"
#include "../../libc/string.h"
#include "../../drivers/hal.h"
#include "../../drivers/keyboard.h"
#include "../../drivers/screen.h"
#include "../../drivers/usb/ehci.h"
#include "../../drivers/usb/ohci.h"
#include "../../drivers/usb/uhci.h"
#include "../../drivers/usb/xhci.h"

static bool sysmgr_console_active(void) {
    return prompt_enabled && keyboard_input_enabled && !script_running;
}

static void sysmgr_console_begin(bool* started) {
    if (!started || *started) {
        return;
    }
    if (sysmgr_console_active()) {
        kprint("\n");
        *started = true;
    }
}

static bool sysmgr_prompt_pending = false;
static uint32_t sysmgr_output_seq = 0;
static uint32_t sysmgr_prompt_seq = 0;
static bool sysmgr_prompt_force = false;
static volatile bool sysmgr_user_shell_pending = false;
static volatile bool sysmgr_exec_pending = false;

#define SYSMGR_EXEC_MAX_ARGS 16
#define SYSMGR_EXEC_MAX_LEN 256
static bool sysmgr_exec_background = false;
static int sysmgr_exec_argc = 0;
static char sysmgr_exec_path[SYSMGR_EXEC_MAX_LEN];
static char sysmgr_exec_argv[SYSMGR_EXEC_MAX_ARGS][SYSMGR_EXEC_MAX_LEN];
static const char* sysmgr_exec_argv_ptrs[SYSMGR_EXEC_MAX_ARGS];

static void sysmgr_launch_process(const char* path,
                                  const char* const* argv,
                                  int argc,
                                  bool background,
                                  const char* err_prefix) {
    process_t* p = bin_create_process(path, argv, argc, false);
    if (!p) {
        if (err_prefix) {
            kprint(err_prefix);
        }
        keyboard_input_enabled = true;
        prompt_enabled = true;
        shell_suspended = false;
        sysmgr_request_prompt();
        return;
    }

    if (background) {
        kprintf("[bg] pid %u\n", p->pid);
        keyboard_input_enabled = true;
        prompt_enabled = true;
        shell_suspended = false;
        sysmgr_request_prompt();
        return;
    }

    proc_set_foreground_pid(p->pid);
}

void sysmgr_note_prompt(void) {
    sysmgr_prompt_pending = false;
    sysmgr_prompt_seq = sysmgr_output_seq;
    sysmgr_prompt_force = false;
}

void sysmgr_request_prompt(void) {
    sysmgr_prompt_pending = true;
    sysmgr_prompt_force = true;
}

void sysmgr_request_user_shell(bool background) {
    (void)background;
    sysmgr_user_shell_pending = true;
}

bool sysmgr_request_exec(const char* path, const char* const* argv, int argc, bool background) {
    if (!path || !argv || argc <= 0 || argc > SYSMGR_EXEC_MAX_ARGS) {
        return false;
    }
    if (sysmgr_exec_pending) {
        return false;
    }

    strncpy(sysmgr_exec_path, path, sizeof(sysmgr_exec_path) - 1);
    sysmgr_exec_path[sizeof(sysmgr_exec_path) - 1] = '\0';

    for (int i = 0; i < argc; i++) {
        if (!argv[i]) {
            return false;
        }
        strncpy(sysmgr_exec_argv[i], argv[i], sizeof(sysmgr_exec_argv[i]) - 1);
        sysmgr_exec_argv[i][sizeof(sysmgr_exec_argv[i]) - 1] = '\0';
        sysmgr_exec_argv_ptrs[i] = sysmgr_exec_argv[i];
    }
    sysmgr_exec_argc = argc;
    sysmgr_exec_background = background;
    sysmgr_exec_pending = true;
    return true;
}

void sysmgr_thread(void) {
    for (;;) {
        bool started = false;
        bool had_output = false;
        bool has_work = workqueue_pending();
        if (has_work) {
            sysmgr_console_begin(&started);
        }
        workqueue_run();
        if (has_work) {
            had_output = true;
        }
        if (xhci_take_rescan_pending()) {
            sysmgr_console_begin(&started);
            xhci_rescan_all_ports(false, false);
            had_output = true;
        }
        if (ehci_take_rescan_pending()) {
            sysmgr_console_begin(&started);
            ehci_rescan_all_ports(true);
            had_output = true;
        }
        if (ohci_take_rescan_pending()) {
            sysmgr_console_begin(&started);
            ohci_rescan_all_ports(true);
            had_output = true;
        }
        if (uhci_take_rescan_pending()) {
            sysmgr_console_begin(&started);
            uhci_rescan_all_ports();
            had_output = true;
        }
        if (proc_reap_is_pending()) {
            hal_disable_interrupts();
            proc_reap_background();
            hal_enable_interrupts();
        }
        if (sysmgr_exec_pending) {
            bool background = sysmgr_exec_background;
            int argc = sysmgr_exec_argc;
            const char* path = sysmgr_exec_path;
            const char* const* argv = sysmgr_exec_argv_ptrs;
            sysmgr_exec_pending = false;
            sysmgr_launch_process(path, argv, argc, background, "bin: failed to start\n");
            had_output = true;
        }
        if (sysmgr_user_shell_pending) {
            sysmgr_user_shell_pending = false;
            const char* path = "/cmd/shell.sys";
            const char* argv[] = { path };
            sysmgr_launch_process(path, argv, 1, false, "sh: failed to start /cmd/shell.sys\n");
            had_output = true;
        }
        timer_task_run_due();
        if (had_output) {
            sysmgr_prompt_pending = true;
            sysmgr_output_seq++;
        } else if (sysmgr_prompt_pending && sysmgr_console_active() &&
                   (sysmgr_prompt_force || sysmgr_prompt_seq != sysmgr_output_seq)) {
            prompt();
            sysmgr_prompt_pending = false;
            sysmgr_prompt_seq = sysmgr_output_seq;
            sysmgr_prompt_force = false;
        }
        hal_wait_for_interrupt();
    }
}

void sysmgr_idle_loop(void) {
    while (1) {
        (void)proc_start_reaper();
        hal_wait_for_interrupt();
        if (!proc_current()) {
            hal_disable_interrupts();
            proc_reap_background();
            process_t* next = proc_take_next();
            if (next) {
                bin_enter_process(next);
            }
            hal_enable_interrupts();
        }
    }
}
