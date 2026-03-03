#include "fscmd.h"
#include "fat16.h"
#include "fat32.h"
#include "xvfs.h"
#include "disk.h"
#include "../kernel/io/console.h"
#include "../drivers/blockdev.h"
#include "../libc/string.h"
#include "../kernel/kernel.h"
#include "../mm/mem.h"
#include "../drivers/keyboard.h"

fs_type_t current_fs = FS_NONE;
int current_drive = -1;

extern DiskInfo disks[MAX_DISKS];   // disk_t 대신 DiskInfo
char current_path[256] = "/";
static uint32_t fat_mount_owner_uid = 0;
static uint32_t fat_mount_owner_gid = 0;
static uint8_t fat_mount_mode = FSCMD_MODE_OWNER_RW;
static bool single_root_mode = true;
#define FSCMD_MAX_MOUNTS 8
static fscmd_mount_info_t g_mounts[FSCMD_MAX_MOUNTS];

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

typedef struct {
    fs_type_t fs;
    int drive;
    char abs_path[256];
    char subpath[256];
} fscmd_resolved_path_t;

static bool fscmd_parse_drive_prefix(const char* path, int* out_drive, const char** out_subpath);

static fs_type_t fscmd_fs_from_disk_type(const char* type) {
    if (!type) return FS_NONE;
    if (strcmp(type, "FAT16") == 0) return FS_FAT16;
    if (strcmp(type, "FAT32") == 0) return FS_FAT32;
    if (strcmp(type, "XVFS") == 0) return FS_XVFS;
    return FS_NONE;
}

static void fscmd_normalize_mount_target(const char* in, char* out, size_t out_len) {
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!in || in[0] != '/') {
        return;
    }
    if (in[1] == '\0') {
        strcpy(out, "/");
        return;
    }

    size_t n = strlen(in);
    while (n > 1 && in[n - 1] == '/') {
        n--;
    }
    if (n >= out_len) {
        n = out_len - 1;
    }
    memcpy(out, in, n);
    out[n] = '\0';
}

static int fscmd_find_mount_target(const char* target) {
    if (!target) return -1;
    for (int i = 0; i < FSCMD_MAX_MOUNTS; i++) {
        if (g_mounts[i].target[0] == '\0') continue;
        if (strcmp(g_mounts[i].target, target) == 0) {
            return i;
        }
    }
    return -1;
}

static bool fscmd_mount_target_matches(const char* abs_path, const char* target, size_t* out_len) {
    if (!abs_path || !target || target[0] != '/') {
        return false;
    }
    if (strcmp(target, "/") == 0) {
        if (out_len) *out_len = 1;
        return true;
    }
    size_t tlen = strlen(target);
    if (strncmp(abs_path, target, tlen) != 0) {
        return false;
    }
    if (abs_path[tlen] != '\0' && abs_path[tlen] != '/') {
        return false;
    }
    if (out_len) *out_len = tlen;
    return true;
}

static int fscmd_alloc_mount_slot(void) {
    for (int i = 0; i < FSCMD_MAX_MOUNTS; i++) {
        if (g_mounts[i].target[0] == '\0') {
            return i;
        }
    }
    return -1;
}

static bool fscmd_has_live_mount(fs_type_t fs, int drive) {
    for (int i = 0; i < FSCMD_MAX_MOUNTS; i++) {
        if (g_mounts[i].target[0] == '\0') {
            continue;
        }
        if (g_mounts[i].fs == fs && g_mounts[i].drive == drive) {
            return true;
        }
    }
    return false;
}

static void fscmd_normalize_path(const char* input, char* out, size_t out_len) {
    char parts[64][64];
    int depth = 0;

    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!input || input[0] == '\0') {
        strcpy(out, "/");
        return;
    }

    const char* p = input;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        char part[64];
        int len = 0;
        while (*p && *p != '/' && len < (int)sizeof(part) - 1) {
            part[len++] = *p++;
        }
        part[len] = '\0';

        if (strcmp(part, ".") == 0) {
            continue;
        }
        if (strcmp(part, "..") == 0) {
            if (depth > 0) depth--;
            continue;
        }
        if (depth < (int)(sizeof(parts) / sizeof(parts[0]))) {
            strncpy(parts[depth], part, sizeof(parts[depth]) - 1);
            parts[depth][sizeof(parts[depth]) - 1] = '\0';
            depth++;
        }
    }

    if (depth == 0) {
        strcpy(out, "/");
        return;
    }

    size_t used = 0;
    out[used++] = '/';
    for (int i = 0; i < depth; i++) {
        size_t plen = strlen(parts[i]);
        if (used + plen + 1 >= out_len) {
            break;
        }
        memcpy(out + used, parts[i], plen);
        used += plen;
        if (i != depth - 1) {
            out[used++] = '/';
        }
    }
    out[used] = '\0';
}

