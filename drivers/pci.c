#include "pci.h"
#include "hal.h"   // hal_in32/out 필요
#include "../drivers/screen.h" // kprintf()
#include "ac97.h"
#include "hda.h"
#include "usb/ehci.h"
#include "usb/ohci.h"
#include "usb/uhci.h"
#include "usb/xhci.h"
#include <stdbool.h>

// ────────────────
// PCI 설정 공간 읽기
// ────────────────
uint32_t pci_read_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t address = (1U << 31)                    // enable bit
                     | ((uint32_t)bus << 16)
                     | ((uint32_t)device << 11)
                     | ((uint32_t)function << 8)
                     | (offset & 0xFC);              // 하위 2비트는 0
    hal_out32(PCI_CONFIG_ADDRESS, address);
    return hal_in32(PCI_CONFIG_DATA);
}

void pci_write_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value) {
    uint32_t address = (1U << 31)                    // Enable bit
                     | ((uint32_t)bus << 16)
                     | ((uint32_t)device << 11)
                     | ((uint32_t)function << 8)
                     | (offset & 0xFC);              // DWORD align

    hal_out32(PCI_CONFIG_ADDRESS, address);
    hal_out32(PCI_CONFIG_DATA, value);
}

// ────────────────
// PCI 장치 탐색
// ────────────────
void pci_scan_all_devices(void) {
    bool ide_channels_set = false;
    enum { AC97_CANDIDATE_MAX = 8 };
    struct ac97_candidate {
        uint8_t bus;
        uint8_t dev;
        uint8_t func;
    } ac97_candidates[AC97_CANDIDATE_MAX];
    int ac97_candidate_count = 0;

    const uint8_t forced_hda_bus = 2;
    const uint8_t forced_hda_dev = 3;
    const uint8_t forced_hda_func = 0;

    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {

                uint32_t vendor_device = pci_read_dword(bus, dev, func, 0x00);
                if (vendor_device == 0xFFFFFFFF)
                    continue;

                uint16_t vendor_id = vendor_device & 0xFFFF;
                uint16_t device_id = (vendor_device >> 16) & 0xFFFF;

                uint32_t class_reg = pci_read_dword(bus, dev, func, 0x08);
                uint8_t class_code = (class_reg >> 24) & 0xFF;
                uint8_t subclass   = (class_reg >> 16) & 0xFF;
                uint8_t prog_if    = (class_reg >> 8) & 0xFF;
                uint8_t revision   = class_reg & 0xFF;

                uint32_t header_type_reg = pci_read_dword(bus, dev, func, 0x0C);
                uint8_t header_type = (header_type_reg >> 16) & 0x7F;

                kprintf("[PCI] bus=%d dev=%d func=%d\n", bus, dev, func);
                kprintf("       Vendor: %04X  Device: %04X\n", vendor_id, device_id);
                kprintf("       Class: %02X  Subclass: %02X  ProgIF: %02X  Rev: %02X\n",
                        class_code, subclass, prog_if, revision);
                kprintf("       Header: %02X\n", header_type);

                bool attached_hda = false;
                if ((uint8_t)bus == forced_hda_bus &&
                    (uint8_t)dev == forced_hda_dev &&
                    (uint8_t)func == forced_hda_func) {
                    kprintf("       [HDA Audio Controller Forced]\n");
                    attached_hda = hda_pci_attach_force((uint8_t)bus, (uint8_t)dev, (uint8_t)func);
                }

                // ===============================
                //  IDE Controller 식별
                // ===============================
                if (class_code == 0x01 && subclass == 0x01) {
                    if (ide_channels_set) {
                        kprintf("       [IDE Controller Detected] (skipping: CH[] already set)\n");
                        continue;
                    }
                    uint32_t bar0 = pci_read_dword(bus, dev, func, 0x10) & ~0x3;
                    uint32_t bar1 = pci_read_dword(bus, dev, func, 0x14) & ~0x3;
                    uint32_t bar2 = pci_read_dword(bus, dev, func, 0x18) & ~0x3;
                    uint32_t bar3 = pci_read_dword(bus, dev, func, 0x1C) & ~0x3;

                    if (!bar0) bar0 = 0x1F0;
                    if (!bar1) bar1 = 0x3F4;
                    if (!bar2) bar2 = 0x170;
                    if (!bar3) bar3 = 0x374;

                    CH[0].io   = (uint16_t)bar0;
                    CH[0].ctrl = (uint16_t)(bar1 + 2);
                    CH[1].io   = (uint16_t)bar2;
                    CH[1].ctrl = (uint16_t)(bar3 + 2);

                    kprintf("       [IDE Controller Detected]\n");
                    kprintf("       CH0: io=%X ctrl=%X\n", CH[0].io, CH[0].ctrl);
                    kprintf("       CH1: io=%X ctrl=%X\n", CH[1].io, CH[1].ctrl);
                    ide_channels_set = true;
                }

                // ===========================================
                //  AC'97 Audio Controller 감지!
                // ===========================================
                if (!attached_hda && class_code == 0x04 && subclass == 0x01 && prog_if == 0x00) {
                    kprintf("       [AC'97 Audio Controller Found] (deferred)\n");
                    if (ac97_candidate_count < AC97_CANDIDATE_MAX) {
                        ac97_candidates[ac97_candidate_count].bus = (uint8_t)bus;
                        ac97_candidates[ac97_candidate_count].dev = (uint8_t)dev;
                        ac97_candidates[ac97_candidate_count].func = (uint8_t)func;
                        ac97_candidate_count++;
                    }
                }

                // ===========================================
                //  Intel HD Audio (HDA) Controller 감지!
                // ===========================================
                if (!attached_hda && class_code == 0x04 && subclass == 0x03) {
                    kprintf("       [HDA Audio Controller Found]\n");
                    hda_pci_attach((uint8_t)bus, (uint8_t)dev, (uint8_t)func);
                    attached_hda = true;
                }

                // ===========================================
                //  EHCI Controller (USB 2.0) 감지!
                // ===========================================
                if (class_code == 0x0C && subclass == 0x03 && prog_if == 0x20) {
                    kprintf("       [EHCI Controller Found] USB 2.0 EHCI Controller!\n");

                    uint32_t bar0 = pci_read_dword(bus, dev, func, 0x10);
                    if (bar0 & 0x1u) {
                        kprintf("       EHCI BAR0 is I/O space? (%08X) skipping\n", bar0);
                        continue;
                    }
                    uint32_t mmio_base = bar0 & ~0xFu;
                    if (mmio_base == 0) {
                        kprint("       EHCI BAR0 MMIO base is 0, skipping attach\n");
                        continue;
                    }

                    // Enable MMIO + Bus mastering
                    uint32_t cmdsts = pci_read_dword(bus, dev, func, 0x04);
                    cmdsts |= (1u << 1) | (1u << 2);
                    pci_write_dword(bus, dev, func, 0x04, cmdsts);

                    uint32_t irq_reg = pci_read_dword(bus, dev, func, 0x3C);
                    uint8_t irq_line = irq_reg & 0xFF;

                    kprintf("       BAR0 MMIO Base = %08X, IRQ=%d\n", mmio_base, irq_line);
                    ehci_pci_attach((uint8_t)bus, (uint8_t)dev, (uint8_t)func, mmio_base, irq_line);
                }

                // ===========================================
                //  UHCI Controller (USB 1.1) 감지!
                // ===========================================
                if (class_code == 0x0C && subclass == 0x03 && prog_if == 0x00) {
                    kprintf("       [UHCI Controller Found] USB 1.1 UHCI Controller!\n");

                    uint32_t bar = pci_read_dword(bus, dev, func, 0x20); // BAR4 (common for Intel UHCI)
                    if ((bar & 0x1u) == 0 || (bar & ~0x1Fu) == 0) {
                        bar = pci_read_dword(bus, dev, func, 0x10); // fallback BAR0
                    }

                    if ((bar & 0x1u) == 0) {
                        kprintf("       UHCI BAR is not I/O space? (%08X) skipping\n", bar);
                    } else {
                        uint16_t io_base = (uint16_t)(bar & ~0x1Fu);

                        // Enable I/O + Bus mastering
                        uint32_t cmdsts = pci_read_dword(bus, dev, func, 0x04);
                        cmdsts |= (1u << 0) | (1u << 2);
                        pci_write_dword(bus, dev, func, 0x04, cmdsts);

                        uint32_t irq_reg = pci_read_dword(bus, dev, func, 0x3C);
                        uint8_t irq_line = irq_reg & 0xFF;

                        kprintf("       UHCI IO Base = %04X, IRQ=%d\n", io_base, irq_line);
                        uhci_pci_attach((uint8_t)bus, (uint8_t)dev, (uint8_t)func, io_base, irq_line);
                    }
                }

                // ===========================================
                //  OHCI Controller (USB 1.1) 감지!
                // ===========================================
                if (class_code == 0x0C && subclass == 0x03 && prog_if == 0x10) {
                    kprintf("       [OHCI Controller Found] USB 1.1 OHCI Controller!\n");

                    uint32_t bar0 = pci_read_dword(bus, dev, func, 0x10);
                    if (bar0 & 0x1u) {
                        kprintf("       OHCI BAR0 is I/O space? (%08X) skipping\n", bar0);
                        continue;
                    }

                    uint32_t mmio_base_lo = bar0 & ~0xFu;
                    uint8_t mem_type = (uint8_t)((bar0 >> 1) & 0x3u);
                    if (mem_type == 0x2u) {
                        uint32_t bar1 = pci_read_dword(bus, dev, func, 0x14);
                        if (bar1 != 0) {
                            kprintf("       OHCI BAR0 is above 4GiB (BAR1=%08X), skipping\n", bar1);
                            continue;
                        }
                    }

                    if (mmio_base_lo == 0) {
                        kprint("       OHCI BAR0 MMIO base is 0, skipping attach\n");
                        continue;
                    }

                    // Enable MMIO + Bus mastering
                    uint32_t cmdsts = pci_read_dword(bus, dev, func, 0x04);
                    cmdsts |= (1u << 1) | (1u << 2);
                    pci_write_dword(bus, dev, func, 0x04, cmdsts);

                    uint32_t irq_reg = pci_read_dword(bus, dev, func, 0x3C);
                    uint8_t irq_line = irq_reg & 0xFF;

                    kprintf("       BAR0 MMIO Base = %08X, IRQ=%d\n", mmio_base_lo, irq_line);
                    ohci_pci_attach(mmio_base_lo, irq_line);
                }

                // ===========================================
                //  xHCI Controller (USB 3.x) 감지!
                // ===========================================
                if (class_code == 0x0C && subclass == 0x03 && prog_if == 0x30) {
                    kprintf("       [xHCI Controller Found] USB 3.x xHCI Controller!\n");

                    uint32_t bar0 = pci_read_dword(bus, dev, func, 0x10);
                    if (bar0 & 0x1u) {
                        kprintf("       xHCI BAR0 is I/O space? (%08X) skipping\n", bar0);
                        continue;
                    }

                    uint32_t mmio_base_lo = bar0 & ~0xFu;
                    uint8_t mem_type = (uint8_t)((bar0 >> 1) & 0x3u);
                    if (mem_type == 0x2u) {
                        uint32_t bar1 = pci_read_dword(bus, dev, func, 0x14);
                        if (bar1 != 0) {
                            kprintf("       xHCI BAR0 is above 4GiB (BAR1=%08X), skipping\n", bar1);
                            continue;
                        }
                    }

                    if (mmio_base_lo == 0) {
                        kprint("       xHCI BAR0 MMIO base is 0, skipping attach\n");
                        continue;
                    }

                    // Enable MMIO + Bus mastering
                    uint32_t cmdsts = pci_read_dword(bus, dev, func, 0x04);
                    cmdsts |= (1u << 1) | (1u << 2);
                    pci_write_dword(bus, dev, func, 0x04, cmdsts);

                    uint32_t irq_reg = pci_read_dword(bus, dev, func, 0x3C);
                    uint8_t irq_line = irq_reg & 0xFF;

                    kprintf("       BAR0 MMIO Base = %08X, IRQ=%d\n", mmio_base_lo, irq_line);
                    xhci_pci_attach((uint8_t)bus, (uint8_t)dev, (uint8_t)func, mmio_base_lo, irq_line);
                }

                kprintf("\n");
            }
        }
    }
    if (!hda_is_present() && ac97_candidate_count > 0) {
        kprint("[PCI] No HDA controller attached; falling back to AC'97\n");
        for (int i = 0; i < ac97_candidate_count; i++) {
            ac97_pci_attach(ac97_candidates[i].bus,
                            ac97_candidates[i].dev,
                            ac97_candidates[i].func);
        }
    }
    kprintf("PCI scan complete.\n");
}

// Never gonna give you up 🎶
