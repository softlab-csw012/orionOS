#include "ramdisk.h"
#include "../drivers/ramdisk.h"
#include "../drivers/screen.h"
#include "../fs/fscmd.h"
#include "../libc/string.h"
#include "../mm/mem.h"
#include "../mm/paging.h"

#define RAMDISK_IDENTITY_MAX 0x04000000u
#define RAMDISK_MAP_BASE     0xC8000000u

static uint8_t* ramdisk_map_module(uint32_t start, uint32_t size) {
    uint32_t map_start = start & 0xFFFFF000u;
    uint32_t map_end = (start + size + 0xFFFu) & 0xFFFFF000u;
    uint32_t map_size = map_end - map_start;

    if (map_size == 0)
        return NULL;

    if (RAMDISK_MAP_BASE + map_size < RAMDISK_MAP_BASE)
        return NULL;

    for (uint32_t offset = 0; offset < map_size; offset += PAGE_SIZE) {
        vmm_map_page(RAMDISK_MAP_BASE + offset,
                     map_start + offset,
                     PAGE_PRESENT | PAGE_RW);
    }

    return (uint8_t*)(RAMDISK_MAP_BASE + (start - map_start));
}

bool ramdisk_load_from_path(const char* path) {
    if (ramdisk_drive_id() >= 0) {
        kprint("[RAMDISK] already attached, skipping file load\n");
        return false;
    }

    if (!path || !*path) {
        kprint("[RAMDISK] no image path provided\n");
        return false;
    }

    if (current_fs == FS_NONE) {
        kprint("[RAMDISK] no filesystem mounted\n");
        return false;
    }

    uint32_t size = fscmd_get_file_size(path);
    if (size == 0) {
        kprintf("[RAMDISK] file not found or empty: %s\n", path);
        return false;
    }

    uint32_t rounded = (size + (RAMDISK_SECTOR_SIZE - 1u)) & ~(RAMDISK_SECTOR_SIZE - 1u);
    uint8_t* buf = (uint8_t*)kmalloc_aligned(rounded, RAMDISK_SECTOR_SIZE);
    if (!buf) {
        kprintf("[RAMDISK] allocation failed (%u bytes)\n", rounded);
        return false;
    }
    memset(buf, 0, rounded);

    int read = fscmd_read_file_by_name(path, buf, size);
    if (read <= 0) {
        kprintf("[RAMDISK] read failed: %s\n", path);
        kfree(buf);
        return false;
    }

    uint8_t drive_id = 0;
    if (!ramdisk_attach(RAMDISK_DRIVE_AUTO, buf, rounded, &drive_id)) {
        kfree(buf);
        return false;
    }

    kprintf("[RAMDISK] loaded %s (%u bytes) as drive %d#\n",
            path, (uint32_t)read, drive_id);
    return true;
}

bool ramdisk_load_from_module(uint32_t start, uint32_t end, const char* name) {
    if (ramdisk_drive_id() >= 0) {
        kprint("[RAMDISK] already attached, skipping module load\n");
        return false;
    }

    if (end <= start) {
        kprint("[RAMDISK] invalid module range\n");
        return false;
    }

    uint32_t size = end - start;
    const char* label = (name && *name) ? name : "module";
    uint8_t drive_id = 0;

    uint8_t* data = (uint8_t*)start;
    if (end > RAMDISK_IDENTITY_MAX) {
        data = ramdisk_map_module(start, size);
        if (!data) {
            kprint("[RAMDISK] failed to map module\n");
            return false;
        }
    }

    if (!ramdisk_attach(RAMDISK_DRIVE_AUTO, data, size, &drive_id)) {
        kprint("[RAMDISK] attach failed for module\n");
        return false;
    }

    kprintf("[RAMDISK] loaded %s (%u bytes) as drive %d#\n",
            label, size, drive_id);
    return true;
}
