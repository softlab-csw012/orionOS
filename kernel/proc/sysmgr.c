#include "sysmgr.h"
#include "proc.h"
#include "timer_task.h"
#include "workqueue.h"
#include "../bin.h"
#include "../kernel.h"
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

void sysmgr_note_prompt(void) {
    sysmgr_prompt_pending = false;
    sysmgr_prompt_seq = sysmgr_output_seq;
    sysmgr_prompt_force = false;
}

void sysmgr_request_prompt(void) {
    sysmgr_prompt_pending = true;
    sysmgr_prompt_force = true;
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
