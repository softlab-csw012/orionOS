#include "xvfs.h"
#include "fscmd.h"
#include "../drivers/ata.h"
#include "../drivers/screen.h"
#include "../kernel/kernel.h"
#include "../kernel/cmd.h"
#include "../libc/string.h"
#include "../mm/mem.h"

static XVFS_Superblock sb;
static uint32_t xvfs_base_lba = 0;
uint8_t xvfs_drive = 0;
static uint32_t current_dir_block = 0;
uint32_t xvfs_root_block = 1;
extern char current_path[256];

#pragma pack(push, 1)
typedef struct {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_first;
    uint32_t sectors;
} MBRPart;
#pragma pack(pop)

static uint32_t xvfs_resolve_path(const char* path, bool want_dir, char* out_name);

static bool read_block(uint32_t lba, void* buf) {
    return ata_read_sector(xvfs_drive, xvfs_base_lba + lba, buf);
}
static bool write_block(uint32_t lba, const void* buf) {
    return ata_write_sector(xvfs_drive, xvfs_base_lba + lba, buf);
}

static bool probe_xvfs(uint8_t drive, uint32_t base_lba, XVFS_Superblock* out_sb) {
    uint8_t sec0[512];
    uint8_t sec1[512];

    if (!ata_read(drive, base_lba + 0, 1, sec0)) return false;
    if (memcmp(sec0, "XVFS2", 5) != 0) return false;
    if (!ata_read(drive, base_lba + 1, 1, sec1)) return false;

    XVFS_Superblock tmp;
    memcpy(&tmp, sec1, sizeof(XVFS_Superblock));

    if (tmp.magic != XVFS_MAGIC || tmp.block_size != 512)
        return false;

    *out_sb = tmp;
    return true;
}
/*
static bool find_xvfs_in_mbr(uint8_t drive, uint32_t* out_base_lba, XVFS_Superblock* out_sb) {
    uint8_t sec[512];
    if (!ata_read(drive, 0, 1, sec)) return false;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return false;

    const MBRPart* p = (const MBRPart*)(sec + 0x1BE);

    for (size_t i = 0; i < 4; i++) {
        if (p[i].type == 0 || p[i].lba_first == 0)
            continue;

        // XVFS는 특정 type을 지정할 수 없으므로 그냥 모든 파티션에서 검사
        if (probe_xvfs(drive, p[i].lba_first, out_sb)) {
            if (out_base_lba) *out_base_lba = p[i].lba_first;
            return true;
        }
    }

    return false;
}
*/
bool xvfs_init(uint8_t drive, uint32_t base_lba) {
    XVFS_Superblock sb_local;
    if (!probe_xvfs(drive, base_lba, &sb_local)) {
        kprintf("[XVFS] No valid filesystem on drive %d\n", drive);
        return false;
    }

    xvfs_drive = drive;
    xvfs_base_lba = base_lba;
    memcpy(&sb, &sb_local, sizeof(sb));

    current_dir_block = sb.data_start; // ✅ 루트 디렉토리로 설정
    kprintf("[XVFS] Mounted drive %d successfully\n", drive);
    kprintf("  Block size: %u, Root LBA=%u\n", sb.block_size, current_dir_block);
    return true;
}
/*
static int xvfs_find_entry(uint32_t dir_block, const char* name, XVFS_FileEntry* out, int* out_index) {
    uint8_t buf[512];
    read_block(dir_block, buf);
    XVFS_FileEntry* e = (XVFS_FileEntry*)buf;

    for (size_t i = 0; i < 512 / sizeof(XVFS_FileEntry); i++) {
        if (e[i].name[0] == 0x00 || e[i].name[0] == 0xE5) continue;
        if (strncmp(e[i].name, name, XVFS_MAX_NAME) == 0) {
            if (out) memcpy(out, &e[i], sizeof(XVFS_FileEntry));
            if (out_index) *out_index = i;
            return 1;
        }
    }
    return 0;
}
*/

static void xvfs_mark_block(uint32_t block, bool used) {
    uint8_t bitbuf[512];
    uint32_t bits_per_block = 512 * 8;
    uint32_t rel = block;
    uint32_t blk = rel / bits_per_block;
    uint32_t bit_index = rel % bits_per_block;
    uint32_t byte_index = bit_index / 8;
    uint8_t bit = 1 << (bit_index % 8);

    ata_read_sector(xvfs_drive, xvfs_base_lba + sb.bitmap_start + blk, bitbuf);

    if (used) {
        if (!(bitbuf[byte_index] & bit)) {
            bitbuf[byte_index] |= bit;
            if (sb.free_blocks > 0) sb.free_blocks--;
        }
    } else {
        if (bitbuf[byte_index] & bit) {
            bitbuf[byte_index] &= ~bit;
            sb.free_blocks++;
        }
    }

    ata_write_sector(xvfs_drive, xvfs_base_lba + sb.bitmap_start + blk, bitbuf);
}

static inline bool xvfs_is_reserved(uint32_t block) {
    return (block < sb.data_start);
}

