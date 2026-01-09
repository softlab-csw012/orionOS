#pragma once
#include <stdint.h>
#include <stdbool.h>

void xhci_pci_attach(uint8_t bus, uint8_t dev, uint8_t func,
                     uint32_t mmio_base, uint8_t irq_line);

void xhci_rescan_all_ports(bool reset_addr_allocator, bool verbose);
void xhci_poll_changes(void);
bool xhci_take_rescan_pending(void);
