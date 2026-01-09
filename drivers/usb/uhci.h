#pragma once
#include <stdint.h>
#include <stdbool.h>

void uhci_pci_attach(uint8_t bus, uint8_t dev, uint8_t func,
                     uint16_t io_base, uint8_t irq_line);

void uhci_rescan_all_ports(void);
void uhci_poll_changes(void);
bool uhci_take_rescan_pending(void);
void uhci_poll(void);
