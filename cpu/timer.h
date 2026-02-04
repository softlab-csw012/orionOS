#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

extern uint32_t tick;
void init_timer(uint32_t freq);
uint32_t uptime_seconds(void);
uint32_t timer_frequency(void);

#endif
