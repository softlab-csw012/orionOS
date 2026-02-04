#include "fsbg.h"
#include "fat16.h"
#include "fat32.h"
#include "xvfs.h"
#include "disk.h"
#include "../drivers/screen.h"
#include "../mm/mem.h"
#include "../libc/string.h"

//───────────────
// Auto-mount flags
//───────────────
static bool fat16_ready = false;
static bool fat32_ready = false;
static bool xvfs_ready  = false;
static int fat16_mounted_disk = -1;
static int fat32_mounted_disk = -1;
static int xvfs_mounted_disk  = -1;

//───────────────
// Auto-mount helper
//───────────────
static void fsbg_auto_mount_if_needed(const char* fs_name) {
    extern DiskInfo disks[MAX_DISKS];

    for (int i = 0; i < MAX_DISKS; i++) {
        if (!disks[i].present || !disks[i].fs_type[0]) continue;

        // FAT16
        if (strcmp(fs_name, "FAT16") == 0 && !fat16_ready && strcmp(disks[i].fs_type, "FAT16") == 0) {
            kprintf("[fsbg] auto-mounting FAT16 on disk %d (LBA=%u)\n", i, disks[i].base_lba);
            if (fat16_init(i, disks[i].base_lba)) {
                fat16_ready = true;
                kprint("[fsbg] FAT16 mounted automatically\n");
            } else {
                kprint("[fsbg] FAT16 auto-mount failed\n");
            }
        }

        // FAT32
        if (strcmp(fs_name, "FAT32") == 0 && !fat32_ready && strcmp(disks[i].fs_type, "FAT32") == 0) {
            kprintf("[fsbg] auto-mounting FAT32 on disk %d (LBA=%u)\n", i, disks[i].base_lba);
            if (fat32_init(i, disks[i].base_lba)) {
                fat32_ready = true;
                kprint("[fsbg] FAT32 mounted automatically\n");
            } else {
                kprint("[fsbg] FAT32 auto-mount failed\n");
            }
        }

        // XVFS
        if (strcmp(fs_name, "XVFS") == 0 && !xvfs_ready && strcmp(disks[i].fs_type, "XVFS") == 0) {
            kprintf("[fsbg] auto-mounting XVFS on disk %d (LBA=%u)\n", i, disks[i].base_lba);
            if (xvfs_init(i, disks[i].base_lba)) {
                xvfs_ready = true;
                kprint("[fsbg] XVFS mounted automatically\n");
            } else {
                kprint("[fsbg] XVFS auto-mount failed\n");
            }
        }
    }
}

//───────────────
// Per-disk mount helper
//───────────────
static bool fsbg_mount_disk(const char* fs_name, int disk) {
    extern DiskInfo disks[MAX_DISKS];

    if (!fs_name)
        return false;
    if (disk < 0)
        return true;
    if (disk >= MAX_DISKS)
        return false;
    if (!disks[disk].present)
        return false;

    if (strcmp(fs_name, "FAT16") == 0) {
        if (fat16_mounted_disk == disk)
            return true;
        if (!fat16_init(disk, disks[disk].base_lba))
            return false;
        fat16_mounted_disk = disk;
        return true;
    }

    if (strcmp(fs_name, "FAT32") == 0) {
        if (fat32_mounted_disk == disk)
            return true;
        if (!fat32_init(disk, disks[disk].base_lba))
            return false;
        fat32_mounted_disk = disk;
        return true;
    }

    if (strcmp(fs_name, "XVFS") == 0) {
        if (xvfs_mounted_disk == disk)
            return true;
        if (!xvfs_init(disk, disks[disk].base_lba))
            return false;
        xvfs_mounted_disk = disk;
        return true;
    }

    return false;
}

// ─────────────────────────────
// Directory helpers
// ─────────────────────────────
#define FSBG_MAX_DIR_ENTRIES 512

typedef struct {
    char name[64];
    bool is_dir;
} FsbgDirEntry;

static void fsbg_strip_trailing_slash(char* path) {
    if (!path) return;
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[len - 1] = '\0';
        len--;
    }
}