static uint32_t xvfs_find_free_block(void) {
    const uint32_t bits_per_block = 512 * 8;
    const uint32_t bitmap_blocks = (sb.total_blocks + bits_per_block - 1) / bits_per_block;
    uint8_t buf[512];

    for (uint32_t blk = 0; blk < bitmap_blocks; blk++) {
        if (!ata_read_sector(xvfs_drive, xvfs_base_lba + sb.bitmap_start + blk, buf))
            return 0;

        for (uint32_t byte = 0; byte < 512; byte++) {
            uint8_t b = buf[byte];
            if (b == 0xFF) continue; // 모두 사용 중이면 패스

            for (int bit = 0; bit < 8; bit++) {
                if (!(b & (1 << bit))) {
                    uint32_t free_block = blk * bits_per_block + (byte * 8 + bit);

                    if (xvfs_is_reserved(free_block))
                        continue; // 예약 영역 건너뜀

                    // 비트맵 갱신 (공용 함수 이용)
                    xvfs_mark_block(free_block, true);
                    return free_block;
                }
            }
        }
    }

    kprint("xvfs: no free blocks available\n");
    return 0;
}

void xvfs_ls(const char* path) {
    uint32_t dir_block;

    if (!path || path[0] == '\0') {
        dir_block = current_dir_block;
    } else {
        dir_block = xvfs_resolve_path(path, true, NULL);
        if (!dir_block) {
            kprint("fl: invalid path\n");
            return;
        }
    }

    uint8_t buf[512];
    read_block(dir_block, buf);
    XVFS_FileEntry* entry = (XVFS_FileEntry*)buf;

    kprint("filename         type             size\n");
    kprint("--------------------------------------\n");

    bool any = false;
    char numbuf[16];

    for (size_t i = 0; i < 512 / sizeof(XVFS_FileEntry); i++) {
        uint8_t first = (uint8_t)entry[i].name[0];
        if (first == 0x00 || first == 0xE5) continue;

        char name[17];
        memcpy(name, entry[i].name, 16);
        name[16] = 0;
        kprint(name);

        int len = strlen(name);
        for (int s = len; s < 16; s++) kprint(" ");

        if (entry[i].attr & 1) {
            kprint("[dir]          ");
            kprint("- bytes\n");
        } else {
            kprint("[file]  ");

            // 파일 사이즈 right align
            itoa(entry[i].size, numbuf, 10);
            int szlen = strlen(numbuf);

            for (int pad = szlen; pad < 8; pad++)
                kprint(" ");

            kprint(numbuf);
            kprint(" bytes\n");
        }

        any = true;
    }

    if (!any)
        kprint("(empty)\n");
}

bool xvfs_find_entry(const char* path, XVFS_FileEntry* out_entry) {
    if (!path || !path[0])
        return false;

    char name[17] = {0};
    uint32_t dir_block = xvfs_resolve_path(path, false, name);
    if (!dir_block || !name[0])
        return false;

    uint8_t buf[512];
    if (!read_block(dir_block, buf))
        return false;

    XVFS_FileEntry* entries = (XVFS_FileEntry*)buf;

    for (size_t i = 0; i < 512 / sizeof(XVFS_FileEntry); i++) {
        uint8_t first = (uint8_t)entries[i].name[0];
        if (first == 0x00 || first == 0xE5) continue;
        if (strncmp(entries[i].name, name, XVFS_MAX_NAME) == 0) {
            if (out_entry) memcpy(out_entry, &entries[i], sizeof(XVFS_FileEntry));
            return true;
        }
    }

    return false;
}

bool xvfs_is_dir(const char* path) {
    if (!path || !path[0])
        return true;

    return xvfs_resolve_path(path, true, NULL) != 0;
}

int xvfs_read_dir_entries(const char* path, XVFS_FileEntry* out_entries, uint32_t max_entries) {
    if (!out_entries || max_entries == 0)
        return -1;

    uint32_t dir_block = 0;
    if (!path || !path[0]) {
        dir_block = current_dir_block;
    } else {
        dir_block = xvfs_resolve_path(path, true, NULL);
    }

    if (!dir_block)
        return -1;

    uint8_t buf[512];
    if (!read_block(dir_block, buf))
        return -1;

    XVFS_FileEntry* entry = (XVFS_FileEntry*)buf;
    uint32_t count = 0;

    for (size_t i = 0; i < 512 / sizeof(XVFS_FileEntry); i++) {
        uint8_t first = (uint8_t)entry[i].name[0];
        if (first == 0x00 || first == 0xE5)
            continue;
        if (count < max_entries)
            out_entries[count++] = entry[i];
    }

    return (int)count;
}

bool xvfs_find_file(const char* path, XVFS_FileEntry* out_entry) {
    char name[17] = {0};

    // ① 부모 디렉터리 블록 찾기
    uint32_t dir_block = xvfs_resolve_path(path, false, name); // false → 파일 대상
    if (!dir_block) {
        kprintf("xvfs_find_file: invalid path: %s\n", path);
        return false;
    }

    // ② 디렉터리 블록 읽기
    uint8_t buf[512];
    if (!read_block(dir_block, buf)) {
        kprintf("xvfs_find_file: failed to read dir block %u\n", dir_block);
        return false;
    }

    XVFS_FileEntry* entries = (XVFS_FileEntry*)buf;

    // ③ 파일 탐색
    for (size_t i = 0; i < 512 / sizeof(XVFS_FileEntry); i++) {
        uint8_t first = (uint8_t)entries[i].name[0];
        if (first == 0x00 || first == 0xE5) continue; // 빈 슬롯 or 삭제됨
        if (entries[i].attr & 1) continue;            // 디렉토리면 패스

        if (strncmp(entries[i].name, name, XVFS_MAX_NAME) == 0) {
            if (out_entry) memcpy(out_entry, &entries[i], sizeof(XVFS_FileEntry));
            return true;
        }
    }

    // ④ 못 찾음
    kprintf("xvfs_find_file: not found: %s\n", path);
    return false;
}

