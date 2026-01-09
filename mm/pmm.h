// mm/pmm.h
#pragma once
#include <stdint.h>
#include <stddef.h>

// 4KB 페이지 크기
#define PAGE_SIZE 4096

// PMM API
void pmm_init(uint32_t mb_info_addr);     // Multiboot2 E820 맵을 기반으로 초기화
void* pmm_alloc_page();                   // 4KB 페이지 하나 할당
void  pmm_free_page(void* addr);          // 페이지 반환
void  pmm_reserve_region(uint32_t start, uint32_t end); // 주어진 물리 영역을 PMM에서 제외
uint64_t pmm_get_total_memory();          // 전체 물리 메모리 용량
uint64_t pmm_get_free_memory();           // 남은 메모리 용량
