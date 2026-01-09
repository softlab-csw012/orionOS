// fs_quick.h
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    FSQ_NONE = 0,        // 읽기 실패/장치 없음
    FSQ_UNKNOWN,
    FSQ_MBR,             // MBR만 있음(종류 모름)
    FSQ_FAT16,
    FSQ_FAT32,
    FSQ_XVFS
} fs_kind_t;

bool ata_present(uint8_t drive);              // IDENTIFY로만 판단
bool disk_has_55aa(uint8_t drive, uint32_t lba);
fs_kind_t fs_quick_probe(uint8_t drive, uint32_t *out_base_lba); // BPB 파싱X
