#include "fat16.h"
#include "fscmd.h"
#include "../drivers/ata.h"
#include "../drivers/screen.h"
#include "../libc/string.h"
#include "../mm/mem.h"
#include "../kernel/cmd.h"
#include "../kernel/kernel.h"
#include "../drivers/keyboard.h"
#include "disk.h"
#include <stdbool.h>

FAT16_BPB_t fat16_bpb;

uint32_t fat_start_lba;
uint32_t root_dir_lba;
uint32_t data_region_lba;
uint32_t root_dir_sectors = 0;
uint32_t root_dir_cluster16;
uint16_t current_dir_cluster16 = 0;

#define MAX_LINES 100
#define MAX_LINE_LENGTH 80

char lines[MAX_LINES][MAX_LINE_LENGTH];
int current_line = 0, current_col = 0;
bool insert_mode = false;
extern char current_path[256];
uint32_t fat16_first_data_sector = 0;
static uint16_t fat16_alloc_hint = 2;

bool fat16_is_dir(FAT16_DirEntry* entry);
int fat16_drive = -1;

#define FAT16_LFN_ATTR 0x0F
#define FAT16_LFN_MAX 255
#define FAT16_LFN_CHARS_PER_ENTRY 13
#define FAT16_LFN_MAX_ENTRIES 20

typedef struct __attribute__((packed)) {
    uint8_t Ord;
    uint16_t Name1[5];
    uint8_t Attr;
    uint8_t Type;
    uint8_t Chksum;
    uint16_t Name2[6];
    uint16_t FstClusLO;
    uint16_t Name3[2];
} FAT16_LFNEntry;

typedef struct {
    uint16_t cluster;
    uint16_t sector;
    uint16_t index;
} FAT16_DirSlot;

typedef struct {
    bool active;
    uint8_t checksum;
    int expected;
    char name[FAT16_LFN_MAX + 1];
    uint32_t slot_count;
    FAT16_DirSlot slots[FAT16_LFN_MAX_ENTRIES];
} FAT16_LFNState;

typedef struct {
    FAT16_DirEntry entry;
    FAT16_DirSlot slot;
    bool has_long;
    char long_name[FAT16_LFN_MAX + 1];
    uint32_t lfn_count;
    FAT16_DirSlot lfn_slots[FAT16_LFN_MAX_ENTRIES];
} FAT16_DirItem;

static int fat16_strcasecmp(const char* a, const char* b);
static uint32_t fat16_dir_slot_lba(const FAT16_DirSlot* slot);
static bool fat16_find_entry_slot(uint16_t dir_cluster,
                                  const char* name,
                                  FAT16_DirEntry* out_entry,
                                  FAT16_DirSlot* out_slot,
                                  FAT16_DirSlot* lfn_slots,
                                  uint32_t* lfn_count);

// --- MBR 파티션 엔트리 ---
typedef struct __attribute__((packed)) {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_first;
    uint32_t sectors;
} MBRPart;

// FAT16 타입 값
static inline int is_fat16_ptype(uint8_t t) {
    return (t == 0x04 || t == 0x06 || t == 0x0E);
}

// BPB 앞 점프 코드가 맞는지 확인
static int is_valid_bootjmp(const uint8_t *sec) {
    return (sec[0] == 0xEB && sec[2] == 0x90) || (sec[0] == 0xE9);
}

static int has_sig55AA(const uint8_t *sec) {
    return sec[510] == 0x55 && sec[511] == 0xAA;
}

static int probe_fat16_pbr(uint8_t drive, uint32_t base_lba, FAT16_BPB_t *out_bpb) {
    uint8_t sec[512];
    if (!ata_read(drive, base_lba, 1, sec)) return 0;
    if (!has_sig55AA(sec) || !is_valid_bootjmp(sec)) return 0;

    FAT16_BPB_t bpb;
    // FAT16_BPB_t에 jmpBoot, OEMName 포함 시 offset=0
    memcpy(&bpb, sec, sizeof(FAT16_BPB_t));

    if (bpb.BytsPerSec != 512) return 0;
    if (bpb.SecPerClus == 0 || (bpb.SecPerClus & (bpb.SecPerClus - 1)) != 0 || bpb.SecPerClus > 128) return 0;
    if (bpb.NumFATs == 0) return 0;

    uint32_t TotSec = bpb.TotSec16 ? bpb.TotSec16 : bpb.TotSec32;
    uint32_t FATSz = bpb.FATSz16;
    if (FATSz == 0) return 0;

    uint32_t RootDirSectors =
        ((bpb.RootEntCnt * 32) + (bpb.BytsPerSec - 1)) / bpb.BytsPerSec;
    uint32_t DataSec =
        TotSec - (bpb.RsvdSecCnt + bpb.NumFATs * FATSz + RootDirSectors);
    if (DataSec == 0) return 0;

    uint32_t CountOfClusters = DataSec / bpb.SecPerClus;

    if (CountOfClusters >= 4085 && CountOfClusters < 65525) {
        if (out_bpb) *out_bpb = bpb;
        return 1;
    }
    return 0;
}

// MBR을 읽어 FAT16 파티션을 찾아 base_lba 반환 (없으면 0)
static int find_fat16_in_mbr(uint8_t drive, uint32_t *out_base_lba, FAT16_BPB_t *out_bpb) {
    uint8_t sec[512];
    if (!ata_read(drive, 0, 1, sec)) return 0;
    if (!has_sig55AA(sec)) return 0; // MBR 아님일 수도 있음

    kprint("MBR found on drive "); print_hex(drive); kprint("\n");

    // MBR 파티션 테이블
    const uint8_t *p = sec + 0x1BE;
    for (int i = 0; i < 4; i++) {
        const MBRPart *pe = (const MBRPart *)(p + i * 16);

        if (is_fat16_ptype(pe->type) && pe->lba_first != 0) {
            // 파티션 시작 LBA에서 PBR 검사
            if (probe_fat16_pbr(drive, pe->lba_first, out_bpb)) {
                if (out_base_lba) *out_base_lba = pe->lba_first;
                return 1;
            } else {
                kprint("  -> PBR check failed\n");
            }
        }
    }
    return 0;
}

bool fat16_init(uint8_t drive, uint32_t base_lba) {
    FAT16_BPB_t bpb;
    bool ok = false;

    // 1) base_lba가 이미 주어졌다면 그 위치부터 검사
    if (probe_fat16_pbr(drive, base_lba, &bpb)) {
        ok = true;
    } 
    // 2) 혹시라도 detect에서 못 잡은 경우 fallback
    else if (find_fat16_in_mbr(drive, &base_lba, &bpb)) {
        ok = true;
    } 
    else if (probe_fat16_pbr(drive, 0, &bpb)) {
        base_lba = 0;
        ok = true;
    }

    if (!ok) {
        kprint("No FAT16 volume found on drive ");
        print_hex(drive);
        kprint(" LBA=");
        print_hex(base_lba);
        kprint("\n");
        return false;
    }

    fat16_drive = drive;
    fat16_bpb   = bpb;

    fat_start_lba     = base_lba + bpb.RsvdSecCnt;
    root_dir_sectors  = ((bpb.RootEntCnt * 32) + (bpb.BytsPerSec - 1)) / bpb.BytsPerSec;
    root_dir_lba      = base_lba + bpb.RsvdSecCnt + (bpb.NumFATs * bpb.FATSz16);
    data_region_lba   = root_dir_lba + root_dir_sectors;

    fat16_first_data_sector = data_region_lba;
    fat16_alloc_hint = 2;

    return true;
}

static void read_sector(uint32_t lba, uint8_t* buffer) {
    ata_read(fat16_drive, lba, 1, buffer);
}
static void write_sector(uint32_t lba, uint8_t* buffer) {
    ata_write(fat16_drive, lba, 1, buffer);
}

// ───────────── 일반 디렉토리용 함수들 ─────────────
static bool _find_entry_pos_in_dir(const char* name, uint16_t cluster, uint32_t* out_lba, uint16_t* out_off, FAT16_DirEntry* out_entry) {
    FAT16_DirSlot slot;
    if (!fat16_find_entry_slot(cluster, name, out_entry, &slot, NULL, NULL))
        return false;

    if (out_lba)
        *out_lba = fat16_dir_slot_lba(&slot);
    if (out_off)
        *out_off = slot.index * sizeof(FAT16_DirEntry);
    return true;
}

void split_path(const char* path, char* dir, char* fname) {
    const char* last_slash = strrchr(path, '/');
    if (last_slash) {
        int dir_len = last_slash - path;
        if (dir_len == 0) {
            strcpy(dir, "/");  // 루트
        } else {
            strncpy(dir, path, dir_len);
            dir[dir_len] = '\0';
        }
        strcpy(fname, last_slash + 1);
    } else {
        dir[0] = '\0';          // 루트 대신 빈 문자열
        strcpy(fname, path);
    }
}

uint16_t fat16_resolve_dir(const char* path) {
    // 루트 처리
    if (strcmp(path, "/") == 0) return 0;

    uint16_t cluster = (path[0] == '/') ? 0 : current_dir_cluster16;
    char temp[256];
    strcpy(temp, path);

    char* token = strtok(temp, "/");
    while (token) {
        FAT16_DirEntry entry;
        if (!fat16_find_entry(token, cluster, &entry)) {
            return 0xFFFF; // 못 찾음
        }
        if (!fat16_is_dir(&entry)) {
            // 마지막 토큰이 아니면 디렉토리여야 함
            if (strtok(NULL, "/")) return 0xFFFF;
            else break; // 마지막이면 파일일 수도 있음
        }
        cluster = entry.FirstCluster;
        token = strtok(NULL, "/");
    }
    return cluster;
}

uint16_t fat16_resolve_path(const char* path) {
    char temp[256];
    strncpy(temp, path, sizeof(temp));
    temp[sizeof(temp)-1] = '\0';

    // 시작 디렉토리 결정
    uint16_t cluster = current_dir_cluster16;
    if (temp[0] == '/') {
        cluster = 2;  // 루트
        memmove(temp, temp+1, strlen(temp)); // 앞의 '/' 제거
    }

    char* token = strtok(temp, "/");
    FAT16_DirEntry entry;

    while (token) {
        if (!fat16_find_entry(token, cluster, &entry)) {
            return 0xFFFF; // 못 찾음
        }
        if (!fat16_is_dir(&entry)) {
            return 0xFFFF; // 디렉토리 아님
        }
        cluster = entry.FirstCluster;
        token = strtok(NULL, "/");
    }

    return cluster;
}

static inline uint32_t bytes_per_cluster(void) {
    return (uint64_t)fat16_bpb.BytsPerSec * (uint64_t)fat16_bpb.SecPerClus;
}