bool xvfs_read_file_range(XVFS_FileEntry* entry, uint32_t offset, uint8_t* out_buf, uint32_t size) {
    if (!entry || !out_buf)
        return false;

    // 파일 크기 초과 방지
    if (offset >= entry->size)
        return false;
    if (offset + size > entry->size)
        size = entry->size - offset;

    uint32_t block_size = sb.block_size;  // 일반적으로 512
    uint8_t tmp[512];
    uint32_t bytes_read = 0;
    uint32_t remaining = size;

    // 파일 시작 블록
    uint32_t start_block = entry->start;
    uint32_t block_offset = offset / block_size;        // 읽기 시작할 블록 인덱스
    uint32_t byte_offset  = offset % block_size;        // 블록 내부 오프셋
    uint32_t current_block = start_block + block_offset;

    // ───── 읽기 루프 ─────
    while (remaining > 0) {
        if (!ata_read_sector(xvfs_drive, xvfs_base_lba + current_block, tmp)) {
            kprintf("xvfs_read_file_range: read error at block %u\n", current_block);
            return false;
        }

        uint32_t to_copy = block_size - byte_offset;
        if (to_copy > remaining)
            to_copy = remaining;

        memcpy(out_buf + bytes_read, tmp + byte_offset, to_copy);

        bytes_read += to_copy;
        remaining  -= to_copy;
        byte_offset = 0;
        current_block++;
    }

    return true;
}

void xvfs_cat(const char* path) {
    char name[17] = {0};

    // ① 부모 디렉토리 블록 얻기
    uint32_t dir_block = xvfs_resolve_path(path, false, name);
    if (!dir_block) {
        kprintf("xvfs: invalid path: %s\n", path);
        return;
    }

    // ② 디렉토리 읽기
    uint8_t buf[512];
    read_block(dir_block, buf);
    XVFS_FileEntry* entry = (XVFS_FileEntry*)buf;

    // ③ 파일 엔트리 탐색
    XVFS_FileEntry* target = NULL;
    for (size_t i = 0; i < 512 / sizeof(XVFS_FileEntry); i++) {
        if ((uint8_t)entry[i].name[0] == 0x00 || (uint8_t)entry[i].name[0] == 0xE5)
            continue;

        if (strncmp(entry[i].name, name, XVFS_MAX_NAME) == 0 && !(entry[i].attr & 1)) {
            target = &entry[i];
            break;
        }
    }

    // ④ 파일이 없으면 메시지 출력
    if (!target) {
        kprintf("xvfs: file not found: %s\n", path);
        return;
    }

    // ⑤ 파일 읽기
    uint32_t start_block = target->start;
    uint32_t size = target->size;

    uint8_t tmp[512];
    uint32_t remaining = size;
    while (remaining > 0) {
        ata_read_sector(xvfs_drive, xvfs_base_lba + start_block, tmp);
        uint32_t chunk = (remaining > 512) ? 512 : remaining;

        for (uint32_t i = 0; i < chunk; i++) {
            if (tmp[i] == '\0') break;
            putchar(tmp[i]);
        }

        remaining -= chunk;
        start_block++;
    }

    kprint("\n");
}

bool xvfs_create_file(const char* fullpath, const uint8_t* data, uint32_t size) {
    uint8_t buf[512];
    char name[17] = {0};
    uint32_t dir_block;

    dir_block = xvfs_resolve_path(fullpath, false, name);

    if (!dir_block) {
        kprintf("xvfs: invalid path: %s\n", fullpath);
        return false;
    }

    read_block(dir_block, buf);
    XVFS_FileEntry* entry = (XVFS_FileEntry*)buf;

    // ───── 빈 엔트리 찾기 ─────
    int slot = -1;
    for (size_t i = 0; i < 512 / sizeof(XVFS_FileEntry); i++) {
        if (entry[i].name[0] == 0 || entry[i].name[0] == (char)0xFF) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        kprintf("xvfs: directory full\n");
        return false;
    }

    // ───── 새 데이터 블록 할당 ─────
    uint32_t start_block = xvfs_find_free_block();
    if (!start_block) {
        kprintf("xvfs: no free blocks\n");
        return false;
    }

    // ───── 파일 엔트리 생성 ─────
    memset(entry[slot].name, 0, 16);
    strncpy(entry[slot].name, name, XVFS_MAX_NAME - 1);
    entry[slot].start = start_block;
    entry[slot].size = size;
    entry[slot].attr = 0; // 일반 파일

    // ───── 데이터 쓰기 ─────
    uint32_t written = 0;
    uint32_t current_block = start_block;

    uint32_t full_sectors = size / 512;
    while (full_sectors > 0) {
        uint16_t count = (full_sectors > 256) ? 256 : (uint16_t)full_sectors;
        ata_write(xvfs_drive, xvfs_base_lba + current_block, count, data + written);
        written += (uint32_t)count * 512;
        current_block += count;
        full_sectors -= count;
        fscmd_write_progress_update(written);
    }

    if (written < size) {
        uint32_t tail = size - written;
        uint8_t tmp[512];
        memset(tmp, 0, 512);
        memcpy(tmp, data + written, tail);
        ata_write_sector(xvfs_drive, xvfs_base_lba + current_block, tmp);
        written += tail;
        current_block++;
        fscmd_write_progress_update(written);
    }

    // ───── 디렉터리 엔트리 저장 ─────
    write_block(dir_block, buf);

    kprintf("xvfs: created '%s' in dir=%u (%u bytes)\n", name, dir_block, size);
    return true;
}

