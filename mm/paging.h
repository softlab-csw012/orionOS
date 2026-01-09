// mm/paging.h
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../cpu/isr.h"

#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define PAGE_USER    0x4
#define PAGE_PWT     (1u << 3)
#define PAGE_PCD     (1u << 4)
#define PAGE_PAT     (1u << 7)
#define PAGE_DIRECTORY_ADDR 0x80000
#define PAGE_TABLE0_ADDR    0x81000
#define PAGE_SIZE 4096

extern uint32_t page_directory[1024];

void paging_init(void);
void map_page(uint32_t* dir, uint32_t virt, uint32_t phys, uint32_t flags);
void dump_mapping(uint32_t addr);

// Minimal VMM helpers (kernel address space)
int vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags);
int vmm_map_page_alloc(uint32_t virt, uint32_t flags, uint32_t* out_phys);
int vmm_map_range_alloc(uint32_t virt, size_t size, uint32_t flags);
int vmm_virt_to_phys(uint32_t virt, uint32_t* out_phys);
int vmm_mark_user_range(uint32_t virt, size_t size);
bool paging_pat_wc_enabled(void);
