#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t sec;
    uint8_t min;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint8_t year;
} ksys_rtc_time_t;

bool ksys_rtc_read(ksys_rtc_time_t* out);