static bool fscmd_make_absolute_path(const char* path, char* out, size_t out_len) {
    char raw[256];
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';

    if (!path || path[0] == '\0') {
        path = current_path;
    }

    if (!path || path[0] == '\0') {
        strcpy(out, "/");
        return true;
    }

    if (path[0] == '/') {
        strncpy(raw, path, sizeof(raw) - 1);
        raw[sizeof(raw) - 1] = '\0';
    } else {
        const char* cwd = current_path[0] ? current_path : "/";
        int n = snprintf(raw, sizeof(raw), "%s%s%s",
                         cwd,
                         (cwd[strlen(cwd) - 1] == '/') ? "" : "/",
                         path);
        if (n <= 0 || (size_t)n >= sizeof(raw)) {
            return false;
        }
    }

    fscmd_normalize_path(raw, out, out_len);
    return true;
}

static bool fscmd_resolve_path(const char* path, fscmd_resolved_path_t* out) {
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->drive = -1;

    int path_drive = -1;
    const char* pref = NULL;
    if (fscmd_parse_drive_prefix(path, &path_drive, &pref)) {
        fs_type_t fs = fscmd_fs_from_disk_type(disks[path_drive].fs_type);
        if (fs == FS_NONE) {
            return false;
        }
        char abs[256];
        if (!pref || pref[0] == '\0') {
            strcpy(abs, "/");
        } else if (pref[0] == '/') {
            strncpy(abs, pref, sizeof(abs) - 1);
            abs[sizeof(abs) - 1] = '\0';
        } else {
            int n = snprintf(abs, sizeof(abs), "/%s", pref);
            if (n <= 0 || (size_t)n >= sizeof(abs)) {
                return false;
            }
        }
        fscmd_normalize_path(abs, out->abs_path, sizeof(out->abs_path));
        strncpy(out->subpath, out->abs_path, sizeof(out->subpath) - 1);
        out->subpath[sizeof(out->subpath) - 1] = '\0';
        out->fs = fs;
        out->drive = path_drive;
        return true;
    }

    if (!fscmd_make_absolute_path(path, out->abs_path, sizeof(out->abs_path))) {
        return false;
    }

    int best = -1;
    size_t best_len = 0;
    for (int i = 0; i < FSCMD_MAX_MOUNTS; i++) {
        if (g_mounts[i].target[0] == '\0') {
            continue;
        }
        size_t mlen = 0;
        if (!fscmd_mount_target_matches(out->abs_path, g_mounts[i].target, &mlen)) {
            continue;
        }
        if (best < 0 || mlen > best_len) {
            best = i;
            best_len = mlen;
        }
    }
    if (best < 0) {
        return false;
    }

    out->fs = g_mounts[best].fs;
    out->drive = g_mounts[best].drive;
    if (best_len <= 1) {
        strncpy(out->subpath, out->abs_path, sizeof(out->subpath) - 1);
        out->subpath[sizeof(out->subpath) - 1] = '\0';
    } else {
        const char* rem = out->abs_path + best_len;
        if (*rem == '\0') {
            strcpy(out->subpath, "/");
        } else {
            strncpy(out->subpath, rem, sizeof(out->subpath) - 1);
            out->subpath[sizeof(out->subpath) - 1] = '\0';
        }
    }
    if (out->subpath[0] == '\0') {
        strcpy(out->subpath, "/");
    }
    return true;
}

static void fscmd_sync_legacy_root_mount(void) {
    int idx = fscmd_find_mount_target("/");
    if (idx < 0) {
        current_drive = -1;
        current_fs = FS_NONE;
        return;
    }
    current_drive = g_mounts[idx].drive;
    current_fs = g_mounts[idx].fs;
}

