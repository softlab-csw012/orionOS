#include "ata.h"
#include "pci.h"
#include "hal.h"
#include "../mm/mem.h"
#include "../libc/string.h"
#include "../fs/disk.h"
#include "../drivers/screen.h"
#include "ramdisk.h"
#include "usb/usb.h"
#include <stdbool.h>
#include <stdint.h>

/* ====== 채널/장치 매핑 ======
   drive 번호: 0=Pri Master (1F0/3F6)
               1=Pri Slave
               2=Sec Master (170/376)
               3=Sec Slave
*/

ata_chan_t CH[2];

#define ATA_CMD_READ      0x20
#define ATA_CMD_WRITE     0x30
#define ATA_CMD_IDENTIFY  0xEC
#define ATA_CMD_CACHE_FLUSH 0xE7

#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF   0x20
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

#define ATA_IO_BASE(drive)  ((drive) < 2 ? 0x1F0 : 0x170)
#define ATA_CTRL_BASE(drive) ((drive) < 2 ? 0x3F6 : 0x376)

bool ata_available[4] = {false, false, false, false};

static inline void ata_400ns(uint8_t ch) {
    (void)hal_in8(CH[ch].ctrl);
    (void)hal_in8(CH[ch].ctrl);
    (void)hal_in8(CH[ch].ctrl);
    (void)hal_in8(CH[ch].ctrl);
}

static inline void ata_disable_irq(uint8_t ch) {
    // nIEN=1 (disable interrupts)
    hal_out8(CH[ch].ctrl, 0x02);
}

static int wait_not_bsy(uint8_t ch, unsigned long limit) {
    while (limit--) {
        uint8_t s = hal_in8(CH[ch].io + 7);
        if (!(s & ATA_SR_BSY)) {
            if (s & ATA_SR_ERR) return -2;
            if (s & ATA_SR_DF)  return -3;
            return 0;
        }
    }
    return -1; // timeout
}

static int wait_drq(uint8_t ch, unsigned long limit) {
    while (limit--) {
        uint8_t s = hal_in8(CH[ch].io + 7);
        if (s & ATA_SR_ERR) return -2;
        if (s & ATA_SR_DF)  return -3;
        if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRQ)) return 0;
    }
    return -1; // timeout
}

/* drive 선택:
   DEV(=io+6)에 0xA0 | (slave<<4) 먼저 쓰고 400ns,
   실제 커맨드 직전엔 0xE0 | (slave<<4) | (LBA24..27) */
static inline void select_dev_only(uint8_t drive) {
    uint8_t ch = drive >> 1;       // 0=primary,1=secondary
    uint8_t sl = drive & 1;        // 0=master, 1=slave
    hal_out8(CH[ch].io + 6, (uint8_t)(0xA0 | (sl << 4)));
    ata_400ns(ch);
}

static inline void set_dev_lba28(uint8_t drive, uint32_t lba) {
    uint8_t ch = drive >> 1;
    uint8_t sl = drive & 1;
    hal_out8(CH[ch].io + 6, (uint8_t)(0xE0 | (sl << 4) | ((lba >> 24) & 0x0F)));
    ata_400ns(ch);
    ata_400ns(ch);
}

bool ata_read(uint8_t drive, uint32_t lba, uint16_t count, uint8_t* buffer) {
    if (count == 0) count = 256;        // 256=0 의미
    if (ramdisk_present(drive)) {
        return ramdisk_read(drive, lba, count, buffer);
    }
    if (drive >= 4) {
        return usb_storage_read_sectors(drive, lba, count, buffer);
    }
    if (!ata_available[drive]) return false;

    uint8_t ch = drive >> 1;
    uint8_t io_count = (count == 256) ? 0 : (uint8_t)count;

    ata_disable_irq(ch);
    select_dev_only(drive);
    if (wait_not_bsy(ch, 100000)) { kprint("BSY timeout before READ\n"); return false; }

    // sector count & LBA(0..23)
    hal_out8(CH[ch].io + 2, io_count);
    hal_out8(CH[ch].io + 3, (uint8_t)(lba & 0xFF));
    hal_out8(CH[ch].io + 4, (uint8_t)((lba >> 8) & 0xFF));
    hal_out8(CH[ch].io + 5, (uint8_t)((lba >> 16) & 0xFF));
    set_dev_lba28(drive, lba);

    hal_out8(CH[ch].io + 7, ATA_CMD_READ);

    for (uint16_t s = 0; s < count; ++s) {
        if (wait_drq(ch, 200000)) { kprint("READ wait DRQ err/timeout\n"); return false; }
        uint16_t* w = (uint16_t*)(buffer + s * 512);
        for (int i = 0; i < 256; ++i) w[i] = hal_in16(CH[ch].io + 0);
        ata_400ns(ch);
    }
    return true;
}

