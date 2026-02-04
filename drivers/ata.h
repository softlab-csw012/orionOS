#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

bool ata_present(uint8_t drive);
bool ata_read(uint8_t drive, uint32_t lba, uint16_t count, uint8_t* buffer);
bool ata_write(uint8_t drive, uint32_t lba, uint16_t count, const uint8_t* buffer);
bool ata_read_sector(uint32_t drive, uint32_t lba, uint8_t* buffer);
bool ata_write_sector(uint32_t drive, uint32_t lba, const uint8_t* buffer);
void ata_init_all(void);
uint32_t ata_get_sector_count(uint8_t drive);
bool ata_flush_cache(uint8_t drive);

typedef enum {
    ATA_BACKEND_NONE = 0,
    ATA_BACKEND_AHCI,
    ATA_BACKEND_PATA,
    ATA_BACKEND_USB,
    ATA_BACKEND_RAMDISK,
} ata_backend_t;

void ata_refresh_drive_map(void);
bool ata_drive_backend(uint8_t drive, ata_backend_t* out_type, int* out_index);
bool ata_drive_model(uint8_t drive, char* out, size_t out_len);
