#include "fat32.h"
#include "fscmd.h"
#include "../drivers/ata.h"
#include "../drivers/screen.h"
#include "../kernel/cmd.h"
#include "../kernel/kernel.h"
#include "../libc/string.h"   // strcmp 등
#include "../mm/mem.h"
#include <stdbool.h>
#include <stdint.h>

#define SECTOR_SIZE 512
#define CAT_BUF_SIZE 4096
#define FAT32_LFN_ATTR 0x0F
#define FAT32_LFN_MAX 255
#define FAT32_LFN_CHARS_PER_ENTRY 13
#define FAT32_LFN_MAX_ENTRIES 20

uint8_t fat32_drive = 0;
static uint32_t fat32_alloc_hint = 3;
static uint8_t fat32_zero_chunk[SECTOR_SIZE * 16];

static FAT32_BPB_t bpb;
static uint32_t fat_start_lba;
static uint32_t data_start_lba;
uint32_t root_dir_cluster32;
uint32_t current_dir_cluster32 = 0;
extern char current_path[256];

typedef struct __attribute__((packed)) {
    uint8_t Ord;
    uint16_t Name1[5];
    uint8_t Attr;
    uint8_t Type;
    uint8_t Chksum;
    uint16_t Name2[6];
    uint16_t FstClusLO;
    uint16_t Name3[2];
} FAT32_LFNEntry;

typedef struct {
    uint32_t cluster;
    uint8_t sector;
    uint16_t index;
} FAT32_DirSlot;

typedef struct {
    bool active;
    uint8_t checksum;
    int expected;
    char name[FAT32_LFN_MAX + 1];
    uint32_t slot_count;
    FAT32_DirSlot slots[FAT32_LFN_MAX_ENTRIES];
} FAT32_LFNState;

typedef struct {
    FAT32_DirEntry entry;
    FAT32_DirSlot slot;
    bool has_long;
    char long_name[FAT32_LFN_MAX + 1];
    uint32_t lfn_count;
    FAT32_DirSlot lfn_slots[FAT32_LFN_MAX_ENTRIES];
} FAT32_DirItem;

static int fat32_strcasecmp(const char* a, const char* b);
static void fat32_make83(const char* filename, char out[12]);

static bool read_sector(uint8_t drive, uint32_t lba, uint8_t* buf) {
    return ata_read(drive, lba, 1, buf);
}

static void write_sector(uint8_t drive, uint32_t lba, const uint8_t* buf) {
    ata_write(drive, lba, 1, buf);
}

static uint32_t cluster_to_lba(uint32_t cluster) {
    return data_start_lba + (cluster - 2) * bpb.SecPerClus;
}

static uint32_t fat32_next_cluster(uint8_t drive, uint32_t cluster) {
    uint8_t buf[SECTOR_SIZE];
    uint32_t bps = bpb.BytsPerSec;        // ← BPB 사용
    uint32_t fat_offset = cluster * 4;
    uint32_t sector = fat_start_lba + (fat_offset / bps);
    uint32_t offset = fat_offset % bps;

    read_sector(drive, sector, buf);
    uint32_t next = (*(uint32_t*)(buf + offset)) & 0x0FFFFFFF;
    return next;
}

static uint32_t fat32_alloc_cluster(uint8_t drive) {
    uint8_t buf[SECTOR_SIZE];
    uint32_t fat_entries_per_sector = bpb.BytsPerSec / 4;
    uint32_t fat_sectors = bpb.FATSz32;
    uint32_t total_entries = fat_entries_per_sector * fat_sectors;
    uint32_t start_cluster = fat32_alloc_hint;
    if (start_cluster < 3 || start_cluster >= total_entries)
        start_cluster = 3;

    uint32_t start_sector = start_cluster / fat_entries_per_sector;
    uint32_t start_index = start_cluster % fat_entries_per_sector;

    for (int pass = 0; pass < 2; pass++) {
        uint32_t s = (pass == 0) ? start_sector : 0;
        uint32_t s_end = (pass == 0) ? fat_sectors : start_sector;

        for (; s < s_end; s++) {
            read_sector(drive, fat_start_lba + s, buf);
            uint32_t* entries = (uint32_t*)buf;

            uint32_t start_i = (s == 0 ? 3 : 0);
            if (pass == 0 && s == start_sector && start_index > start_i)
                start_i = start_index;

            for (uint32_t i = start_i; i < fat_entries_per_sector; i++) {
                uint32_t clus = s * fat_entries_per_sector + i;
                uint32_t val = entries[i] & 0x0FFFFFFF;

                // 완전히 비어있는 클러스터만 사용
                if (val == 0) {
                    // 새 클러스터를 EOC로 표시
                    entries[i] = 0x0FFFFFFF;
                    write_sector(drive, fat_start_lba + s, buf);

                    // FAT 미러(NumFATs > 1인 경우) 동기화
                    for (uint8_t f = 1; f < bpb.NumFATs; f++) {
                        write_sector(drive, fat_start_lba + f * bpb.FATSz32 + s, buf);
                    }

                    // 데이터 영역 초기화
                    uint32_t base_lba = cluster_to_lba(clus);
                    uint32_t sectors_left = bpb.SecPerClus;
                    while (sectors_left > 0) {
                        uint16_t chunk = (sectors_left > 16) ? 16 : (uint16_t)sectors_left;
                        ata_write(drive, base_lba, chunk, fat32_zero_chunk);
                        base_lba += chunk;
                        sectors_left -= chunk;
                    }

                    fat32_alloc_hint = clus + 1;
                    return clus;
                }
            }
        }
    }

    kprint("FAT32: No free cluster available!\n");
    return 0;
}

static uint8_t fat32_lfn_checksum(const uint8_t short_name[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + short_name[i]);
    }
    return sum;
}

static bool fat32_lfn_is_valid_char(char c) {
    if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7F)
        return false;
    if (c == '\"' || c == '*' || c == '/' || c == ':' || c == '<' ||
        c == '>' || c == '?' || c == '\\' || c == '|') {
        return false;
    }
    return true;
}

static bool fat32_lfn_prepare_name(const char* in, char* out, size_t out_size) {
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
    if (out_len > FAT32_LFN_MAX || out_len + 1 > out_size)
        return false;

    for (size_t i = 0; i < out_len; i++) {
        char c = in[start + i];
        if (!fat32_lfn_is_valid_char(c))
            return false;
        out[i] = c;
    }
    out[out_len] = '\0';

    if (strcmp(out, ".") == 0 || strcmp(out, "..") == 0)
        return false;

    return true;
}

static bool fat32_short_valid_char(char c, bool* has_lower) {
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

static bool fat32_is_valid_short_name(const char* name, bool* has_lower) {
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
        if (!fat32_short_valid_char(name[i], has_lower))
            return false;
    }
    for (size_t i = 0; i < ext_len; i++) {
        if (!fat32_short_valid_char(dot[1 + i], has_lower))
            return false;
    }

    return true;
}

static bool fat32_name_needs_lfn(const char* name) {
    bool has_lower = false;
    if (!fat32_is_valid_short_name(name, &has_lower))
        return true;
    return has_lower;
}

static void fat32_sanitize_component(const char* in, char* out, size_t out_size, bool* has_lower) {
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

static void fat32_extract_base_ext(const char* name,
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

    fat32_sanitize_component(base_tmp, base, base_size, has_lower);
    fat32_sanitize_component(ext_tmp, ext, ext_size, has_lower);
}

static void fat32_make_short_name_from_base_ext(const char* base, const char* ext, uint8_t out[11]) {
    memset(out, ' ', 11);
    for (int i = 0; i < 8 && base[i]; i++)
        out[i] = (uint8_t)base[i];
    for (int i = 0; i < 3 && ext[i]; i++)
        out[8 + i] = (uint8_t)ext[i];
}

static bool fat32_short_name_exists(uint32_t dir_cluster, const uint8_t short_name[11]) {
    uint8_t buf[SECTOR_SIZE];
    uint32_t cluster = dir_cluster;

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        for (uint8_t s = 0; s < bpb.SecPerClus; s++) {
            uint32_t lba = cluster_to_lba(cluster) + s;
            read_sector(fat32_drive, lba, buf);
            FAT32_DirEntry* entry = (FAT32_DirEntry*)buf;

            for (size_t i = 0; i < SECTOR_SIZE / sizeof(FAT32_DirEntry); i++) {
                if (entry[i].Name[0] == 0x00)
                    return false;
                if (entry[i].Name[0] == 0xE5)
                    continue;
                if (entry[i].Attr == FAT32_LFN_ATTR)
                    continue;

                if (memcmp(entry[i].Name, short_name, 11) == 0)
                    return true;
            }
        }
        cluster = fat32_next_cluster(fat32_drive, cluster);
    }

    return false;
}

static int fat32_count_digits(uint32_t n) {
    int digits = 1;
    while (n >= 10) {
        n /= 10;
        digits++;
    }
    return digits;
}