static inline uint32_t lba_of_cluster(uint16_t cl) {
    if (cl < 2 || cl >= 0xFFF8) {
        // 잘못된 클러스터 → 보호
        return data_region_lba; 
    }
    return data_region_lba + (uint64_t)(cl - 2) * (uint64_t)fat16_bpb.SecPerClus;
}

static void _format_83(const char* name, char* out_name, char* out_ext) {
    memset(out_name, ' ', 8);
    memset(out_ext, ' ', 3);


    int i = 0, ni = 0, ei = 0;
    bool ext = false;
    while (name[i] && (ni < 8 || ei < 3)) {
        if (name[i] == '.') {
            ext = true;
            ++i;
            continue;
        }
        if (!ext && ni < 8) out_name[ni++] = toupper(name[i]);
        else if (ext && ei < 3) out_ext[ei++] = toupper(name[i]);
        ++i;
    }
}

void format_filename(const char* input, char* name, char* ext) {
    const char* dot = strchr(input, '.');
    memset(name, ' ', 8);
    memset(ext, ' ', 3);
    if (dot) {
        int name_len = dot - input;
        for (int i = 0; i < name_len && i < 8; ++i)
            name[i] = toupper(input[i]);
        for (int i = 0; i < 3 && dot[1 + i]; ++i)
            ext[i] = toupper(dot[1 + i]);
    } else {
        for (int i = 0; i < 8 && input[i]; ++i)
            name[i] = toupper(input[i]);
    }
}

void _get_fullname(FAT16_DirEntry* entry, char* out) {
    int i, j = 0;
    for (i = 0; i < 8 && entry->Name[i] != ' '; i++)
        out[j++] = tolower(entry->Name[i]);
    if (entry->Ext[0] != ' ') {
        out[j++] = '.';
        for (i = 0; i < 3 && entry->Ext[i] != ' '; i++)
        out[j++] = tolower(entry->Ext[i]);
    }
    out[j] = '\0';
}

bool _root_find_free_pos(uint32_t* out_lba, uint16_t* out_off) {
    uint8_t sector[512];
    uint32_t entries_per_sector = fat16_bpb.BytsPerSec / sizeof(FAT16_DirEntry);
    for (uint32_t i=0; i<root_dir_sectors; i++) {
        uint32_t lba = root_dir_lba + i;
        ata_read(fat16_drive, lba, 1, sector);
        for (uint32_t j=0; j<entries_per_sector; j++) {
            FAT16_DirEntry* de = (FAT16_DirEntry*)(sector + j*sizeof(FAT16_DirEntry));
            if ((uint8_t)de->Name[0] == 0x00 || (uint8_t)de->Name[0] == 0xE5) {
                if (out_lba) *out_lba = lba;
                if (out_off) *out_off = (uint16_t)(j*sizeof(FAT16_DirEntry));
                return true;
            }
        }
    }
    return false;
}

static void _write_entry_at(uint32_t lba, uint16_t byte_offset, const FAT16_DirEntry* de) {
    uint8_t sector[512];
    ata_read(fat16_drive, lba, 1, sector);

    memcpy(sector + byte_offset, de, sizeof(FAT16_DirEntry));

    ata_write(fat16_drive, lba, 1, sector);
}

static void _root_write_entry_at(uint32_t lba, uint16_t offset, const FAT16_DirEntry* entry) {
    uint8_t sector[512];
    ata_read(fat16_drive, lba, 1, sector);
    memcpy(sector + offset, entry, sizeof(FAT16_DirEntry));
    ata_write(fat16_drive, lba, 1, sector);
}

static uint16_t _alloc_cluster(void) {
    // FAT 한 장의 엔트리 수 = FATSz16 * BytsPerSec / 2
    uint32_t total = ((uint64_t)fat16_bpb.FATSz16 * (uint64_t)fat16_bpb.BytsPerSec) / 2U;
    uint32_t start = fat16_alloc_hint;
    if (start < 2 || start >= total)
        start = 2;

    for (int pass = 0; pass < 2; pass++) {
        uint32_t i = (pass == 0) ? start : 2;
        uint32_t end = (pass == 0) ? total : start;

        for (; i < end; i++) {
            if (fat16_get_fat_entry((uint16_t)i) == 0x0000) {
                fat16_set_fat_entry((uint16_t)i, 0xFFFF);
                fat16_alloc_hint = (uint16_t)(i + 1);
                return (uint16_t)i;
            }
        }
    }
    return 0;
}

static uint8_t fat16_lfn_checksum(const uint8_t short_name[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + short_name[i]);
    }
    return sum;
}

static bool fat16_lfn_is_valid_char(char c) {
    if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7F)
        return false;
    if (c == '\"' || c == '*' || c == '/' || c == ':' || c == '<' ||
        c == '>' || c == '?' || c == '\\' || c == '|') {
        return false;
    }
    return true;
}

static bool fat16_lfn_prepare_name(const char* in, char* out, size_t out_size) {
    if (!in || !out || out_size == 0)
        return false;

    size_t len = strlen(in);
    while (len > 0 && (in[len - 1] == ' ' || in[len - 1] == '.'))
        len--;

    size_t start = 0;
    while (start < len && in[start] == ' ')
        start++;

    if (start >= len)
        return false;

    size_t out_len = len - start;
    if (out_len > FAT16_LFN_MAX || out_len + 1 > out_size)
        return false;

    for (size_t i = 0; i < out_len; i++) {
        char c = in[start + i];
        if (!fat16_lfn_is_valid_char(c))
            return false;
        out[i] = c;
    }
    out[out_len] = '\0';

    if (strcmp(out, ".") == 0 || strcmp(out, "..") == 0)
        return false;

    return true;
}

static bool fat16_short_valid_char(char c, bool* has_lower) {
    if (c >= 'a' && c <= 'z') {
        if (has_lower) *has_lower = true;
        c -= 32;
    }
    if (c < 0x20 || c > 0x7E)
        return false;
    if (c == ' ' || c == '+' || c == ',' || c == ';' || c == '=' ||
        c == '[' || c == ']' || c == ':')
        return false;
    if (c == '.')
        return false;
    return true;
}

static bool fat16_is_valid_short_name(const char* name, bool* has_lower) {
    if (!name || !name[0])
        return false;

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return true;

    const char* dot = strrchr(name, '.');
    if (dot && dot == name)
        return false;
    if (dot && dot[1] == '\0')
        return false;

    const char* first_dot = strchr(name, '.');
    if (first_dot && dot && first_dot != dot)
        return false;

    size_t base_len;
    size_t ext_len;
    if (dot) {
        base_len = (size_t)(dot - name);
        ext_len = strlen(dot + 1);
    } else {
        base_len = strlen(name);
        ext_len = 0;
    }

    if (base_len < 1 || base_len > 8 || ext_len > 3)
        return false;

    for (size_t i = 0; i < base_len; i++) {
        if (!fat16_short_valid_char(name[i], has_lower))
            return false;
    }
    for (size_t i = 0; i < ext_len; i++) {
        if (!fat16_short_valid_char(dot[1 + i], has_lower))
            return false;
    }

    return true;
}

static bool fat16_name_needs_lfn(const char* name) {
    bool has_lower = false;
    if (!fat16_is_valid_short_name(name, &has_lower))
        return true;
    return has_lower;
}

static void fat16_sanitize_component(const char* in, char* out, size_t out_size, bool* has_lower) {
    size_t j = 0;
    if (out_size == 0)
        return;

    for (size_t i = 0; in[i] && j < out_size - 1; i++) {
        char c = in[i];
        if (c >= 'a' && c <= 'z') {
            if (has_lower) *has_lower = true;
            c -= 32;
        }
        if (c < 0x20 || c > 0x7E || c == ' ' || c == '+' || c == ',' ||
            c == ';' || c == '=' || c == '[' || c == ']' || c == ':')
            continue;
        out[j++] = c;
    }
    out[j] = '\0';
}

static void fat16_extract_base_ext(const char* name,
                                   char* base,
                                   size_t base_size,
                                   char* ext,
                                   size_t ext_size,
                                   bool* has_lower) {
    const char* dot = strrchr(name, '.');
    size_t base_len;
    size_t ext_len;
    if (dot) {
        base_len = (size_t)(dot - name);
        ext_len = strlen(dot + 1);
    } else {
        base_len = strlen(name);
        ext_len = 0;
    }

    char base_tmp[256];
    char ext_tmp[256];

    if (base_len >= sizeof(base_tmp))
        base_len = sizeof(base_tmp) - 1;
    if (ext_len >= sizeof(ext_tmp))
        ext_len = sizeof(ext_tmp) - 1;

    memcpy(base_tmp, name, base_len);
    base_tmp[base_len] = '\0';
    memcpy(ext_tmp, dot ? dot + 1 : "", ext_len);
    ext_tmp[ext_len] = '\0';

    fat16_sanitize_component(base_tmp, base, base_size, has_lower);
    fat16_sanitize_component(ext_tmp, ext, ext_size, has_lower);
}

static void fat16_make_short_name_from_base_ext(const char* base, const char* ext, uint8_t out[11]) {
    memset(out, ' ', 11);
    for (int i = 0; i < 8 && base[i]; i++)
        out[i] = (uint8_t)base[i];
    for (int i = 0; i < 3 && ext[i]; i++)
        out[8 + i] = (uint8_t)ext[i];
}

static bool fat16_short_name_exists(uint16_t dir_cluster, const uint8_t short_name[11]) {
    uint8_t buf[SECTOR_SIZE];

    if (dir_cluster == 0) {
        for (uint32_t s = 0; s < root_dir_sectors; s++) {
            uint32_t lba = root_dir_lba + s;
            read_sector(lba, buf);
            FAT16_DirEntry* entry = (FAT16_DirEntry*)buf;

            for (size_t i = 0; i < SECTOR_SIZE / sizeof(FAT16_DirEntry); i++) {
                if ((uint8_t)entry[i].Name[0] == 0x00)
                    return false;
                if ((uint8_t)entry[i].Name[0] == 0xE5)
                    continue;
                if ((uint8_t)entry[i].Attr == FAT16_LFN_ATTR)
                    continue;

                if (memcmp(entry[i].Name, short_name, 8) == 0 &&
                    memcmp(entry[i].Ext, short_name + 8, 3) == 0)
                    return true;
            }
        }
        return false;
    }

    uint16_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < 0xFFF8) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint8_t s = 0; s < fat16_bpb.SecPerClus; s++) {
            read_sector(lba + s, buf);
            FAT16_DirEntry* entry = (FAT16_DirEntry*)buf;

            for (size_t i = 0; i < SECTOR_SIZE / sizeof(FAT16_DirEntry); i++) {
                if ((uint8_t)entry[i].Name[0] == 0x00)
                    return false;
                if ((uint8_t)entry[i].Name[0] == 0xE5)
                    continue;
                if ((uint8_t)entry[i].Attr == FAT16_LFN_ATTR)
                    continue;

                if (memcmp(entry[i].Name, short_name, 8) == 0 &&
                    memcmp(entry[i].Ext, short_name + 8, 3) == 0)
                    return true;
            }
        }
        cluster = fat16_next_cluster(cluster);
    }

    return false;
}

