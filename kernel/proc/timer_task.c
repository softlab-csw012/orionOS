#include "timer_task.h"
#include "../../cpu/timer.h"
#include "../../drivers/hal.h"
#include "../../libc/string.h"

#define MAX_TIMER_TASKS 32
#define TIMER_HZ 100u

typedef struct {
    uint32_t id;
    uint32_t due_tick;
    uint32_t interval_ticks;
    timer_task_fn fn;
    void* ctx;
    bool active;
} timer_task_t;

static timer_task_t tasks[MAX_TIMER_TASKS];
static uint32_t next_id = 1;

static bool tick_elapsed(uint32_t now, uint32_t when) {
    return (int32_t)(now - when) >= 0;
}

void timer_task_init(void) {
    memset(tasks, 0, sizeof(tasks));
    next_id = 1;
}

static int timer_task_alloc(uint32_t delay_ticks, uint32_t interval_ticks,
                            timer_task_fn fn, void* ctx) {
    if (!fn) {
        return -1;
    }
    if (delay_ticks == 0) {
        delay_ticks = 1;
    }

    int slot = -1;
    hal_disable_interrupts();
    for (int i = 0; i < MAX_TIMER_TASKS; i++) {
        if (!tasks[i].active) {
            slot = i;
            break;
        }
    }
    if (slot >= 0) {
        uint32_t id = next_id++;
        if (next_id == 0) {
            next_id = 1;
        }
        tasks[slot].id = id;
        tasks[slot].due_tick = tick + delay_ticks;
        tasks[slot].interval_ticks = interval_ticks;
        tasks[slot].fn = fn;
        tasks[slot].ctx = ctx;
        tasks[slot].active = true;
    }
    hal_enable_interrupts();

    if (slot < 0) {
        return -1;
    }
    return (int)tasks[slot].id;
}

int timer_task_schedule_ticks(uint32_t delay_ticks, uint32_t interval_ticks,
                              timer_task_fn fn, void* ctx) {
    return timer_task_alloc(delay_ticks, interval_ticks, fn, ctx);
}

static uint32_t ms_to_ticks(uint32_t ms) {
    uint32_t q = ms / 1000u;
    uint32_t r = ms % 1000u;
    if (TIMER_HZ == 0) {
        return 1;
    }
    if (q > UINT32_MAX / TIMER_HZ) {
        return UINT32_MAX;
    }
    uint32_t ticks = q * TIMER_HZ;

    if (r) {
        uint32_t rem = r * TIMER_HZ;
        ticks += rem / 1000u;
        if ((rem % 1000u) != 0) {
            ticks++;
        }
    }

    if (ticks == 0) {
        ticks = 1;
    }
    return ticks;
}

int timer_task_schedule_ms(uint32_t delay_ms, uint32_t interval_ms,
                           timer_task_fn fn, void* ctx) {
    uint32_t delay_ticks = ms_to_ticks(delay_ms);
    uint32_t interval_ticks = interval_ms ? ms_to_ticks(interval_ms) : 0;
    return timer_task_alloc(delay_ticks, interval_ticks, fn, ctx);
}

bool timer_task_cancel(uint32_t id) {
    if (id == 0) {
        return false;
    }
    bool removed = false;
    hal_disable_interrupts();
    for (int i = 0; i < MAX_TIMER_TASKS; i++) {
        if (tasks[i].active && tasks[i].id == id) {
            memset(&tasks[i], 0, sizeof(tasks[i]));
            removed = true;
            break;
        }
    }
    hal_enable_interrupts();
    return removed;
}

bool timer_task_due(void) {
    bool due = false;
    hal_disable_interrupts();
    uint32_t now = tick;
    for (int i = 0; i < MAX_TIMER_TASKS; i++) {
        if (!tasks[i].active) {
            continue;
        }
        if (tick_elapsed(now, tasks[i].due_tick)) {
            due = true;
            break;
        }
    }
    hal_enable_interrupts();
    return due;
}

void timer_task_run_due(void) {
    for (;;) {
        timer_task_fn fn = NULL;
        void* ctx = NULL;
        uint32_t interval = 0;

        hal_disable_interrupts();
        uint32_t now = tick;
        for (int i = 0; i < MAX_TIMER_TASKS; i++) {
            if (!tasks[i].active) {
                continue;
            }
            if (!tick_elapsed(now, tasks[i].due_tick)) {
                continue;
            }
            fn = tasks[i].fn;
            ctx = tasks[i].ctx;
            interval = tasks[i].interval_ticks;
            if (interval == 0) {
                memset(&tasks[i], 0, sizeof(tasks[i]));
            } else {
                tasks[i].due_tick = now + interval;
            }
            break;
        }
        hal_enable_interrupts();

        if (!fn) {
            break;
        }
        fn(ctx);
    }
}
