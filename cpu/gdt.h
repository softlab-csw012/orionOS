#pragma once
#include <stdint.h>

// GDT 엔트리 구조체 (Packed)
struct gdt_entry {
    uint16_t limit_low;     // 세그먼트 한계 (하위 16비트)
    uint16_t base_low;      // 베이스 주소 (하위 16비트)
    uint8_t  base_middle;   // 베이스 주소 (중간 8비트)
    uint8_t  access;        // 접근 플래그
    uint8_t  granularity;   // Granularity + limit 상위 4비트
    uint8_t  base_high;     // 베이스 주소 (상위 8비트)
} __attribute__((packed));

// GDTR 레지스터용 포인터
struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

void gdt_install(void);
void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran);
