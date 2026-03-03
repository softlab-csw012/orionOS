#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LIMINE_MEMMAP_USABLE                  0
#define LIMINE_MEMMAP_RESERVED                1
#define LIMINE_MEMMAP_ACPI_RECLAIMABLE        2
#define LIMINE_MEMMAP_ACPI_NVS                3
#define LIMINE_MEMMAP_BAD_MEMORY              4
#define LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE  5
#define LIMINE_MEMMAP_KERNEL_AND_MODULES      6
#define LIMINE_MEMMAP_FRAMEBUFFER             7

typedef struct limine_memmap_entry {
    uint64_t base;
    uint64_t length;
    uint64_t type;
} limine_memmap_entry_t;

typedef struct limine_memmap_response {
    uint64_t revision;
    uint64_t entry_count;
    limine_memmap_entry_t** entries;
} limine_memmap_response_t;

typedef struct limine_file {
    uint64_t revision;
    void* address;
    uint64_t size;
    char* path;
    char* cmdline;
    uint32_t media_type;
    uint32_t unused;
    uint32_t tftp_ip;
    uint32_t tftp_port;
    uint32_t partition_index;
    uint32_t mbr_disk_id;
    uint8_t gpt_disk_uuid[16];
    uint8_t gpt_part_uuid[16];
    uint8_t part_uuid[16];
} limine_file_t;

typedef struct limine_module_response {
    uint64_t revision;
    uint64_t module_count;
    limine_file_t** modules;
} limine_module_response_t;

typedef struct limine_framebuffer {
    void* address;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    uint8_t memory_model;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;
    uint8_t unused[7];
    uint64_t edid_size;
    void* edid;
} limine_framebuffer_t;

typedef struct limine_framebuffer_response {
    uint64_t revision;
    uint64_t framebuffer_count;
    limine_framebuffer_t** framebuffers;
} limine_framebuffer_response_t;

typedef struct limine_executable_address_response {
    uint64_t revision;
    uint64_t physical_base;
    uint64_t virtual_base;
} limine_executable_address_response_t;

extern volatile limine_memmap_response_t* limine_memmap_response_slot __asm__("limine_memmap_response");
extern volatile limine_module_response_t* limine_module_response_slot __asm__("limine_module_response");
extern volatile limine_framebuffer_response_t* limine_framebuffer_response_slot __asm__("limine_framebuffer_response");
extern volatile limine_executable_address_response_t* limine_executable_address_response_slot __asm__("limine_executable_address_response");

volatile limine_memmap_response_t* limine_memmap_response_ptr(void);
volatile limine_module_response_t* limine_module_response_ptr(void);
volatile limine_framebuffer_response_t* limine_framebuffer_response_ptr(void);
volatile limine_executable_address_response_t* limine_executable_address_response_ptr(void);

#define limine_memmap_response limine_memmap_response_ptr()
#define limine_module_response limine_module_response_ptr()
#define limine_framebuffer_response limine_framebuffer_response_ptr()
#define limine_executable_address_response limine_executable_address_response_ptr()

void limine_snapshot_bootinfo(void);

static inline bool limine_bootinfo_ready(void) {
    return limine_memmap_response != NULL;
}