bool xvfs_write_file(const char* fullpath, const uint8_t* data, uint32_t size) {
    uint8_t buf[512];
    char name[17] = {0};
    uint32_t dir_block;

    dir_block = xvfs_resolve_path(fullpath, false, name);

    if (!dir_block) {
        kprintf("xvfs: invalid path: %s\n", fullpath);
        return false;
    }

    read_block(dir_block, buf);
    XVFS_FileEntry* e = (XVFS_FileEntry*)buf;

    XVFS_FileEntry* target = NULL;
    for (size_t i = 0; i < 512 / sizeof(XVFS_FileEntry); i++) {
        if ((uint8_t)e[i].name[0] == 0x00 || (uint8_t)e[i].name[0] == 0xE5)
            continue;
        if (strncmp(e[i].name, name, XVFS_MAX_NAME) == 0) {
            target = &e[i];
            break;
        }
    }

    if (!target) {
        kprintf("xvfs: '%s' not found, creating\n", name);
        return xvfs_create_file(fullpath, data, size);
    }

    // ───── 덮어쓰기 ─────
    uint32_t written = 0;
    uint32_t current_block = target->start;

    uint32_t full_sectors = size / 512;
    while (full_sectors > 0) {
        uint16_t count = (full_sectors > 256) ? 256 : (uint16_t)full_sectors;
        ata_write(xvfs_drive, xvfs_base_lba + current_block, count, data + written);
        written += (uint32_t)count * 512;
        current_block += count;
        full_sectors -= count;
        fscmd_write_progress_update(written);
    }

    if (written < size) {
        uint32_t tail = size - written;
        uint8_t tmp[512];
        memset(tmp, 0, 512);
        memcpy(tmp, data + written, tail);
        ata_write_sector(xvfs_drive, xvfs_base_lba + current_block, tmp);
        written += tail;
        current_block++;
        fscmd_write_progress_update(written);
    }

    target->size = size;
    write_block(dir_block, buf);

    kprintf("xvfs: wrote '%s' (%u bytes)\n", name, size);
    return true;
}

bool xvfs_rm(const char* path) {
    char name[17] = {0};
    uint32_t dir_block = xvfs_resolve_path(path, false, name);

    if (!dir_block) {
        kprintf("xvfs: invalid path: %s\n", path);
        return false;
    }

    uint8_t buf[512];
    read_block(dir_block, buf);
    XVFS_FileEntry* entry = (XVFS_FileEntry*)buf;

    int found = -1;
    uint32_t start_block = 0;
    uint32_t file_blocks = 0;

    for (size_t i = 0; i < 512 / sizeof(XVFS_FileEntry); i++) {
        if ((uint8_t)entry[i].name[0] == 0x00 || (uint8_t)entry[i].name[0] == 0xE5)
            continue;
        if (strncmp(entry[i].name, name, 16) == 0 && !(entry[i].attr & 1)) {
            found = i;
            start_block = entry[i].start;
            file_blocks = (entry[i].size + 511) / 512;
            break;
        }
    }

    if (found == -1) {
        kprintf("xvfs: file not found: %s\n", path);
        return false;
    }

    // 데이터 지우고 블록 해제
    uint8_t zero[512];
    memset(zero, 0, 512);
    for (uint32_t b = 0; b < file_blocks; b++) {
        uint32_t blk = start_block + b;
        if (!xvfs_is_reserved(blk)) {
            ata_write_sector(xvfs_drive, xvfs_base_lba + blk, zero);
            xvfs_mark_block(blk, false);
        }
    }

    // 디렉터리 엔트리 삭제
    memset(&entry[found], 0, sizeof(XVFS_FileEntry));
    entry[found].name[0] = 0xE5;
    write_block(dir_block, buf);

    kprintf("xvfs: deleted '%s' (%u blocks freed)\n", path, file_blocks);
    return true;
}

bool xvfs_exists(const char* filename) {
    return xvfs_find_entry(filename, NULL);
}

