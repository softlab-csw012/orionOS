#ifndef DRIVERS_HAL_H
#define DRIVERS_HAL_H

#include <stdint.h>

uint8_t hal_in8(uint16_t port);
void hal_out8(uint16_t port, uint8_t data);
uint16_t hal_in16(uint16_t port);
void hal_out16(uint16_t port, uint16_t data);
uint32_t hal_in32(uint16_t port);
void hal_out32(uint16_t port, uint32_t data);

void hal_enable_interrupts(void);
void hal_disable_interrupts(void);
void hal_halt(void);
void hal_wait_for_interrupt(void);
void hal_pause(void);
void hal_invlpg(const void* addr);
void hal_wbinvd(void);

#endif
