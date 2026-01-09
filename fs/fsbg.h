#ifndef FSBG_H
#define FSBG_H

#include <stdint.h>
#include <stdbool.h>

bool fat16_create_file_compat(const char*, const uint8_t*, uint32_t);
bool fat32_create_file_compat(const char*, const uint8_t*, uint32_t);

typedef struct {
    const char* name;
    bool (*exists)(const char* path);
    uint32_t (*get_size)(const char* path);
    bool (*create)(const char* path, const uint8_t* data, uint32_t size);
    uint32_t (*read_file)(const char* path, uint8_t* buf, uint32_t maxsize);
    bool (*write_file)(const char* path, const uint8_t* buf, uint32_t size);
    bool (*remove)(const char* path);
} FSDriver;

bool fsbg_copy(FSDriver* src, FSDriver* dst, const char* src_name, const char* dst_name);
bool fsbg_move(FSDriver* src, FSDriver* dst, const char* src_name, const char* dst_name);
bool fsbg_copy_disk(const char* src_arg, const char* dst_arg);

extern FSDriver fs_fat16;
extern FSDriver fs_fat32;
extern FSDriver fs_xvfs;

#endif