uint32_t xvfs_read_file_by_name(const char* filename, uint8_t* outbuf, uint32_t maxsize) {
    XVFS_FileEntry target;
    if (!xvfs_find_entry(filename, &target) || (target.attr & 1)) {
        kprintf("xvfs: file not found: %s\n", filename);
        return 0;
    }

    uint32_t size = target.size;
    if (size > maxsize) size = maxsize;

    uint32_t start = target.start;
    uint8_t tmp[512];
    uint32_t remaining = size;
    uint32_t offset = 0;

    // ✅ 파일 블록은 여전히 xvfs_base_lba 기준으로 읽어야 함
    while (remaining > 0) {
        ata_read_sector(xvfs_drive, xvfs_base_lba + start, tmp);
        uint32_t chunk = (remaining > 512) ? 512 : remaining;
        memcpy(outbuf + offset, tmp, chunk);
        offset += chunk;
        remaining -= chunk;
        start++;
    }

    return size;
}

bool xvfs_cp(const char* src_path, const char* dst_path) {
    uint8_t buffer[CAT_BUF_SIZE];
    char src_name[17] = {0}, dst_name[17] = {0};

    // ① 원본 파일 위치 찾기
    uint32_t src_dir_block = xvfs_resolve_path(src_path, false, src_name);
    if (!src_dir_block) {
        kprintf("xvfs_cp: invalid source path: %s\n", src_path);
        return false;
    }

    // ② 원본 파일 찾기
    uint8_t src_buf[512];
    read_block(src_dir_block, src_buf);
    XVFS_FileEntry* src_entry = (XVFS_FileEntry*)src_buf;

    XVFS_FileEntry* src_target = NULL;
    for (size_t i = 0; i < 512 / sizeof(XVFS_FileEntry); i++) {
        if ((uint8_t)src_entry[i].name[0] == 0x00 || (uint8_t)src_entry[i].name[0] == 0xE5) continue;
        if (strncmp(src_entry[i].name, src_name, XVFS_MAX_NAME) == 0 && !(src_entry[i].attr & 1)) {
            src_target = &src_entry[i];
            break;
        }
    }

    if (!src_target) {
        kprintf("xvfs_cp: source file not found: %s\n", src_name);
        return false;
    }

    // ③ 원본 데이터 읽기
    uint32_t size = src_target->size;
    uint32_t remaining = size;
    uint32_t lba = src_target->start;
    uint32_t offset = 0;

    while (remaining > 0) {
        uint8_t tmp[512];
        ata_read_sector(xvfs_drive, xvfs_base_lba + lba++, tmp);
        uint32_t chunk = (remaining > 512) ? 512 : remaining;
        memcpy(buffer + offset, tmp, chunk);
        offset += chunk;
        remaining -= chunk;
    }

    // ④ 대상 디렉토리 위치 찾기
    uint32_t dst_dir_block = xvfs_resolve_path(dst_path, true, dst_name);
    if (!dst_dir_block) {
        // 부모가 없으면 파일 경로로 판단하고 부모만 resolve
        dst_dir_block = xvfs_resolve_path(dst_path, false, dst_name);
        if (!dst_dir_block) {
            kprintf("xvfs_cp: invalid destination path: %s\n", dst_path);
            return false;
        }
    }

    // ⑤ 기존 파일 있으면 삭제
    uint8_t dst_buf[512];
    read_block(dst_dir_block, dst_buf);
    XVFS_FileEntry* dst_entry = (XVFS_FileEntry*)dst_buf;
    for (size_t i = 0; i < 512 / sizeof(XVFS_FileEntry); i++) {
        if ((uint8_t)dst_entry[i].name[0] == 0x00 || (uint8_t)dst_entry[i].name[0] == 0xE5) continue;
        if (strncmp(dst_entry[i].name, dst_name, XVFS_MAX_NAME) == 0) {
            xvfs_rm(dst_path);
            break;
        }
    }

    // ⑥ 새 파일로 쓰기
    if (!xvfs_write_file(dst_path, buffer, size)) {
        kprintf("xvfs_cp: failed to write destination: %s\n", dst_path);
        return false;
    }

    kprintf("xvfs_cp: copied %s > %s (%u bytes)\n", src_name, dst_name, size);
    return true;
}

bool xvfs_mv(const char* src_path, const char* dst_path) {
    char src_name[17] = {0}, dst_name[17] = {0};

    // ① 원본 디렉토리 찾기
    uint32_t src_dir_block = xvfs_resolve_path(src_path, false, src_name);
    if (!src_dir_block) {
        kprintf("xvfs_mv: invalid source path: %s\n", src_path);
        return false;
    }

    // ② 대상 부모 디렉토리 찾기
    uint32_t dst_dir_block = xvfs_resolve_path(dst_path, false, dst_name);
    if (!dst_dir_block) {
        kprintf("xvfs_mv: invalid destination path: %s\n", dst_path);
        return false;
    }

    // ③ 복사 시도
    if (!xvfs_cp(src_path, dst_path)) {
        kprintf("xvfs_mv: copy failed\n");
        return false;
    }

    // ④ 원본 삭제
    if (!xvfs_rm(src_path)) {
        kprintf("xvfs_mv: failed to delete source\n");
        return false;
    }

    kprintf("xvfs_mv: moved %s > %s\n", src_name, dst_name);
    return true;
}

