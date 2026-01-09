#ifndef XVFS_H
#define XVFS_H

#include <stdint.h>
#include <stdbool.h>

#define XVFS_MAGIC 0x58564653  // 'XVFS'
#define XVFS_BLOCK_SIZE 512
#define XVFS_MAX_FILES (XVFS_BLOCK_SIZE / sizeof(XVFS_FileEntry))
#define XVFS_MAX_NAME 16
#define CAT_BUF_SIZE 4096

extern uint8_t xvfs_drive;

typedef struct {
    uint32_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t bitmap_start;
    uint32_t data_start;
    uint32_t free_blocks;
    uint32_t root_dir_block;
} __attribute__((packed)) XVFS_Superblock;

typedef struct {
    char name[XVFS_MAX_NAME];
    uint32_t start;
    uint32_t size;
    uint8_t attr; // 0 = file, 1 = dir
} __attribute__((packed)) XVFS_FileEntry;

bool xvfs_init(uint8_t drive, uint32_t base_lba);
void xvfs_ls(const char* path);
bool xvfs_find_entry(const char* path, XVFS_FileEntry* out_entry);
bool xvfs_find_file(const char* path, XVFS_FileEntry* out_entry);
bool xvfs_is_dir(const char* path);
int xvfs_read_dir_entries(const char* path, XVFS_FileEntry* out_entries, uint32_t max_entries);
void xvfs_cat(const char* filename);
bool xvfs_rm(const char* filename);
bool xvfs_exists(const char* filename);
uint32_t xvfs_read_file_by_name(const char* filename, uint8_t* outbuf, uint32_t maxsize);
bool xvfs_cp(const char* src, const char* dst);
bool xvfs_mv(const char* src, const char* dst);
bool xvfs_read_file_range(XVFS_FileEntry* entry, uint32_t offset, uint8_t* out_buf, uint32_t size);
uint32_t xvfs_get_file_size(const char* path);
int xvfs_read_file(XVFS_FileEntry* entry, uint8_t* out_buf, uint32_t offset, uint32_t size);
bool xvfs_read_file_partial(const char* path, uint32_t offset, uint8_t* out_buf, uint32_t size);
uint32_t xvfs_total_clusters();
uint32_t xvfs_free_clusters();

bool xvfs_mkdir(const char* name);
bool xvfs_mkdir_path(const char* path);
bool xvfs_cd(const char* path);
bool xvfs_rmdir(const char* path);

bool xvfs_create_file(const char* name, const uint8_t* data, uint32_t size);
bool xvfs_write_file(const char* name, const uint8_t* data, uint32_t size);
bool xvfs_format_at(uint8_t drive, uint32_t base_lba, uint32_t total_sectors);
bool xvfs_format(uint8_t drive);

#endif