static int fat16_count_digits(uint32_t n) {
    int digits = 1;
    while (n >= 10) {
        n /= 10;
        digits++;
    }
    return digits;
}

static bool fat16_generate_short_name(uint16_t dir_cluster, const char* long_name, uint8_t out[11]) {
    char base[32] = {0};
    char ext[8] = {0};

    fat16_extract_base_ext(long_name, base, sizeof(base), ext, sizeof(ext), NULL);

    if (!base[0])
        strncpy(base, "FILE", sizeof(base) - 1);

    uint8_t candidate[11];
    fat16_make_short_name_from_base_ext(base, ext, candidate);
    if (!fat16_short_name_exists(dir_cluster, candidate)) {
        memcpy(out, candidate, 11);
        return true;
    }

    for (uint32_t n = 1; n < 10000; n++) {
        int digits = fat16_count_digits(n);
        int prefix_len = 8 - (digits + 1);
        if (prefix_len < 1)
            prefix_len = 1;

        char alias_base[9];
        size_t base_len = strlen(base);
        if ((int)base_len > prefix_len)
            base_len = (size_t)prefix_len;
        memcpy(alias_base, base, base_len);
        alias_base[base_len] = '\0';

        char tmp[9];
        int written = snprintf(tmp, sizeof(tmp), "%s~%u", alias_base, n);
        if (written <= 0 || written >= (int)sizeof(tmp))
            continue;

        fat16_make_short_name_from_base_ext(tmp, ext, candidate);
        if (!fat16_short_name_exists(dir_cluster, candidate)) {
            memcpy(out, candidate, 11);
            return true;
        }
    }

    return false;
}

static void fat16_lfn_reset(FAT16_LFNState* st) {
    if (!st)
        return;
    st->active = false;
    st->checksum = 0;
    st->expected = 0;
    st->name[0] = '\0';
    st->slot_count = 0;
}

static void fat16_lfn_copy_chars(char* dst, const uint16_t* src, size_t count, bool* end_seen) {
    for (size_t i = 0; i < count; i++) {
        uint16_t ch = src[i];
        if (ch == 0x0000) {
            if (end_seen) *end_seen = true;
            dst[i] = '\0';
            continue;
        }
        if (ch == 0xFFFF || (end_seen && *end_seen)) {
            dst[i] = '\0';
            continue;
        }
        dst[i] = (ch <= 0x7F) ? (char)ch : '?';
    }
}

static void fat16_lfn_push(FAT16_LFNState* st, const FAT16_LFNEntry* lfn, const FAT16_DirSlot* slot) {
    uint8_t ord = lfn->Ord;
    uint8_t seq = ord & 0x1F;

    if (ord & 0x40) {
        st->active = true;
        st->checksum = lfn->Chksum;
        st->expected = seq;
        st->slot_count = 0;
        memset(st->name, 0, sizeof(st->name));
    }

    if (!st->active)
        return;

    if (seq == 0 || seq > FAT16_LFN_MAX_ENTRIES) {
        fat16_lfn_reset(st);
        return;
    }

    if (seq != st->expected) {
        fat16_lfn_reset(st);
        return;
    }

    if (st->slot_count < FAT16_LFN_MAX_ENTRIES)
        st->slots[st->slot_count++] = *slot;

    size_t base = (size_t)(seq - 1) * FAT16_LFN_CHARS_PER_ENTRY;
    if (base + FAT16_LFN_CHARS_PER_ENTRY > sizeof(st->name)) {
        fat16_lfn_reset(st);
        return;
    }

    bool end_seen = false;
    uint16_t name1[5];
    uint16_t name2[6];
    uint16_t name3[2];
    for (size_t i = 0; i < 5; i++)
        name1[i] = lfn->Name1[i];
    for (size_t i = 0; i < 6; i++)
        name2[i] = lfn->Name2[i];
    for (size_t i = 0; i < 2; i++)
        name3[i] = lfn->Name3[i];
    fat16_lfn_copy_chars(st->name + base, name1, 5, &end_seen);
    fat16_lfn_copy_chars(st->name + base + 5, name2, 6, &end_seen);
    fat16_lfn_copy_chars(st->name + base + 11, name3, 2, &end_seen);

    st->expected--;
}

static uint32_t fat16_dir_slot_lba(const FAT16_DirSlot* slot) {
    if (slot->cluster == 0)
        return root_dir_lba + slot->sector;
    return cluster_to_lba(slot->cluster) + slot->sector;
}

static void fat16_dir_write_raw(const FAT16_DirSlot* slot, const void* entry_data) {
    uint8_t buf[SECTOR_SIZE];
    uint32_t lba = fat16_dir_slot_lba(slot);
    read_sector(lba, buf);
    memcpy(buf + slot->index * sizeof(FAT16_DirEntry), entry_data, sizeof(FAT16_DirEntry));
    write_sector(lba, buf);
}

static void fat16_dir_mark_deleted(const FAT16_DirSlot* slot) {
    uint8_t buf[SECTOR_SIZE];
    uint32_t lba = fat16_dir_slot_lba(slot);
    read_sector(lba, buf);
    FAT16_DirEntry* entry = (FAT16_DirEntry*)buf;
    entry[slot->index].Name[0] = 0xE5;
    write_sector(lba, buf);
}

typedef bool (*fat16_dir_iter_cb)(const FAT16_DirItem* item, void* ctx);

static bool fat16_iterate_dir(uint16_t dir_cluster, fat16_dir_iter_cb cb, void* ctx) {
    uint8_t buf[SECTOR_SIZE];
    FAT16_LFNState lfn;
    fat16_lfn_reset(&lfn);
    size_t entries_per_sector = SECTOR_SIZE / sizeof(FAT16_DirEntry);

    if (dir_cluster == 0) {
        for (uint16_t s = 0; s < root_dir_sectors; s++) {
            uint32_t lba = root_dir_lba + s;
            read_sector(lba, buf);
            FAT16_DirEntry* entry = (FAT16_DirEntry*)buf;

            for (size_t i = 0; i < entries_per_sector; i++) {
                uint8_t first = (uint8_t)entry[i].Name[0];
                if (first == 0x00)
                    return true;

                FAT16_DirSlot slot = { 0, s, (uint16_t)i };

                if (first == 0xE5) {
                    fat16_lfn_reset(&lfn);
                    continue;
                }

                if ((uint8_t)entry[i].Attr == FAT16_LFN_ATTR) {
                    const FAT16_LFNEntry* lfn_entry = (const FAT16_LFNEntry*)&entry[i];
                    fat16_lfn_push(&lfn, lfn_entry, &slot);
                    continue;
                }

                FAT16_DirItem item;
                memset(&item, 0, sizeof(item));
                item.entry = entry[i];
                item.slot = slot;

                uint8_t short_name[11];
                memcpy(short_name, item.entry.Name, 8);
                memcpy(short_name + 8, item.entry.Ext, 3);
                if (lfn.active && lfn.expected == 0 &&
                    fat16_lfn_checksum(short_name) == lfn.checksum) {
                    item.has_long = true;
                    strncpy(item.long_name, lfn.name, sizeof(item.long_name) - 1);
                    item.long_name[sizeof(item.long_name) - 1] = '\0';
                    item.lfn_count = lfn.slot_count;
                    for (uint32_t k = 0; k < lfn.slot_count; k++)
                        item.lfn_slots[k] = lfn.slots[k];
                } else {
                    item.has_long = false;
                    item.long_name[0] = '\0';
                    item.lfn_count = 0;
                }

                fat16_lfn_reset(&lfn);

                if (!cb(&item, ctx))
                    return false;
            }
        }
        return true;
    }

    uint16_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < 0xFFF8) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint8_t s = 0; s < fat16_bpb.SecPerClus; s++) {
            read_sector(lba + s, buf);
            FAT16_DirEntry* entry = (FAT16_DirEntry*)buf;

            for (size_t i = 0; i < entries_per_sector; i++) {
                uint8_t first = (uint8_t)entry[i].Name[0];
                if (first == 0x00)
                    return true;

                FAT16_DirSlot slot = { cluster, s, (uint16_t)i };

                if (first == 0xE5) {
                    fat16_lfn_reset(&lfn);
                    continue;
                }

                if ((uint8_t)entry[i].Attr == FAT16_LFN_ATTR) {
                    const FAT16_LFNEntry* lfn_entry = (const FAT16_LFNEntry*)&entry[i];
                    fat16_lfn_push(&lfn, lfn_entry, &slot);
                    continue;
                }

                FAT16_DirItem item;
                memset(&item, 0, sizeof(item));
                item.entry = entry[i];
                item.slot = slot;

                uint8_t short_name[11];
                memcpy(short_name, item.entry.Name, 8);
                memcpy(short_name + 8, item.entry.Ext, 3);
                if (lfn.active && lfn.expected == 0 &&
                    fat16_lfn_checksum(short_name) == lfn.checksum) {
                    item.has_long = true;
                    strncpy(item.long_name, lfn.name, sizeof(item.long_name) - 1);
                    item.long_name[sizeof(item.long_name) - 1] = '\0';
                    item.lfn_count = lfn.slot_count;
                    for (uint32_t k = 0; k < lfn.slot_count; k++)
                        item.lfn_slots[k] = lfn.slots[k];
                } else {
                    item.has_long = false;
                    item.long_name[0] = '\0';
                    item.lfn_count = 0;
                }

                fat16_lfn_reset(&lfn);

                if (!cb(&item, ctx))
                    return false;
            }
        }
        cluster = fat16_next_cluster(cluster);
    }

    return true;
}