static bool fscmd_mount_backend(fs_type_t fs, int drive, uint32_t base_lba) {
    if (fscmd_has_live_mount(fs, drive)) {
        return true;
    }
    switch (fs) {
        case FS_FAT16:
            return fat16_init((uint8_t)drive, base_lba);
        case FS_FAT32:
            return fat32_init((uint8_t)drive, base_lba);
        case FS_XVFS:
            return xvfs_init((uint8_t)drive, base_lba);
        default:
            return false;
    }
}

bool fscmd_mount_drive_at(int drive, const char* target) {
    if (drive < 0 || drive >= MAX_DISKS) {
        return false;
    }
    if (!disks[drive].present) {
        return false;
    }
    if (!target || target[0] != '/') {
        return false;
    }
    char norm_target[32];
    fscmd_normalize_mount_target(target, norm_target, sizeof(norm_target));
    if (norm_target[0] == '\0') {
        return false;
    }

    fs_type_t fs = fscmd_fs_from_disk_type(disks[drive].fs_type);
    if (fs == FS_NONE) {
        return false;
    }
    if (!fscmd_mount_backend(fs, drive, disks[drive].base_lba)) {
        return false;
    }

    int idx = fscmd_find_mount_target(norm_target);
    if (idx < 0) {
        idx = fscmd_alloc_mount_slot();
        if (idx < 0) {
            return false;
        }
    }

    g_mounts[idx].fs = fs;
    g_mounts[idx].drive = drive;
    strncpy(g_mounts[idx].target, norm_target, sizeof(g_mounts[idx].target) - 1);
    g_mounts[idx].target[sizeof(g_mounts[idx].target) - 1] = '\0';

    fscmd_sync_legacy_root_mount();
    if (strcmp(norm_target, "/") == 0) {
        fscmd_reset_path();
    }
    return true;
}

bool fscmd_unmount(const char* target) {
    char norm_target[32];
    fscmd_normalize_mount_target(target, norm_target, sizeof(norm_target));
    int idx = fscmd_find_mount_target(norm_target);
    if (idx < 0) {
        return false;
    }
    memset(&g_mounts[idx], 0, sizeof(g_mounts[idx]));
    fscmd_sync_legacy_root_mount();
    if (current_fs == FS_NONE) {
        strcpy(current_path, "/");
    }
    return true;
}

void fscmd_clear_mounts(void) {
    memset(g_mounts, 0, sizeof(g_mounts));
    fat16_drive = -1;
    fat32_drive = (uint8_t)-1;
    xvfs_drive = (uint8_t)-1;
    current_drive = -1;
    current_fs = FS_NONE;
    strcpy(current_path, "/");
}

int fscmd_list_mounts(fscmd_mount_info_t* out, int max_entries) {
    if (!out || max_entries <= 0) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < FSCMD_MAX_MOUNTS && n < max_entries; i++) {
        if (g_mounts[i].target[0] == '\0') {
            continue;
        }
        out[n++] = g_mounts[i];
    }
    return n;
}

const char* fs_to_string(fs_type_t type) {
    switch (type) {
        case FS_NONE:  return "NONE";
        case FS_FAT16: return "FAT16";
        case FS_FAT32: return "FAT32";
        case FS_XVFS:  return "XVFS";
        default:       return "UNKNOWN";
    }
}

static bool fscmd_parse_drive_prefix(const char* path, int* out_drive, const char** out_subpath) {
    if (single_root_mode) {
        return false;
    }
    if (!path || !*path) {
        return false;
    }
    int i = 0;
    int drive = 0;
    while (path[i] >= '0' && path[i] <= '9') {
        drive = drive * 10 + (path[i] - '0');
        i++;
    }
    if (i == 0 || path[i] != '#') {
        return false;
    }
    if (drive < 0 || drive >= MAX_DISKS) {
        return false;
    }
    if (!disks[drive].present) {
        return false;
    }
    if (out_drive) {
        *out_drive = drive;
    }
    if (out_subpath) {
        *out_subpath = (path[i + 1] != '\0') ? (path + i + 1) : "/";
    }
    return true;
}

void fscmd_set_single_root_mode(bool enabled) {
    single_root_mode = enabled;
}

bool fscmd_get_single_root_mode(void) {
    return single_root_mode;
}

