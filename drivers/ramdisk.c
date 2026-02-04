#include "ramdisk.h"
#include "../drivers/screen.h"
#include "../fs/disk.h"
#include "../fs/fs_quick.h"
#include "../libc/string.h"

typedef struct {
    bool present;
    uint8_t drive;
    uint8_t* data;
    uint32_t size_bytes;
    uint32_t sector_count;
} ramdisk_t;

static ramdisk_t g_ramdisk = {0};

static void ramdisk_recount_disks(void) {
    disk_count = 0;
    for (int i = 0; i < MAX_DISKS; i++) {
        if (disks[i].present)
            disk_count++;
    }
}

static int ramdisk_find_free_drive(void) {
    for (int i = MAX_DISKS - 1; i >= 0; i--) {
        if (!disks[i].present)
            return i;
    }
    return -1;
}

int ramdisk_drive_id(void) {
    return g_ramdisk.present ? g_ramdisk.drive : -1;
}

bool ramdisk_present(uint8_t drive) {
    return g_ramdisk.present && g_ramdisk.drive == drive;
}

bool ramdisk_read(uint8_t drive, uint32_t lba, uint16_t count, uint8_t* buffer) {
    if (!buffer || !ramdisk_present(drive))
        return false;

    if (count == 0)
        count = 256;

    uint64_t offset = (uint64_t)lba * RAMDISK_SECTOR_SIZE;
    uint64_t bytes = (uint64_t)count * RAMDISK_SECTOR_SIZE;

    if (offset + bytes > g_ramdisk.size_bytes)
        return false;

    memcpy(buffer, g_ramdisk.data + offset, (size_t)bytes);
    return true;
}

bool ramdisk_write(uint8_t drive, uint32_t lba, uint16_t count, const uint8_t* buffer) {
    if (!buffer || !ramdisk_present(drive))
        return false;

    if (count == 0)
        count = 256;

    uint64_t offset = (uint64_t)lba * RAMDISK_SECTOR_SIZE;
    uint64_t bytes = (uint64_t)count * RAMDISK_SECTOR_SIZE;

    if (offset + bytes > g_ramdisk.size_bytes)
        return false;

    memcpy(g_ramdisk.data + offset, buffer, (size_t)bytes);
    return true;
}

uint32_t ramdisk_get_sector_count(uint8_t drive) {
    if (!ramdisk_present(drive))
        return 0;
    return g_ramdisk.sector_count;
}

uint32_t ramdisk_get_size_bytes(uint8_t drive) {
    if (!ramdisk_present(drive))
        return 0;
    return g_ramdisk.size_bytes;
}

const uint8_t* ramdisk_data(uint8_t drive) {
    if (!ramdisk_present(drive))
        return NULL;
    return g_ramdisk.data;
}

bool ramdisk_attach(uint8_t drive, uint8_t* data, uint32_t size_bytes, uint8_t* out_drive) {
    if (g_ramdisk.present) {
        kprint("[RAMDISK] already attached\n");
        return false;
    }

    if (!data || size_bytes < RAMDISK_SECTOR_SIZE) {
        kprint("[RAMDISK] invalid image\n");
        return false;
    }

    uint32_t usable = size_bytes & ~(RAMDISK_SECTOR_SIZE - 1u);
    if (usable != size_bytes) {
        kprintf("[RAMDISK] image size not sector-aligned, trimming %u -> %u bytes\n",
                size_bytes, usable);
        size_bytes = usable;
    }

    if (size_bytes < RAMDISK_SECTOR_SIZE) {
        kprint("[RAMDISK] image too small after trim\n");
        return false;
    }

    if (drive == RAMDISK_DRIVE_AUTO) {
        int free_id = ramdisk_find_free_drive();
        if (free_id < 0) {
            kprint("[RAMDISK] no free drive slot\n");
            return false;
        }
        drive = (uint8_t)free_id;
    }

    if (drive >= MAX_DISKS) {
        kprint("[RAMDISK] invalid drive id\n");
        return false;
    }

    if (disks[drive].present) {
        kprint("[RAMDISK] drive id already in use\n");
        return false;
    }

    g_ramdisk.present = true;
    g_ramdisk.drive = drive;
    g_ramdisk.data = data;
    g_ramdisk.size_bytes = size_bytes;
    g_ramdisk.sector_count = size_bytes / RAMDISK_SECTOR_SIZE;

    uint32_t base = 0;
    fs_kind_t kind = fs_quick_probe(drive, &base);

    disks[drive].present = true;
    disks[drive].id = drive;
    disks[drive].base_lba = base;

    switch (kind) {
        case FSQ_FAT16:
            strcpy(disks[drive].fs_type, "FAT16");
            break;
        case FSQ_FAT32:
            strcpy(disks[drive].fs_type, "FAT32");
            break;
        case FSQ_XVFS:
            strcpy(disks[drive].fs_type, "XVFS");
            break;
        case FSQ_MBR:
            strcpy(disks[drive].fs_type, "MBR");
            break;
        default:
            strcpy(disks[drive].fs_type, "Unknown");
            break;
    }

    ramdisk_recount_disks();

    if (out_drive)
        *out_drive = drive;

    kprintf("[RAMDISK] attached drive %d (%u sectors)\n",
            drive, g_ramdisk.sector_count);
    return true;
}
