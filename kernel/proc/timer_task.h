#ifndef TIMER_TASK_H
#define TIMER_TASK_H

#include <stdint.h>
#include <stdbool.h>

typedef void (*timer_task_fn)(void* ctx);

void timer_task_init(void);
int timer_task_schedule_ticks(uint32_t delay_ticks, uint32_t interval_ticks,
                              timer_task_fn fn, void* ctx);
int timer_task_schedule_ms(uint32_t delay_ms, uint32_t interval_ms,
                           timer_task_fn fn, void* ctx);
bool timer_task_cancel(uint32_t id);
bool timer_task_due(void);
void timer_task_run_due(void);

#endif