void fscmd_set_fat_mount_policy(uint32_t owner_uid, uint32_t owner_gid, uint8_t mode) {
    fat_mount_owner_uid = owner_uid;
    fat_mount_owner_gid = owner_gid;
    fat_mount_mode = mode & FSCMD_MODE_OWNER_RW;
}

void fscmd_get_fat_mount_policy(uint32_t* out_owner_uid, uint32_t* out_owner_gid, uint8_t* out_mode) {
    if (out_owner_uid) *out_owner_uid = fat_mount_owner_uid;
    if (out_owner_gid) *out_owner_gid = fat_mount_owner_gid;
    if (out_mode) *out_mode = fat_mount_mode;
}

bool fscmd_get_path_meta(const char* path, uint32_t* out_owner_uid, uint32_t* out_owner_gid, uint8_t* out_mode) {
    if (!out_owner_uid || !out_owner_gid || !out_mode) {
        return false;
    }

    fscmd_resolved_path_t rp;
    if (!fscmd_resolve_path(path, &rp)) {
        return false;
    }

    if (rp.fs == FS_FAT16 || rp.fs == FS_FAT32) {
        *out_owner_uid = fat_mount_owner_uid;
        *out_owner_gid = fat_mount_owner_gid;
        *out_mode = fat_mount_mode;
        return true;
    }

    if (rp.fs == FS_XVFS) {
        if (!fscmd_mount_backend(rp.fs, rp.drive, disks[rp.drive].base_lba)) {
            return false;
        }
        XVFS_FileEntry e;
        if (rp.subpath[0] && xvfs_find_entry(rp.subpath, &e)) {
            *out_owner_uid = 0;
            *out_owner_gid = 0;
            *out_mode = FSCMD_MODE_OWNER_RW;
            return true;
        }
        *out_owner_uid = 0;
        *out_owner_gid = 0;
        *out_mode = FSCMD_MODE_OWNER_RW;
        return true;
    }

    return false;
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
    fscmd_resolved_path_t rp;
    if (!fscmd_resolve_path(path, &rp) ||
        !fscmd_mount_backend(rp.fs, rp.drive, disks[rp.drive].base_lba)) {
        kprint("No filesystem mounted.\n");
        return;
    }

    if (rp.fs == FS_FAT16) {
        fat16_ls(rp.subpath);
    } else if (rp.fs == FS_FAT32) {
        fat32_ls(rp.subpath);
    } else if (rp.fs == FS_XVFS) {
        xvfs_ls(rp.subpath);
    }
}