static bool fat16_find_free_slots(uint16_t dir_cluster, uint32_t needed, FAT16_DirSlot* slots) {
    uint8_t buf[SECTOR_SIZE];
    uint32_t run = 0;
    size_t entries_per_sector = SECTOR_SIZE / sizeof(FAT16_DirEntry);
    uint32_t entries_per_cluster = (uint32_t)fat16_bpb.SecPerClus * entries_per_sector;

    if (dir_cluster == 0) {
        for (uint16_t s = 0; s < root_dir_sectors; s++) {
            uint32_t lba = root_dir_lba + s;
            read_sector(lba, buf);
            FAT16_DirEntry* entry = (FAT16_DirEntry*)buf;

            for (size_t i = 0; i < entries_per_sector; i++) {
                uint8_t first = (uint8_t)entry[i].Name[0];
                if (first == 0x00 || first == 0xE5) {
                    if (run < needed) {
                        slots[run].cluster = 0;
                        slots[run].sector = s;
                        slots[run].index = (uint16_t)i;
                    }
                    run++;
                    if (run == needed)
                        return true;
                } else {
                    run = 0;
                }
            }
        }
        return false;
    }

    uint16_t cluster = dir_cluster;
    uint16_t last = 0;
    while (cluster >= 2 && cluster < 0xFFF8) {
        last = cluster;
        uint32_t lba = cluster_to_lba(cluster);
        for (uint8_t s = 0; s < fat16_bpb.SecPerClus; s++) {
            read_sector(lba + s, buf);
            FAT16_DirEntry* entry = (FAT16_DirEntry*)buf;

            for (size_t i = 0; i < entries_per_sector; i++) {
                uint8_t first = (uint8_t)entry[i].Name[0];
                if (first == 0x00 || first == 0xE5) {
                    if (run < needed) {
                        slots[run].cluster = cluster;
                        slots[run].sector = s;
                        slots[run].index = (uint16_t)i;
                    }
                    run++;
                    if (run == needed)
                        return true;
                } else {
                    run = 0;
                }
            }
        }
        cluster = fat16_next_cluster(cluster);
    }

    if (needed == 0)
        return true;

    if (entries_per_cluster == 0)
        return false;

    uint32_t clusters_needed = (needed + entries_per_cluster - 1) / entries_per_cluster;
    if (clusters_needed == 0)
        clusters_needed = 1;

    uint16_t first_new = 0;
    uint16_t prev = last;
    for (uint32_t c = 0; c < clusters_needed; c++) {
        uint16_t new_cl = _alloc_cluster();
        if (new_cl < 2 || new_cl >= 0xFFF8)
            return false;

        fat16_set_fat_entry(new_cl, 0xFFFF);
        if (prev >= 2)
            fat16_set_fat_entry(prev, new_cl);
        if (!first_new)
            first_new = new_cl;
        prev = new_cl;

        uint8_t zero[SECTOR_SIZE];
        memset(zero, 0, sizeof(zero));
        uint32_t base_lba = cluster_to_lba(new_cl);
        for (uint8_t s = 0; s < fat16_bpb.SecPerClus; s++)
            write_sector(base_lba + s, zero);
    }

    uint32_t remaining = needed;
    uint16_t cl = first_new;
    uint32_t slot_index = 0;
    while (remaining > 0 && cl >= 2 && cl < 0xFFF8) {
        for (uint8_t s = 0; s < fat16_bpb.SecPerClus && remaining > 0; s++) {
            for (size_t i = 0; i < entries_per_sector && remaining > 0; i++) {
                slots[slot_index].cluster = cl;
                slots[slot_index].sector = s;
                slots[slot_index].index = (uint16_t)i;
                slot_index++;
                remaining--;
            }
        }
        if (remaining == 0)
            break;
        cl = fat16_next_cluster(cl);
    }

    return remaining == 0;
}

static void fat16_build_short_name_str(const FAT16_DirEntry* e, char* out, size_t out_size) {
    char name[9];
    char ext[4];
    memcpy(name, e->Name, 8);
    memcpy(ext, e->Ext, 3);
    name[8] = 0;
    ext[3] = 0;

    for (int i = 7; i >= 0 && name[i] == ' '; i--) name[i] = 0;
    for (int i = 2; i >= 0 && ext[i] == ' '; i--) ext[i] = 0;

    if (ext[0])
        snprintf(out, out_size, "%s.%s", name, ext);
    else
        snprintf(out, out_size, "%s", name);
}

static bool fat16_dir_item_matches(const FAT16_DirItem* item, const char* name) {
    if (!item || !name || !name[0])
        return false;

    if (item->has_long && fat16_strcasecmp(item->long_name, name) == 0)
        return true;

    char short_name[16];
    fat16_build_short_name_str(&item->entry, short_name, sizeof(short_name));
    if (fat16_strcasecmp(short_name, name) == 0)
        return true;

    return false;
}

static void fat16_write_lfn_entries(const FAT16_DirSlot* slots,
                                    uint32_t count,
                                    const char* long_name,
                                    uint8_t checksum) {
    size_t name_len = strlen(long_name);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t ord = (uint8_t)(count - i);
        FAT16_LFNEntry lfn;
        memset(&lfn, 0, sizeof(lfn));
        if (i == 0)
            ord |= 0x40;
        lfn.Ord = ord;
        lfn.Attr = FAT16_LFN_ATTR;
        lfn.Type = 0;
        lfn.Chksum = checksum;
        lfn.FstClusLO = 0;

        size_t part_index = (size_t)(ord & 0x1F) - 1;
        size_t start = part_index * FAT16_LFN_CHARS_PER_ENTRY;

        bool end_seen = false;
        for (int j = 0; j < 13; j++) {
            uint16_t ch;
            char c = (start + (size_t)j < name_len) ? long_name[start + j] : '\0';
            if (c == '\0') {
                if (!end_seen) {
                    ch = 0x0000;
                    end_seen = true;
                } else {
                    ch = 0xFFFF;
                }
            } else {
                ch = (uint16_t)(uint8_t)c;
            }

            if (j < 5)
                lfn.Name1[j] = ch;
            else if (j < 11)
                lfn.Name2[j - 5] = ch;
            else
                lfn.Name3[j - 11] = ch;
        }

        fat16_dir_write_raw(&slots[i], &lfn);
    }
}

typedef struct {
    const char* name;
    FAT16_DirEntry* out_entry;
    FAT16_DirSlot* out_slot;
    FAT16_DirSlot* out_lfn_slots;
    uint32_t* out_lfn_count;
    bool found;
} fat16_find_ctx_t;

static bool fat16_find_entry_cb(const FAT16_DirItem* item, void* vctx) {
    fat16_find_ctx_t* c = (fat16_find_ctx_t*)vctx;
    if ((uint8_t)item->entry.Name[0] == 0xE5)
        return true;
    if (fat16_dir_item_matches(item, c->name)) {
        if (c->out_entry)
            *c->out_entry = item->entry;
        if (c->out_slot)
            *c->out_slot = item->slot;
        if (c->out_lfn_slots && item->lfn_count > 0) {
            for (uint32_t i = 0; i < item->lfn_count; i++)
                c->out_lfn_slots[i] = item->lfn_slots[i];
        }
        if (c->out_lfn_count)
            *c->out_lfn_count = item->lfn_count;
        c->found = true;
        return false;
    }
    return true;
}

static bool fat16_find_entry_slot(uint16_t dir_cluster,
                                  const char* name,
                                  FAT16_DirEntry* out_entry,
                                  FAT16_DirSlot* out_slot,
                                  FAT16_DirSlot* lfn_slots,
                                  uint32_t* lfn_count) {
    fat16_find_ctx_t ctx = {
        .name = name,
        .out_entry = out_entry,
        .out_slot = out_slot,
        .out_lfn_slots = lfn_slots,
        .out_lfn_count = lfn_count,
        .found = false,
    };

    fat16_iterate_dir(dir_cluster, fat16_find_entry_cb, &ctx);
    return ctx.found;
}

// 교체: 기존 strcmp(name, filename)==0 대신 8.3 정규화 후 바이트 비교
bool _root_find_entry_pos(const char* filename,
                                 uint32_t* out_lba, uint16_t* out_off,
                                 FAT16_DirEntry* out_de) {
    uint8_t sector[512];
    uint32_t entries_per_sector = fat16_bpb.BytsPerSec / sizeof(FAT16_DirEntry);

    // 입력 파일명을 8.3 대문자/공백패딩으로 정규화
    char wantN[8], wantE[3];
    _format_83(filename, wantN, wantE);

    for (uint32_t i=0; i<root_dir_sectors; i++) {
        uint32_t lba = root_dir_lba + i;
        ata_read(fat16_drive, lba, 1, sector);

        for (uint32_t j=0; j<entries_per_sector; j++) {
            FAT16_DirEntry* de = (FAT16_DirEntry*)(sector + j*sizeof(FAT16_DirEntry));
            if ((uint8_t)de->Name[0] == 0x00) return false;      // 끝
            if ((uint8_t)de->Name[0] == 0xE5) continue;          // 삭제
            if ((uint8_t)de->Attr == 0x0F) continue;             // LFN 무시

            // 8.3 바이트 비교
            if (!memcmp(de->Name, wantN, 8) && !memcmp(de->Ext, wantE, 3)) {
                if (out_lba) *out_lba = lba;
                if (out_off) *out_off = (uint16_t)(j*sizeof(FAT16_DirEntry));
                if (out_de)  *out_de  = *de;
                return true;
            }
        }
    }
    return false;
}

bool fat16_find_file(const char* filename, FAT16_DirEntry* out_entry) {
    return fat16_find_file_path(filename, out_entry);
}

bool fat16_find_file_path(const char* path, FAT16_DirEntry* out) {
    char tmp[256];
    strcpy(tmp, path);

    // 절대 경로면 루트, 아니면 현재 디렉토리
    uint16_t cluster = (tmp[0] == '/') ? 0 : current_dir_cluster16;

    // strtok 같은 동작: '/' 기준으로 토큰화
    char* token = strtok(tmp, "/");
    char* next;

    while (token) {
        next = strtok(NULL, "/");

        FAT16_DirEntry entry;
        if (!fat16_find_entry(token, cluster, &entry)) {
            return false; // 못 찾음
        }

        if (next) {
            // 중간 단계는 무조건 디렉토리여야 함
            if (!fat16_is_dir(&entry)) return false;
            cluster = entry.FirstCluster;
        } else {
            // 마지막은 파일/디렉토리 둘 다 가능
            *out = entry;
            return true;
        }

        token = next;
    }
    return false;
}

bool fat16_exists(const char* filename) {
    FAT16_DirEntry entry;
    return fat16_find_file(filename, &entry);
}

bool fat16_find_file_raw(const char* filename, uint32_t* sector_out, uint16_t* offset_out) {
    // 기존 fat16_find_file() 안에서 파일 찾은 위치 정보를 얻어서 반환하도록 만드셈
    // (간단한 구현 예시이므로, 필요시 루트/서브디렉토리 대응도 추가해야 함)
    char name[8], ext[3];
    format_filename(filename, name, ext);

    uint32_t sector;

    for (int s = 0; s < fat16_bpb.RootEntCnt * 32 / 512; s++) {
        sector = root_dir_lba + s;
        uint8_t buf[512];
        ata_read_sector(fat16_drive, sector, buf);

        for (int i = 0; i < 512; i += 32) {
            if (buf[i] == 0x00) break;
            if (buf[i] == 0xE5) continue;  // deleted
            if (!(buf[i + 11] & 0x08) &&  // not volume label
                memcmp(&buf[i], name, 8) == 0 &&
                memcmp(&buf[i + 8], ext, 3) == 0) {
                *sector_out = sector;
                *offset_out = i;
                return true;
            }
        }
    }

    return false;
}