static const char* fsbg_basename(const char* path) {
    if (!path || !path[0]) return "";
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool fsbg_join_path(char* out, size_t out_size, const char* base, const char* name) {
    if (!out || !out_size || !name || !name[0]) return false;

    if (!base || base[0] == '\0') {
        return snprintf(out, out_size, "%s", name) < (int)out_size;
    }

    if (base[strlen(base) - 1] == '/')
        return snprintf(out, out_size, "%s%s", base, name) < (int)out_size;

    return snprintf(out, out_size, "%s/%s", base, name) < (int)out_size;
}

static bool fsbg_is_dir_by_fs(const char* fs, int disk, const char* path) {
    if (!fs) return false;

    if (disk >= 0 && !fsbg_mount_disk(fs, disk))
        return false;

    if (strcmp(fs, "FAT16") == 0) {
        if (!path || !path[0] || strcmp(path, "/") == 0) return true;
        FAT16_DirEntry entry;
        if (!fat16_find_file_path(path, &entry)) return false;
        return (entry.Attr & 0x10) != 0;
    }

    if (strcmp(fs, "FAT32") == 0) {
        if (!path || !path[0] || strcmp(path, "/") == 0) return true;
        FAT32_DirEntry entry;
        if (!fat32_find_file(path, &entry)) return false;
        return (entry.Attr & 0x10) != 0;
    }

    if (strcmp(fs, "XVFS") == 0) {
        if (!path || !path[0] || strcmp(path, "/") == 0) return true;
        return xvfs_is_dir(path);
    }

    return false;
}

static bool fsbg_mkdir_by_fs(const char* fs, int disk, const char* path) {
    if (!fs || !path) return false;

    if (disk >= 0 && !fsbg_mount_disk(fs, disk))
        return false;

    if (strcmp(fs, "FAT16") == 0) return fat16_mkdir(path);
    if (strcmp(fs, "FAT32") == 0) return fat32_mkdir(path);
    if (strcmp(fs, "XVFS") == 0) return xvfs_mkdir_path(path);

    return false;
}

static bool fsbg_rmdir_by_fs(const char* fs, int disk, const char* path) {
    if (!fs || !path || !path[0]) return false;

    if (disk >= 0 && !fsbg_mount_disk(fs, disk))
        return false;

    if (strcmp(fs, "FAT16") == 0) return fat16_rmdir(path);
    if (strcmp(fs, "FAT32") == 0) return fat32_rmdir(path);
    if (strcmp(fs, "XVFS") == 0) return xvfs_rmdir(path);

    return false;
}

static int fsbg_list_dir_entries(const char* fs, int disk, const char* path, FsbgDirEntry* out, int max_entries) {
    if (!fs || !out || max_entries <= 0) return -1;

    if (disk >= 0 && !fsbg_mount_disk(fs, disk))
        return -1;

    if (strcmp(fs, "FAT16") == 0) {
        uint16_t cluster = 0;
        FAT16_DirEntry dir_entry;

        if (path && path[0] && strcmp(path, "/") != 0) {
            if (!fat16_find_file_path(path, &dir_entry) || !(dir_entry.Attr & 0x10))
                return -1;
            cluster = dir_entry.FirstCluster;
        }

        size_t name_len = sizeof(out[0].name);
        char* names = (char*)kmalloc((size_t)max_entries * name_len, 0, NULL);
        bool* is_dir = (bool*)kmalloc((size_t)max_entries * sizeof(bool), 0, NULL);
        if (!names || !is_dir) {
            if (names) kfree(names);
            if (is_dir) kfree(is_dir);
            return -1;
        }

        int count = fat16_list_dir_lfn(cluster, names, is_dir, max_entries, name_len);
        if (count < 0) {
            kfree(names);
            kfree(is_dir);
            return -1;
        }

        for (int i = 0; i < count; i++) {
            char* src = names + ((size_t)i * name_len);
            strncpy(out[i].name, src, name_len - 1);
            out[i].name[name_len - 1] = '\0';
            out[i].is_dir = is_dir[i];
        }

        kfree(names);
        kfree(is_dir);
        return count;
    }

    if (strcmp(fs, "FAT32") == 0) {
        uint32_t cluster = root_dir_cluster32;
        if (path && path[0] && strcmp(path, "/") != 0) {
            cluster = fat32_resolve_dir(path);
        }
        if (cluster < 2 || cluster >= 0x0FFFFFF8)
            return -1;

        size_t name_len = sizeof(out[0].name);
        char* names = (char*)kmalloc((size_t)max_entries * name_len, 0, NULL);
        bool* is_dir = (bool*)kmalloc((size_t)max_entries * sizeof(bool), 0, NULL);
        if (!names || !is_dir) {
            if (names) kfree(names);
            if (is_dir) kfree(is_dir);
            return -1;
        }

        int count = fat32_list_dir_lfn(cluster, names, is_dir, max_entries, name_len);
        if (count < 0) {
            kfree(names);
            kfree(is_dir);
            return -1;
        }

        for (int i = 0; i < count; i++) {
            char* src = names + ((size_t)i * name_len);
            strncpy(out[i].name, src, name_len - 1);
            out[i].name[name_len - 1] = '\0';
            out[i].is_dir = is_dir[i];
        }

        kfree(names);
        kfree(is_dir);
        return count;
    }

    if (strcmp(fs, "XVFS") == 0) {
        XVFS_FileEntry* entries = (XVFS_FileEntry*)kmalloc(sizeof(XVFS_FileEntry) * max_entries, 0, NULL);
        if (!entries) return -1;

        int count = xvfs_read_dir_entries(path, entries, max_entries);
        if (count < 0) {
            kfree(entries);
            return -1;
        }
        if (count >= max_entries) {
            kprint("[fsbg] XVFS directory too large\n");
            kfree(entries);
            return -1;
        }

        int out_count = 0;
        for (int i = 0; i < count; i++) {
            char name[64];
            strncpy(name, entries[i].name, XVFS_MAX_NAME);
            name[XVFS_MAX_NAME] = '\0';

            if (!name[0]) continue;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

            strncpy(out[out_count].name, name, sizeof(out[out_count].name) - 1);
            out[out_count].name[sizeof(out[out_count].name) - 1] = '\0';
            out[out_count].is_dir = (entries[i].attr & 1) != 0;
            out_count++;
        }

        kfree(entries);
        return out_count;
    }

    return -1;
}

static bool fsbg_copy_file_disk(FSDriver* src, FSDriver* dst,
                                const char* src_fs, const char* dst_fs,
                                int src_disk, int dst_disk,
                                const char* src_name, const char* dst_name) {
    if (!src || !dst || !src_name || !dst_name)
        return false;

    if (!fsbg_mount_disk(src_fs, src_disk)) {
        kprintf("[fsbg] mount failed for %s on disk %d\n", src_fs, src_disk);
        return false;
    }

    if (!src->exists(src_name)) {
        kprintf("[fsbg] source not found: %s (%s)\n", src_name, src_fs);
        return false;
    }

    uint32_t size = src->get_size ? src->get_size(src_name) : 0;
    if (size == 0) {
        uint8_t dummy = 0;
        if (!fsbg_mount_disk(dst_fs, dst_disk)) {
            kprintf("[fsbg] mount failed for %s on disk %d\n", dst_fs, dst_disk);
            return false;
        }
        if (dst->exists(dst_name))
            dst->remove(dst_name);
        if (!dst->create(dst_name, &dummy, 0)) {
            kprintf("[fsbg] create/write failed on %s\n", dst_fs);
            return false;
        }
        kprintf("[fsbg] copied %s (%s -> %s, 0 bytes)\n", src_name, src_fs, dst_fs);
        return true;
    }

    uint8_t* buf = (uint8_t*)kmalloc(size, 0, NULL);
    if (!buf) {
        kprintf("[fsbg] memory alloc failed\n");
        return false;
    }

    int read = src->read_file(src_name, buf, size);
    if (read <= 0) {
        kprintf("[fsbg] read failed from %s\n", src_fs);
        kfree(buf);
        return false;
    }
    if ((uint32_t)read < size) {
        size = (uint32_t)read;
    }

    if (!fsbg_mount_disk(dst_fs, dst_disk)) {
        kprintf("[fsbg] mount failed for %s on disk %d\n", dst_fs, dst_disk);
        kfree(buf);
        return false;
    }

    if (dst->exists(dst_name))
        dst->remove(dst_name);

    if (!dst->create(dst_name, buf, size)) {
        kprintf("[fsbg] create/write failed on %s\n", dst_fs);
        kfree(buf);
        return false;
    }

    kprintf("[fsbg] copied %s (%s -> %s, %u bytes)\n", src_name, src_fs, dst_fs, size);
    kfree(buf);
    return true;
}

static bool fsbg_copy_dir_recursive(FSDriver* src, FSDriver* dst,
                                    const char* src_fs, const char* dst_fs,
                                    int src_disk, int dst_disk,
                                    const char* src_dir, const char* dst_dir,
                                    bool remove_src) {
    if (!src || !dst || !src_fs || !dst_fs) return false;

    if (dst_dir && dst_dir[0]) {
        if (!fsbg_mount_disk(dst_fs, dst_disk)) {
            kprintf("[fsbg] mount failed for %s on disk %d\n", dst_fs, dst_disk);
            return false;
        }
        if (dst->exists(dst_dir)) {
            if (!fsbg_is_dir_by_fs(dst_fs, dst_disk, dst_dir)) {
                kprintf("[fsbg] destination exists and is not a directory: %s\n", dst_dir);
                return false;
            }
        } else if (!fsbg_mkdir_by_fs(dst_fs, dst_disk, dst_dir)) {
            kprintf("[fsbg] failed to create directory: %s\n", dst_dir);
            return false;
        }
    }

    FsbgDirEntry* entries = (FsbgDirEntry*)kmalloc(sizeof(FsbgDirEntry) * FSBG_MAX_DIR_ENTRIES, 0, NULL);
    if (!entries) {
        kprint("[fsbg] memory alloc failed\n");
        return false;
    }

    int count = fsbg_list_dir_entries(src_fs, src_disk, src_dir, entries, FSBG_MAX_DIR_ENTRIES);
    if (count < 0) {
        kfree(entries);
        kprintf("[fsbg] failed to list directory: %s\n", src_dir ? src_dir : "");
        return false;
    }

    bool ok = true;
    for (int i = 0; i < count; i++) {
        char src_child[256];
        char dst_child[256];

        if (!fsbg_join_path(src_child, sizeof(src_child), src_dir, entries[i].name) ||
            !fsbg_join_path(dst_child, sizeof(dst_child), dst_dir, entries[i].name)) {
            kprint("[fsbg] path too long\n");
            ok = false;
            break;
        }

        if (entries[i].is_dir) {
            if (!fsbg_copy_dir_recursive(src, dst, src_fs, dst_fs, src_disk, dst_disk, src_child, dst_child, remove_src)) {
                ok = false;
                break;
            }
        } else {
            if (!fsbg_copy_file_disk(src, dst, src_fs, dst_fs, src_disk, dst_disk, src_child, dst_child)) {
                ok = false;
                break;
            }
            if (remove_src) {
                if (!fsbg_mount_disk(src_fs, src_disk) || !src->remove(src_child)) {
                kprintf("[fsbg] failed to remove file: %s\n", src_child);
                ok = false;
                break;
                }
            }
        }
    }

    kfree(entries);

    if (ok && remove_src && src_dir && src_dir[0]) {
        if (!fsbg_rmdir_by_fs(src_fs, src_disk, src_dir)) {
            kprintf("[fsbg] failed to remove directory: %s\n", src_dir);
            ok = false;
        }
    }

    return ok;
}

//───────────────
// Compat Wrappers
//───────────────
bool fat16_create_file_compat(const char* path, const uint8_t* data, uint32_t size) {
    // 1) 파일 만들기
    if (fat16_create_file(path, (int)size) < 0)
        return false;

    // 2) 내용 쓰기
    if (fat16_write_file(path, (const char*)data, (int)size) < 0)
        return false;

    return true;
}

bool fat32_create_file_compat(const char* path, const uint8_t* data, uint32_t size) {
    if (!fat32_create_file(path))
        return false;

    if (fat32_write_file(path, data, size) <= 0)
        return false;

    return true;
}

bool fat16_write_file_compat(const char* path, const uint8_t* data, uint32_t size) {
    return fat16_write_file(path, (const char*)data, (int)size) >= 0;
}

FSDriver* fsbg_get_driver_by_fs(const char* fs) {
    if (!fs) return NULL;
    if (strcmp(fs, "FAT16") == 0) return &fs_fat16;
    if (strcmp(fs, "FAT32") == 0) return &fs_fat32;
    if (strcmp(fs, "XVFS")  == 0) return &fs_xvfs;
    return NULL;
}

// ─────────────────────────────
// 각 파일시스템 드라이버 등록
// ─────────────────────────────

FSDriver fs_fat16 = {
    .name = "FAT16",
    .exists = fat16_exists,
    .get_size = fat16_get_file_size,
    .create = fat16_create_file_compat,   // ✅ 래퍼 사용
    .read_file = (uint32_t (*)(const char*, uint8_t*, uint32_t))fat16_read_file_by_name,
    .write_file = fat16_write_file_compat,
    .remove = fat16_rm,
};

FSDriver fs_fat32 = {
    .name = "FAT32",
    .exists = fat32_exists,
    .get_size = fat32_get_file_size,
    .create = fat32_create_file_compat,   // ✅ 래퍼 사용
    .read_file = (uint32_t (*)(const char*, uint8_t*, uint32_t))fat32_read_file_by_name,
    .write_file = fat32_write_file,
    .remove = fat32_rm,
};

FSDriver fs_xvfs = {
    .name = "XVFS",
    .exists = xvfs_exists,
    .get_size = xvfs_get_file_size,
    .create = xvfs_create_file, // 이미 시그니처 맞음
    .read_file = (uint32_t (*)(const char*, uint8_t*, uint32_t))xvfs_read_file_by_name,
    .write_file = xvfs_write_file,
    .remove = xvfs_rm,
};

// ─────────────────────────────
// 공통 복사 함수 (전체 파일 단위)
// ─────────────────────────────
bool fsbg_copy(FSDriver* src, FSDriver* dst, const char* src_name, const char* dst_name) {
    if (!src || !dst || !src_name || !dst_name) {
        kprintf("[fsbg] invalid args\n");
        return false;
    }

    // ✅ 자동 마운트
    fsbg_auto_mount_if_needed(src->name);
    fsbg_auto_mount_if_needed(dst->name);

    if (!src->exists(src_name)) {
        kprintf("[fsbg] source not found: %s (%s)\n", src_name, src->name);
        return false;
    }

    uint32_t size = src->get_size ? src->get_size(src_name) : 0;
    if (size == 0) {
        uint8_t dummy = 0;
        if (dst->exists(dst_name))
            dst->remove(dst_name);
        if (!dst->create(dst_name, &dummy, 0)) {
            kprintf("[fsbg] create/write failed on %s\n", dst->name);
            return false;
        }
        kprintf("[fsbg] copied %s (%s -> %s, 0 bytes)\n", src_name, src->name, dst->name);
        return true;
    }

    uint8_t* buf = (uint8_t*)kmalloc(size, 0, NULL);
    if (!buf) {
        kprintf("[fsbg] memory alloc failed\n");
        return false;
    }

    int read = src->read_file(src_name, buf, size);
    if (read <= 0) {
        kprintf("[fsbg] read failed from %s\n", src->name);
        kfree(buf);
        return false;
    }
    if ((uint32_t)read < size) {
        size = (uint32_t)read;
    }

    if (dst->exists(dst_name))
        dst->remove(dst_name);

    if (!dst->create(dst_name, buf, size)) {
        kprintf("[fsbg] create/write failed on %s\n", dst->name);
        kfree(buf);
        return false;
    }

    kprintf("[fsbg] copied %s (%s -> %s, %u bytes)\n", src_name, src->name, dst->name, size);
    kfree(buf);
    return true;
}

// ─────────────────────────────
// 이동 (복사 후 원본 삭제)
// ─────────────────────────────
bool fsbg_move(FSDriver* src, FSDriver* dst, const char* src_name, const char* dst_name) {
    if (!src || !dst || !src_name || !dst_name) {
        kprintf("[fsbg] invalid args\n");
        return false;
    }

    fsbg_auto_mount_if_needed(src->name);
    fsbg_auto_mount_if_needed(dst->name);

    if (fsbg_is_dir_by_fs(src->name, -1, src_name)) {
        return fsbg_copy_dir_recursive(src, dst, src->name, dst->name, -1, -1, src_name, dst_name, true);
    }

    if (!fsbg_copy(src, dst, src_name, dst_name))
        return false;

    if (!src->remove(src_name)) {
        kprintf("[fsbg] remove failed on %s\n", src->name);
        return false;
    }

    return true;
}

// ─────────────────────────────
// 디스크 간 복사 (디렉터리 지원)
// ─────────────────────────────
bool fsbg_copy_disk(const char* src_arg, const char* dst_arg) {
    extern DiskInfo disks[MAX_DISKS];

    if (!src_arg || !dst_arg) {
        kprint("Usage: cp -b <src>#/<file> <dst>#/<dir>/\n");
        return false;
    }

    int src_disk = src_arg[0] - '0';
    int dst_disk = dst_arg[0] - '0';
    if (src_disk < 0 || src_disk >= MAX_DISKS || dst_disk < 0 || dst_disk >= MAX_DISKS) {
        kprint("[cp -b] invalid disk number\n");
        return false;
    }

    const char* src_path = strchr(src_arg, '#');
    const char* dst_path = strchr(dst_arg, '#');
    if (!src_path || !dst_path) {
        kprint("Usage: cp -b <src>#/<file> <dst>#/<dir>/\n");
        return false;
    }
    src_path++;
    dst_path++;

    const char* src_fs = disks[src_disk].fs_type;
    const char* dst_fs = disks[dst_disk].fs_type;

    FSDriver* src = fsbg_get_driver_by_fs(src_fs);
    FSDriver* dst = fsbg_get_driver_by_fs(dst_fs);
    if (!src || !dst) {
        kprintf("[cp -b] unsupported fs (src=%s, dst=%s)\n", src_fs, dst_fs);
        return false;
    }

    // 경로 정리
    char src_fixed[128];
    strncpy(src_fixed, src_path, sizeof(src_fixed) - 1);
    src_fixed[sizeof(src_fixed) - 1] = '\0';

    char dst_fixed[256];
    strncpy(dst_fixed, dst_path, sizeof(dst_fixed) - 1);
    dst_fixed[sizeof(dst_fixed) - 1] = '\0';

    fsbg_strip_trailing_slash(src_fixed);
    fsbg_strip_trailing_slash(dst_fixed);

    fsbg_auto_mount_if_needed(src->name);
    fsbg_auto_mount_if_needed(dst->name);

    bool src_is_dir = fsbg_is_dir_by_fs(src_fs, src_disk, src_fixed);

    if (src_is_dir) {
        char dst_dir[256];
        const char* base = fsbg_basename(src_fixed);

        if (base[0]) {
            if (!fsbg_join_path(dst_dir, sizeof(dst_dir), dst_fixed, base)) {
                kprint("[cp -b] destination path too long\n");
                return false;
            }
        } else {
            strncpy(dst_dir, dst_fixed, sizeof(dst_dir) - 1);
            dst_dir[sizeof(dst_dir) - 1] = '\0';
        }

        kprintf("[cp -b] %s(%s) -> %s(%s)\n", src_fixed, src_fs, dst_dir, dst_fs);
        return fsbg_copy_dir_recursive(src, dst, src_fs, dst_fs, src_disk, dst_disk, src_fixed, dst_dir, false);
    }

    if (!src->exists(src_fixed)) {
        kprintf("[cp -b] source not found: %s\n", src_fixed);
        return false;
    }

    // 파일 이름 추출
    char filename[64];
    const char* basename = strrchr(src_fixed, '/');
    strcpy(filename, basename ? basename + 1 : src_fixed);

    // 대상 경로 구성
    char dst_full[256];
    if (!fsbg_join_path(dst_full, sizeof(dst_full), dst_fixed, filename)) {
        kprint("[cp -b] destination path too long\n");
        return false;
    }

    kprintf("[cp -b] %s(%s) -> %s(%s)\n", src_fixed, src_fs, dst_full, dst_fs);

    // 복사 실행
    return fsbg_copy_file_disk(src, dst, src_fs, dst_fs, src_disk, dst_disk, src_fixed, dst_full);
}
