#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SECTOR_SIZE 512

extern uint32_t root_dir_cluster32;
extern uint32_t current_dir_cluster32;
extern uint8_t fat32_drive;

typedef struct __attribute__((packed)) {
    uint8_t  jmpBoot[3];       // JMP short instruction (EB 58 90 등)
    uint8_t  OEMName[8];       // OEM Name ("MSWIN4.1", "mkfs.fat", "ORIONOS ")
    uint16_t BytsPerSec;
    uint8_t  SecPerClus;
    uint16_t RsvdSecCnt;
    uint8_t  NumFATs;
    uint16_t RootEntCnt;
    uint16_t TotSec16;
    uint8_t  Media;
    uint16_t FATSz16;
    uint16_t SecPerTrk;
    uint16_t NumHeads;
    uint32_t HiddSec;
    uint32_t TotSec32;

    // FAT32 전용 확장 영역
    uint32_t FATSz32;
    uint16_t ExtFlags;
    uint16_t FSVer;
    uint32_t RootClus;
    uint16_t FSInfo;
    uint16_t BkBootSec;
    uint8_t  Reserved[12];
    uint8_t  DrvNum;
    uint8_t  Reserved1;
    uint8_t  BootSig;
    uint32_t VolID;
    uint8_t  VolLab[11];
    uint8_t  FilSysType[8];
} FAT32_BPB_t;

typedef struct {
    uint8_t Name[11];
    uint8_t Attr;
    uint8_t NTRes;
    uint8_t CrtTimeTenth;
    uint16_t CrtTime;
    uint16_t CrtDate;
    uint16_t LstAccDate;
    uint16_t FstClusHI;
    uint16_t WrtTime;
    uint16_t WrtDate;
    uint16_t FstClusLO;
    uint32_t FileSize;
} __attribute__((packed)) FAT32_DirEntry;

bool probe_fat32_pbr(uint8_t drive, uint32_t base_lba);
bool fat32_init(uint8_t drive, uint32_t lba_start);
void fat32_ls(const char* path);
int fat32_read_dir(uint32_t cluster, FAT32_DirEntry* out, uint32_t max);
int fat32_list_dir_lfn(uint32_t cluster, char* names, bool* is_dir, int max_entries, size_t name_len);
int fat32_read_file(const char* filename, uint8_t* buffer, uint32_t offset, uint32_t size);
bool fat32_find_file(const char* filename, FAT32_DirEntry* out_entry);
void fat32_cat(const char* filename);
bool fat32_rm(const char* filename);
bool fat32_exists(const char* filename);
int fat32_read_file_by_name(const char* filename, uint8_t* buffer, uint32_t bufsize);
bool fat32_cp(const char* src, const char* dst);
bool fat32_mv(const char* src, const char* dst);
uint32_t fat32_get_file_size(const char* filename);
bool fat32_read_file_partial(const char* filename, uint32_t offset, uint8_t* out_buf, uint32_t size);
uint32_t fat32_find_dir_cluster(uint32_t start_cluster, const char* dirname);
bool fat32_mkdir(const char* dirname);
bool fat32_rmdir(const char* dirname);
uint32_t fat32_resolve_dir(const char* path);
bool fat32_cd(const char* path);
bool fat32_read_file_range(FAT32_DirEntry* entry, uint32_t offset, uint8_t* out_buf, uint32_t size);
uint32_t fat32_total_clusters();
uint32_t fat32_free_clusters();

bool fat32_create_file(const char* fullpath);
bool fat32_write_file(const char* fullpath, const uint8_t* data, uint32_t size);
bool fat32_format_at(uint8_t drive, uint32_t base_lba, uint32_t total_sectors, const char* label);
bool fat32_format(uint8_t drive, const char* label);

#endif
