// mm/paging.h
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAGE_PRESENT 0x001u
#define PAGE_RW      0x002u
#define PAGE_USER    0x004u
#define PAGE_PWT     0x008u
#define PAGE_PCD     0x010u
#define PAGE_PAT     0x080u
#define PAGE_DIRECTORY_ADDR 0x80000
#define PAGE_TABLE0_ADDR    0x81000
#define PAGE_SIZE 4096

extern uint64_t page_directory[512];

void paging_init(void);
void map_page(void* dir, uint32_t virt, uint32_t phys, uint32_t flags);
void dump_mapping(uint32_t addr);
void* paging_kernel_dir(void);
uint32_t paging_kernel_dir_phys(void);
void* paging_current_dir(void);
uint32_t paging_current_dir_phys(void);
void paging_set_current_dir(void* dir, uint32_t phys);
void* paging_create_user_dir(uint32_t* out_phys);
void paging_destroy_user_dir(void* dir, uint32_t phys);

// Minimal VMM helpers (kernel address space)
int vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags);
int vmm_map_page_alloc(uint32_t virt, uint32_t flags, uint32_t* out_phys);
int vmm_map_range_alloc(uint32_t virt, size_t size, uint32_t flags);
int vmm_virt_to_phys(uint32_t virt, uint32_t* out_phys);
int vmm_query_page(uint32_t virt, uint32_t* out_phys, uint32_t* out_flags);
int vmm_mark_user_range(uint32_t virt, size_t size);
bool paging_pat_wc_enabled(void);
uint32_t paging_wc_cache_flags(void);
