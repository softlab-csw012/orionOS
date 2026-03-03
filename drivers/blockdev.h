#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    BLOCKDEV_BACKEND_NONE = 0,
    BLOCKDEV_BACKEND_AHCI,
    BLOCKDEV_BACKEND_PATA,
    BLOCKDEV_BACKEND_USB,
    BLOCKDEV_BACKEND_RAMDISK,
} blockdev_backend_t;

bool blockdev_present(uint8_t drive);
bool blockdev_read(uint8_t drive, uint32_t lba, uint16_t count, uint8_t* buffer);
bool blockdev_write(uint8_t drive, uint32_t lba, uint16_t count, const uint8_t* buffer);
bool blockdev_read_sector(uint32_t drive, uint32_t lba, uint8_t* buffer);
bool blockdev_write_sector(uint32_t drive, uint32_t lba, const uint8_t* buffer);
void blockdev_refresh_drive_map(void);
uint32_t blockdev_get_sector_count(uint8_t drive);
bool blockdev_flush_cache(uint8_t drive);
bool blockdev_drive_model(uint8_t drive, char* out, size_t out_len);
bool blockdev_drive_backend(uint8_t drive, blockdev_backend_t* out_type, int* out_index);
const char* blockdev_backend_name(blockdev_backend_t backend);