uint16_t fat16_get_fat_entry(uint16_t cluster) {
    uint32_t fat_offset = cluster * FAT_ENTRY_SIZE;
    uint32_t fat_sector = fat_start_lba + (fat_offset / fat16_bpb.BytsPerSec);
    uint32_t offset_in_sector = fat_offset % fat16_bpb.BytsPerSec;

    uint8_t sector[512];
    ata_read(fat16_drive, fat_sector, 1, sector);

    return *(uint16_t*)(sector + offset_in_sector);
}

void fat16_read_cluster(uint16_t cluster, uint8_t* buffer) {
    uint32_t lba = data_region_lba + (cluster - 2) * fat16_bpb.SecPerClus;
    for (uint8_t i = 0; i < fat16_bpb.SecPerClus; i++) {
        ata_read(fat16_drive, lba + i, 1, buffer + (i * 512));
    }
}

bool fat16_write_cluster(uint16_t cluster, const uint8_t* buf) {
    // cluster 0,1은 FAT에서 예약 영역임.
    if (cluster < 2) return false;

    // 시작 LBA 계산
    uint32_t first_data_lba = fat16_first_data_sector;
    uint32_t start_lba = first_data_lba + 
                         (uint32_t)(cluster - 2) * fat16_bpb.SecPerClus;

    if (!ata_write(fat16_drive, start_lba, fat16_bpb.SecPerClus, buf)) {
        kprintf("[FAT16] write fail at cluster %u\n", cluster);
        return false;
    }
    
    return true;
}

void fat16_set_fat_entry(uint16_t cluster, uint16_t value) {
    uint32_t fat_offset = cluster * 2;
    for (int f = 0; f < fat16_bpb.NumFATs; f++) {
        uint32_t fat_sector = fat_start_lba + f * fat16_bpb.FATSz16 + (fat_offset / 512);
        uint32_t offset_in_sector = fat_offset % 512;

        uint8_t sector[512];
        ata_read(fat16_drive, fat_sector, 1, sector);
        *(uint16_t*)(sector + offset_in_sector) = value;
        ata_write(fat16_drive, fat_sector, 1, sector);
    }
}

static bool fat16_ls_cb(const FAT16_DirItem* item, void* ctx) {
    (void)ctx;
    if (item->entry.Attr & 0x08)
        return true;

    char short_name[16];
    fat16_build_short_name_str(&item->entry, short_name, sizeof(short_name));
    const char* name = (item->has_long && item->long_name[0]) ? item->long_name : short_name;

    kprint(name);
    if (item->entry.Attr & 0x10) kprint("/");

    int namelen = strlen(name) + ((item->entry.Attr & 0x10) ? 1 : 0);
    for (int s = namelen; s < 16; s++) kprint(" ");

    if (item->entry.Attr & 0x10)
        kprint("[dir]          ");
    else
        kprint("[file]  ");

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        kprint("- bytes\n");
        return true;
    }

    if (item->entry.Attr & 0x10) {
        kprint("- bytes\n");
        return true;
    }

    char sizebuf[16];
    itoa(item->entry.FileSize, sizebuf, 10);
    int szlen = strlen(sizebuf);
    for (int pad = szlen; pad < 8; pad++)
        kprint(" ");

    kprint(sizebuf);
    kprint(" bytes\n");
    return true;
}

void fat16_ls(const char* path) {
    uint16_t cluster;

    if (!path || path[0] == '\0') {
        cluster = current_dir_cluster16;
    } else {
        cluster = fat16_resolve_dir(path);
        if (cluster == 0xFFFF) {
            kprint("fl: invalid path\n");
            return;
        }
    }

    kprint("filename         type             size\n");
    kprint("--------------------------------------\n");
    fat16_iterate_dir(cluster, fat16_ls_cb, NULL);
}

typedef struct {
    char* names;
    bool* is_dir;
    int max;
    int count;
    size_t name_len;
} fat16_list_ctx_t;

static bool fat16_list_dir_cb(const FAT16_DirItem* item, void* vctx) {
    fat16_list_ctx_t* ctx = (fat16_list_ctx_t*)vctx;
    if (item->entry.Attr & 0x08)
        return true;

    char short_name[16];
    fat16_build_short_name_str(&item->entry, short_name, sizeof(short_name));
    const char* name = (item->has_long && item->long_name[0]) ? item->long_name : short_name;

    if (!name[0])
        return true;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return true;

    if (ctx->count >= ctx->max)
        return false;

    char* dest = ctx->names + ((size_t)ctx->count * ctx->name_len);
    strncpy(dest, name, ctx->name_len - 1);
    dest[ctx->name_len - 1] = '\0';
    ctx->is_dir[ctx->count] = (item->entry.Attr & 0x10) != 0;
    ctx->count++;
    return true;
}

int fat16_list_dir_lfn(uint16_t cluster, char* names, bool* is_dir, int max_entries, size_t name_len) {
    if (!names || !is_dir || max_entries <= 0 || name_len == 0)
        return -1;

    fat16_list_ctx_t ctx = {
        .names = names,
        .is_dir = is_dir,
        .max = max_entries,
        .count = 0,
        .name_len = name_len,
    };

    fat16_iterate_dir(cluster, fat16_list_dir_cb, &ctx);
    return ctx.count;
}

int fat16_read_file(FAT16_DirEntry* entry, uint8_t* out_buf, uint32_t offset, uint32_t size) {
    if (!entry || entry->FirstCluster == 0) 
        return -1;

    uint32_t file_size = entry->FileSize;

    // offset이 파일 끝을 넘으면 읽을 게 없음
    if (offset >= file_size)
        return 0;

    // 실제 읽을 크기 결정
    uint32_t readable = file_size - offset;
    if (size > readable)
        size = readable;

    uint32_t bytes_to_read = size;
    uint32_t bytes_read = 0;

    uint16_t cluster = entry->FirstCluster;

    // 클러스터 크기 계산
    uint32_t cluster_size = fat16_bpb.SecPerClus * 512;

    static uint8_t cluster_buf_static[32768]; // 최고 cluster size(32KB) 여유
    uint8_t* cluster_buf = cluster_buf_static;
    uint8_t* dest = out_buf;

    // ───────────────────────────────────────
    // 1) offset 위치까지 클러스터/바이트 스킵
    // ───────────────────────────────────────

    // offset이 몇 번째 클러스터인가?
    uint32_t skip_clusters = offset / cluster_size;
    uint32_t intra_offset  = offset % cluster_size;

    // 클러스터 단위 건너뛰기
    for (uint32_t i = 0; i < skip_clusters; i++) {
        cluster = fat16_get_fat_entry(cluster);
        if (cluster >= 0xFFF8) {
            return 0;
        }
    }

    // ───────────────────────────────────────
    // 2) 데이터 읽기 시작
    // ───────────────────────────────────────
    while (cluster < 0xFFF8 && bytes_to_read > 0) {
        fat16_read_cluster(cluster, cluster_buf);

        uint32_t read_start = intra_offset;
        uint32_t avail = cluster_size - read_start;
        uint32_t copy  = (bytes_to_read < avail) ? bytes_to_read : avail;

        if (copy > cluster_size - read_start)
            copy = cluster_size - read_start;

        memcpy(dest, cluster_buf + read_start, copy);

        dest += copy;
        bytes_read += copy;
        bytes_to_read -= copy;
        intra_offset = 0;

        cluster = fat16_get_fat_entry(cluster);
    }

    return bytes_read;
}

bool _find_entry_in_dir(const char* filename, uint16_t cluster, FAT16_DirEntry* out) {
    uint32_t lba;
    uint16_t off;
    return _find_entry_pos_in_dir(filename, cluster, &lba, &off, out);
}

void fat16_cat(const char* path) {
    FAT16_DirEntry entry;

    // 파일 엔트리 찾기 (경로 전체 지원)
    if (!fat16_find_file_path(path, &entry)) {
        kprint("cat: file not found\n");
        return;
    }

    if (fat16_is_dir(&entry)) {
        kprint("cat: is a directory\n");
        return;
    }

    // 파일 읽기
    uint32_t remaining = entry.FileSize;
    uint16_t cl = entry.FirstCluster;
    uint8_t cluster_buf[fat16_bpb.SecPerClus * fat16_bpb.BytsPerSec];

    while (cl >= 2 && cl < 0xFFF8) {
        fat16_read_cluster(cl, cluster_buf);

        uint32_t to_read = (remaining > sizeof(cluster_buf)) ? sizeof(cluster_buf) : remaining;
        for (uint32_t i = 0; i < to_read; i++) {
            putchar((char)cluster_buf[i]);
        }

        remaining -= to_read;
        if (remaining == 0) break;

        cl = fat16_get_fat_entry(cl);
    }

    kprint("\n");
}

