#ifndef FSCMD_H
#define FSCMD_H

#include <stdint.h>
#include <stdbool.h>

// 지원되는 파일시스템 타입
typedef enum {
    FS_NONE = 0,
    FS_FAT16,
    FS_FAT32,
    FS_XVFS
} fs_type_t;

// 현재 마운트된 드라이브와 FS 상태
extern fs_type_t current_fs;
extern int current_drive;
extern char current_path[256];

// 명령어 (공통 인터페이스)
const char* fs_to_string(fs_type_t type);
void fscmd_reset_path(void);
void fscmd_ls(const char* path);
void fscmd_cat(const char* path);
bool fscmd_rm(const char* path);
bool fscmd_exists(const char* path);
int fscmd_read_file_by_name(const char* path, uint8_t* buf, uint32_t size);
bool fscmd_cp(const char* src, const char* dst);
bool fscmd_mv(const char* src, const char* dst);
uint32_t fscmd_get_file_size(const char* filename);
bool fscmd_read_file_partial(const char* filename, uint32_t offset, uint8_t* buf, uint32_t size);
int fscmd_read_file(const char* filename, uint8_t* buffer, uint32_t offset, uint32_t size);
bool fscmd_mkdir(const char* dirname);
bool fscmd_cd(const char* path);
bool fscmd_rmdir(const char* dirname);
bool fscmd_find_file(const char* path, void* out_entry);
bool fscmd_write_file(const char* filename, const char* data, uint32_t len);
void fscmd_write_progress_begin(const char* label, uint32_t total);
void fscmd_write_progress_update(uint32_t written);
void fscmd_write_progress_finish(bool success);
bool fscmd_read_file_range(void* entry, uint32_t offset, uint8_t* out_buf, uint32_t size);
bool fscmd_format(uint8_t drive, const char* fs);

#endif