static uint32_t xvfs_resolve_path(const char* path, bool want_dir, char* out_name) {
    // want_dir = true  → 마지막 토큰이 디렉토리여야 함 (cd, ls 등)
    // want_dir = false → 마지막 토큰은 파일로 간주 (create, cat, rm 등)

    uint32_t dir_block = (path[0] == '/') ? sb.data_start : current_dir_block;

    // 경로 복사
    char tmp[128];
    strncpy(tmp, path, 127);
    tmp[127] = 0;

    char* token = strtok(tmp, "/");
    char last_token[16] = {0};
    char* next = NULL;

    while (token) {
        strcpy(last_token, token);
        next = strtok(NULL, "/");

        uint8_t buf[512];
        read_block(dir_block, buf);
        XVFS_FileEntry* entry = (XVFS_FileEntry*)buf;

        bool found = false;
        for (size_t i = 0; i < 512 / sizeof(XVFS_FileEntry); i++) {
            if ((uint8_t)entry[i].name[0] == 0x00 || (uint8_t)entry[i].name[0] == 0xE5)
                continue;

            if (strncmp(entry[i].name, token, XVFS_MAX_NAME) == 0) {
                if (next != NULL) {
                    // 중간 경로는 반드시 디렉토리
                    if (!(entry[i].attr & 1))
                        return 0;
                    dir_block = entry[i].start;
                    found = true;
                } else {
                    // 마지막 토큰
                    if (want_dir) {
                        // cd, ls, mkdir — 디렉토리만 허용
                        if (entry[i].attr & 1) {
                            dir_block = entry[i].start;
                            found = true;
                        } else {
                            return 0; // 마지막이 파일인데 디렉토리 요구
                        }
                    } else {
                        // cat, write, rm — 부모 디렉토리까지만 반환
                        // out_name에 파일 이름만 남기고 종료
                        if (out_name)
                            strncpy(out_name, token, 16);
                        return dir_block;
                    }
                }
                break;
            }
        }

        // 중간 폴더를 찾지 못함
        if (!found) {
            if (next == NULL && !want_dir) {
                // 마지막 이름이 실제로 존재하지 않아도 → 부모 디렉토리 반환
                if (out_name)
                    strncpy(out_name, token, 16);
                return dir_block;
            }
            return 0;
        }

        token = next;
    }

    if (out_name)
        strncpy(out_name, last_token, 16);

    return dir_block;
}

static bool xvfs_create_dir_at(uint32_t parent_block, const char* name) {
    uint8_t buf[512];
    if (!name || !*name)
        return false;

    if (!read_block(parent_block, buf)) {
        kprint("xvfs: failed to read parent directory\n");
        return false;
    }

    XVFS_FileEntry* entry = (XVFS_FileEntry*)buf;

    for (size_t i = 0; i < 512 / sizeof(XVFS_FileEntry); i++) {
        if ((uint8_t)entry[i].name[0] == 0x00 || (uint8_t)entry[i].name[0] == 0xE5)
            continue;
        if (strncmp(entry[i].name, name, XVFS_MAX_NAME) == 0) {
            kprintf("xvfs: '%s' already exists\n", name);
            return false;
        }
    }

    int slot = -1;
    for (size_t i = 0; i < 512 / sizeof(XVFS_FileEntry); i++) {
        if (entry[i].name[0] == 0 || entry[i].name[0] == (char)0xE5) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        kprint("xvfs: directory full\n");
        return false;
    }

    uint32_t dir_block = xvfs_find_free_block();
    if (!dir_block) {
        kprint("xvfs: no free blocks\n");
        return false;
    }

    memset(entry[slot].name, 0, XVFS_MAX_NAME);
    strncpy(entry[slot].name, name, XVFS_MAX_NAME - 1);
    entry[slot].start = dir_block;
    entry[slot].size = 0;
    entry[slot].attr = 1;

    XVFS_FileEntry newdir[512 / sizeof(XVFS_FileEntry)];
    memset(newdir, 0, sizeof(newdir));

    strcpy(newdir[0].name, ".");
    newdir[0].start = dir_block;
    newdir[0].attr = 1;

    strcpy(newdir[1].name, "..");
    newdir[1].start = parent_block;
    newdir[1].attr = 1;

    if (!write_block(dir_block, newdir)) {
        kprint("xvfs: failed to write new directory block\n");
        return false;
    }

    if (!write_block(parent_block, entry)) {
        kprint("xvfs: failed to update parent directory\n");
        return false;
    }

    kprintf("xvfs: directory '%s' created at block %u (parent=%u)\n",
            name, dir_block, parent_block);

    return true;
}

bool xvfs_mkdir(const char* name) {
    return xvfs_create_dir_at(current_dir_block, name);
}

bool xvfs_mkdir_path(const char* path) {
    if (!path || !*path)
        return false;

    char trimmed[256];
    strncpy(trimmed, path, sizeof(trimmed) - 1);
    trimmed[sizeof(trimmed) - 1] = '\0';

    size_t len = strlen(trimmed);
    while (len > 0 && trimmed[len - 1] == '/') {
        trimmed[len - 1] = '\0';
        len--;
    }
    if (trimmed[0] == '\0')
        return false;

    char name[17] = {0};
    uint32_t parent_block = xvfs_resolve_path(trimmed, false, name);
    if (!parent_block || !name[0]) {
        kprintf("xvfs: invalid path: %s\n", path);
        return false;
    }

    return xvfs_create_dir_at(parent_block, name);
}