int fat16_create_file(const char* filename, int initial_size) {
    char dir[256];
    char name[256];
    split_path(filename, dir, name);

    if (name[0] == '\0') {
        kprint("No filename provided!\n");
        return 0;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        kprint("Invalid filename!\n");
        return 0;
    }

    uint16_t dir_cluster = fat16_resolve_dir(dir);
    if (dir_cluster == 0xFFFF) {
        kprint("Invalid directory path!\n");
        return 0;
    }

    if (fat16_find_entry_slot(dir_cluster, name, NULL, NULL, NULL, NULL)) {
        kprint("File already exists!\n");
        return 0;
    }

    char long_name[FAT16_LFN_MAX + 1];
    bool needs_lfn = fat16_name_needs_lfn(name);
    if (needs_lfn) {
        if (!fat16_lfn_prepare_name(name, long_name, sizeof(long_name))) {
            kprint("Invalid filename!\n");
            return 0;
        }
    } else {
        strncpy(long_name, name, sizeof(long_name) - 1);
        long_name[sizeof(long_name) - 1] = '\0';
    }

    uint8_t short_name[11];
    if (needs_lfn) {
        if (!fat16_generate_short_name(dir_cluster, long_name, short_name)) {
            kprint("Failed to generate short name!\n");
            return 0;
        }
    } else {
        char short_n[8];
        char short_e[3];
        _format_83(name, short_n, short_e);
        memcpy(short_name, short_n, 8);
        memcpy(short_name + 8, short_e, 3);
        if (fat16_short_name_exists(dir_cluster, short_name)) {
            kprint("File already exists!\n");
            return 0;
        }
    }

    uint32_t lfn_count = needs_lfn ?
        (uint32_t)((strlen(long_name) + FAT16_LFN_CHARS_PER_ENTRY - 1) / FAT16_LFN_CHARS_PER_ENTRY) : 0;
    if (lfn_count > FAT16_LFN_MAX_ENTRIES) {
        kprint("Filename too long!\n");
        return 0;
    }

    FAT16_DirSlot slots[FAT16_LFN_MAX_ENTRIES + 1];
    uint32_t needed = lfn_count + 1;
    if (!fat16_find_free_slots(dir_cluster, needed, slots)) {
        kprint("No free slot in target directory!\n");
        return 0;
    }

    FAT16_DirEntry ne;
    memset(&ne, 0, sizeof(ne));
    memcpy(ne.Name, short_name, 8);
    memcpy(ne.Ext, short_name + 8, 3);
    ne.Attr = 0x20;

    /* ---------- 1) 클러스터 하나 먼저 ------------ */
    uint16_t first = _alloc_cluster();
    if (first < 2 || first >= 0xFFF8) {
        kprint("No cluster available!\n");
        return 0;
    }
    fat16_set_fat_entry(first, 0xFFFF);   // end mark
    ne.FirstCluster = first;

    /* ---------- 2) 초기 크기만큼 추가 확보 ---------- */
    uint32_t cluster_size = fat16_bpb.SecPerClus * 512;
    int needed_clusters = (initial_size + cluster_size - 1) / cluster_size;

    uint16_t prev = first;
    for (int i = 1; i < needed_clusters; i++) {
        uint16_t cl = _alloc_cluster();
        if (cl < 2 || cl >= 0xFFF8) {
            kprint("WARNING: partial cluster alloc\n");
            break;
        }

        fat16_set_fat_entry(prev, cl);
        fat16_set_fat_entry(cl, 0xFFFF);

        uint8_t* zero = kmalloc(cluster_size, 0, 0);
        if (zero) {
            memset(zero, 0, cluster_size);
            fat16_write_cluster(cl, zero);
            kfree(zero);
        }

        prev = cl;
    }

    /* ---------- 3) 파일 크기 기록 ---------- */
    ne.FileSize = initial_size;

    /* ---------- 4) LFN/Short 엔트리 저장 ---------- */
    if (lfn_count > 0) {
        uint8_t checksum = fat16_lfn_checksum(short_name);
        fat16_write_lfn_entries(slots, lfn_count, long_name, checksum);
    }
    fat16_dir_write_raw(&slots[lfn_count], &ne);

    return 1;
}

int fat16_write_file(const char* filename, const char* data, int size) {
    if (!filename || (!data && size > 0))
        return -1;

    char dir[256], fname[256];
    split_path(filename, dir, fname);

    // --------- 1) target 디렉토리 ---------
    uint16_t dir_cluster = fat16_resolve_dir(dir);
    if (dir_cluster == 0xFFFF)
        return -1;

    // --------- 2) 기존 파일 삭제 ---------
    FAT16_DirEntry entry;
    FAT16_DirSlot slot;

    bool exists = fat16_find_entry_slot(dir_cluster, fname, &entry, &slot, NULL, NULL);
    if (!exists) {
        if (!fat16_create_file(filename, 0))
            return -1;
        return fat16_write_file(filename, data, size);
    }

    if (entry.Attr & 0x10)
        return -1;

    if (exists) {
        uint16_t cl = entry.FirstCluster;
        uint8_t zero[SECTOR_SIZE];
        memset(zero, 0, SECTOR_SIZE);

        while (cl >= 2 && cl < 0xFFF8) {
            uint16_t next = fat16_next_cluster(cl);
            fat16_set_fat_entry(cl, 0);

            uint32_t cl_lba = cluster_to_lba(cl);
            for (int i = 0; i < fat16_bpb.SecPerClus; i++)
                write_sector(cl_lba + i, zero);

            cl = next;
        }
    }

    // --------- 3) 새 cluster chain 할당 + 쓰기 ---------
    uint32_t remaining = size;
    const uint8_t* p = (const uint8_t*)data;

    uint16_t first_cluster = 0;
    uint16_t prev_cluster  = 0;

    while (remaining > 0) {
        uint16_t cl = _alloc_cluster();
        if (cl < 2 || cl >= 0xFFF8)
            return -1;

        if (!first_cluster)
            first_cluster = cl;
        else
            fat16_set_fat_entry(prev_cluster, cl);

        fat16_set_fat_entry(cl, 0xFFFF);

        uint32_t lba = cluster_to_lba(cl);

        uint32_t tocpy = (remaining > (fat16_bpb.SecPerClus*SECTOR_SIZE))
                         ? (fat16_bpb.SecPerClus*SECTOR_SIZE)
                         : remaining;

        uint32_t full_sectors = tocpy / SECTOR_SIZE;
        uint32_t tail_bytes = tocpy % SECTOR_SIZE;

        if (full_sectors > 0) {
            ata_write(fat16_drive, lba, (uint16_t)full_sectors, p);
        }

        if (tail_bytes > 0) {
            uint8_t tmp[SECTOR_SIZE];
            memset(tmp, 0, SECTOR_SIZE);
            memcpy(tmp, p + (full_sectors * SECTOR_SIZE), tail_bytes);
            ata_write(fat16_drive, lba + full_sectors, 1, tmp);
        }

        remaining -= tocpy;
        p += tocpy;
        prev_cluster = cl;
        fscmd_write_progress_update((uint32_t)size - remaining);
    }

    // --------- 4) 디렉토리 Entry 갱신 ---------
    FAT16_DirEntry ne;
    memset(&ne, 0, sizeof(ne));
    memcpy(ne.Name, entry.Name, 8);
    memcpy(ne.Ext, entry.Ext, 3);
    ne.Attr = 0x20;
    ne.FirstCluster = first_cluster;
    ne.FileSize = size;
    fat16_dir_write_raw(&slot, &ne);

    return size;
}

bool fat16_rm(const char* path) {
    char dir[256], fname[256];
    split_path(path, dir, fname);

    if (fname[0] == '\0') {
        kprint("rm: invalid path\n");
        return false;
    }

    // ── 디렉토리 클러스터 찾기 ──
    uint16_t cluster = fat16_resolve_dir(dir);
    if (cluster == 0xFFFF) {
        kprint("rm: invalid path\n");
        return false;
    }

    FAT16_DirEntry entry;
    FAT16_DirSlot slot;
    FAT16_DirSlot lfn_slots[FAT16_LFN_MAX_ENTRIES];
    uint32_t lfn_count = 0;

    if (!fat16_find_entry_slot(cluster, fname, &entry, &slot, lfn_slots, &lfn_count)) {
        kprint("rm: file not found\n");
        return false;
    }

    // ── 클러스터 체인 삭제 ──
    uint16_t cl = entry.FirstCluster;
    uint8_t zero[512] = {0};

    while (cl >= 2 && cl < 0xFFF8) {
        uint16_t next = fat16_get_fat_entry(cl);

        uint32_t start_lba = lba_of_cluster(cl);
        for (int i = 0; i < fat16_bpb.SecPerClus; i++) {
            ata_write(fat16_drive, start_lba + i, 1, zero);
        }

        fat16_set_fat_entry(cl, 0x0000);
        cl = next;
    }

    // ── 디렉토리 엔트리 삭제 (LFN 포함) ──
    for (uint32_t i = 0; i < lfn_count; i++)
        fat16_dir_mark_deleted(&lfn_slots[i]);
    fat16_dir_mark_deleted(&slot);

    return true;
}

// 디렉토리 여부 판단
bool fat16_is_dir(FAT16_DirEntry* entry) {
    return (entry->Attr & 0x10) != 0;
}

bool fat16_find_entry(const char* name, uint16_t dir_cluster, FAT16_DirEntry* out) {
    return fat16_find_entry_slot(dir_cluster, name, out, NULL, NULL, NULL);
}

uint16_t fat16_next_cluster(uint16_t cluster) {
    uint32_t fat_lba = fat_start_lba + (cluster * 2) / 512;
    uint8_t sector[512];
    ata_read(fat16_drive, fat_lba, 1, sector);

    int offset = (cluster * 2) % 512;
    return *((uint16_t*)(sector + offset));
}

int fat16_read_dir(uint16_t cluster, FAT16_DirEntry* out_entries, int max_entries) {
    int count = 0;
    uint8_t sector[512];

    if (cluster == 0) {
        for (uint32_t i = 0; i < root_dir_sectors && count < max_entries; i++) {
            ata_read(fat16_drive, root_dir_lba + i, 1, sector);
            FAT16_DirEntry* entries = (FAT16_DirEntry*)sector;
            for (size_t j = 0; j < (512 / sizeof(FAT16_DirEntry)) && count < max_entries; j++) {
                if (entries[j].Name[0] == 0x00) {
                    // ✅ break 대신 return
                    return count;
                }
                if ((entries[j].Attr & 0x0F) == 0x0F) continue; // LFN 무시
                if ((uint8_t)entries[j].Name[0] == 0xE5) continue; // 삭제된 엔트리 무시
                out_entries[count++] = entries[j];
            }
        }
    } else {
        uint16_t current = cluster;
        while (current < 0xFFF8 && count < max_entries) {
            uint32_t lba = cluster_to_lba(current);
            for (int s = 0; s < fat16_bpb.SecPerClus; s++) {
                ata_read(fat16_drive, lba + s, 1, sector);
                FAT16_DirEntry* entries = (FAT16_DirEntry*)sector;
                for (size_t j = 0; j < (512 / sizeof(FAT16_DirEntry)) && count < max_entries; j++) {
                    if (entries[j].Name[0] == 0x00) {
                        return count;  // ✅ 여기서도 return
                    }
                    if ((entries[j].Attr & 0x0F) == 0x0F) continue;
                    if ((uint8_t)entries[j].Name[0] == 0xE5) continue;
                    out_entries[count++] = entries[j];
                }
            }
            current = fat16_next_cluster(current);
        }
    }

    return count;
}

static int fat16_strcasecmp(const char* a, const char* b) {
    while (*a && *b) {
        char ca = (*a >= 'a' && *a <= 'z') ? *a - 32 : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? *b - 32 : *b;
        if (ca != cb) return 1;
        a++; b++;
    }
    return (*a == *b) ? 0 : 1;
}

bool compare_filename(const char* name, const char* entry_name, const char* entry_ext) {
    char formatted[13]; // 8 + '.' + 3 + '\0'
    int ni = 0, i = 0;

    // 이름 복원
    for (i = 0; i < 8 && entry_name[i] != ' '; i++)
        formatted[ni++] = entry_name[i];

    // 확장자가 있는 경우
    if (entry_ext[0] != ' ') {
        formatted[ni++] = '.';
        for (i = 0; i < 3 && entry_ext[i] != ' '; i++)
            formatted[ni++] = entry_ext[i];
    }

    formatted[ni] = '\0';

    return fat16_strcasecmp(formatted, name) == 0;
}

