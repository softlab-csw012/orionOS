#ifndef DISK_H
#define DISK_H

#include <stdbool.h>   // bool
#include <stdint.h>    // uint8_t, uint32_t
#include "fat16.h"     // FAT16_BPB_t 구조체

#define MAX_DISKS 8

typedef struct {
    bool present;
    uint8_t id;
    char fs_type[8];
    FAT16_BPB_t bpb;
    uint32_t base_lba;
    uint32_t fat_start_lba;
    uint32_t root_dir_lba;
    uint32_t data_region_lba;
    uint32_t root_dir_sectors;
} DiskInfo;

extern DiskInfo disks[MAX_DISKS];   // 여기서 extern
extern int disk_count;

void detect_disks_quick(void);
void cmd_disk_ls();
void disk_request_rescan(void);

#endif
