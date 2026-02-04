#include "fscmd.h"
#include "fat16.h"
#include "fat32.h"
#include "xvfs.h"
#include "disk.h"
#include "../drivers/screen.h"
#include "../drivers/ata.h"
#include "../libc/string.h"
#include "../kernel/kernel.h"
#include "../mm/mem.h"
#include "../drivers/keyboard.h"

fs_type_t current_fs = FS_NONE;
int current_drive = -1;

extern DiskInfo disks[MAX_DISKS];   // disk_t 대신 DiskInfo
char current_path[256] = "/";

static bool write_progress_active = false;
static uint32_t write_progress_total = 0;
static uint32_t write_progress_last = 0;
static const char* write_progress_label = NULL;
static uint32_t write_progress_step = 0;
static uint32_t write_progress_next = 0;
static bool write_progress_small = false;
static int write_progress_row = -1;
static int write_progress_col = -1;
static uint32_t write_progress_pad_len = 0;

static void fscmd_render_progress(uint32_t percent) {
    char buf[64];
    uint32_t idx = 0;
    const char* label = write_progress_label ? write_progress_label : "write";

    while (label[idx] && idx < sizeof(buf) - 1) {
        buf[idx] = label[idx];
        idx++;
    }

    if (idx + 4 >= sizeof(buf))
        idx = sizeof(buf) - 5;

    buf[idx++] = ':';
    buf[idx++] = ' ';
    idx += (uint32_t)int_to_str((int)percent, buf + idx);
    if (idx < sizeof(buf) - 1)
        buf[idx++] = '%';

    uint32_t pad_len = write_progress_pad_len;
    if (pad_len >= sizeof(buf))
        pad_len = sizeof(buf) - 1;
    while (idx < pad_len && idx < sizeof(buf) - 1)
        buf[idx++] = ' ';

    buf[idx] = '\0';

    int old_offset = get_cursor_offset();
    kprint_at(buf, write_progress_col, write_progress_row);
    set_cursor_offset(old_offset);
}

extern uint32_t root_dir_cluster16;
extern uint32_t root_dir_cluster32;

typedef struct __attribute__((packed)) {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_first;
    uint32_t sectors;
} MBRPart;

const char* fs_to_string(fs_type_t type) {
    switch (type) {
        case FS_NONE:  return "NONE";
        case FS_FAT16: return "FAT16";
        case FS_FAT32: return "FAT32";
        case FS_XVFS:  return "XVFS";
        default:       return "UNKNOWN";
    }
}

void fscmd_reset_path(void) {
    if (current_fs == FS_FAT16)
        current_dir_cluster16 = root_dir_cluster16;
    else if (current_fs == FS_FAT32)
        current_dir_cluster32 = root_dir_cluster32;

    strcpy(current_path, "/");

    kprintf("[RESET_PATH] current_path=%s (fs=%d)\n", current_path, current_fs);
}

// ─────────────────────────────
// ls 명령어 (FAT16 / FAT32 공통)
// ─────────────────────────────
void fscmd_ls(const char* path) {
    if (current_fs == FS_FAT16) {
        fat16_ls(path);
    } 
    else if (current_fs == FS_FAT32) {
        fat32_ls(path);
    } 
    else if (current_fs == FS_XVFS) {
        xvfs_ls(path);
    } 
    else {
        kprint("No filesystem mounted.\n");
    }
}

