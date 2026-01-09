#ifndef PCI_H
#define PCI_H

#include <stdint.h>

// ────────────────
// PCI I/O 포트
// ────────────────
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

// ────────────────
// PCI 장치 식별
// ────────────────
typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision_id;
    uint32_t bar[6];  // Base Address Registers
} pci_device_t;

// ────────────────
// ATA 채널 구조체
// ────────────────
typedef struct {
    uint16_t io;    // DATA=io+0, STATUS/CMD=io+7
    uint16_t ctrl;  // ALTSTATUS/DEVCTL
} ata_chan_t;

// 전역 ATA 채널
extern ata_chan_t CH[2];

// ────────────────
// 함수 선언
// ────────────────
uint32_t pci_read_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
void pci_write_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value);

// IDE 컨트롤러 찾아서 CH[] 세팅
void pci_scan_all_devices(void);

#endif
