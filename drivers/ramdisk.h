#pragma once
#include <stdint.h>
#include <stdbool.h>

#define RAMDISK_SECTOR_SIZE 512u
#define RAMDISK_DRIVE_AUTO 0xFF

bool ramdisk_attach(uint8_t drive, uint8_t* data, uint32_t size_bytes, uint8_t* out_drive);
bool ramdisk_present(uint8_t drive);
bool ramdisk_read(uint8_t drive, uint32_t lba, uint16_t count, uint8_t* buffer);
bool ramdisk_write(uint8_t drive, uint32_t lba, uint16_t count, const uint8_t* buffer);
uint32_t ramdisk_get_sector_count(uint8_t drive);
uint32_t ramdisk_get_size_bytes(uint8_t drive);
const uint8_t* ramdisk_data(uint8_t drive);
int ramdisk_drive_id(void);