int fscmd_list_dir(const char* path, char* names, uint8_t* is_dir, uint32_t max_entries, size_t name_len) {
    if (!names || !is_dir || max_entries == 0 || name_len == 0) {
        return -1;
    }

    if (current_fs == FS_FAT16) {
        uint16_t cluster;
        if (!path || path[0] == '\0') {
            cluster = current_dir_cluster16;
        } else {
            cluster = fat16_resolve_dir(path);
            if (cluster == 0xFFFF) {
                return -1;
            }
        }
        return fat16_list_dir_lfn(cluster, names, (bool*)is_dir, (int)max_entries, name_len);
    }

    if (current_fs == FS_FAT32) {
        uint32_t cluster;
        if (!path || path[0] == '\0') {
            cluster = current_dir_cluster32;
        } else {
            cluster = fat32_resolve_dir(path);
            if (cluster < 2 || cluster >= 0x0FFFFFF8) {
                return -1;
            }
        }
        return fat32_list_dir_lfn(cluster, names, (bool*)is_dir, (int)max_entries, name_len);
    }

    if (current_fs == FS_XVFS) {
        if (max_entries > 256) {
            max_entries = 256;
        }
        XVFS_FileEntry* entries = (XVFS_FileEntry*)kmalloc(max_entries * sizeof(XVFS_FileEntry), 0, NULL);
        if (!entries) {
            return -1;
        }
        int count = xvfs_read_dir_entries(path, entries, max_entries);
        if (count < 0) {
            kfree(entries);
            return -1;
        }
        for (int i = 0; i < count; i++) {
            char* dest = names + ((size_t)i * name_len);
            strncpy(dest, entries[i].name, name_len - 1);
            dest[name_len - 1] = '\0';
            is_dir[i] = (entries[i].attr & 1u) ? 1u : 0u;
        }
        kfree(entries);
        return count;
    }

    kprint("No filesystem mounted.\n");
    return -1;
}

void fscmd_cat(const char* path) {
    if (current_fs == FS_FAT16) {
        fat16_cat(path);
    } 
    else if (current_fs == FS_FAT32) {
        fat32_cat(path);
    } 
    else if (current_fs == FS_XVFS) {
        xvfs_cat(path);
    } 
    else {
        kprint("No filesystem mounted.\n");
    }
}

bool fscmd_rm(const char* path) {
    if (current_fs == FS_FAT16) {
        return fat16_rm(path);
    } 
    else if (current_fs == FS_FAT32) {
        return fat32_rm(path);
    } 
    else if (current_fs == FS_XVFS) {
        return xvfs_rm(path);
    } 
    else {
        kprint("No filesystem mounted.\n");
        return false;
    }
}

void fscmd_write_progress_begin(const char* label, uint32_t total) {
    write_progress_active = true;
    write_progress_total = total;
    write_progress_label = (label && *label) ? label : "write";
    write_progress_last = 0;
    write_progress_step = 0;
    write_progress_next = 0;
    write_progress_small = false;
    write_progress_row = get_cursor_row();
    write_progress_col = get_cursor_col();
    write_progress_pad_len = 0;

    if (total == 0) {
        kprintf("%s: 100%%\n", write_progress_label);
        write_progress_last = 100;
    } else {
        write_progress_pad_len = (uint32_t)strlen(write_progress_label) + 6;
        if (total < 100) {
            write_progress_small = true;
            write_progress_next = 1;
        } else {
            write_progress_step = total / 100;
            if (write_progress_step == 0)
                write_progress_step = 1;
            write_progress_next = write_progress_step;
        }
        kprintf("%s: 0%%\n", write_progress_label);
    }
}

void fscmd_write_progress_update(uint32_t written) {
    if (!write_progress_active || write_progress_total == 0)
        return;

    if (written > write_progress_total)
        written = write_progress_total;

    if (!write_progress_small) {
        if (written < write_progress_next && written < write_progress_total)
            return;

        uint32_t percent = written / write_progress_step;
        if (percent > 100)
            percent = 100;
        if (written < write_progress_total && percent >= 100)
            percent = 99;

        if (percent == write_progress_last) {
            if (percent >= 99 && written < write_progress_total)
                write_progress_next = write_progress_total;
            else
                write_progress_next = (percent + 1) * write_progress_step;
            return;
        }

        write_progress_last = percent;
        fscmd_render_progress(percent);

        if (percent >= 99 && written < write_progress_total)
            write_progress_next = write_progress_total;
        else
            write_progress_next = (percent + 1) * write_progress_step;
    } else {
        if (written < write_progress_next && written < write_progress_total)
            return;

        uint32_t percent = (written * 100U) / write_progress_total;
        if (percent == write_progress_last) {
            write_progress_next = written + 1;
            return;
        }

        write_progress_last = percent;
        fscmd_render_progress(percent);
        write_progress_next = written + 1;
    }
}

