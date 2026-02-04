#include "hal.h"
#include "../cpu/ports.h"

uint8_t hal_in8(uint16_t port) {
    return port_byte_in(port);
}

void hal_out8(uint16_t port, uint8_t data) {
    port_byte_out(port, data);
}

uint16_t hal_in16(uint16_t port) {
    return port_word_in(port);
}

void hal_out16(uint16_t port, uint16_t data) {
    port_word_out(port, data);
}

uint32_t hal_in32(uint16_t port) {
    return port_dword_in(port);
}

void hal_out32(uint16_t port, uint32_t data) {
    port_dword_out(port, data);
}

void hal_enable_interrupts(void) {
    asm volatile("sti");
}

void hal_disable_interrupts(void) {
    asm volatile("cli");
}

void hal_halt(void) {
    asm volatile("hlt");
}

void hal_wait_for_interrupt(void) {
    asm volatile("sti\n\thlt");
}

void hal_pause(void) {
    asm volatile("pause");
}

void hal_invlpg(const void* addr) {
    asm volatile("invlpg (%0)" :: "r"(addr) : "memory");
}

void hal_wbinvd(void) {
    asm volatile("wbinvd");
}
