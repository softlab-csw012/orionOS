#ifndef FSCMD_H
#define FSCMD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// 지원되는 파일시스템 타입
typedef enum {
    FS_NONE = 0,
    FS_DEVFS,
    FS_FAT16,
    FS_FAT32,
    FS_XVFS
} fs_type_t;

typedef struct {
    fs_type_t fs;
    int drive;
    char target[32];
} fscmd_mount_info_t;

// 현재 마운트된 드라이브와 FS 상태
extern fs_type_t current_fs;
extern int current_drive;
extern char current_path[256];

#define FSCMD_MODE_OWNER_R  0x01u
#define FSCMD_MODE_OWNER_W  0x02u
#define FSCMD_MODE_OWNER_RW (FSCMD_MODE_OWNER_R | FSCMD_MODE_OWNER_W)

// 명령어 (공통 인터페이스)
const char* fs_to_string(fs_type_t type);
void fscmd_set_fat_mount_policy(uint32_t owner_uid, uint32_t owner_gid, uint8_t mode);
void fscmd_get_fat_mount_policy(uint32_t* out_owner_uid, uint32_t* out_owner_gid, uint8_t* out_mode);
bool fscmd_get_path_meta(const char* path, uint32_t* out_owner_uid, uint32_t* out_owner_gid, uint8_t* out_mode);
void fscmd_reset_path(void);
void fscmd_ls(const char* path);
int fscmd_list_dir(const char* path, char* names, uint8_t* is_dir, uint32_t max_entries, size_t name_len);
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
bool fscmd_write_file_at(const char* filename, uint32_t offset, const char* data, uint32_t len);
bool fscmd_append_file(const char* filename, const char* data, uint32_t len);
void fscmd_write_progress_begin(const char* label, uint32_t total);
void fscmd_write_progress_update(uint32_t written);
void fscmd_write_progress_finish(bool success);
bool fscmd_read_file_range(void* entry, uint32_t offset, uint8_t* out_buf, uint32_t size);
bool fscmd_format(uint8_t drive, const char* fs);
void fscmd_set_single_root_mode(bool enabled);
bool fscmd_get_single_root_mode(void);
bool fscmd_mount_drive_at(int drive, const char* target);
bool fscmd_mount_devfs_at(const char* target);
bool fscmd_unmount(const char* target);
void fscmd_clear_mounts(void);
int fscmd_list_mounts(fscmd_mount_info_t* out, int max_entries);
bool fscmd_resolve_devfs_path(const char* path, char* out_subpath, size_t out_len);

#endif