void fscmd_write_progress_finish(bool success) {
    if (!write_progress_active)
        return;

    if (success)
        fscmd_write_progress_update(write_progress_total);

    write_progress_active = false;
    write_progress_total = 0;
    write_progress_label = NULL;
    write_progress_last = 0;
    write_progress_step = 0;
    write_progress_next = 0;
    write_progress_small = false;
    write_progress_row = -1;
    write_progress_col = -1;
    write_progress_pad_len = 0;
}

bool fscmd_write_file(const char* filename, const char* data, uint32_t len) {
    const char* fs = disks[current_drive].fs_type;

    //kprintf("[DEBUG] fscmd_write_file(): drive=%d, fs=%s\n",
    //        current_drive, fs);

    if (strcmp(fs, "FAT16") == 0) {
        int written = fat16_write_file(filename, data, (int)len);
        return written >= 0;
    } else if (strcmp(fs, "FAT32") == 0) {
        return fat32_write_file(filename, (const uint8_t*)data, len);
    } else if (strcmp(fs, "XVFS") == 0) {
        return xvfs_write_file(filename, (const uint8_t*)data, len);
    }

    kprintf("[DEBUG] No mounted filesystem on drive %d\n", current_drive);
    return false;
}

bool fscmd_exists(const char* path) {
    if (current_fs == FS_FAT16)
        return fat16_exists(path);
    else if (current_fs == FS_FAT32)
        return fat32_exists(path);
    else if (current_fs == FS_XVFS)
        return xvfs_exists(path);
    else {
        kprint("No filesystem mounted.\n");
        return false;
    }
}

int fscmd_read_file_by_name(const char* path, uint8_t* buf, uint32_t size) {
    if (current_fs == FS_FAT16)
        return fat16_read_file_by_name(path, buf, size);
    else if (current_fs == FS_FAT32)
        return fat32_read_file_by_name(path, buf, size);
    else if (current_fs == FS_XVFS)
        return xvfs_read_file_by_name(path, buf, size);
    else {
        kprint("No filesystem mounted.\n");
        return -1;
    }
}

// ─────────────────────────────
// 파일 복사 (공통 명령어)
// ─────────────────────────────
bool fscmd_cp(const char* src, const char* dst) {
    if (current_fs == FS_FAT16)
        return fat16_cp(src, dst);
    else if (current_fs == FS_FAT32)
        return fat32_cp(src, dst);
    else if (current_fs == FS_XVFS)
        return xvfs_cp(src, dst);
    else {
        kprint("No filesystem mounted.\n");
        return false;
    }
}

// ─────────────────────────────
// 파일 이동 (공통 명령어)
// ─────────────────────────────
bool fscmd_mv(const char* src, const char* dst) {
    if (current_fs == FS_FAT16)
        return fat16_mv(src, dst);
    else if (current_fs == FS_FAT32)
        return fat32_mv(src, dst);
    else if (current_fs == FS_XVFS)
        return xvfs_mv(src, dst);
    else {
        kprint("No filesystem mounted.\n");
        return false;
    }
}

// ─────────────────────────────
// 파일 크기 반환 (공통 명령어)
// ─────────────────────────────
uint32_t fscmd_get_file_size(const char* filename) {
    if (current_fs == FS_FAT16)
        return fat16_get_file_size(filename);
    else if (current_fs == FS_FAT32)
        return fat32_get_file_size(filename);
    else if (current_fs == FS_XVFS)
        return xvfs_get_file_size(filename);
    else {
        kprint("No filesystem mounted.\n");
        return 0;
    }
}

// ─────────────────────────────
// 파일 일부분 읽기 (공통 명령어)
// ─────────────────────────────
bool fscmd_read_file_partial(const char* filename, uint32_t offset, uint8_t* buf, uint32_t size) {
    if (current_fs == FS_FAT16)
        return fat16_read_file_partial(filename, offset, buf, size);
    else if (current_fs == FS_FAT32)
        return fat32_read_file_partial(filename, offset, buf, size);
    else if (current_fs == FS_XVFS)
        return xvfs_read_file_partial(filename, offset, buf, size);
    else {
        kprint("No filesystem mounted.\n");
        return false;
    }
}