bool xvfs_cd(const char* path) {
    if (!path || !*path)
        return false;

    char normalized[256];
    normalize_path(normalized, current_path, path);

    uint32_t dir_block = xvfs_resolve_path(normalized, true, NULL);
    if (!dir_block) {
        kprintf("xvfs: directory not found: %s\n", path);
        return false;
    }

    current_dir_block = dir_block;

    strncpy(current_path, normalized, sizeof(current_path) - 1);
    current_path[sizeof(current_path) - 1] = '\0';
    
    kprintf("xvfs: changed to %s (block=%u)\n", current_path, dir_block);
    return true;
}

bool xvfs_rmdir(const char* path) {
    char name[17] = {0};

    uint32_t parent_block = xvfs_resolve_path(path, false, name);
    if (!parent_block) {
        kprintf("xvfs: invalid path: %s\n", path);
        return false;
    }

    uint8_t buf[512];
    read_block(parent_block, buf);
    XVFS_FileEntry* entry = (XVFS_FileEntry*)buf;

    int found = -1;
    uint32_t dir_block = 0;

    for (size_t i = 0; i < 512 / sizeof(XVFS_FileEntry); i++) {
        if ((uint8_t)entry[i].name[0] == 0x00 || (uint8_t)entry[i].name[0] == 0xE5)
            continue;
        if (strncmp(entry[i].name, name, XVFS_MAX_NAME) == 0 && (entry[i].attr & 1)) {
            found = i;
            dir_block = entry[i].start;
            break;
        }
    }

    if (found == -1) {
        kprintf("xvfs: directory not found: %s\n", path);
        return false;
    }

    uint8_t dirbuf[512];
    read_block(dir_block, dirbuf);
    XVFS_FileEntry* contents = (XVFS_FileEntry*)dirbuf;

    for (size_t j = 0; j < 512 / sizeof(XVFS_FileEntry); j++) {
        uint8_t c = (uint8_t)contents[j].name[0];
        if (c == 0x00 || c == 0xE5)
            continue;
        if (strcmp(contents[j].name, ".") == 0 || strcmp(contents[j].name, "..") == 0)
            continue;

        kprintf("xvfs: directory not empty: %s\n", path);
        return false;
    }

    // 비트맵에서 해제
    xvfs_mark_block(dir_block, false);

    // 부모 엔트리 제거
    memset(&entry[found], 0, sizeof(XVFS_FileEntry));
    entry[found].name[0] = 0xE5;
    write_block(parent_block, buf);

    kprintf("xvfs: directory '%s' removed\n", name);
    return true;
}

uint32_t xvfs_get_file_size(const char* path) {
    XVFS_FileEntry entry;

    if (!xvfs_find_file(path, &entry)) {
        kprintf("xvfs_get_file_size: file not found: %s\n", path);
        return 0;
    }

    return entry.size;
}

int xvfs_read_file(XVFS_FileEntry* entry, uint8_t* out_buf, uint32_t offset, uint32_t size) {
    if (!entry || !out_buf)
        return -1;

    uint32_t file_size = entry->size;

    // offset이 파일 크기를 넘으면 읽을 게 없음
    if (offset >= file_size)
        return 0;

    // 읽을 수 있는 최대 크기 조정
    if (offset + size > file_size)
        size = file_size - offset;

    uint32_t block_size = sb.block_size;   // 일반적으로 512
    uint32_t bytes_to_read = size;
    uint32_t bytes_read = 0;

    uint32_t file_start_block = entry->start;

    uint32_t block_offset = offset / block_size;  // 몇 번째 블록인가?
    uint32_t intra_offset = offset % block_size;  // 블록 안에서 어디서 시작?

    uint32_t current_block = file_start_block + block_offset;

    uint8_t tmp[512];

    while (bytes_to_read > 0)
    {
        // 블록 읽기
        if (!ata_read_sector(xvfs_drive,
                             xvfs_base_lba + current_block,
                             tmp))
        {
            kprintf("xvfs_read_file: read error at block %u\n", current_block);
            return bytes_read > 0 ? (int)bytes_read : -1;
        }

        // 이번 블록에서 얼마나 읽을 수 있는지
        uint32_t available = block_size - intra_offset;
        uint32_t copy = (bytes_to_read < available) ? bytes_to_read : available;

        // 데이터 복사
        memcpy(out_buf + bytes_read, tmp + intra_offset, copy);

        bytes_read   += copy;
        bytes_to_read -= copy;

        // 이후 블록부터는 intra offset 없음
        intra_offset = 0;

        // 다음 블록으로 이동
        current_block++;
    }

    return (int)bytes_read;
}

bool xvfs_read_file_partial(const char* path, uint32_t offset, uint8_t* out_buf, uint32_t size) {
    XVFS_FileEntry entry;

    // ① 파일 찾기
    if (!xvfs_find_file(path, &entry)) {
        kprintf("xvfs_read_file_partial: file not found: %s\n", path);
        return false;
    }

    // ② 범위 체크
    if (offset >= entry.size) {
        kprintf("xvfs_read_file_partial: offset beyond file size (%u >= %u)\n", offset, entry.size);
        return false;
    }

    // ③ 범위를 넘어가면 자동으로 잘라냄
    if (offset + size > entry.size)
        size = entry.size - offset;

    // ④ 부분 읽기 수행
    return xvfs_read_file_range(&entry, offset, out_buf, size);
}