uint32_t cluster_to_lba(uint16_t cluster) {
    return data_region_lba + ((cluster - 2) * fat16_bpb.SecPerClus);
}

bool fat16_cd(const char* path) {
    if (!path || !*path)
        return false;

    char normalized[256];
    normalize_path(normalized, current_path, path);

    uint16_t cluster = fat16_resolve_dir(normalized);
    if (cluster == 0xFFFF) {
        //kprintf("Directory not found: %s\n", path);
        return false;
    }

    current_dir_cluster16 = cluster;
    strncpy(current_path, normalized, sizeof(current_path) - 1);
    current_path[sizeof(current_path) - 1] = '\0';

    kprintf("Changed directory to: %s\n", current_path);
    return true;
}

bool fat16_mkdir(const char* dirname) {
    if (!dirname || !*dirname)
        return false;

    char dir[256], name[256];
    split_path(dirname, dir, name);

    if (name[0] == '\0')
        return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        kprint("mkdir: invalid name\n");
        return false;
    }

    uint16_t parent = fat16_resolve_dir(dir);
    if (parent == 0xFFFF) {
        kprint("mkdir: invalid path\n");
        return false;
    }

    if (fat16_find_entry_slot(parent, name, NULL, NULL, NULL, NULL)) {
        kprintf("mkdir: name already exists (%s)\n", name);
        return false;
    }

    char long_name[FAT16_LFN_MAX + 1];
    bool needs_lfn = fat16_name_needs_lfn(name);
    if (needs_lfn) {
        if (!fat16_lfn_prepare_name(name, long_name, sizeof(long_name))) {
            kprint("mkdir: invalid name\n");
            return false;
        }
    } else {
        strncpy(long_name, name, sizeof(long_name) - 1);
        long_name[sizeof(long_name) - 1] = '\0';
    }

    uint8_t short_name[11];
    if (needs_lfn) {
        if (!fat16_generate_short_name(parent, long_name, short_name)) {
            kprint("mkdir: failed to generate short name\n");
            return false;
        }
    } else {
        char short_n[8];
        char short_e[3];
        _format_83(name, short_n, short_e);
        memcpy(short_name, short_n, 8);
        memcpy(short_name + 8, short_e, 3);
        if (fat16_short_name_exists(parent, short_name)) {
            kprintf("mkdir: name already exists (%s)\n", name);
            return false;
        }
    }

    uint32_t lfn_count = needs_lfn ?
        (uint32_t)((strlen(long_name) + FAT16_LFN_CHARS_PER_ENTRY - 1) / FAT16_LFN_CHARS_PER_ENTRY) : 0;
    if (lfn_count > FAT16_LFN_MAX_ENTRIES) {
        kprint("mkdir: name too long\n");
        return false;
    }

    FAT16_DirSlot slots[FAT16_LFN_MAX_ENTRIES + 1];
    uint32_t needed = lfn_count + 1;
    if (!fat16_find_free_slots(parent, needed, slots)) {
        kprint("No free slot in target directory!\n");
        return false;
    }

    // 2️⃣ 새 클러스터 할당
    uint16_t new_cl = _alloc_cluster();
    if (new_cl < 2 || new_cl >= 0xFFF8) {
        kprint("Failed to allocate cluster!\n");
        return false;
    }
    fat16_set_fat_entry(new_cl, 0xFFFF);

    // 3️⃣ 디렉토리 엔트리 만들기
    FAT16_DirEntry new_dir;
    memset(&new_dir, 0, sizeof(FAT16_DirEntry));
    memcpy(new_dir.Name, short_name, 8);
    memcpy(new_dir.Ext, short_name + 8, 3);
    new_dir.Attr = 0x10;  // Directory
    new_dir.FirstCluster = new_cl;
    new_dir.FileSize = 0;

    if (lfn_count > 0) {
        uint8_t checksum = fat16_lfn_checksum(short_name);
        fat16_write_lfn_entries(slots, lfn_count, long_name, checksum);
    }
    fat16_dir_write_raw(&slots[lfn_count], &new_dir);

    // 5️⃣ 새 디렉토리 클러스터 초기화 (. / ..)
    uint8_t sector[512];
    memset(sector, 0, sizeof(sector));

    FAT16_DirEntry* dot = (FAT16_DirEntry*)&sector[0];
    memset(dot, 0, sizeof(FAT16_DirEntry));
    memset(dot->Name, ' ', 8);
    memset(dot->Ext,  ' ', 3);
    dot->Name[0] = '.';
    dot->Attr = 0x10;
    dot->FirstCluster = new_cl;
    dot->FileSize = 0;

    FAT16_DirEntry* dotdot = (FAT16_DirEntry*)&sector[32];
    memset(dotdot, 0, sizeof(FAT16_DirEntry));
    memset(dotdot->Name, ' ', 8);
    memset(dotdot->Ext,  ' ', 3);
    dotdot->Name[0] = '.';
    dotdot->Name[1] = '.';
    dotdot->Attr = 0x10;
    dotdot->FirstCluster = parent;
    dotdot->FileSize = 0;

    ata_write(fat16_drive, cluster_to_lba(new_cl), 1, sector);
    if (fat16_bpb.SecPerClus > 1) {
        uint8_t zero[512];
        memset(zero, 0, sizeof(zero));
        for (uint8_t s = 1; s < fat16_bpb.SecPerClus; s++)
            ata_write(fat16_drive, cluster_to_lba(new_cl) + s, 1, zero);
    }

    return true;
}

bool is_dir_empty(uint16_t clus) {
    uint8_t sector[512];
    uint32_t lba = lba_of_cluster(clus);

    for (int i = 0; i < fat16_bpb.SecPerClus; i++) {
        ata_read(fat16_drive, lba + i, 1, sector);

        for (size_t j = 0; j < 512 / sizeof(FAT16_DirEntry); j++) {
            FAT16_DirEntry* entry = (FAT16_DirEntry*)&sector[j * 32];
            if ((uint8_t)entry->Name[0] == 0x00) return true;  // 더 이상 없음
            if ((uint8_t)entry->Name[0] == 0xE5) continue;     // 삭제된 엔트리
            if ((uint8_t)(entry->Attr & 0x0F) == 0x0F) continue; // LFN 무시

            // '.'와 '..' 제외
            if (!(entry->Name[0] == '.' && (entry->Name[1] == ' ' || entry->Name[1] == '.')))
                return false; // 다른 게 있으면 빈 게 아님
        }
    }
    return true;
}

void free_cluster_chain(uint16_t start) {
    uint16_t cl = start;
    while (cl >= 0x0002 && cl < 0xFFF8) {
        uint16_t next = fat16_get_fat_entry(cl);
        fat16_set_fat_entry(cl, 0x0000);  // Free
        cl = next;
    }
}

bool fat16_rmdir(const char* path) {
    char dir[256], name[256];
    split_path(path, dir, name);

    if (name[0] == '\0')
        return false;

    uint16_t parent = fat16_resolve_dir(dir);
    if (parent == 0xFFFF) {
        kprint("rmdir: invalid path\n");
        return false;
    }

    FAT16_DirEntry entry;

    FAT16_DirSlot slot;
    FAT16_DirSlot lfn_slots[FAT16_LFN_MAX_ENTRIES];
    uint32_t lfn_count = 0;

    if (!fat16_find_entry_slot(parent, name, &entry, &slot, lfn_slots, &lfn_count))
        return false;

    if (!(entry.Attr & 0x10)) return false; // not a directory

    if (!is_dir_empty(entry.FirstCluster)) {
        kprint("Directory not empty!\n");
        return false;
    }

    // Free clusters
    free_cluster_chain(entry.FirstCluster);

    // Delete entry (LFN 포함)
    for (uint32_t i = 0; i < lfn_count; i++)
        fat16_dir_mark_deleted(&lfn_slots[i]);
    fat16_dir_mark_deleted(&slot);

    kprint("Directory removed.\n");
    return true;
}

bool fat16_read_file_partial(const char* filename, uint32_t offset, uint8_t* out_buf, uint32_t size) {
    FAT16_DirEntry entry;
    if (!fat16_find_file(filename, &entry)) return false;

    return fat16_read_file_range(&entry, offset, out_buf, size);
}

bool fat16_read_file_range(FAT16_DirEntry* entry, uint32_t offset, uint8_t* out_buf, uint32_t size) {
    if (!entry || offset >= entry->FileSize) return false;

    if (offset + size > entry->FileSize)
        size = entry->FileSize - offset;

    uint32_t cluster_size = fat16_bpb.SecPerClus * fat16_bpb.BytsPerSec;
    uint8_t* temp = kmalloc(cluster_size, 0, NULL);  // ✅ 스택 대신 heap
    if (!temp) {
        kprint("Error: kmalloc failed in fat16_read_file_range\n");
        return false;
    }

    uint32_t cluster = entry->FirstCluster;
    uint32_t bytes_read = 0;
    uint32_t skip_bytes = offset;

    while (skip_bytes >= cluster_size) {
        cluster = fat16_next_cluster(cluster);
        if (cluster >= 0xFFF8) {
            kfree(temp);
            return false;
        }
        skip_bytes -= cluster_size;
    }

    while (bytes_read < size && cluster < 0xFFF8) {
        fat16_read_cluster(cluster, temp);

        uint32_t copy_start = skip_bytes;
        uint32_t to_copy = cluster_size - copy_start;
        if (to_copy > size - bytes_read)
            to_copy = size - bytes_read;

        memcpy(out_buf + bytes_read, temp + copy_start, to_copy);
        bytes_read += to_copy;
        skip_bytes = 0;

        cluster = fat16_next_cluster(cluster);
    }

    kfree(temp);
    return true;
}

uint32_t fat16_get_file_size(const char* filename) {
    FAT16_DirEntry entry;
    if (fat16_find_file_path(filename, &entry)) {
        return entry.FileSize;
    }
    return 0;
}

int fat16_read_file_by_name(const char* fname, uint8_t* out_buf, uint32_t max_size) {
    FAT16_DirEntry entry;

    if (!fat16_find_file_path(fname, &entry)) {
        return -1;
    }

    // ★ 핵심 보호
    if (entry.FirstCluster < 2 || entry.FirstCluster >= 0xFFF8) {
        return -1;
    }

    return fat16_read_file(&entry, out_buf, 0, max_size);
}

