#include "blockdev.h"
#include "ata.h"

bool blockdev_present(uint8_t drive) {
    return ata_present(drive);
}

bool blockdev_read(uint8_t drive, uint32_t lba, uint16_t count, uint8_t* buffer) {
    return ata_read(drive, lba, count, buffer);
}

bool blockdev_write(uint8_t drive, uint32_t lba, uint16_t count, const uint8_t* buffer) {
    return ata_write(drive, lba, count, buffer);
}

bool blockdev_read_sector(uint32_t drive, uint32_t lba, uint8_t* buffer) {
    return ata_read_sector(drive, lba, buffer);
}

bool blockdev_write_sector(uint32_t drive, uint32_t lba, const uint8_t* buffer) {
    return ata_write_sector(drive, lba, buffer);
}

void blockdev_refresh_drive_map(void) {
    ata_refresh_drive_map();
}

uint32_t blockdev_get_sector_count(uint8_t drive) {
    return ata_get_sector_count(drive);
}

bool blockdev_flush_cache(uint8_t drive) {
    return ata_flush_cache(drive);
}

bool blockdev_drive_model(uint8_t drive, char* out, size_t out_len) {
    return ata_drive_model(drive, out, out_len);
}

bool blockdev_drive_backend(uint8_t drive, blockdev_backend_t* out_type, int* out_index) {
    ata_backend_t backend = ATA_BACKEND_NONE;
    bool ok = ata_drive_backend(drive, &backend, out_index);
    if (!ok || !out_type) {
        if (out_type)
            *out_type = BLOCKDEV_BACKEND_NONE;
        return ok;
    }

    switch (backend) {
    case ATA_BACKEND_AHCI:
        *out_type = BLOCKDEV_BACKEND_AHCI;
        break;
    case ATA_BACKEND_PATA:
        *out_type = BLOCKDEV_BACKEND_PATA;
        break;
    case ATA_BACKEND_USB:
        *out_type = BLOCKDEV_BACKEND_USB;
        break;
    case ATA_BACKEND_RAMDISK:
        *out_type = BLOCKDEV_BACKEND_RAMDISK;
        break;
    default:
        *out_type = BLOCKDEV_BACKEND_NONE;
        break;
    }
    return true;
}

const char* blockdev_backend_name(blockdev_backend_t backend) {
    switch (backend) {
    case BLOCKDEV_BACKEND_AHCI:
        return "ahci";
    case BLOCKDEV_BACKEND_PATA:
        return "pata";
    case BLOCKDEV_BACKEND_USB:
        return "usb";
    case BLOCKDEV_BACKEND_RAMDISK:
        return "ram";
    default:
        return "unknown";
    }
}
