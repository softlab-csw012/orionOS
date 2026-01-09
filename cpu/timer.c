#include "timer.h"
#include "isr.h"
#include "ports.h"
#include "../drivers/usb/usb.h"
#include "../drivers/usb/uhci.h"
#include "../drivers/screen.h"
#include "../libc/function.h"
#include "../kernel/proc/proc.h"

uint32_t tick = 0;
static uint32_t timer_freq_hz = 100;
static uint32_t slice_ticks = 0;
static uint32_t slice_pid = 0;

#define PROC_TIME_SLICE_TICKS 5u

static void timer_callback(registers_t *regs) {
    tick++;
    screen_cursor_blink_tick();
    usb_poll();
    uhci_poll();

    uint32_t pid = proc_current_pid();
    if (pid == 0) {
        slice_ticks = 0;
        slice_pid = 0;
        UNUSED(regs);
        return;
    }

    if (pid != slice_pid) {
        slice_ticks = 0;
        slice_pid = pid;
    }

    slice_ticks++;
    if (slice_ticks >= PROC_TIME_SLICE_TICKS) {
        slice_ticks = 0;
        proc_schedule(regs, true);
    }
}

uint32_t uptime_seconds() {
    return timer_freq_hz ? (tick / timer_freq_hz) : 0;
}

uint32_t timer_frequency(void) {
    return timer_freq_hz;
}

void init_timer(uint32_t freq) {
    timer_freq_hz = freq ? freq : 100u;

    /* Install the function we just wrote */
    register_interrupt_handler(IRQ0, timer_callback);

    /* Get the PIT value: hardware clock at 1193180 Hz */
    uint32_t divisor = 1193180 / timer_freq_hz;
    uint8_t low  = (uint8_t)(divisor & 0xFF);
    uint8_t high = (uint8_t)( (divisor >> 8) & 0xFF);
    /* Send the command */
    port_byte_out(0x43, 0x36); /* Command port */
    port_byte_out(0x40, low);
    port_byte_out(0x40, high);
}