bool fat16_rename(const char* oldname, const char* newname) {
    uint32_t lba;
    uint16_t off;
    FAT16_DirEntry entry;

    char old_dir[256], old_base[256];
    split_path(oldname, old_dir, old_base);

    uint16_t parent = fat16_resolve_dir(old_dir);
    if (parent == 0xFFFF) {
        kprint("mv: invalid source path\n");
        return false;
    }

    if (!_find_entry_pos_in_dir(old_base, parent, &lba, &off, &entry)) {
        kprint("mv: source not found\n");
        return false;
    }

    // 새 이름 8.3 포맷 적용
    _format_83(newname, (char*)entry.Name, (char*)entry.Ext);

    if (parent == 0)
        _root_write_entry_at(lba, off, &entry);
    else
        _write_entry_at(lba, off, &entry);
    return true;
}

// ───────────────────────────────
// dirname: 경로의 디렉토리 부분만 추출
// ex) "foo/bar/a.txt" → "foo/bar"
// ───────────────────────────────
static const char* _basename(const char* path) {
    const char* slash = strrchr(path, '/');
    if (slash) return slash + 1;
    return path;
}

bool fat16_is_directory_by_path(const char* path) {
    FAT16_DirEntry e;
    if (!fat16_find_file_path(path, &e)) return false;
    return fat16_is_dir(&e);
}

// ───────────────────────────────
// 파일 복사
// ───────────────────────────────
bool fat16_cp(const char* src, const char* dst) {
    FAT16_DirEntry dst_entry;
    static char newpath[256];

    // --- Source must exist ---
    if (!fat16_exists(src))
        return false;

    // --- Case 1: cp X → "/" means "/X" ---
    if (strcmp(dst, "/") == 0) {
        snprintf(newpath, sizeof(newpath), "/%s", _basename(src));
        dst = newpath;
    }

    // --- Case 2: dst exists and is directory ---
    else if (fat16_find_file_path(dst, &dst_entry)) {
        if (fat16_is_dir(&dst_entry)) {
            snprintf(newpath, sizeof(newpath), "%s/%s", dst, _basename(src));
            dst = newpath;
        }
    }

    // --- Case 3: dst is directory but missing filename ---
    else if (fat16_is_directory_by_path(dst)) {
        return false;
    }

    // --- Read entire original file ---
    uint32_t size = fat16_get_file_size(src);
    if (size == 0)
        return false;

    uint8_t* buf = kmalloc(size, 0, NULL);
    if (!buf)
        return false;

    if (!fat16_read_file_by_name(src, buf, size)) {
        kfree(buf);
        return false;
    }

    // --- Write ---
    int written = fat16_write_file(dst, (const char*)buf, size);

    kfree(buf);
    return (written > 0);
}

// ───────────────────────────────
// 파일 이동
// ───────────────────────────────
bool fat16_mv(const char* src, const char* dst) {
    FAT16_DirEntry src_entry, dst_entry;
    char newpath[256];

    // ── src 확인 ──
    if (!fat16_find_file_path(src, &src_entry)) {
        kprint("mv: source not found\n");
        return false;
    }

    // ── 1. dst가 "/" (루트) 인 경우 ──
    if (strcmp(dst, "/") == 0) {
        snprintf(newpath, sizeof(newpath), "/%s", _basename(src));
        dst = newpath;
    }
    // ── 2. dst가 디렉토리인 경우 ──
    else if (fat16_find_file_path(dst, &dst_entry) && fat16_is_dir(&dst_entry)) {
        strcpy(newpath, dst);
        size_t len = strlen(newpath);
        if (len > 0 && newpath[len-1] != '/') strcat(newpath, "/");
        strcat(newpath, _basename(src));
        dst = newpath;
    }

    // ── 3. 무조건 cp+rm (rename은 경로 문제 많음) ──
    if (!fat16_cp(src, dst)) return false;
    return fat16_rm(src);
}

// ✅ 총 클러스터 수 계산
uint32_t fat16_total_clusters() {
    // 보호 코드 (SecPerClus이 0이면 잘못된 BPB)
    if (fat16_bpb.SecPerClus == 0 || fat16_bpb.BytsPerSec == 0)
        return 0;

    uint32_t total_sectors = (fat16_bpb.TotSec16 != 0)
        ? fat16_bpb.TotSec16
        : fat16_bpb.TotSec32;

    // 총 섹터수가 0이면 잘못된 BPB
    if (total_sectors == 0)
        return 0;

    uint32_t root_dir_sectors =
        ((fat16_bpb.RootEntCnt * 32) + (fat16_bpb.BytsPerSec - 1)) /
        fat16_bpb.BytsPerSec;

    // 전체 데이터 영역 섹터 수
    uint32_t data_sectors = total_sectors -
        (fat16_bpb.RsvdSecCnt +
         (fat16_bpb.NumFATs * fat16_bpb.FATSz16) +
         root_dir_sectors);

    if (data_sectors == 0)
        return 0;

    uint32_t total_clusters = data_sectors / fat16_bpb.SecPerClus;
    return total_clusters;
}

// ✅ 빈(Free) 클러스터 수 계산
uint32_t fat16_free_clusters() {
    // BPB 값이 제대로 초기화되지 않았으면 0 반환
    if (fat16_bpb.SecPerClus == 0 || fat16_bpb.BytsPerSec == 0)
        return 0;

    uint8_t sector[512];
    uint32_t free_count = 0;

    // FAT 관련 값이 비정상일 경우 방어
    if (fat16_bpb.FATSz16 == 0 || fat16_bpb.NumFATs == 0)
        return 0;

    uint32_t fat_start = fat_start_lba + fat16_bpb.RsvdSecCnt;
    uint32_t entries_per_sector = fat16_bpb.BytsPerSec / 2; // FAT16 엔트리 = 2바이트
    uint32_t total_fat_sectors = fat16_bpb.FATSz16;

    for (uint32_t s = 0; s < total_fat_sectors; s++) {
        if (!ata_read(fat16_drive, fat_start + s, 1, sector))
            continue;

        for (uint32_t i = 0; i < entries_per_sector; i++) {
            uint16_t entry = *(uint16_t*)&sector[i * 2];
            if (entry == 0x0000)
                free_count++;
        }
    }

    return free_count;
}

bool fat16_format_at(uint8_t drive, uint32_t base_lba, uint32_t total_sectors, const char* label) {
    FAT16_BPB_t bpb;
    uint8_t sector[512];

    if (total_sectors == 0) {
        kprintf("[FAT16] Drive %d not detected or empty.\n", drive);
        return false;
    }

    kprintf("[FAT16] Formatting drive %d (base LBA=%u, %u sectors)...\n",
            drive, base_lba, total_sectors);
    memset(&bpb, 0, sizeof(FAT16_BPB_t));

    /* ────────────────
       기본 BPB 값
    ──────────────── */
    memcpy(bpb.jmpBoot, "\xEB\x3C\x90", 3);          // JMP short 0x3C + NOP
    memcpy(bpb.OEMName, "ORIONOS ", 8);              // OEM Name
    bpb.BytsPerSec = 512;
    bpb.SecPerClus = 4;
    bpb.RsvdSecCnt = 1;
    bpb.NumFATs = 2;
    bpb.RootEntCnt = 512;
    bpb.Media = 0xF8;
    bpb.SecPerTrk = 32;
    bpb.NumHeads = 64;
    bpb.HiddSec = base_lba;

    if (total_sectors < 65536) {
        bpb.TotSec16 = (uint16_t)total_sectors;
        bpb.TotSec32 = 0;
    } else {
        bpb.TotSec16 = 0;
        bpb.TotSec32 = total_sectors;
    }

    /* FAT 크기 계산 */
    uint32_t root_dir_sectors = ((bpb.RootEntCnt * 32) + (bpb.BytsPerSec - 1)) / bpb.BytsPerSec;
    uint32_t data_sectors = total_sectors - (bpb.RsvdSecCnt + root_dir_sectors + (bpb.NumFATs * 250));
    uint32_t clusters = data_sectors / bpb.SecPerClus;
    uint16_t fatsz = (clusters * 2 + (bpb.BytsPerSec - 1)) / bpb.BytsPerSec;
    if (fatsz < 1) fatsz = 1;
    bpb.FATSz16 = fatsz;

    /* Extended Boot Record */
    bpb.DrvNum = 0x80;
    bpb.BootSig = 0x29;
    bpb.VolID = 0x12345678;
    memset(bpb.VolLab, ' ', 11);
    if (label && *label)
        strncpy((char*)bpb.VolLab, label, strlen(label) > 11 ? 11 : strlen(label));
    memcpy(bpb.FilSysType, "FAT16   ", 8);

    /* ────────────────
       부트 섹터 (BPB)
    ──────────────── */
    memset(sector, 0, 512);
    memcpy(sector, &bpb, sizeof(FAT16_BPB_t));

    // ── 여기에 부트 코드 영역 일부 넣기 (55AA 서명 포함)
    const uint8_t bootcode_stub[] = {
        0xFA,             // cli
        0x31, 0xC0,       // xor ax, ax
        0x8E, 0xD0,       // mov ss, ax
        0xBC, 0x00, 0x7C, // mov sp, 0x7C00
        0xFB,             // sti
        0xE9, 0x00, 0x00  // jmp short $
    };
    memcpy(sector + 62, bootcode_stub, sizeof(bootcode_stub));
    sector[510] = 0x55;
    sector[511] = 0xAA;

    ata_write_sector(drive, base_lba + 0, sector);

    /* ────────────────
       FAT 테이블 초기화
    ──────────────── */
    memset(sector, 0, 512);
    sector[0] = 0xF8; // Media descriptor
    sector[1] = 0xFF;
    sector[2] = 0xFF;
    sector[3] = 0xFF;

    uint32_t fat_start = base_lba + bpb.RsvdSecCnt;
    for (uint8_t f = 0; f < bpb.NumFATs; f++) {
        for (uint32_t i = 0; i < bpb.FATSz16; i++) {
            ata_write_sector(drive, fat_start + f * bpb.FATSz16 + i, sector);
            memset(sector, 0, 512);
        }
    }

    /* ────────────────
       루트 디렉터리 초기화
    ──────────────── */
    memset(sector, 0, 512);

    memset(sector, 0, 512);
    uint32_t root_start = fat_start + bpb.NumFATs * bpb.FATSz16;
    uint32_t root_sectors = root_dir_sectors;

    for (uint32_t s = 0; s < root_sectors; s++) {
        memset(sector, 0, 512);
        ata_write_sector(drive, root_start + s, sector);
    }

    kprintf("[FAT16] Format complete.\n");
    kprintf("[FAT16] Root at LBA %u, FAT size %u sectors.\n", root_start, bpb.FATSz16);
    return true;
}

bool fat16_format(uint8_t drive, const char* label) {
    uint32_t total_sectors = ata_get_sector_count(drive);
    return fat16_format_at(drive, 0, total_sectors, label);
}
