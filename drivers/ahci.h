#ifndef DRIVERS_AHCI_H
#define DRIVERS_AHCI_H

#include <stdint.h>
#include <stdbool.h>

void ahci_pci_attach(uint8_t bus, uint8_t dev, uint8_t func,
                     uint32_t mmio_base, uint8_t irq_line);
bool ahci_is_present(void);
uint32_t ahci_sata_port_count(void);
bool ahci_identify(uint16_t* out_id);
bool ahci_read(uint64_t lba, uint16_t count, void* buf);
bool ahci_write(uint64_t lba, uint16_t count, const void* buf);
bool ahci_identify_port(uint32_t port_index, uint16_t* out_id);
bool ahci_read_port(uint32_t port_index, uint64_t lba, uint16_t count, void* buf);
bool ahci_write_port(uint32_t port_index, uint64_t lba, uint16_t count, const void* buf);

#endif
