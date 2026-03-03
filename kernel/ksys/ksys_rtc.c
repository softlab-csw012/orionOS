#include "ksys_rtc.h"
#include "../../cpu/ports.h"

static uint8_t cmos_read_u8(uint8_t reg) {
    port_byte_out(0x70, reg);
    return port_byte_in(0x71);
}

static uint8_t bcd_to_bin_u8(uint8_t val) {
    return (uint8_t)((val & 0x0Fu) + ((val >> 4) * 10u));
}

bool ksys_rtc_read(ksys_rtc_time_t* out) {
    if (!out) {
        return false;
    }

    uint8_t status_b = cmos_read_u8(0x0B);
    bool binary_mode = (status_b & 0x04u) != 0;

    uint8_t sec = cmos_read_u8(0x00);
    uint8_t min = cmos_read_u8(0x02);
    uint8_t hour = cmos_read_u8(0x04);
    uint8_t day = cmos_read_u8(0x07);
    uint8_t month = cmos_read_u8(0x08);
    uint8_t year = cmos_read_u8(0x09);

    if (!binary_mode) {
        sec = bcd_to_bin_u8(sec);
        min = bcd_to_bin_u8(min);
        hour = bcd_to_bin_u8(hour & 0x7Fu);
        day = bcd_to_bin_u8(day);
        month = bcd_to_bin_u8(month);
        year = bcd_to_bin_u8(year);
    }

    out->sec = sec;
    out->min = min;
    out->hour = hour;
    out->day = day;
    out->month = month;
    out->year = year;
    return true;
}
