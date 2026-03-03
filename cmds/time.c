#include "syscall.h"
#include "stdio.h"
#include "string.h"

static int is_leap_year_2000(int yy) {
    int year = 2000 + yy;
    return ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0));
}

int main(void) {
    sys_rtc_time_t t;
    if (!sys_rtc_read(&t)) {
        eprint("time: failed to read rtc\n");
        return 1;
    }

    t.hour = (uint8_t)(t.hour + 9u);
    if (t.hour >= 24u) {
        static const uint8_t days_in_month[12] =
            {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        uint8_t dim = days_in_month[(t.month > 0 && t.month <= 12) ? (t.month - 1) : 0];
        if (t.month == 2 && is_leap_year_2000(t.year)) {
            dim = 29;
        }

        t.hour = (uint8_t)(t.hour - 24u);
        t.day = (uint8_t)(t.day + 1u);
        if (t.day > dim) {
            t.day = 1;
            t.month = (uint8_t)(t.month + 1u);
            if (t.month > 12u) {
                t.month = 1;
                t.year = (uint8_t)(t.year + 1u);
            }
        }
    }

    printf("Time: %02d:%02d:%02d  Date: %02d/%02d/20%02d KST\n",
           (int)t.hour, (int)t.min, (int)t.sec,
           (int)t.day, (int)t.month, (int)t.year);
    return 0;
}

void _start(void) {
    int rc = main();
    sys_exit((uint32_t)rc);
}
