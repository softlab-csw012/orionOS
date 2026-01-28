#ifndef FAT16_H
#define FAT16_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define FAT_ENTRY_SIZE 2  // FAT16이므로 엔트리 하나당 2바이트
#define CLUSTER_EOF 0xFFF8  // FAT16 EOF 마커 이상이면 끝
#define SECTOR_SIZE 512

extern uint32_t fat_start_lba;
extern uint32_t data_region_lba;
extern uint32_t root_dir_lba;
extern uint32_t root_dir_cluster16;
extern uint16_t current_dir_cluster16;
extern int fat16_drive;
extern uint16_t current_dir_cluster;

typedef struct __attribute__((packed)) {
    uint8_t  jmpBoot[3];       // JMP instruction to boot code
    uint8_t  OEMName[8];       // OEM name (optional)
    uint16_t BytsPerSec;       // Bytes per sector
    uint8_t  SecPerClus;       // Sectors per cluster
    uint16_t RsvdSecCnt;       // Reserved sector count
    uint8_t  NumFATs;          // Number of FAT tables
    uint16_t RootEntCnt;       // Max root dir entries
    uint16_t TotSec16;         // Total sectors (if < 65536)
    uint8_t  Media;            // Media descriptor
    uint16_t FATSz16;          // Sectors per FAT
    uint16_t SecPerTrk;        // Sectors per track
    uint16_t NumHeads;         // Number of heads
    uint32_t HiddSec;          // Hidden sectors
    uint32_t TotSec32;         // Total sectors (if >= 65536)
    // FAT16 Extended Boot Record
    uint8_t  DrvNum;
    uint8_t  Reserved1;
    uint8_t  BootSig;
    uint32_t VolID;
    uint8_t  VolLab[11];
    uint8_t  FilSysType[8];
} FAT16_BPB_t;

typedef struct __attribute__((packed)) {
    char Name[8];
    char Ext[3];
    uint8_t Attr;
    uint8_t NTRes;
    uint8_t CrtTimeTenth;
    uint16_t CrtTime;
    uint16_t CrtDate;
    uint16_t LstAccDate;
    uint16_t FstClusHI;
    uint16_t WrtTime;
    uint16_t WrtDate;
    uint16_t FirstCluster;
    uint32_t FileSize;
} FAT16_DirEntry;

extern FAT16_BPB_t fat16_bpb;

/* 초기화 및 기타 함수 */
bool fat16_init(uint8_t drive, uint32_t base_lba);
uint16_t fat16_next_cluster(uint16_t cluster);
uint16_t fat16_get_fat_entry(uint16_t cluster);
void fat16_set_fat_entry(uint16_t cluster, uint16_t value);
void format_filename(const char* input, char* name, char* ext);
bool fat16_find_file(const char* filename, FAT16_DirEntry* out_entry);
bool fat16_find_file_path(const char* path, FAT16_DirEntry* out_entry);
bool fat16_find_entry(const char* name, uint16_t dir_cluster, FAT16_DirEntry* out);
bool fat16_find_file_raw(const char* filename, uint32_t* sector_out, uint16_t* offset_out);
uint16_t fat16_resolve_dir(const char* path);
void fat16_ls(const char* path);
void fat16_cat(const char* filename);
int fat16_create_file(const char* filename, int initial_size);
int fat16_write_file(const char* filename, const char* data, int size);
bool fat16_rm(const char* filename);
int fat16_read_file(FAT16_DirEntry* entry, uint8_t* out_buf, uint32_t offset, uint32_t size);
bool fat16_exists(const char* filename);
int fat16_read_dir(uint16_t cluster, FAT16_DirEntry* out_entries, int max_entries);
int fat16_list_dir_lfn(uint16_t cluster, char* names, bool* is_dir, int max_entries, size_t name_len);
bool compare_filename(const char* name, const char* entry_name, const char* entry_ext);
uint32_t cluster_to_lba(uint16_t cluster);
bool fat16_cd(const char* dirname);
bool _root_find_free_pos(uint32_t* out_lba, uint16_t* out_off);
bool _root_find_entry_pos(const char* filename, uint32_t* out_lba, uint16_t* out_off, FAT16_DirEntry* out_entry);
bool fat16_mkdir(const char* dirname);
bool fat16_rmdir(const char* dirname);
void _get_fullname(FAT16_DirEntry* entry, char* out);
bool fat16_read_file_partial(const char* filename, uint32_t offset, uint8_t* out_buf, uint32_t size);
bool fat16_read_file_range(FAT16_DirEntry* entry, uint32_t offset, uint8_t* out_buf, uint32_t size);
uint32_t fat16_get_file_size(const char* filename);
int fat16_read_file_by_name(const char* fname, uint8_t* out_buf, uint32_t max_size);
bool fat16_cp(const char* src, const char* dst);
bool fat16_mv(const char* src, const char* dst);
bool fat16_rename(const char* oldname, const char* newname);
uint32_t fat16_free_clusters();
uint32_t fat16_total_clusters();
bool fat16_format_at(uint8_t drive, uint32_t base_lba, uint32_t total_sectors, const char* label);
bool fat16_format(uint8_t drive, const char* label);

#endif