bool fscmd_mkdir(const char* dirname) {
    if (current_fs == FS_FAT16) {
        return fat16_mkdir(dirname);
    }
    else if (current_fs == FS_FAT32) {
        return fat32_mkdir(dirname);
    }
    else if (current_fs == FS_XVFS) {
        return xvfs_mkdir(dirname);
    }
    else {
        kprint("No filesystem mounted.\n");
        return false;
    }
}

bool fscmd_cd(const char* path) {
    if (current_fs == FS_FAT16) {
        return fat16_cd(path);
    }
    else if (current_fs == FS_FAT32) {
        return fat32_cd(path);
    }
    else if (current_fs == FS_XVFS) {
        return xvfs_cd(path);
    }
    else {
        kprint("No filesystem mounted.\n");
        return false;
    }
}

bool fscmd_rmdir(const char* dirname) {
    if (current_fs == FS_FAT16) {
        return fat16_rmdir(dirname);
    }
    else if (current_fs == FS_FAT32) {
        return fat32_rmdir(dirname);
    }
    else if (current_fs == FS_XVFS) {
        return xvfs_rmdir(dirname);
    }
    else {
        kprint("No filesystem mounted.\n");
        return false;
    }
}

bool fscmd_find_file(const char* path, void* out_entry) {
    if (current_fs == FS_FAT16) {
        return fat16_find_file(path, (FAT16_DirEntry*)out_entry);
    }
    else if (current_fs == FS_FAT32) {
        return fat32_find_file(path, (FAT32_DirEntry*)out_entry);
    }
    else if (current_fs == FS_XVFS) {
        return xvfs_find_file(path, (XVFS_FileEntry*)out_entry);
    }
    else {
        kprint("No filesystem mounted.\n");
        return false;
    }
}

bool fscmd_read_file_range(void* entry, uint32_t offset, uint8_t* out_buf, uint32_t size) {
    if (!entry || !out_buf || size == 0) {
        kprint("fscmd_read_file_range: invalid arguments\n");
        return false;
    }

    if (current_fs == FS_FAT16) {
        return fat16_read_file_range((FAT16_DirEntry*)entry, offset, out_buf, size);
    }
    else if (current_fs == FS_FAT32) {
        return fat32_read_file_range((FAT32_DirEntry*)entry, offset, out_buf, size);
    }
    else if (current_fs == FS_XVFS) {
        return xvfs_read_file_range((XVFS_FileEntry*)entry, offset, out_buf, size);
    }
    else {
        kprint("No filesystem mounted.\n");
        return false;
    }
}