bool ata_write(uint8_t drive, uint32_t lba, uint16_t count, const uint8_t* buffer) {
    if (count == 0) count = 256;
    if (ramdisk_present(drive)) {
        return ramdisk_write(drive, lba, count, buffer);
    }
    if (drive >= 4) {
        return usb_storage_write_sectors(drive, lba, count, buffer);
    }
    if (!ata_available[drive]) return false;

    uint8_t ch = drive >> 1;
    uint8_t io_count = (count == 256) ? 0 : (uint8_t)count;

    ata_disable_irq(ch);
    select_dev_only(drive);
    if (wait_not_bsy(ch, 100000)) { kprint("BSY timeout before WRITE\n"); return false; }

    hal_out8(CH[ch].io + 2, io_count);
    hal_out8(CH[ch].io + 3, (uint8_t)(lba & 0xFF));
    hal_out8(CH[ch].io + 4, (uint8_t)((lba >> 8) & 0xFF));
    hal_out8(CH[ch].io + 5, (uint8_t)((lba >> 16) & 0xFF));
    set_dev_lba28(drive, lba);

    hal_out8(CH[ch].io + 7, ATA_CMD_WRITE);

    for (uint16_t s = 0; s < count; ++s) {
        if (wait_drq(ch, 200000)) { kprint("WRITE wait DRQ err/timeout\n"); return false; }
        const uint16_t* r = (const uint16_t*)(buffer + s * 512);
        for (int i = 0; i < 256; ++i) hal_out16(CH[ch].io + 0, r[i]);
        ata_400ns(ch);
    }
    return true;
}

bool ata_read_sector(uint32_t drive, uint32_t lba, uint8_t* buffer) {
    return ata_read((uint8_t)drive, lba, 1, buffer);
}
bool ata_write_sector(uint32_t drive, uint32_t lba, const uint8_t* buffer) {
    return ata_write((uint8_t)drive, lba, 1, buffer);
}

bool ata_flush_cache(uint8_t drive) {
    if (ramdisk_present(drive)) {
        return true;
    }
    if (drive >= 4) {
        return usb_storage_sync(drive);
    }
    if (drive > 3 || !ata_available[drive]) return false;

    uint8_t ch = drive >> 1;
    ata_disable_irq(ch);
    select_dev_only(drive);
    if (wait_not_bsy(ch, 100000)) return false;
    hal_out8(CH[ch].io + 7, ATA_CMD_CACHE_FLUSH);
    if (wait_not_bsy(ch, 1000000)) return false;
    return true;
}

// 장치 타입 판별용
typedef enum { ATA_NONE=0, ATA_ATA=1, ATA_ATAPI=2 } ata_type_t;

static ata_type_t ata_identify_try(uint8_t drive, uint8_t cmd_identify) {
    uint8_t ch = drive >> 1;
    uint8_t sl = drive & 1;

    // 장치 선택
    hal_out8(CH[ch].io + 6, 0xA0 | (sl << 4));
    ata_400ns(ch);

    // 레지스터 초기화
    hal_out8(CH[ch].io + 2, 0);
    hal_out8(CH[ch].io + 3, 0);
    hal_out8(CH[ch].io + 4, 0);
    hal_out8(CH[ch].io + 5, 0);

    // IDENTIFY (ATA: 0xEC / ATAPI: 0xA1)
    hal_out8(CH[ch].io + 7, cmd_identify);

    // BSY 해제 대기
    if (wait_not_bsy(ch, 100000)) return ATA_NONE;

    // 상태/시그니처 확인
    uint8_t status = hal_in8(CH[ch].io + 7);
    (void)hal_in8(CH[ch].io + 2); // Sector Count
    (void)hal_in8(CH[ch].io + 3); // Sector Number
    (void)hal_in8(CH[ch].io + 4); // LBA mid
    (void)hal_in8(CH[ch].io + 5); // LBA high

    // 에러면 실패
    if (status & (ATA_SR_ERR | ATA_SR_DF)) return ATA_NONE;

    // DRQ 대기
    int to = 200000;
    while (!(status & (ATA_SR_DRQ | ATA_SR_ERR | ATA_SR_DF)) && to-- > 0) {
        status = hal_in8(CH[ch].io + 7);
    }
    if (status & (ATA_SR_ERR | ATA_SR_DF)) return ATA_NONE;
    if (!(status & ATA_SR_DRQ)) return ATA_NONE;

    // 512바이트(256워드) 읽어서 DRQ 비우기 (IDENTIFY 데이터)
    for (int i = 0; i < 256; ++i) (void)hal_in16(CH[ch].io + 0);
    ata_400ns(ch);

    // 명령 종류에 따라 타입 반환
    return (cmd_identify == 0xEC) ? ATA_ATA : ATA_ATAPI;
}