uint32_t xvfs_total_clusters() {
    // 방어 코드 추가
    if (sb.total_blocks == 0 || sb.block_size == 0)
        return 0;
    return sb.total_blocks;
}

// ✅ 남은 블록(빈 블록) 수 계산
uint32_t xvfs_free_clusters() {
    if (sb.total_blocks == 0) return 0;
    if (sb.free_blocks != 0)
        return sb.free_blocks;  // ⚠️ 여기가 문제 가능성 높음

    const uint32_t bits_per_block = 512 * 8;
    const uint32_t bitmap_blocks = (sb.total_blocks + bits_per_block - 1) / bits_per_block;
    uint8_t buf[512];
    uint32_t free_count = 0;

    for (uint32_t blk = 0; blk < bitmap_blocks; blk++) {
        if (!ata_read_sector(xvfs_drive, xvfs_base_lba + sb.bitmap_start + blk, buf))
            continue;
        for (uint32_t byte = 0; byte < 512; byte++) {
            uint8_t b = buf[byte];
            if (b == 0xFF) continue;
            for (int bit = 0; bit < 8; bit++) {
                if (!(b & (1 << bit)))
                    free_count++;
            }
        }
    }

    return free_count;
}

bool xvfs_format_at(uint8_t drive, uint32_t base_lba, uint32_t total_sectors) {
    uint8_t sector[512];
    memset(sector, 0, 512);

    if (total_sectors == 0) {
        kprintf("[XVFS] Drive %d not detected or empty.\n", drive);
        return false;
    }

    // ────────────────────────────────
    // Superblock 계산
    // ────────────────────────────────
    XVFS_Superblock sb;
    memset(&sb, 0, sizeof(XVFS_Superblock));

    sb.magic = XVFS_MAGIC;
    sb.block_size = 512;
    sb.total_blocks = total_sectors;
    sb.bitmap_start = 2;  // sector 0=signature, 1=superblock, 2부터 bitmap
    sb.data_start = 10;   // bitmap 이후 데이터 블록 시작
    sb.root_dir_block = sb.data_start;
    sb.free_blocks = total_sectors - sb.data_start - 1;  // root 한 개 차지

    kprintf("[XVFS] Formatting drive %d (base LBA=%u)...\n", drive, base_lba);
    kprintf("  Total sectors: %u\n", total_sectors);
    kprintf("  Data start: %u\n", sb.data_start);

    // ────────────────────────────────
    // [LBA 0] 시그니처 섹터 ("XVFS2")
    // ────────────────────────────────
    memset(sector, 0, 512);
    memcpy(sector, "XVFS2", 5);
    sector[510] = 0x55;
    sector[511] = 0xAA;
    ata_write_sector(drive, base_lba + 0, sector);

    // ────────────────────────────────
    // [LBA 1] Superblock 저장
    // ────────────────────────────────
    memset(sector, 0, 512);
    memcpy(sector, &sb, sizeof(XVFS_Superblock));
    ata_write_sector(drive, base_lba + 1, sector);

    // ────────────────────────────────
    // [LBA 2~9] 비트맵 영역 초기화
    // ────────────────────────────────
    memset(sector, 0, 512);
    uint32_t bitmap_blocks = sb.data_start - sb.bitmap_start;
    for (uint32_t i = 0; i < bitmap_blocks; i++) {
        ata_write_sector(drive, base_lba + sb.bitmap_start + i, sector);
    }

    // 예약 영역 및 루트 디렉터리 비트마크 설정
    for (uint32_t b = 0; b < sb.data_start + 1; b++) {
        uint32_t bits_per_block = 512 * 8;
        uint32_t blk = b / bits_per_block;
        uint32_t bit = b % bits_per_block;
        uint32_t byte = bit / 8;
        uint8_t mask = 1 << (bit % 8);

        ata_read_sector(drive, base_lba + sb.bitmap_start + blk, sector);
        sector[byte] |= mask;
        ata_write_sector(drive, base_lba + sb.bitmap_start + blk, sector);
    }

    // ────────────────────────────────
    // [Root Directory 블록 초기화]
    // ────────────────────────────────
    XVFS_FileEntry rootdir[512 / sizeof(XVFS_FileEntry)];
    memset(rootdir, 0, sizeof(rootdir));

    ata_write_sector(drive, base_lba + sb.root_dir_block, (const uint8_t*)rootdir);

    // ────────────────────────────────
    // 완료 메시지
    // ────────────────────────────────
    kprintf("[XVFS] Format complete!\n");
    kprintf("  Magic: 0x%X\n", sb.magic);
    kprintf("  Root block: %u\n", sb.root_dir_block);
    kprintf("  Free blocks: %u\n", sb.free_blocks);

    return true;
}

bool xvfs_format(uint8_t drive) {
    uint32_t total_sectors = ata_get_sector_count(drive);
    return xvfs_format_at(drive, 0, total_sectors);
}