static bool fat32_generate_short_name(uint32_t dir_cluster, const char* long_name, uint8_t out[11]) {
    char base[32] = {0};
    char ext[8] = {0};

    fat32_extract_base_ext(long_name, base, sizeof(base), ext, sizeof(ext), NULL);

    if (!base[0])
        strncpy(base, "FILE", sizeof(base) - 1);

    uint8_t candidate[11];
    fat32_make_short_name_from_base_ext(base, ext, candidate);
    if (!fat32_short_name_exists(dir_cluster, candidate)) {
        memcpy(out, candidate, 11);
        return true;
    }

    for (uint32_t n = 1; n < 10000; n++) {
        int digits = fat32_count_digits(n);
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

        fat32_make_short_name_from_base_ext(tmp, ext, candidate);
        if (!fat32_short_name_exists(dir_cluster, candidate)) {
            memcpy(out, candidate, 11);
            return true;
        }
    }

    return false;
}

static void fat32_lfn_reset(FAT32_LFNState* st) {
    if (!st)
        return;
    st->active = false;
    st->checksum = 0;
    st->expected = 0;
    st->name[0] = '\0';
    st->slot_count = 0;
}

static void fat32_lfn_copy_chars(char* dst, const uint16_t* src, size_t count, bool* end_seen) {
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

static void fat32_lfn_push(FAT32_LFNState* st, const FAT32_LFNEntry* lfn, const FAT32_DirSlot* slot) {
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

    if (seq == 0 || seq > FAT32_LFN_MAX_ENTRIES) {
        fat32_lfn_reset(st);
        return;
    }

    if (seq != st->expected) {
        fat32_lfn_reset(st);
        return;
    }

    if (st->slot_count < FAT32_LFN_MAX_ENTRIES)
        st->slots[st->slot_count++] = *slot;

    size_t base = (size_t)(seq - 1) * FAT32_LFN_CHARS_PER_ENTRY;
    if (base + FAT32_LFN_CHARS_PER_ENTRY > sizeof(st->name)) {
        fat32_lfn_reset(st);
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
    fat32_lfn_copy_chars(st->name + base, name1, 5, &end_seen);
    fat32_lfn_copy_chars(st->name + base + 5, name2, 6, &end_seen);
    fat32_lfn_copy_chars(st->name + base + 11, name3, 2, &end_seen);

    st->expected--;
}

static void fat32_dir_write_raw(const FAT32_DirSlot* slot, const void* entry_data) {
    uint8_t buf[SECTOR_SIZE];
    uint32_t lba = cluster_to_lba(slot->cluster) + slot->sector;
    read_sector(fat32_drive, lba, buf);
    memcpy(buf + slot->index * sizeof(FAT32_DirEntry), entry_data, sizeof(FAT32_DirEntry));
    write_sector(fat32_drive, lba, buf);
}

static void fat32_dir_mark_deleted(const FAT32_DirSlot* slot) {
    uint8_t buf[SECTOR_SIZE];
    uint32_t lba = cluster_to_lba(slot->cluster) + slot->sector;
    read_sector(fat32_drive, lba, buf);
    FAT32_DirEntry* entry = (FAT32_DirEntry*)buf;
    entry[slot->index].Name[0] = 0xE5;
    write_sector(fat32_drive, lba, buf);
}

typedef bool (*fat32_dir_iter_cb)(const FAT32_DirItem* item, void* ctx);

static bool fat32_iterate_dir(uint32_t dir_cluster, fat32_dir_iter_cb cb, void* ctx) {
    uint8_t buf[SECTOR_SIZE];
    uint32_t cluster = dir_cluster;
    FAT32_LFNState lfn;
    fat32_lfn_reset(&lfn);

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        for (uint8_t s = 0; s < bpb.SecPerClus; s++) {
            uint32_t lba = cluster_to_lba(cluster) + s;
            read_sector(fat32_drive, lba, buf);
            FAT32_DirEntry* entry = (FAT32_DirEntry*)buf;

            for (size_t i = 0; i < SECTOR_SIZE / sizeof(FAT32_DirEntry); i++) {
                uint8_t first = entry[i].Name[0];
                if (first == 0x00)
                    return true;

                FAT32_DirSlot slot = { cluster, s, (uint16_t)i };

                if (first == 0xE5) {
                    fat32_lfn_reset(&lfn);
                    continue;
                }

                if (entry[i].Attr == FAT32_LFN_ATTR) {
                    const FAT32_LFNEntry* lfn_entry = (const FAT32_LFNEntry*)&entry[i];
                    fat32_lfn_push(&lfn, lfn_entry, &slot);
                    continue;
                }

                FAT32_DirItem item;
                memset(&item, 0, sizeof(item));
                item.entry = entry[i];
                item.slot = slot;

                if (lfn.active && lfn.expected == 0 &&
                    fat32_lfn_checksum(item.entry.Name) == lfn.checksum) {
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

                fat32_lfn_reset(&lfn);

                if (!cb(&item, ctx))
                    return false;
            }
        }
        cluster = fat32_next_cluster(fat32_drive, cluster);
    }

    return true;
}

static bool fat32_find_free_slots(uint32_t dir_cluster, uint32_t needed, FAT32_DirSlot* slots) {
    uint8_t buf[SECTOR_SIZE];
    uint32_t cluster = dir_cluster;
    uint32_t run = 0;

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        for (uint8_t s = 0; s < bpb.SecPerClus; s++) {
            uint32_t lba = cluster_to_lba(cluster) + s;
            read_sector(fat32_drive, lba, buf);
            FAT32_DirEntry* entry = (FAT32_DirEntry*)buf;

            for (size_t i = 0; i < SECTOR_SIZE / sizeof(FAT32_DirEntry); i++) {
                uint8_t first = entry[i].Name[0];
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
        cluster = fat32_next_cluster(fat32_drive, cluster);
    }

    return false;
}

static void fat32_build_short_name_str(const FAT32_DirEntry* e, char* out, size_t out_size) {
    char name[9];
    char ext[4];
    memcpy(name, e->Name, 8);
    memcpy(ext, e->Name + 8, 3);
    name[8] = 0;
    ext[3] = 0;

    for (int i = 7; i >= 0 && name[i] == ' '; i--) name[i] = 0;
    for (int i = 2; i >= 0 && ext[i] == ' '; i--) ext[i] = 0;

    if (ext[0])
        snprintf(out, out_size, "%s.%s", name, ext);
    else
        snprintf(out, out_size, "%s", name);
}

static bool fat32_dir_item_matches(const FAT32_DirItem* item, const char* name) {
    if (!item || !name || !name[0])
        return false;

    if (item->has_long && fat32_strcasecmp(item->long_name, name) == 0)
        return true;

    char short_name[16];
    fat32_build_short_name_str(&item->entry, short_name, sizeof(short_name));
    if (fat32_strcasecmp(short_name, name) == 0)
        return true;

    return false;
}

static void fat32_write_lfn_entries(const FAT32_DirSlot* slots,
                                    uint32_t count,
                                    const char* long_name,
                                    uint8_t checksum) {
    size_t name_len = strlen(long_name);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t ord = (uint8_t)(count - i);
        FAT32_LFNEntry lfn;
        memset(&lfn, 0, sizeof(lfn));
        if (i == 0)
            ord |= 0x40;
        lfn.Ord = ord;
        lfn.Attr = FAT32_LFN_ATTR;
        lfn.Type = 0;
        lfn.Chksum = checksum;
        lfn.FstClusLO = 0;

        size_t part_index = (size_t)(ord & 0x1F) - 1;
        size_t start = part_index * FAT32_LFN_CHARS_PER_ENTRY;

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

        fat32_dir_write_raw(&slots[i], &lfn);
    }
}

typedef struct {
    const char* name;
    FAT32_DirEntry* out_entry;
    FAT32_DirSlot* out_slot;
    FAT32_DirSlot* out_lfn_slots;
    uint32_t* out_lfn_count;
    bool found;
} fat32_find_ctx_t;

static bool fat32_find_entry_cb(const FAT32_DirItem* item, void* vctx) {
    fat32_find_ctx_t* c = (fat32_find_ctx_t*)vctx;
    if (item->entry.Name[0] == 0xE5)
        return true;
    if (fat32_dir_item_matches(item, c->name)) {
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

typedef struct {
    const char* name;
    uint32_t found;
} fat32_dir_find_ctx_t;

static bool fat32_find_dir_cb(const FAT32_DirItem* item, void* vctx) {
    fat32_dir_find_ctx_t* c = (fat32_dir_find_ctx_t*)vctx;
    if (!(item->entry.Attr & 0x10))
        return true;
    if (fat32_dir_item_matches(item, c->name)) {
        uint32_t found = ((uint32_t)item->entry.FstClusHI << 16) | item->entry.FstClusLO;
        if (found >= 2 && found < 0x0FFFFFF8) {
            c->found = found;
            return false;
        }
    }
    return true;
}

static bool fat32_find_entry_in_dir(uint32_t dir_cluster, const char* name, FAT32_DirEntry* out_entry);
static bool fat32_find_entry_slot(uint32_t dir_cluster,
                                  const char* name,
                                  FAT32_DirEntry* out_entry,
                                  FAT32_DirSlot* out_slot,
                                  FAT32_DirSlot* lfn_slots,
                                  uint32_t* lfn_count);
bool fat32_init(uint8_t drive, uint32_t base_lba) {
    uint8_t buf[SECTOR_SIZE];
    if (!read_sector(drive, base_lba, buf))
        return false;

    // ① FAT32 식별 시그니처 검사
    const char* type = (const char*)(buf + 0x52);
    if (memcmp(type, "FAT32", 5) != 0) {
        // FAT32가 아님
        return false;
    }

    // ② FAT32 구조체 복사
    memcpy(&bpb, buf, sizeof(FAT32_BPB_t));

    // ③ BPB sanity check (we only support 512-byte sectors right now)
    if (bpb.BytsPerSec != SECTOR_SIZE) return false;
    if (bpb.SecPerClus == 0 || (bpb.SecPerClus & (bpb.SecPerClus - 1)) != 0 || bpb.SecPerClus > 128)
        return false;
    if (bpb.NumFATs == 0 || bpb.RsvdSecCnt == 0 || bpb.FATSz32 == 0) return false;
    if (bpb.RootClus < 2) return false;
    if (buf[510] != 0x55 || buf[511] != 0xAA) return false;

    // ③ FAT 크기 확인 (FAT32는 BPB_FATSz16 == 0 이어야 함)
    if (bpb.FATSz16 != 0 && bpb.FATSz32 == 0) {
        return false;  // FAT16일 가능성 있음
    }

    // ④ 계산
    fat_start_lba   = base_lba + bpb.RsvdSecCnt;
    data_start_lba  = base_lba + bpb.RsvdSecCnt + (bpb.NumFATs * bpb.FATSz32);
    root_dir_cluster32 = bpb.RootClus;
    fat32_drive     = drive;
    fat32_alloc_hint = 3;

    // ⑤ 디버그 출력
    kprintf("[FAT32] Mounted drive %d successfully.\n", drive);
    kprintf("         BytesPerSec=%u, SecPerClus=%u\n", bpb.BytsPerSec, bpb.SecPerClus);
    kprintf("         FAT LBA=%u, DATA LBA=%u\n", fat_start_lba, data_start_lba);
    kprintf("         RootClus=%u (LBA=%u)\n", root_dir_cluster32, cluster_to_lba(root_dir_cluster32));

    current_dir_cluster32 = root_dir_cluster32;
    return true;
}

void _get_fullname32(const FAT32_DirEntry* entry, char* out) {
    char name[9];
    char ext[4];

    memcpy(name, entry->Name, 8);
    memcpy(ext, entry->Name + 8, 3);
    name[8] = 0;
    ext[3] = 0;

    // ──────────────────────────────
    // 1. FAT 삭제표시(0xE5) → 0x05 교체
    // ──────────────────────────────
    if ((uint8_t)name[0] == 0x05)
        name[0] = (char)0xE5;  // FAT 규약에 따라 복원

    // ──────────────────────────────
    // 2. 공백 제거
    // ──────────────────────────────
    int i = 7;
    while (i >= 0 && name[i] == ' ') name[i--] = 0;

    i = 2;
    while (i >= 0 && ext[i] == ' ') ext[i--] = 0;

    // ──────────────────────────────
    // 3. 대문자/문자 정규화
    // ──────────────────────────────
    for (int j = 0; j < 8 && name[j]; j++) {
        if (name[j] >= 'a' && name[j] <= 'z') name[j] -= 32;
        if (name[j] < 0x20 || name[j] > 0x7E) name[j] = '?';
    }
    for (int j = 0; j < 3 && ext[j]; j++) {
        if (ext[j] >= 'a' && ext[j] <= 'z') ext[j] -= 32;
        if (ext[j] < 0x20 || ext[j] > 0x7E) ext[j] = '?';
    }

    // ──────────────────────────────
    // 4. 출력 조합
    // ──────────────────────────────
    if (ext[0])
        snprintf(out, 16, "%s.%s", name, ext);
    else
        snprintf(out, 16, "%s", name);
}

static void fat32_split_path(const char* path,
                             char* dir_out,
                             size_t dir_size,
                             char* name_out,
                             size_t name_size) {
    if (!path) path = "";

    if (dir_size > 0) dir_out[0] = '\0';
    if (name_size > 0) name_out[0] = '\0';

    const char* last_slash = strrchr(path, '/');
    if (!last_slash) {
        if (name_size > 0) {
            size_t copy = strlen(path);
            if (copy >= name_size) copy = name_size - 1;
            memcpy(name_out, path, copy);
            name_out[copy] = '\0';
        }
        return;
    }

    size_t dir_len = (size_t)(last_slash - path);
    if (dir_len == 0) {
        if (dir_size > 1) {
            dir_out[0] = '/';
            dir_out[1] = '\0';
        } else if (dir_size == 1) {
            dir_out[0] = '\0';
        }
    } else if (dir_size > 0) {
        size_t copy = dir_len;
        if (copy >= dir_size) copy = dir_size - 1;
        memcpy(dir_out, path, copy);
        dir_out[copy] = '\0';
    }

    if (name_size > 0) {
        size_t copy = strlen(last_slash + 1);
        if (copy >= name_size) copy = name_size - 1;
        memcpy(name_out, last_slash + 1, copy);
        name_out[copy] = '\0';
    }
}

uint32_t fat32_resolve_dir(const char* dirpath) {
    uint32_t current = current_dir_cluster32;
    if (current < 2) current = root_dir_cluster32;

    if (!dirpath || dirpath[0] == '\0')
        return current;

    if (strcmp(dirpath, "/") == 0)
        return root_dir_cluster32;

    char tmp[256];
    strncpy(tmp, dirpath, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char* token = tmp;
    if (token[0] == '/')
        token++;

    uint32_t cluster = (dirpath[0] == '/') ? root_dir_cluster32 : current;
    token = strtok(token, "/");

    while (token) {
        if (token[0] == '\0') {
            token = strtok(NULL, "/");
            continue;
        }

        uint32_t next = fat32_find_dir_cluster(cluster, token);
        if (next < 2 || next >= 0x0FFFFFF8)
            return 0;
        cluster = next;
        token = strtok(NULL, "/");
    }

    return cluster;
}

static bool fat32_ls_cb(const FAT32_DirItem* item, void* ctx) {
    (void)ctx;
    if (item->entry.Attr & 0x08)
        return true;

    char short_name[16];
    fat32_build_short_name_str(&item->entry, short_name, sizeof(short_name));
    const char* name = item->has_long && item->long_name[0] ? item->long_name : short_name;

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

void fat32_ls(const char* path) {
    uint32_t cluster;

    // 기본: 현재 디렉토리
    if (!path || path[0] == '\0') {
        cluster = current_dir_cluster32;
    }
    else {
        cluster = fat32_resolve_dir(path);
        if (cluster < 2 || cluster >= 0x0FFFFFF8) {
            kprint("fl: invalid path\n");
            return;
        }
    }

    kprint("filename         type             size\n");
    kprint("--------------------------------------\n");
    fat32_iterate_dir(cluster, fat32_ls_cb, NULL);
}

typedef struct {
    char* names;
    bool* is_dir;
    int max;
    int count;
    size_t name_len;
} fat32_list_ctx_t;

static bool fat32_list_dir_cb(const FAT32_DirItem* item, void* vctx) {
    fat32_list_ctx_t* ctx = (fat32_list_ctx_t*)vctx;
    if (item->entry.Attr & 0x08)
        return true;

    char short_name[16];
    fat32_build_short_name_str(&item->entry, short_name, sizeof(short_name));
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

int fat32_list_dir_lfn(uint32_t cluster, char* names, bool* is_dir, int max_entries, size_t name_len) {
    if (!names || !is_dir || max_entries <= 0 || name_len == 0)
        return -1;

    fat32_list_ctx_t ctx = {
        .names = names,
        .is_dir = is_dir,
        .max = max_entries,
        .count = 0,
        .name_len = name_len,
    };

    fat32_iterate_dir(cluster, fat32_list_dir_cb, &ctx);
    return ctx.count;
}

int fat32_read_dir(uint32_t cluster, FAT32_DirEntry* out, uint32_t max) {
    uint32_t count = 0;
    uint8_t buf[SECTOR_SIZE];

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        for (uint32_t s = 0; s < bpb.SecPerClus; s++) {
            read_sector(fat32_drive, cluster_to_lba(cluster) + s, buf);
            FAT32_DirEntry* entry = (FAT32_DirEntry*)buf;

            for (size_t  i = 0; i < SECTOR_SIZE / sizeof(FAT32_DirEntry); i++) {
                if (entry[i].Name[0] == 0x00) return count;
                if (entry[i].Name[0] == 0xE5) continue;
                if (entry[i].Attr == 0x0F) continue;

                if (count < max) {
                    out[count++] = entry[i];
                } else {
                    return count;
                }
            }
        }
        cluster = fat32_next_cluster(fat32_drive, cluster);
    }
    return count;
}

int fat32_read_file(const char* filename, uint8_t* buffer, uint32_t offset, uint32_t size) {
    FAT32_DirEntry entry;
    if (!fat32_find_file(filename, &entry))
        return 0;

    if (offset >= entry.FileSize)
        return 0;

    uint32_t to_read = size;
    if (offset + to_read > entry.FileSize)
        to_read = entry.FileSize - offset;

    if (to_read == 0)
        return 0;

    if (!fat32_read_file_range(&entry, offset, buffer, to_read))
        return 0;

    return to_read;
}

uint32_t fat32_get_fat_entry(uint32_t cluster) {
    uint32_t bytes_per_sector = bpb.BytsPerSec;
    uint32_t fat_offset = cluster * 4;                     // 클러스터당 4바이트
    uint32_t sector_num = fat_start_lba + (fat_offset / bytes_per_sector);
    uint32_t ent_offset = fat_offset % bytes_per_sector;

    uint8_t sector[512];
    ata_read(fat32_drive, sector_num, 1, sector);

    uint32_t value = *(uint32_t*)(sector + ent_offset);
    value &= 0x0FFFFFFF;  // 상위 4비트는 예약됨 (28비트만 유효)

    return value;
}

bool fat32_find_file(const char* filename, FAT32_DirEntry* out_entry) {
    char dir[256];
    char name[64];
    fat32_split_path(filename, dir, sizeof(dir), name, sizeof(name));

    if (name[0] == '\0')
        return false;

    uint32_t dir_cluster = fat32_resolve_dir(dir);
    if (dir_cluster < 2 || dir_cluster >= 0x0FFFFFF8)
        return false;

    return fat32_find_entry_in_dir(dir_cluster, name, out_entry);
}

bool fat32_read_file_range(FAT32_DirEntry* entry, uint32_t offset, uint8_t* out_buf, uint32_t size) {
    if (!entry || offset >= entry->FileSize)
        return false;

    // 파일 크기 초과 방지
    if (offset + size > entry->FileSize)
        size = entry->FileSize - offset;

    uint32_t cluster_size = bpb.SecPerClus * bpb.BytsPerSec;
    uint8_t* temp = kmalloc(cluster_size, 0, NULL);
    if (!temp) {
        kprint("Error: kmalloc failed in fat32_read_file_range\n");
        return false;
    }

    // FAT32의 첫 클러스터 계산
    uint32_t cluster = ((uint32_t)entry->FstClusHI << 16) | entry->FstClusLO;
    uint32_t bytes_read = 0;
    uint32_t skip_bytes = offset;

    // ───── 오프셋만큼 건너뛰기 ─────
    while (skip_bytes >= cluster_size) {
        cluster = fat32_get_fat_entry(cluster);
        if (cluster >= 0x0FFFFFF8) {
            kprint("Error: offset exceeds file cluster chain\n");
            kfree(temp);
            return false;
        }
        skip_bytes -= cluster_size;
    }

    // ───── 데이터 읽기 ─────
    while (bytes_read < size && cluster < 0x0FFFFFF8) {
        // 클러스터 → LBA
        uint32_t lba = data_start_lba + (cluster - 2) * bpb.SecPerClus;

        // 클러스터 전체 읽기
        for (uint32_t s = 0; s < bpb.SecPerClus; s++)
            ata_read(fat32_drive, lba + s, 1, temp + s * bpb.BytsPerSec);

        uint32_t copy_start = skip_bytes;
        uint32_t to_copy = cluster_size - copy_start;
        if (to_copy > size - bytes_read)
            to_copy = size - bytes_read;

        memcpy(out_buf + bytes_read, temp + copy_start, to_copy);
        bytes_read += to_copy;
        skip_bytes = 0;

        cluster = fat32_get_fat_entry(cluster);
    }

    kfree(temp);
    return true;
}

void fat32_cat(const char* fullpath) {
    if (!fullpath || fullpath[0] == '\0') {
        kprint("cat: missing filename\n");
        return;
    }

    char dir[256];
    char name[64];
    fat32_split_path(fullpath, dir, sizeof(dir), name, sizeof(name));

    if (name[0] == '\0') {
        kprintf("cat: invalid path: %s\n", fullpath);
        return;
    }

    uint32_t dir_cluster = fat32_resolve_dir(dir);
    if (dir_cluster < 2 || dir_cluster >= 0x0FFFFFF8) {
        kprintf("cat: invalid path: %s\n", fullpath);
        return;
    }

    FAT32_DirEntry entry;
    if (!fat32_find_entry_in_dir(dir_cluster, name, &entry)) {
        kprintf("cat: file not found: %s\n", fullpath);
        return;
    }

    if (entry.Attr & 0x10) {
        kprintf("cat: %s is a directory\n", fullpath);
        return;
    }

    uint32_t file_cluster = ((uint32_t)entry.FstClusHI << 16) | entry.FstClusLO;
    uint32_t remaining = entry.FileSize;
    if (remaining == 0) {
        kprint("\n");
        return;
    }
    if (file_cluster < 2) {
        kprintf("cat: invalid file cluster: %s\n", fullpath);
        return;
    }

    uint8_t sector[SECTOR_SIZE];
    uint32_t current = file_cluster;

    while (current >= 2 && current < 0x0FFFFFF8 && remaining > 0) {
        for (uint8_t s = 0; s < bpb.SecPerClus; s++) {
            read_sector(fat32_drive, cluster_to_lba(current) + s, sector);

            uint32_t chunk = (remaining > SECTOR_SIZE) ? SECTOR_SIZE : remaining;
            for (uint32_t i = 0; i < chunk; i++)
                putchar(sector[i]);

            remaining -= chunk;
            if (remaining == 0)
                break;
        }

        current = fat32_next_cluster(fat32_drive, current);
    }

    kprint("\n");
}

// ─────────────────────────────
// 파일 생성
// ─────────────────────────────
bool fat32_create_file(const char* fullpath) {
    char dir[256];
    char name[64];
    fat32_split_path(fullpath, dir, sizeof(dir), name, sizeof(name));

    if (name[0] == '\0') {
        kprintf("FAT32: invalid path %s\n", fullpath);
        return false;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        kprint("FAT32: invalid name\n");
        return false;
    }

    uint32_t dir_cluster = fat32_resolve_dir(dir);
    if (dir_cluster < 2 || dir_cluster >= 0x0FFFFFF8) {
        kprintf("FAT32: invalid path %s\n", fullpath);
        return false;
    }

    if (fat32_find_entry_in_dir(dir_cluster, name, NULL)) {
        kprintf("FAT32: File already exists (%s)\n", name);
        return false;
    }

    char long_name[FAT32_LFN_MAX + 1];
    bool needs_lfn = fat32_name_needs_lfn(name);
    if (needs_lfn) {
        if (!fat32_lfn_prepare_name(name, long_name, sizeof(long_name))) {
            kprintf("FAT32: invalid name %s\n", name);
            return false;
        }
    } else {
        strncpy(long_name, name, sizeof(long_name) - 1);
        long_name[sizeof(long_name) - 1] = '\0';
    }

    uint8_t short_name[11];
    if (needs_lfn) {
        if (!fat32_generate_short_name(dir_cluster, long_name, short_name)) {
            kprint("FAT32: failed to generate short name\n");
            return false;
        }
    } else {
        char fatname[12];
        fat32_make83(name, fatname);
        memcpy(short_name, fatname, 11);
        if (fat32_short_name_exists(dir_cluster, short_name)) {
            kprintf("FAT32: File already exists (%s)\n", name);
            return false;
        }
    }

    uint32_t lfn_count = needs_lfn ? (uint32_t)((strlen(long_name) + FAT32_LFN_CHARS_PER_ENTRY - 1) / FAT32_LFN_CHARS_PER_ENTRY) : 0;
    if (lfn_count > FAT32_LFN_MAX_ENTRIES) {
        kprint("FAT32: name too long\n");
        return false;
    }

    FAT32_DirSlot slots[FAT32_LFN_MAX_ENTRIES + 1];
    uint32_t needed = lfn_count + 1;
    if (!fat32_find_free_slots(dir_cluster, needed, slots)) {
        kprintf("FAT32: No free slot for %s\n", fullpath);
        return false;
    }

    uint32_t newclus = fat32_alloc_cluster(fat32_drive);
    if (!newclus) {
        kprint("FAT32: No free cluster.\n");
        return false;
    }

    if (lfn_count > 0) {
        uint8_t checksum = fat32_lfn_checksum(short_name);
        fat32_write_lfn_entries(slots, lfn_count, long_name, checksum);
    }

    FAT32_DirEntry entry;
    memset(&entry, 0, sizeof(entry));
    memcpy(entry.Name, short_name, 11);
    entry.Attr = 0x20;  // 일반 파일
    entry.FileSize = 0;
    entry.FstClusLO = (uint16_t)(newclus & 0xFFFF);
    entry.FstClusHI = (uint16_t)(newclus >> 16);

    fat32_dir_write_raw(&slots[lfn_count], &entry);

    kprintf("FAT32: created %s in dir cluster %u\n", name, dir_cluster);
    return true;
}

// ─────────────────────────────
// 파일 쓰기 (데이터 저장)
// ─────────────────────────────
bool fat32_write_file(const char* fullpath, const uint8_t* data, uint32_t size) {
    char dir[256];
    char name[64];
    fat32_split_path(fullpath, dir, sizeof(dir), name, sizeof(name));

    if (name[0] == '\0') {
        kprintf("FAT32: invalid path %s\n", fullpath);
        return false;
    }

    uint32_t dir_cluster = fat32_resolve_dir(dir);
    if (dir_cluster < 2 || dir_cluster >= 0x0FFFFFF8) {
        kprintf("FAT32: invalid path %s\n", fullpath);
        return false;
    }

    FAT32_DirEntry fe;
    FAT32_DirSlot fe_slot;
    if (!fat32_find_entry_slot(dir_cluster, name, &fe, &fe_slot, NULL, NULL)) {
        if (!fat32_create_file(fullpath)) {
            kprintf("FAT32: failed to create file %s\n", fullpath);
            return false;
        }
        return fat32_write_file(fullpath, data, size);
    }

    uint32_t file_cluster =
        ((uint32_t)fe.FstClusHI << 16) | fe.FstClusLO;
    const uint8_t* src = data;
    uint32_t remaining = size;

    while (remaining > 0 && file_cluster < 0x0FFFFFF8) {
        uint32_t lba = cluster_to_lba(file_cluster);
        uint32_t cluster_bytes = bpb.SecPerClus * SECTOR_SIZE;
        uint32_t tocpy = (remaining > cluster_bytes) ? cluster_bytes : remaining;
        uint32_t full_sectors = tocpy / SECTOR_SIZE;
        uint32_t tail_bytes = tocpy % SECTOR_SIZE;

        if (full_sectors > 0) {
            ata_write(fat32_drive, lba, (uint16_t)full_sectors, src);
            src += full_sectors * SECTOR_SIZE;
            remaining -= full_sectors * SECTOR_SIZE;
        }

        if (tail_bytes > 0) {
            uint8_t tmp[SECTOR_SIZE];
            memset(tmp, 0, SECTOR_SIZE);
            memcpy(tmp, src, tail_bytes);
            ata_write(fat32_drive, lba + full_sectors, 1, tmp);
            src += tail_bytes;
            remaining -= tail_bytes;
        }

        fscmd_write_progress_update(size - remaining);

        if (remaining > 0) {
            uint32_t nextclus = fat32_alloc_cluster(fat32_drive);
            if (!nextclus) {
                kprint("FAT32: No more clusters available!\n");
                return false;
            }

            // FAT 링크 연결
            uint8_t fatbuf[SECTOR_SIZE];
            uint32_t fat_sector = (file_cluster * 4) / SECTOR_SIZE;
            uint32_t fat_offset = (file_cluster * 4) % SECTOR_SIZE;
            read_sector(fat32_drive, fat_start_lba + fat_sector, fatbuf);
            ((uint32_t*)(fatbuf + fat_offset))[0] = nextclus;
            write_sector(fat32_drive, fat_start_lba + fat_sector, fatbuf);

            file_cluster = nextclus;
        }
    }

    // 파일 크기 갱신
    fe.FileSize = size;
    fat32_dir_write_raw(&fe_slot, &fe);

    kprintf("FAT32: wrote %s (%u bytes)\n", name, size);
    return true;
}

// ────────────────────────────────
// FAT32 8.3 파일명 변환 (공백 패딩 포함)
// ────────────────────────────────
static void fat32_make83(const char* filename, char out[12]) {
    memset(out, ' ', 11);
    out[11] = 0;

    int name_index = 0;
    int ext_index = 8;
    bool in_ext = false;

    for (int i = 0; filename[i] && i < 255; i++) {
        char c = filename[i];

        if (c == '.') {
            in_ext = true;
            continue;
        }

        // FAT은 대문자만 허용
        if (c >= 'a' && c <= 'z')
            c -= 32;

        // 허용되지 않는 문자 제거
        if (c < 0x20 || c > 0x7E || c == ' ' || c == '+' || c == ',' ||
            c == ';' || c == '=' || c == '[' || c == ']' || c == ':')
            continue;

        if (!in_ext && name_index < 8)
            out[name_index++] = c;
        else if (in_ext && ext_index < 11)
            out[ext_index++] = c;
    }
}

// ────────────────────────────────
// FAT32 파일 완전 삭제
// ────────────────────────────────
bool fat32_rm(const char* fullpath) {
    if (!fullpath || fullpath[0] == '\0') {
        kprint("rm: missing filename\n");
        return false;
    }

    char dir[256];
    char name[64];
    fat32_split_path(fullpath, dir, sizeof(dir), name, sizeof(name));

    if (name[0] == '\0') {
        kprintf("rm: invalid path: %s\n", fullpath);
        return false;
    }

    uint32_t dir_cluster = fat32_resolve_dir(dir);
    if (dir_cluster < 2 || dir_cluster >= 0x0FFFFFF8) {
        kprintf("rm: invalid path: %s\n", fullpath);
        return false;
    }

    FAT32_DirEntry entry;
    FAT32_DirSlot slot;
    FAT32_DirSlot lfn_slots[FAT32_LFN_MAX_ENTRIES];
    uint32_t lfn_count = 0;

    if (!fat32_find_entry_slot(dir_cluster, name, &entry, &slot, lfn_slots, &lfn_count)) {
        kprintf("FAT32: file not found: %s\n", fullpath);
        return false;
    }

    // ✅ 파일 클러스터 찾음
    uint32_t cl = ((uint32_t)entry.FstClusHI << 16) | entry.FstClusLO;
    uint8_t zero_sector[SECTOR_SIZE];
    memset(zero_sector, 0, SECTOR_SIZE);

    // ─────────────────────────────
    // ② FAT 체인 해제
    // ─────────────────────────────
    while (cl >= 2 && cl < 0x0FFFFFF8) {
        uint32_t next = fat32_next_cluster(fat32_drive, cl);

        uint8_t fatbuf[SECTOR_SIZE];
        uint32_t fat_sector = fat_start_lba + (cl * 4) / SECTOR_SIZE;
        uint32_t fat_offset = (cl * 4) % SECTOR_SIZE;
        read_sector(fat32_drive, fat_sector, fatbuf);
        *(uint32_t*)(fatbuf + fat_offset) = 0;
        write_sector(fat32_drive, fat_sector, fatbuf);

        // 데이터 클러스터 0으로 초기화
        uint32_t start_lba = cluster_to_lba(cl);
        for (uint8_t n = 0; n < bpb.SecPerClus; n++)
            write_sector(fat32_drive, start_lba + n, zero_sector);

        cl = next;
    }

    // ─────────────────────────────
    // ③ 디렉토리 엔트리 삭제 표시 (LFN 포함)
    // ─────────────────────────────
    for (uint32_t i = 0; i < lfn_count; i++)
        fat32_dir_mark_deleted(&lfn_slots[i]);
    fat32_dir_mark_deleted(&slot);

    kprintf("FAT32: deleted '%s'\n", fullpath);
    return true;
}

// 경로가 아닌 디렉터리 클러스터와 파일 이름으로 엔트리를 찾는 헬퍼
static bool fat32_find_entry_in_dir(uint32_t dir_cluster, const char* name, FAT32_DirEntry* out_entry) {
    return fat32_find_entry_slot(dir_cluster, name, out_entry, NULL, NULL, NULL);
}

static bool fat32_find_entry_slot(uint32_t dir_cluster,
                                  const char* name,
                                  FAT32_DirEntry* out_entry,
                                  FAT32_DirSlot* out_slot,
                                  FAT32_DirSlot* lfn_slots,
                                  uint32_t* lfn_count) {
    fat32_find_ctx_t ctx = {
        .name = name,
        .out_entry = out_entry,
        .out_slot = out_slot,
        .out_lfn_slots = lfn_slots,
        .out_lfn_count = lfn_count,
        .found = false,
    };

    fat32_iterate_dir(dir_cluster, fat32_find_entry_cb, &ctx);
    return ctx.found;
}

bool fat32_exists(const char* filename) {
    char dir[256];
    char name[64];
    fat32_split_path(filename, dir, sizeof(dir), name, sizeof(name));

    if (name[0] == '\0')
        return false;

    uint32_t dir_cluster = fat32_resolve_dir(dir);
    if (dir_cluster < 2 || dir_cluster >= 0x0FFFFFF8)
        return false;

    return fat32_find_entry_in_dir(dir_cluster, name, NULL);
}

int fat32_read_file_by_name(const char* filename, uint8_t* buffer, uint32_t bufsize) {
    char dir[256];
    char name[64];
    fat32_split_path(filename, dir, sizeof(dir), name, sizeof(name));

    if (name[0] == '\0')
        return -1;

    uint32_t dir_cluster = fat32_resolve_dir(dir);
    if (dir_cluster < 2 || dir_cluster >= 0x0FFFFFF8)
        return -1;

    FAT32_DirEntry entry;
    if (!fat32_find_entry_in_dir(dir_cluster, name, &entry))
        return -1;

    uint32_t to_read = entry.FileSize;
    if (to_read > bufsize) to_read = bufsize;
    if (to_read == 0) return 0;

    if (!fat32_read_file_range(&entry, 0, buffer, to_read))
        return -1;

    return to_read; // 정확히 읽은 바이트 수 반환
}

// ─────────────────────────────
// FAT32 파일 복사
// ─────────────────────────────
bool fat32_cp(const char* src, const char* dst) {
    if (!src || !dst || !*src || !*dst) {
        kprint("fat32_cp: invalid argument\n");
        return false;
    }

    FAT32_DirEntry src_entry, dst_entry;
    char src_dir_path[256], dst_dir_path[256];
    char src_name[64], dst_name[64];
    char final_dir_path[256];
    char final_name[64];
    char path_buf[512];

    // 1️⃣ 원본 경로 해석 및 엔트리 조회
    fat32_split_path(src, src_dir_path, sizeof(src_dir_path), src_name, sizeof(src_name));
    if (src_name[0] == '\0') {
        kprintf("fat32_cp: invalid source path: %s\n", src);
        return false;
    }

    uint32_t src_dir = fat32_resolve_dir(src_dir_path);
    if (src_dir < 2 || src_dir >= 0x0FFFFFF8 ||
        !fat32_find_entry_in_dir(src_dir, src_name, &src_entry)) {
        kprintf("fat32_cp: source file not found: %s\n", src);
        return false;
    }
    if (src_entry.Attr & 0x10) {
        kprintf("fat32_cp: source is a directory: %s\n", src);
        return false;
    }

    // 2️⃣ 대상 경로 해석 (부모 디렉터리 존재 확인)
    fat32_split_path(dst, dst_dir_path, sizeof(dst_dir_path), dst_name, sizeof(dst_name));
    uint32_t dst_dir = fat32_resolve_dir(dst_dir_path);
    if (dst_dir < 2 || dst_dir >= 0x0FFFFFF8) {
        kprintf("fat32_cp: invalid destination path: %s\n", dst);
        return false;
    }

    strncpy(final_dir_path, dst_dir_path, sizeof(final_dir_path) - 1);
    final_dir_path[sizeof(final_dir_path) - 1] = '\0';
    strncpy(final_name, dst_name, sizeof(final_name) - 1);
    final_name[sizeof(final_name) - 1] = '\0';

    // 3️⃣ 대상이 디렉터리인지/파일인지 판별
    if (final_name[0] == '\0') {
        strncpy(final_name, src_name, sizeof(final_name) - 1);
        final_name[sizeof(final_name) - 1] = '\0';
    } else if (fat32_find_entry_in_dir(dst_dir, final_name, &dst_entry)) {
        if (dst_entry.Attr & 0x10) {
            strncpy(final_dir_path, dst, sizeof(final_dir_path) - 1);
            final_dir_path[sizeof(final_dir_path) - 1] = '\0';
            strncpy(final_name, src_name, sizeof(final_name) - 1);
            final_name[sizeof(final_name) - 1] = '\0';
        } else {
            fat32_rm(dst);
        }
    }

    if (final_name[0] == '\0') {
        kprintf("fat32_cp: invalid destination path: %s\n", dst);
        return false;
    }

    int written;
    if (final_dir_path[0] == '\0') {
        written = snprintf(path_buf, sizeof(path_buf), "%s", final_name);
    } else if (strcmp(final_dir_path, "/") == 0) {
        written = snprintf(path_buf, sizeof(path_buf), "/%s", final_name);
    } else if (final_dir_path[strlen(final_dir_path) - 1] == '/') {
        written = snprintf(path_buf, sizeof(path_buf), "%s%s", final_dir_path, final_name);
    } else {
        written = snprintf(path_buf, sizeof(path_buf), "%s/%s", final_dir_path, final_name);
    }

    if (written < 0 || written >= (int)sizeof(path_buf)) {
        kprintf("fat32_cp: destination path too long\n");
        return false;
    }

    const char* target_path = path_buf;
    uint32_t size = src_entry.FileSize;

    // 4️⃣ 빈 파일은 바로 생성
    if (size == 0)
        return fat32_write_file(target_path, NULL, 0);

    // 5️⃣ 원본 내용 읽기 (필요한 만큼만 동적 할당)
    uint8_t* data = kmalloc(size, 0, NULL);
    if (!data) {
        kprintf("fat32_cp: failed to allocate %u bytes\n", size);
        return false;
    }
    if (!fat32_read_file_range(&src_entry, 0, data, size)) {
        kprintf("fat32_cp: failed to read source file: %s\n", src);
        kfree(data);
        return false;
    }

    // 6️⃣ 대상에 쓰기
    bool ok = fat32_write_file(target_path, data, size);
    kfree(data);

    if (!ok) {
        kprintf("fat32_cp: failed to write destination: %s\n", dst);
        return false;
    }

    return true;
}

// ─────────────────────────────
// FAT32 파일 이동 (rename 효과)
// ─────────────────────────────
bool fat32_mv(const char* src, const char* dst) {
    // 1 원본 존재 확인
    if (!fat32_exists(src))
        return false;

    // 2 대상이 이미 있으면 삭제
    if (fat32_exists(dst))
        fat32_rm(dst);

    // 3 복사 시도
    if (!fat32_cp(src, dst))
        return false;

    // 4 원본 삭제
    if (!fat32_rm(src))
        return false;

    // 5 결과 검증
    if (!fat32_exists(dst))
        return false;

    return true;
}

uint32_t fat32_get_file_size(const char* filename) {
    FAT32_DirEntry entry;
    if (!fat32_find_file(filename, &entry))
        return 0;
    return entry.FileSize;
}

bool fat32_read_file_partial(const char* filename, uint32_t offset, uint8_t* out_buf, uint32_t size) {
    FAT32_DirEntry entry;
    if (!fat32_find_file(filename, &entry))
        return false;

    if (offset >= entry.FileSize)
        return false;

    uint32_t to_read = size;
    if (offset + to_read > entry.FileSize)
        to_read = entry.FileSize - offset;

    if (to_read == 0)
        return true;

    return fat32_read_file_range(&entry, offset, out_buf, to_read);
}

//dir
static int fat32_strcasecmp(const char* a, const char* b) {
    while (*a && *b) {
        char ca = (*a >= 'a' && *a <= 'z') ? *a - 32 : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? *b - 32 : *b;
        if (ca != cb) return 1;
        a++; b++;
    }
    return (*a == *b) ? 0 : 1;
}

bool fat32_mkdir(const char* dirname) {
    if (!dirname || !dirname[0]) {
        kprint("mkdir: missing name\n");
        return false;
    }

    char dir_path[256];
    char name_only[64];
    fat32_split_path(dirname, dir_path, sizeof(dir_path), name_only, sizeof(name_only));

    if (name_only[0] == '\0') {
        kprint("mkdir: invalid path\n");
        return false;
    }
    if (strcmp(name_only, ".") == 0 || strcmp(name_only, "..") == 0) {
        kprint("mkdir: invalid name\n");
        return false;
    }

    uint32_t cluster = fat32_resolve_dir(dir_path);
    if (cluster < 2 || cluster >= 0x0FFFFFF8) {
        kprintf("mkdir: invalid directory: %s\n", dir_path);
        return false;
    }

    if (fat32_find_entry_in_dir(cluster, name_only, NULL)) {
        kprintf("mkdir: name already exists (%s)\n", name_only);
        return false;
    }

    char long_name[FAT32_LFN_MAX + 1];
    bool needs_lfn = fat32_name_needs_lfn(name_only);
    if (needs_lfn) {
        if (!fat32_lfn_prepare_name(name_only, long_name, sizeof(long_name))) {
            kprint("mkdir: invalid name\n");
            return false;
        }
    } else {
        strncpy(long_name, name_only, sizeof(long_name) - 1);
        long_name[sizeof(long_name) - 1] = '\0';
    }

    uint8_t short_name[11];
    if (needs_lfn) {
        if (!fat32_generate_short_name(cluster, long_name, short_name)) {
            kprint("FAT32: failed to generate short name\n");
            return false;
        }
    } else {
        char fatname[12];
        fat32_make83(name_only, fatname);
        memcpy(short_name, fatname, 11);
        if (fat32_short_name_exists(cluster, short_name)) {
            kprintf("mkdir: name already exists (%s)\n", name_only);
            return false;
        }
    }

    uint32_t lfn_count = needs_lfn ? (uint32_t)((strlen(long_name) + FAT32_LFN_CHARS_PER_ENTRY - 1) / FAT32_LFN_CHARS_PER_ENTRY) : 0;
    if (lfn_count > FAT32_LFN_MAX_ENTRIES) {
        kprint("mkdir: name too long\n");
        return false;
    }

    FAT32_DirSlot slots[FAT32_LFN_MAX_ENTRIES + 1];
    uint32_t needed = lfn_count + 1;
    if (!fat32_find_free_slots(cluster, needed, slots)) {
        kprint("FAT32: No free dir entry for mkdir\n");
        return false;
    }

    uint32_t newclus = fat32_alloc_cluster(fat32_drive);
    if (!newclus) {
        kprint("FAT32: No free cluster for mkdir\n");
        return false;
    }

    if (lfn_count > 0) {
        uint8_t checksum = fat32_lfn_checksum(short_name);
        fat32_write_lfn_entries(slots, lfn_count, long_name, checksum);
    }

    uint8_t buf[SECTOR_SIZE];
    FAT32_DirEntry entry;
    memset(&entry, 0, sizeof(entry));
    memcpy(entry.Name, short_name, 11);
    entry.Attr = 0x10;
    entry.FstClusLO = (uint16_t)(newclus & 0xFFFF);
    entry.FstClusHI = (uint16_t)(newclus >> 16);
    entry.FileSize = 0;

    fat32_dir_write_raw(&slots[lfn_count], &entry);

    // ─────────────────────────────
    // ④ 새 디렉터리 클러스터 초기화 (. / ..)
    // ─────────────────────────────
    memset(buf, 0, SECTOR_SIZE);
    FAT32_DirEntry* de = (FAT32_DirEntry*)buf;

    // '.' 엔트리
    memset(de[0].Name, ' ', 11);
    memcpy(de[0].Name, ".          ", 11);
    de[0].Attr = 0x10;
    de[0].FstClusLO = (uint16_t)(newclus & 0xFFFF);
    de[0].FstClusHI = (uint16_t)(newclus >> 16);

    // '..' 엔트리 (부모는 현재 디렉터리)
    memset(de[1].Name, ' ', 11);
    memcpy(de[1].Name, "..         ", 11);
    de[1].Attr = 0x10;
    de[1].FstClusLO = (uint16_t)(cluster & 0xFFFF);
    de[1].FstClusHI = (uint16_t)(cluster >> 16);

    // 나머지 엔트리 비움
    for (size_t x = 2; x < SECTOR_SIZE / sizeof(FAT32_DirEntry); x++)
        de[x].Name[0] = 0x00;

    // 새 디렉터리 클러스터 전체 초기화
    for (uint8_t ss = 0; ss < bpb.SecPerClus; ss++)
        write_sector(fat32_drive, cluster_to_lba(newclus) + ss, buf);

    kprintf("FAT32: Directory '%s' created (cluster %u)\n", dirname, newclus);
    return true;
}

bool fat32_rmdir(const char* dirname) {
    if (!dirname || !dirname[0]) {
        kprint("rmdir: missing argument\n");
        return false;
    }

    char dir_path[256];
    char name_only[64];
    fat32_split_path(dirname, dir_path, sizeof(dir_path), name_only, sizeof(name_only));

    if (name_only[0] == '\0') {
        kprint("rmdir: invalid path\n");
        return false;
    }

    uint32_t cluster = fat32_resolve_dir(dir_path);
    if (cluster < 2 || cluster >= 0x0FFFFFF8) {
        kprintf("rmdir: invalid directory: %s\n", dir_path);
        return false;
    }

    FAT32_DirEntry entry;
    FAT32_DirSlot slot;
    FAT32_DirSlot lfn_slots[FAT32_LFN_MAX_ENTRIES];
    uint32_t lfn_count = 0;

    if (!fat32_find_entry_slot(cluster, name_only, &entry, &slot, lfn_slots, &lfn_count)) {
        kprintf("rd: no such directory: %s\n", dirname);
        return false;
    }

    if (!(entry.Attr & 0x10)) {
        kprint("rmdir: not a directory\n");
        return false;
    }

    // 디렉터리 클러스터 번호
    uint32_t dirclus =
        ((uint32_t)entry.FstClusHI << 16) | entry.FstClusLO;

    // ───────────────────────────────
    // 내부가 비어있는지 확인
    // ───────────────────────────────
    uint8_t inner[SECTOR_SIZE];
    bool empty = true;
    read_sector(fat32_drive, cluster_to_lba(dirclus), inner);
    FAT32_DirEntry* de = (FAT32_DirEntry*)inner;

    for (size_t j = 2; j < SECTOR_SIZE / sizeof(FAT32_DirEntry); j++) {
        if (de[j].Name[0] == 0x00) break;  // 끝
        if (de[j].Name[0] == 0xE5) continue;
        if (de[j].Attr == 0x0F) continue;

        // '.' '..' 제외
        if (!(de[j].Name[0] == '.' && (de[j].Name[1] == ' ' || de[j].Name[1] == '.'))) {
            empty = false;
            break;
        }
    }

    if (!empty) {
        kprint("rmdir: directory not empty\n");
        return false;
    }

    // ───────────────────────────────
    // FAT 체인 해제
    // ───────────────────────────────
    uint32_t cl = dirclus;
    while (cl >= 2 && cl < 0x0FFFFFF8) {
        uint32_t next = fat32_next_cluster(fat32_drive, cl);

        uint8_t fatbuf[SECTOR_SIZE];
        uint32_t fat_sector = fat_start_lba + (cl * 4) / SECTOR_SIZE;
        uint32_t fat_offset = (cl * 4) % SECTOR_SIZE;
        read_sector(fat32_drive, fat_sector, fatbuf);
        *(uint32_t*)(fatbuf + fat_offset) = 0;
        write_sector(fat32_drive, fat_sector, fatbuf);

        cl = next;
    }

    // ───────────────────────────────
    // 디렉터리 엔트리 삭제 표시
    // ───────────────────────────────
    for (uint32_t i = 0; i < lfn_count; i++)
        fat32_dir_mark_deleted(&lfn_slots[i]);
    fat32_dir_mark_deleted(&slot);

    kprintf("rmdir: directory '%s' deleted.\n", dirname);
    return true;
}

uint32_t fat32_find_dir_cluster(uint32_t start_cluster, const char* dirname) {
    if (!dirname || !dirname[0]) return 0;

    // "." / ".." 처리
    if (strcmp(dirname, ".") == 0)
        return start_cluster;

    if (strcmp(dirname, "..") == 0) {
        uint8_t buf[SECTOR_SIZE];
        read_sector(fat32_drive, cluster_to_lba(start_cluster), buf);
        FAT32_DirEntry* de = (FAT32_DirEntry*)buf;
        uint32_t parent = ((uint32_t)de[1].FstClusHI << 16) | de[1].FstClusLO;
        if (parent < 2) parent = root_dir_cluster32;
        return parent;
    }

    fat32_dir_find_ctx_t ctx = {
        .name = dirname,
        .found = 0,
    };
    fat32_iterate_dir(start_cluster, fat32_find_dir_cb, &ctx);
    return ctx.found;
}

bool fat32_cd(const char* path) {
    if (!path || !path[0]) {
        kprint("cd: missing path\n");
        return false;
    }

    if (strcmp(path, "/") == 0) {
        current_dir_cluster32 = root_dir_cluster32;
        strcpy(current_path, "/");
        kprintf("Changed directory to: / (cluster=%u)\n", root_dir_cluster32);
        return true;
    }

    char clean[128];
    memset(clean, 0, sizeof(clean));

    size_t len = 0;
    while (path[len] && len < sizeof(clean) - 1) {
        clean[len] = path[len];
        len++;
    }
    clean[len] = '\0';

    // 끝 슬래시 제거
    while (len > 0 && (clean[len - 1] == '/' || clean[len - 1] == ' '))
        clean[--len] = '\0';

    // 초기 디렉토리
    uint32_t cluster = (clean[0] == '/') ? root_dir_cluster32 : current_dir_cluster32;

    char new_path[256];
    if (clean[0] == '/')
        strcpy(new_path, "/");
    else
        strcpy(new_path, current_path);

    const char* p = clean;
    char segment[64];

    while (*p) {
        // 세그먼트 추출
        int idx = 0;
        memset(segment, 0, sizeof(segment));
        while (*p && *p != '/' && idx < (int)sizeof(segment) - 1)
            segment[idx++] = *p++;
        if (*p == '/') p++;

        if (segment[0] == '\0') continue;

        // 현재 디렉토리
        if (strcmp(segment, ".") == 0) continue;

        // 상위 디렉토리
        else if (strcmp(segment, "..") == 0) {
            if (cluster == root_dir_cluster32 || cluster < 2) {
                cluster = root_dir_cluster32;
                strcpy(new_path, "/");
                continue;
            }

            uint8_t buf[SECTOR_SIZE];
            read_sector(fat32_drive, cluster_to_lba(cluster), buf);
            FAT32_DirEntry* de = (FAT32_DirEntry*)buf;
            uint32_t parent = ((uint32_t)de[1].FstClusHI << 16) | de[1].FstClusLO;
            if (parent < 2 || parent == cluster)
                parent = root_dir_cluster32;

            cluster = parent;

            if (strcmp(new_path, "/") != 0) {
                char* last = strrchr(new_path, '/');
                if (last && last != new_path)
                    *last = '\0';
                else
                    strcpy(new_path, "/");
            }
        }

        // 하위 디렉토리 이동
        else {
            uint32_t next = fat32_find_dir_cluster(cluster, segment);
            if (next < 2 || next >= 0x0FFFFFF8) {
                kprintf("cd: no such directory: %s\n", segment);
                return false;
            }

            cluster = next;

            if (strcmp(new_path, "/") != 0)
                strncat(new_path, "/", sizeof(new_path) - strlen(new_path) - 1);
            strncat(new_path, segment, sizeof(new_path) - strlen(new_path) - 1);
        }
    }

    // 경로 정리
    size_t n = strlen(new_path);
    while (n > 1 && new_path[n - 1] == '/')
        new_path[--n] = '\0';

    current_dir_cluster32 = cluster;
    strncpy(current_path, new_path, sizeof(current_path) - 1);
    current_path[sizeof(current_path) - 1] = '\0';

    kprintf("Changed directory to: %s (cluster=%u)\n", current_path, current_dir_cluster32);
    return true;
}

// ✅ 총 클러스터 수 계산
uint32_t fat32_total_clusters() {
    // 보호 코드 추가
    if (bpb.SecPerClus == 0) return 0;
    if (bpb.FATSz32 == 0 || bpb.NumFATs == 0) return 0;
    if (bpb.TotSec32 == 0) return 0;

    uint32_t total_sectors = bpb.TotSec32;
    uint32_t data_sectors =
        total_sectors - (bpb.RsvdSecCnt + (bpb.NumFATs * bpb.FATSz32));
    if (data_sectors == 0) return 0;

    uint32_t total_clusters = data_sectors / bpb.SecPerClus;
    return total_clusters;
}

// ✅ 빈(Free) 클러스터 수 계산
uint32_t fat32_free_clusters() {
    if (bpb.SecPerClus == 0 || bpb.FATSz32 == 0) return 0;

    uint32_t fat_start = fat_start_lba + bpb.RsvdSecCnt;
    uint8_t sector[512];
    uint32_t free_count = 0;

    // FAT 섹터 반복
    for (uint32_t s = 0; s < bpb.FATSz32; s++) {
        if (!ata_read(fat32_drive, fat_start + s, 1, sector))
            continue;

        for (uint32_t i = 0; i < 512; i += 4) {
            uint32_t entry = *(uint32_t*)&sector[i] & 0x0FFFFFFF;
            if (entry == 0x00000000)
                free_count++;
        }
    }

    return free_count;
}

// (FAT32_BPB_t, FAT32_DirEntry, ata_*, kprintf, memset, memcpy 등은 정의되어 있다고 가정)
bool fat32_format_at(uint8_t drive, uint32_t base_lba, uint32_t total_sectors, const char* label) {
    FAT32_BPB_t bpb;
    uint8_t sector[512];

    if (total_sectors == 0) {
        kprintf("[FAT32] Drive %d not detected or empty.\n", drive);
        return false;
    }

    kprintf("[FAT32] Formatting drive %d (base LBA=%u, %u sectors)...\n",
            drive, base_lba, total_sectors);
    memset(&bpb, 0, sizeof(FAT32_BPB_t));

    /* ────────────────
       기본 BPB 설정
    ──────────────── */
    memcpy(bpb.jmpBoot, "\xEB\x58\x90", 3);  // JMP short + NOP
    memcpy(bpb.OEMName, "ORIONOS ", 8);
    bpb.BytsPerSec = 512;
    bpb.SecPerClus = 8;       // 4KB cluster
    bpb.RsvdSecCnt = 32;      // FAT32 기본 예약 영역
    bpb.NumFATs = 2;
    bpb.RootEntCnt = 0;       // FAT32는 항상 0
    bpb.Media = 0xF8;
    bpb.SecPerTrk = 63;
    bpb.NumHeads = 255;
    bpb.HiddSec = base_lba;
    bpb.TotSec16 = 0;
    bpb.TotSec32 = total_sectors;

    /* ────────────────
       FAT 크기 계산
    ──────────────── */
    uint32_t data_sectors, cluster_count, fatsz;

    // FAT32 포맷 루프 (대략적인 FAT 크기 계산)
    for (uint32_t sec_per_clus = 1; sec_per_clus <= 128; sec_per_clus <<= 1) {
        data_sectors = total_sectors - (bpb.RsvdSecCnt + (bpb.NumFATs * 1));
        cluster_count = data_sectors / sec_per_clus;
        fatsz = ((cluster_count * 4) + (bpb.BytsPerSec - 1)) / bpb.BytsPerSec;
        if (cluster_count >= 65525) {
            bpb.SecPerClus = sec_per_clus;
            bpb.FATSz32 = fatsz;
            break;
        }
    }

    /* ────────────────
       FAT32 확장 필드
    ──────────────── */
    bpb.ExtFlags = 0;
    bpb.FSVer = 0;
    bpb.RootClus = 2;   // 루트 디렉터리 시작 클러스터
    bpb.FSInfo = 1;     // FSInfo 섹터
    bpb.BkBootSec = 6;  // 백업 부트섹터
    memset(bpb.Reserved, 0, sizeof(bpb.Reserved));
    bpb.DrvNum = 0x80;
    bpb.BootSig = 0x29;
    bpb.VolID = 0x12345678;
    memset(bpb.VolLab, ' ', 11);
    if (label && *label)
        strncpy((char*)bpb.VolLab, label, strlen(label) > 11 ? 11 : strlen(label));
    memcpy(bpb.FilSysType, "FAT32   ", 8);

    /* ────────────────
       부트 섹터 작성
    ──────────────── */
    memset(sector, 0, 512);
    memcpy(sector, &bpb, sizeof(FAT32_BPB_t));

    const uint8_t bootcode_stub[] = {
        0xFA,             // cli
        0x31, 0xC0,       // xor ax, ax
        0x8E, 0xD0,       // mov ss, ax
        0xBC, 0x00, 0x7C, // mov sp, 0x7C00
        0xFB,             // sti
        0xE9, 0x00, 0x00  // jmp short $
    };
    memcpy(sector + 90, bootcode_stub, sizeof(bootcode_stub));
    sector[510] = 0x55;
    sector[511] = 0xAA;
    ata_write_sector(drive, base_lba + 0, sector);

    /* ────────────────
       FSInfo 섹터 작성
    ──────────────── */
    memset(sector, 0, 512);
    *(uint32_t*)(sector + 0) = 0x41615252;   // Lead signature "RRaA"
    *(uint32_t*)(sector + 484) = 0x61417272; // Structure signature "rrAa"
    *(uint32_t*)(sector + 488) = 0xFFFFFFFF; // Free cluster count (unknown)
    *(uint32_t*)(sector + 492) = 0x00000003; // Next free cluster
    *(uint32_t*)(sector + 508) = 0xAA550000; // Trail signature
    sector[510] = 0x55;
    sector[511] = 0xAA;
    ata_write_sector(drive, base_lba + 1, sector);

    /* ────────────────
       백업 부트섹터 작성 (LBA 6)
    ──────────────── */
    ata_write_sector(drive, base_lba + 6, (uint8_t*)&bpb);

    /* ────────────────
       FAT 초기화
    ──────────────── */
    memset(sector, 0, 512);
    sector[0] = 0xF8;  // Media descriptor
    sector[1] = 0xFF;
    sector[2] = 0xFF;
    sector[3] = 0x0F;  // FAT32 signature
    sector[4] = 0xFF;
    sector[5] = 0xFF;
    sector[6] = 0xFF;
    sector[7] = 0x0F;  // EOC marker for cluster 2 (root dir)

    uint32_t fat_start = base_lba + bpb.RsvdSecCnt;
    for (uint8_t f = 0; f < bpb.NumFATs; f++) {
        for (uint32_t i = 0; i < bpb.FATSz32; i++) {
            ata_write_sector(drive, fat_start + f * bpb.FATSz32 + i, sector);
            memset(sector, 0, 512);
        }
    }

    /* ────────────────
       루트 디렉터리 초기화 (클러스터 2)
    ──────────────── */
    memset(sector, 0, 512);
    FAT32_DirEntry root_entry;
    memset(&root_entry, 0, sizeof(FAT32_DirEntry));

    uint32_t data_start = bpb.RsvdSecCnt + bpb.NumFATs * bpb.FATSz32;
    uint32_t root_lba = base_lba + data_start + (bpb.SecPerClus * (bpb.RootClus - 2));
    ata_write_sector(drive, root_lba, sector);

    kprintf("[FAT32] Format complete.\n");
    kprintf("[FAT32] FAT size %u sectors, root cluster at %u (LBA %u)\n",
            bpb.FATSz32, bpb.RootClus, root_lba);
    return true;
}

bool fat32_format(uint8_t drive, const char* label) {
    uint32_t total_sectors = ata_get_sector_count(drive);
    return fat32_format_at(drive, 0, total_sectors, label);
}