static void ata_soft_reset(uint8_t ch) {
    hal_out8(CH[ch].ctrl, 0x04 | 0x02); // SRST=1
    ata_400ns(ch);
    ata_400ns(ch);
    ata_400ns(ch);
    hal_out8(CH[ch].ctrl, 0x02); // SRST=0

    // VMware는 soft-reset 이후 최소 수백 microsecond 기다림 필요
    for (volatile int i = 0; i < 100000; i++);
    
    wait_not_bsy(ch, 1000000);
}
// i love OSDev
bool ata_present(uint8_t drive) {
    if (drive > 3) return false;
    uint8_t ch = drive >> 1;
    uint8_t sl = drive & 1;

    uint8_t status_raw = hal_in8(CH[ch].io + 7);
    kprintf("Drive %d (ch=%d, sl=%d) status=%X\n", drive, ch, sl, status_raw);

    // IRQ 비활성화 + 소프트리셋 한 번
    ata_disable_irq(ch);
    ata_soft_reset(ch);

    // 장치 선택 후 상태 확인
    hal_out8(CH[ch].io + 6, 0xA0 | (sl << 4));
    ata_400ns(ch);

    uint8_t status = hal_in8(CH[ch].io + 7);
    if (status == 0xFF || status == 0x00) return false; // 완전 없음

    // 먼저 ATA IDENTIFY 시도
    ata_type_t t = ata_identify_try(drive, 0xEC);
    if (t == ATA_ATA) return true;

    // ATA 실패 시, ATAPI 시그니처 검사 (LBAmid=0x14, LBAhigh=0xEB)
    // 또는 일부 구현에선 IDENTIFY(0xEC) 후 ERR로 떨어지면서 이 시그니처가 남음
    hal_out8(CH[ch].io + 6, 0xA0 | (sl << 4));
    ata_400ns(ch);
    uint8_t lba_mid  = hal_in8(CH[ch].io + 4);
    uint8_t lba_high = hal_in8(CH[ch].io + 5);

    kprintf("Drive %d ATAPI check: LBAmid=%X, LBAhigh=%X\n", drive, lba_mid, lba_high);

    if (lba_mid == 0x14 && lba_high == 0xEB) {
        // ATAPI IDENTIFY PACKET
        t = ata_identify_try(drive, 0xA1);
        if (t == ATA_ATAPI) return true;
    }

    // 그래도 실패면 없음
    return false;
}

// ───────────────────────────────
// 초기화: 드라이브 스캔
void ata_init_all(void) {
    for (int d = 0; d < 4; d++) {
        if (ata_present(d)) {
            ata_available[d] = true;
            disks[d].present = true;
            disks[d].id = d;
            strcpy(disks[d].fs_type, "Unknown");  // ← 문자열 복사
            disks[d].base_lba = 0;                // 기본값 초기화
            kprintf("ATA drive %d detected.\n", d);
        } else {
            ata_available[d] = false;
            disks[d].present = false;
            strcpy(disks[d].fs_type, "None");
        }
    }
}

uint32_t ata_get_sector_count(uint8_t drive) {
    if (ramdisk_present(drive))
        return ramdisk_get_sector_count(drive);
    if (drive >= 4) return usb_storage_get_sector_count(drive);
    if (drive > 3 || !ata_available[drive]) return 0;

    uint8_t ch = drive >> 1;
    uint8_t sl = drive & 1;
    uint16_t id_data[256];

    // 드라이브 선택
    hal_out8(CH[ch].io + 6, 0xA0 | (sl << 4));
    ata_400ns(ch);

    // IDENTIFY 명령 전송
    hal_out8(CH[ch].io + 7, ATA_CMD_IDENTIFY);
    if (wait_not_bsy(ch, 100000)) return 0;
    if (wait_drq(ch, 100000)) return 0;

    // 512바이트 읽기
    for (int i = 0; i < 256; i++)
        id_data[i] = hal_in16(CH[ch].io + 0);

    // LBA28 섹터 수는 word[60] + word[61] << 16
    uint32_t total_sectors = ((uint32_t)id_data[61] << 16) | id_data[60];

    return total_sectors;
}
