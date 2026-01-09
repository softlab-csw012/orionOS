#pragma once
#include <stdint.h>
#include <stdbool.h>

bool ata_present(uint8_t drive);
bool ata_read(uint8_t drive, uint32_t lba, uint16_t count, uint8_t* buffer);
bool ata_write(uint8_t drive, uint32_t lba, uint16_t count, const uint8_t* buffer);
bool ata_read_sector(uint32_t drive, uint32_t lba, uint8_t* buffer);
bool ata_write_sector(uint32_t drive, uint32_t lba, const uint8_t* buffer);
void ata_init_all(void);
uint32_t ata_get_sector_count(uint8_t drive);
bool ata_flush_cache(uint8_t drive);