bool fscmd_format(uint8_t drive, const char* fs) {
    if (!fs || !*fs) {
        kprint("Usage: format <drive#># <filesystem>\n");
        kprint("Example: format 0# fat16\n");
        return false;
    }

    // 디스크 존재 확인
    uint32_t total = ata_get_sector_count(drive);
    if (total == 0) {
        kprintf("[format] drive %d not detected.\n", drive);
        return false;
    }

    uint32_t base_lba = disks[drive].base_lba;
    uint32_t part_sectors = 0;
    int part_index = -1;

    if (base_lba > 0) {
        uint8_t mbr[512];
        if (ata_read(drive, 0, 1, mbr) && mbr[510] == 0x55 && mbr[511] == 0xAA) {
            MBRPart* p = (MBRPart*)(mbr + 0x1BE);
            for (int i = 0; i < 4; i++) {
                if (p[i].type == 0) continue;
                if (p[i].lba_first == base_lba) {
                    part_index = i;
                    part_sectors = p[i].sectors;
                    break;
                }
            }
            if (part_index < 0) {
                for (int i = 0; i < 4; i++) {
                    if (p[i].type == 0) continue;
                    part_index = i;
                    base_lba = p[i].lba_first;
                    part_sectors = p[i].sectors;
                    break;
                }
            }
        }
        if (part_sectors == 0 && total > base_lba)
            part_sectors = total - base_lba;
    }

    // 파일시스템 문자열을 소문자로 정규화
    char type[16];
    strncpy(type, fs, sizeof(type) - 1);
    type[sizeof(type) - 1] = '\0';
    for (int i = 0; type[i]; i++)
        if (type[i] >= 'A' && type[i] <= 'Z')
            type[i] += 32;

    if (strcmp(type, "fat16") == 0) {
        if (base_lba > 0 && part_sectors > 0) {
            kprintf("[format] Formatting drive %d partition (LBA=%u, %u sectors) as FAT16...\n",
                    drive, base_lba, part_sectors);
            if (fat16_format_at(drive, base_lba, part_sectors, "ORION16")) {
                kprintf("[format] Drive %d formatted successfully (FAT16)\n", drive);
                kprint("[format] Format completed. Please reboot the system.\n");
                goto update_mbr_type;
            }
        } else {
            kprintf("[format] Formatting drive %d as FAT16...\n", drive);
            if (fat16_format(drive, "ORION16")) {
                kprintf("[format] Drive %d formatted successfully (FAT16)\n", drive);
                kprint("[format] Format completed. Please reboot the system.\n");
                return true;
            }
        }
    }
    else if (strcmp(type, "fat32") == 0) {
        if (base_lba > 0 && part_sectors > 0) {
            kprintf("[format] Formatting drive %d partition (LBA=%u, %u sectors) as FAT32...\n",
                    drive, base_lba, part_sectors);
            if (fat32_format_at(drive, base_lba, part_sectors, "ORION32")) {
                kprintf("[format] Drive %d formatted successfully (FAT32)\n", drive);
                kprint("[format] Format completed. Please reboot the system.\n");
                goto update_mbr_type;
            }
        } else {
            kprintf("[format] Formatting drive %d as FAT32...\n", drive);
            if (fat32_format(drive, "ORION32")) {
                kprintf("[format] Drive %d formatted successfully (FAT32)\n", drive);
                kprint("[format] Format completed. Please reboot the system.\n");
                return true;
            }
        }
    }
    else if (strcmp(type, "xvfs") == 0) {
        if (base_lba > 0 && part_sectors > 0) {
            kprintf("[format] Formatting drive %d partition (LBA=%u, %u sectors) as XVFS...\n",
                    drive, base_lba, part_sectors);
            if (xvfs_format_at(drive, base_lba, part_sectors)) {
                kprintf("[format] Drive %d formatted successfully (XVFS)\n", drive);
                kprint("[format] Format completed. Please reboot the system.\n");
                goto update_mbr_type;
            }
        } else {
            kprintf("[format] Formatting drive %d as XVFS...\n", drive);
            if (xvfs_format(drive)) {
                kprintf("[format] Drive %d formatted successfully (XVFS)\n", drive);
                kprint("[format] Format completed. Please reboot the system.\n");
                return true;
            }
        }
    }
    else {
        kprintf("[format] Unsupported filesystem: %s\n", fs);
        kprint("Supported types: fat16, fat32, xvfs\n");
        return false;
    }

    kprintf("[format] Failed to format drive %d (%s)\n", drive, fs);
    return false;

update_mbr_type:
    if (part_index >= 0) {
        uint8_t mbr[512];
        if (ata_read(drive, 0, 1, mbr) && mbr[510] == 0x55 && mbr[511] == 0xAA) {
            MBRPart* p = (MBRPart*)(mbr + 0x1BE);
            if (strcmp(type, "fat16") == 0)
                p[part_index].type = 0x06;
            else if (strcmp(type, "fat32") == 0)
                p[part_index].type = 0x0C;
            else if (strcmp(type, "xvfs") == 0)
                p[part_index].type = 0x83;
            ata_write(drive, 0, 1, mbr);
        }
    }
    return true;
}

int fscmd_read_file(const char* filename, uint8_t* buffer, uint32_t offset, uint32_t size) {
    if (current_fs == FS_FAT16) {
        FAT16_DirEntry entry;
        if (!fat16_find_file(filename, &entry))
            return -1;

        return fat16_read_file(&entry, buffer, offset, size);
    }

    else if (current_fs == FS_FAT32) {
        return fat32_read_file(filename, buffer, offset, size);
    }

    else if (
        current_fs == FS_XVFS) {
        XVFS_FileEntry entry;
        if (!xvfs_find_file(filename, &entry))
            return -1;

        return xvfs_read_file(&entry, buffer, offset, size);
    }

    else {
        kprint("No filesystem mounted.\n");
        return -1;
    }
}
