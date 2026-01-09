#include "hal.h"
#include "spk.h"

#include <stdint.h>

#define PIT_FREQ     1193180
#define PIT_CHANNEL2 0x42
#define PIT_CMD_PORT 0x43
#define SPKR_CTRL    0x61

//beep
void beep_on(uint32_t freq) {
    if (freq < 400)
        freq = 400;        // 🔥 최소 가청 보정
    if (freq > 5000)
        freq = 5000;       // 선택적 상한

    uint32_t divisor = PIT_FREQ / freq;

    hal_out8(PIT_CMD_PORT, 0xB6);
    hal_out8(PIT_CHANNEL2, divisor & 0xFF);
    hal_out8(PIT_CHANNEL2, (divisor >> 8) & 0xFF);

    uint8_t tmp = hal_in8(SPKR_CTRL);
    hal_out8(SPKR_CTRL, tmp | 3);
}

void beep_off() {
    uint8_t tmp = hal_in8(SPKR_CTRL) & 0xFC; // 비트 0,1 끄기
    hal_out8(SPKR_CTRL, tmp);
}

void beep(uint32_t freq, uint32_t ms) {
    beep_on(freq);
    for (volatile uint32_t i = 0; i < ms * 1000; i++);  // 간단한 지연 (정밀 X)
    beep_off();
}
