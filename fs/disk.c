#include "disk.h"
#include "../drivers/ata.h"
#include "../mm/mem.h"
#include "../libc/string.h"
#include "../drivers/screen.h"
#include "../drivers/ramdisk.h"
#include "../kernel/proc/workqueue.h"
#include "fat16.h"
#include "xvfs.h"
#include "fs_quick.h"

DiskInfo disks[MAX_DISKS];   // 전역 배열
int disk_count = 0;  // 전역 카운트

static volatile bool disk_rescan_pending = false;
static volatile bool disk_rescan_again = false;
#define EFLAGS_IF 0x200u

static uint32_t irq_save(void) {
    uint32_t flags = 0;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void irq_restore(uint32_t flags) {
    if (flags & EFLAGS_IF) {
        __asm__ volatile("sti" ::: "memory");
    }
}

static void disk_rescan_work(void* ctx) {
    (void)ctx;
    for (;;) {
        detect_disks_quick();

        bool repeat = false;
        uint32_t flags = irq_save();
        if (disk_rescan_again) {
            disk_rescan_again = false;
            repeat = true;
        } else {
            disk_rescan_pending = false;
        }
        irq_restore(flags);

        if (!repeat) {
            break;
        }
    }
}

#pragma pack(push,1)
typedef struct {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_first;
    uint32_t sectors;
} MBRPart;
#pragma pack(pop)

static inline bool is_fat16_ptype(uint8_t t){ return t==0x04||t==0x06||t==0x0E; }
static inline bool is_fat32_ptype(uint8_t t){ return t==0x0B||t==0x0C; }
static inline bool has_55aa(const uint8_t *sec) { return sec[510]==0x55 && sec[511]==0xAA; }
static inline bool valid_bootjmp(const uint8_t *sec) {
    // EB ?? 90 또는 E9 로 시작하면 부트 섹터로 인정
    return (sec[0]==0xEB && sec[2]==0x90) || sec[0]==0xE9;
}

static inline bool is_fat_sig(const uint8_t *sec, const char *sig8) {
    return (memcmp(sec + 0x36, sig8, 8) == 0) ||   // 흔한 위치
           (memcmp(sec + 0x52, sig8, 8) == 0);    // mkfs.fat 등
}

static inline bool is_xvfs_sig_at(uint8_t drive, uint32_t base_lba) {
    uint8_t sec0[512], sec1[512];
    if (!ata_read(drive, base_lba + 0, 1, sec0)) return false;
    if (memcmp(sec0, "XVFS2", 5) != 0) return false;
    if (!ata_read(drive, base_lba + 1, 1, sec1)) return false;
    uint32_t magic = *(uint32_t*)sec1;
    return (magic == 0x58564653);
}

static inline bool is_xvfs_sig(uint8_t drive) {
    uint8_t sec0[512], sec1[512];

    // 0번 섹터 읽기 (부트 블록)
    if (!ata_read(drive, 0, 1, sec0)) return false;
    if (memcmp(sec0, "XVFS2", 5) != 0) return false;

    // 1번 섹터 읽기 (슈퍼블록)
    if (!ata_read(drive, 1, 1, sec1)) return false;
    uint32_t magic = *(uint32_t*)sec1;
    if (magic != 0x58564653) return false;

    return true;  // 두 조건 모두 만족 → 진짜 XVFS
}

fs_kind_t fs_quick_probe(uint8_t drive, uint32_t *out_base_lba) {
    uint8_t sec[512];

    // 장치 유무는 섹터0 읽기 성공 여부로만 판단
    if (!ata_read(drive, 0, 1, sec)) return FSQ_NONE;
    if (!has_55aa(sec)) return FSQ_UNKNOWN;

    // ─────────────────────────────
    // 0️⃣ XVFS 먼저 검사
    // ─────────────────────────────
    if (is_xvfs_sig(drive)) {
        if (out_base_lba) *out_base_lba = 0;
        return FSQ_XVFS;
    }

    // ─────────────────────────────
    // 1️⃣ FAT 시그니처 검사
    // ─────────────────────────────
    if (is_fat_sig(sec, "FAT16   ")) { if (out_base_lba) *out_base_lba = 0; return FSQ_FAT16; }
    if (is_fat_sig(sec, "FAT32   ")) { if (out_base_lba) *out_base_lba = 0; return FSQ_FAT32; }

    // ─────────────────────────────
    // 2️⃣ MBR 파티션 검사
    // ─────────────────────────────
    const MBRPart *p = (const MBRPart *)(sec + 0x1BE);
    bool any_entry = false;
    for (int i = 0; i < 4; i++)
        if (p[i].type != 0) { any_entry = true; break; }

    if (any_entry) {
        for (int i = 0; i < 4; i++) {
            uint8_t t = p[i].type;
            if (t == 0) continue;

            uint32_t base = p[i].lba_first;
            if (out_base_lba) *out_base_lba = base;

            if (ata_read(drive, base, 1, sec) && has_55aa(sec)) {
                if (is_fat_sig(sec, "FAT16   ")) return FSQ_FAT16;
                if (is_fat_sig(sec, "FAT32   ")) return FSQ_FAT32;
                if (is_xvfs_sig_at(drive, base)) return FSQ_XVFS;  // ⚡ 파티션 내부에서도 체크
            }
            return FSQ_MBR;
        }
        return FSQ_MBR;
    }

    return FSQ_UNKNOWN;
}

void detect_disks_quick(void) {
    disk_count = 0;
    kprint("[DISK] Quick detection start\n");

    for (uint8_t d = 0; d < MAX_DISKS; d++) {
        uint32_t base = 0;
        fs_kind_t kind = fs_quick_probe(d, &base);

        // ATA 레벨에서 감지 안 됨
        if (kind == FSQ_NONE) {
            kprintf("[DISK] drive %d > no response\n", d);
            disks[d].present = false;
            disks[d].id = d;
            disks[d].base_lba = 0;
            strcpy(disks[d].fs_type, "None");
            continue;
        }

        // NTFS 필터: FAT 시그니처 없고 "NTFS" 문자열 포함 시 Unknown 처리
        uint8_t sec[512];
        if (ata_read(d, base, 1, sec)) {
            if (memcmp(sec + 0x03, "NTFS", 4) == 0 || memcmp(sec + 0x52, "NTFS", 4) == 0) {
                kprintf("[DISK] drive %d > NTFS detected, marking Unknown\n", d);
                kind = FSQ_UNKNOWN;
            }
        }

        disks[d].present = true;
        disks[d].id = d;
        disks[d].base_lba = base;

        switch (kind) {
            case FSQ_FAT16:
                strcpy(disks[d].fs_type, "FAT16");
                break;
            case FSQ_FAT32:
                strcpy(disks[d].fs_type, "FAT32");
                break;
            case FSQ_XVFS:
                strcpy(disks[d].fs_type, "XVFS");
                break;
            case FSQ_MBR:
                strcpy(disks[d].fs_type, "MBR");
                break;
            default:
                strcpy(disks[d].fs_type, "Unknown");
                break;
        }

        kprintf("[DISK] drive %d detected as %s (base LBA=%u)\n",
                disks[d].id,
                disks[d].fs_type,
                disks[d].base_lba);

        disk_count++;
    }

    if (disk_count == 0)
        kprint("[DISK] No drives found.\n");
    else
        kprintf("[DISK] Total %d drive(s) detected.\n", disk_count);
}

void cmd_disk_ls() {
    kprint("Detected disks:\n");

    if (disk_count == 0) {
        kprint("  (no disks found)\n");
        return;
    }

    for (int i = 0; i < MAX_DISKS; i++) {
        if (!disks[i].present) continue;
        const char* fs = disks[i].fs_type;
        uint32_t base = disks[i].base_lba;
        uint8_t id = disks[i].id;
        const char* ram_suffix = ramdisk_present(id) ? " (ramdisk)" : "";
        
        if (base == 0 && (strcmp(fs, "XVFS") == 0)) {
            kprintf("  %d#: %s (superfloppy)%s\n", id, fs, ram_suffix);
            continue;
        }
        if (strcmp(fs, "XVFS") == 0) {
            kprintf("  %d#: %s (base LBA=%u, partitioned)%s\n", id, fs, base, ram_suffix);
            continue;
        }
        if (base == 0 && (strcmp(fs, "FAT16") == 0 || strcmp(fs, "FAT32") == 0)) {
            kprintf("  %d#: %s (superfloppy)%s\n", id, fs, ram_suffix);
            continue;
        }
        if (strcmp(fs, "FAT16") == 0 || strcmp(fs, "FAT32") == 0) {
            kprintf("  %d#: %s (base LBA=%u, partitioned)%s\n", id, fs, base, ram_suffix);
            continue;
        }
        if (strcmp(fs, "MBR") == 0) {
            kprintf("  %d#: MBR (non-FAT partitions present, base LBA=%u)%s\n", id, base, ram_suffix);
            continue;
        }
        if (strcmp(fs, "Unknown") == 0) {
            kprintf("  %d#: Unknown or unsupported filesystem (base LBA=%u)%s\n", id, base, ram_suffix);
            continue;
        }

        kprintf("  %d#: %s (base LBA=%u)%s\n", id, fs, base, ram_suffix);
    }

    kprintf("[DISK] Total %d drive(s) detected.\n", disk_count);
}

void disk_request_rescan(void) {
    bool enqueue = false;
    uint32_t flags = irq_save();
    if (!disk_rescan_pending) {
        disk_rescan_pending = true;
        enqueue = true;
    } else {
        disk_rescan_again = true;
    }
    irq_restore(flags);

    if (enqueue) {
        if (!workqueue_enqueue(disk_rescan_work, NULL)) {
            flags = irq_save();
            disk_rescan_pending = false;
            irq_restore(flags);
        }
    }
}