int fscmd_list_dir(const char* path, char* names, uint8_t* is_dir, uint32_t max_entries, size_t name_len) {
    if (!names || !is_dir || max_entries == 0 || name_len == 0) {
        return -1;
    }
    fscmd_resolved_path_t rp;
    if (!fscmd_resolve_path(path, &rp) ||
        !fscmd_mount_backend(rp.fs, rp.drive, disks[rp.drive].base_lba)) {
        return -1;
    }

    if (rp.fs == FS_FAT16) {
        uint16_t cluster = fat16_resolve_dir(rp.subpath);
        if (cluster == 0xFFFF) {
            return -1;
        }
        return fat16_list_dir_lfn(cluster, names, (bool*)is_dir, (int)max_entries, name_len);
    }

    if (rp.fs == FS_FAT32) {
        uint32_t cluster = fat32_resolve_dir(rp.subpath);
        if (cluster < 2 || cluster >= 0x0FFFFFF8) {
            return -1;
        }
        return fat32_list_dir_lfn(cluster, names, (bool*)is_dir, (int)max_entries, name_len);
    }

    if (rp.fs == FS_XVFS) {
        if (max_entries > 256) {
            max_entries = 256;
        }
        XVFS_FileEntry* entries = (XVFS_FileEntry*)kmalloc(max_entries * sizeof(XVFS_FileEntry), 0, NULL);
        if (!entries) {
            return -1;
        }
        int count = xvfs_read_dir_entries(rp.subpath, entries, max_entries);
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
    return -1;
}

void fscmd_cat(const char* path) {
    fscmd_resolved_path_t rp;
    if (!fscmd_resolve_path(path, &rp) ||
        !fscmd_mount_backend(rp.fs, rp.drive, disks[rp.drive].base_lba)) {
        kprint("No filesystem mounted.\n");
        return;
    }
    if (rp.fs == FS_FAT16) {
        fat16_cat(rp.subpath);
    } else if (rp.fs == FS_FAT32) {
        fat32_cat(rp.subpath);
    } else if (rp.fs == FS_XVFS) {
        xvfs_cat(rp.subpath);
    }
}

bool fscmd_rm(const char* path) {
    fscmd_resolved_path_t rp;
    if (!fscmd_resolve_path(path, &rp) ||
        !fscmd_mount_backend(rp.fs, rp.drive, disks[rp.drive].base_lba)) {
        return false;
    }
    if (rp.fs == FS_FAT16) return fat16_rm(rp.subpath);
    if (rp.fs == FS_FAT32) return fat32_rm(rp.subpath);
    if (rp.fs == FS_XVFS) return xvfs_rm(rp.subpath);
    return false;
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
    fscmd_resolved_path_t rp;
    if (!fscmd_resolve_path(filename, &rp) ||
        !fscmd_mount_backend(rp.fs, rp.drive, disks[rp.drive].base_lba)) {
        return false;
    }

    if (rp.fs == FS_FAT16) {
        int written = fat16_write_file(rp.subpath, data, (int)len);
        return written >= 0;
    }
    if (rp.fs == FS_FAT32) {
        return fat32_write_file(rp.subpath, (const uint8_t*)data, len);
    }
    if (rp.fs == FS_XVFS) {
        return xvfs_write_file(rp.subpath, (const uint8_t*)data, len);
    }
    return false;
}

bool fscmd_write_file_at(const char* filename, uint32_t offset, const char* data, uint32_t len) {
    if (!filename || *filename == '\0') {
        return false;
    }
    if (!data && len > 0) {
        return false;
    }

    uint32_t old_size = fscmd_get_file_size(filename);
    if (offset > old_size) {
        if (offset - old_size > 0xFFFFFFFFu - old_size) {
            return false;
        }
    }
    if (len > 0 && offset > 0xFFFFFFFFu - len) {
        return false;
    }

    uint32_t end_pos = offset + len;
    uint32_t new_size = old_size;
    if (end_pos > new_size) {
        new_size = end_pos;
    }

    uint8_t* merged = NULL;
    if (new_size > 0) {
        merged = (uint8_t*)kmalloc(new_size, 0, NULL);
        if (!merged) {
            return false;
        }
        memset(merged, 0, new_size);
    }

    if (old_size > 0) {
        int read = fscmd_read_file_by_name(filename, merged, old_size);
        if (read < 0 || (uint32_t)read != old_size) {
            if (merged) {
                kfree(merged);
            }
            return false;
        }
    }

    if (len > 0) {
        memcpy(merged + offset, data, len);
    }
    bool ok = fscmd_write_file(filename, (const char*)merged, new_size);
    if (merged) {
        kfree(merged);
    }
    return ok;
}

bool fscmd_append_file(const char* filename, const char* data, uint32_t len) {
    if (!filename || *filename == '\0') {
        return false;
    }
    if (!data && len > 0) {
        return false;
    }
    if (len == 0) {
        return true;
    }

    uint32_t old_size = fscmd_get_file_size(filename);
    if (old_size > 0xFFFFFFFFu - len) {
        return false;
    }

    return fscmd_write_file_at(filename, old_size, data, len);
}

bool fscmd_exists(const char* path) {
    fscmd_resolved_path_t rp;
    if (!fscmd_resolve_path(path, &rp) ||
        !fscmd_mount_backend(rp.fs, rp.drive, disks[rp.drive].base_lba)) {
        return false;
    }

    if (rp.fs == FS_FAT16) return fat16_exists(rp.subpath);
    if (rp.fs == FS_FAT32) return fat32_exists(rp.subpath);
    if (rp.fs == FS_XVFS) return xvfs_exists(rp.subpath);
    return false;
}

int fscmd_read_file_by_name(const char* path, uint8_t* buf, uint32_t size) {
    fscmd_resolved_path_t rp;
    if (!fscmd_resolve_path(path, &rp) ||
        !fscmd_mount_backend(rp.fs, rp.drive, disks[rp.drive].base_lba)) {
        return -1;
    }

    if (rp.fs == FS_FAT16) return fat16_read_file_by_name(rp.subpath, buf, size);
    if (rp.fs == FS_FAT32) return fat32_read_file_by_name(rp.subpath, buf, size);
    if (rp.fs == FS_XVFS) return xvfs_read_file_by_name(rp.subpath, buf, size);
    return -1;
}

// ─────────────────────────────
// 파일 복사 (공통 명령어)
// ─────────────────────────────
bool fscmd_cp(const char* src, const char* dst) {
    fscmd_resolved_path_t s, d;
    if (!fscmd_resolve_path(src, &s) || !fscmd_resolve_path(dst, &d)) {
        return false;
    }
    if (s.fs != d.fs || s.drive != d.drive) {
        kprint("cp: cross-mount copy not supported yet\n");
        return false;
    }
    if (!fscmd_mount_backend(s.fs, s.drive, disks[s.drive].base_lba)) {
        return false;
    }
    if (s.fs == FS_FAT16) return fat16_cp(s.subpath, d.subpath);
    if (s.fs == FS_FAT32) return fat32_cp(s.subpath, d.subpath);
    if (s.fs == FS_XVFS) return xvfs_cp(s.subpath, d.subpath);
    return false;
}

// ─────────────────────────────
// 파일 이동 (공통 명령어)
// ─────────────────────────────
bool fscmd_mv(const char* src, const char* dst) {
    fscmd_resolved_path_t s, d;
    if (!fscmd_resolve_path(src, &s) || !fscmd_resolve_path(dst, &d)) {
        return false;
    }
    if (s.fs != d.fs || s.drive != d.drive) {
        kprint("mv: cross-mount move not supported yet\n");
        return false;
    }
    if (!fscmd_mount_backend(s.fs, s.drive, disks[s.drive].base_lba)) {
        return false;
    }
    if (s.fs == FS_FAT16) return fat16_mv(s.subpath, d.subpath);
    if (s.fs == FS_FAT32) return fat32_mv(s.subpath, d.subpath);
    if (s.fs == FS_XVFS) return xvfs_mv(s.subpath, d.subpath);
    return false;
}

// ─────────────────────────────
// 파일 크기 반환 (공통 명령어)
// ─────────────────────────────
uint32_t fscmd_get_file_size(const char* filename) {
    fscmd_resolved_path_t rp;
    if (!fscmd_resolve_path(filename, &rp) ||
        !fscmd_mount_backend(rp.fs, rp.drive, disks[rp.drive].base_lba)) {
        return 0;
    }

    if (rp.fs == FS_FAT16) return fat16_get_file_size(rp.subpath);
    if (rp.fs == FS_FAT32) return fat32_get_file_size(rp.subpath);
    if (rp.fs == FS_XVFS) return xvfs_get_file_size(rp.subpath);
    return 0;
}

// ─────────────────────────────
// 파일 일부분 읽기 (공통 명령어)
// ─────────────────────────────
bool fscmd_read_file_partial(const char* filename, uint32_t offset, uint8_t* buf, uint32_t size) {
    fscmd_resolved_path_t rp;
    if (!fscmd_resolve_path(filename, &rp) ||
        !fscmd_mount_backend(rp.fs, rp.drive, disks[rp.drive].base_lba)) {
        return false;
    }

    if (rp.fs == FS_FAT16) return fat16_read_file_partial(rp.subpath, offset, buf, size);
    if (rp.fs == FS_FAT32) return fat32_read_file_partial(rp.subpath, offset, buf, size);
    if (rp.fs == FS_XVFS) return xvfs_read_file_partial(rp.subpath, offset, buf, size);
    return false;
}

bool fscmd_mkdir(const char* dirname) {
    fscmd_resolved_path_t rp;
    if (!fscmd_resolve_path(dirname, &rp) ||
        !fscmd_mount_backend(rp.fs, rp.drive, disks[rp.drive].base_lba)) {
        return false;
    }
    if (rp.fs == FS_FAT16) return fat16_mkdir(rp.subpath);
    if (rp.fs == FS_FAT32) return fat32_mkdir(rp.subpath);
    if (rp.fs == FS_XVFS) return xvfs_mkdir(rp.subpath);
    return false;
}

bool fscmd_cd(const char* path) {
    fscmd_resolved_path_t rp;
    if (!fscmd_resolve_path(path, &rp) ||
        !fscmd_mount_backend(rp.fs, rp.drive, disks[rp.drive].base_lba)) {
        return false;
    }

    bool ok = false;
    if (rp.subpath[0] == '\0' ||
        (rp.subpath[0] == '/' && rp.subpath[1] == '\0')) {
        ok = true;
    } else if (rp.fs == FS_FAT16) {
        ok = fat16_resolve_dir(rp.subpath) != 0xFFFF;
    } else if (rp.fs == FS_FAT32) {
        uint32_t cl = fat32_resolve_dir(rp.subpath);
        ok = (cl >= 2 && cl < 0x0FFFFFF8);
    } else if (rp.fs == FS_XVFS) {
        ok = xvfs_is_dir(rp.subpath);
    }

    if (!ok) {
        return false;
    }

    strncpy(current_path, rp.abs_path, sizeof(current_path) - 1);
    current_path[sizeof(current_path) - 1] = '\0';
    return true;
}

bool fscmd_rmdir(const char* dirname) {
    fscmd_resolved_path_t rp;
    if (!fscmd_resolve_path(dirname, &rp) ||
        !fscmd_mount_backend(rp.fs, rp.drive, disks[rp.drive].base_lba)) {
        return false;
    }
    if (rp.fs == FS_FAT16) return fat16_rmdir(rp.subpath);
    if (rp.fs == FS_FAT32) return fat32_rmdir(rp.subpath);
    if (rp.fs == FS_XVFS) return xvfs_rmdir(rp.subpath);
    return false;
}

bool fscmd_find_file(const char* path, void* out_entry) {
    fscmd_resolved_path_t rp;
    if (!fscmd_resolve_path(path, &rp) ||
        !fscmd_mount_backend(rp.fs, rp.drive, disks[rp.drive].base_lba)) {
        return false;
    }
    if (rp.fs == FS_FAT16) return fat16_find_file(rp.subpath, (FAT16_DirEntry*)out_entry);
    if (rp.fs == FS_FAT32) return fat32_find_file(rp.subpath, (FAT32_DirEntry*)out_entry);
    if (rp.fs == FS_XVFS) return xvfs_find_file(rp.subpath, (XVFS_FileEntry*)out_entry);
    return false;
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
    uint32_t total = blockdev_get_sector_count(drive);
    if (total == 0) {
        kprintf("[format] drive %d not detected.\n", drive);
        return false;
    }

    uint32_t base_lba = disks[drive].base_lba;
    uint32_t part_sectors = 0;
    int part_index = -1;

    if (base_lba > 0) {
        uint8_t mbr[512];
        if (blockdev_read(drive, 0, 1, mbr) && mbr[510] == 0x55 && mbr[511] == 0xAA) {
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
        if (blockdev_read(drive, 0, 1, mbr) && mbr[510] == 0x55 && mbr[511] == 0xAA) {
            MBRPart* p = (MBRPart*)(mbr + 0x1BE);
            if (strcmp(type, "fat16") == 0)
                p[part_index].type = 0x06;
            else if (strcmp(type, "fat32") == 0)
                p[part_index].type = 0x0C;
            else if (strcmp(type, "xvfs") == 0)
                p[part_index].type = 0x83;
            blockdev_write(drive, 0, 1, mbr);
        }
    }
    return true;
}

int fscmd_read_file(const char* filename, uint8_t* buffer, uint32_t offset, uint32_t size) {
    fscmd_resolved_path_t rp;
    if (!fscmd_resolve_path(filename, &rp) ||
        !fscmd_mount_backend(rp.fs, rp.drive, disks[rp.drive].base_lba)) {
        return -1;
    }

    if (rp.fs == FS_FAT16) {
        FAT16_DirEntry entry;
        if (!fat16_find_file(rp.subpath, &entry))
            return -1;

        return fat16_read_file(&entry, buffer, offset, size);
    }

    else if (rp.fs == FS_FAT32) {
        return fat32_read_file(rp.subpath, buffer, offset, size);
    }

    else if (rp.fs == FS_XVFS) {
        XVFS_FileEntry entry;
        if (!xvfs_find_file(rp.subpath, &entry))
            return -1;

        return xvfs_read_file(&entry, buffer, offset, size);
    }

    return -1;
}
