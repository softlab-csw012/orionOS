#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct usb_hc usb_hc_t;
typedef struct ohci_async_in ohci_async_in_t;

typedef struct ohci_hcca {
    uint32_t interrupt_table[32];
    uint16_t frame_number;
    uint16_t pad1;
    uint32_t done_head;
    uint8_t reserved[116];
} __attribute__((packed, aligned(256))) ohci_hcca_t;

typedef struct ohci_ed {
    uint32_t flags;
    uint32_t tail_td;
    uint32_t head_td;
    uint32_t next_ed;
} __attribute__((packed, aligned(16))) ohci_ed_t;

typedef struct ohci_td {
    uint32_t flags;
    uint32_t cbp;
    uint32_t next_td;
    uint32_t be;
} __attribute__((packed, aligned(16))) ohci_td_t;

typedef struct ohci_ctrl {
    uint32_t base;
    volatile uint32_t* regs;
    uint8_t irq_line;
    uint8_t next_addr;
    usb_hc_t* usbhc;
    ohci_hcca_t* hcca;

    ohci_ed_t* ctrl_ed;
    ohci_td_t* ctrl_td_setup;
    ohci_td_t* ctrl_td_data;
    ohci_td_t* ctrl_td_status;
    ohci_td_t* ctrl_td_tail;

    ohci_ed_t* bulk_in_ed;
    ohci_ed_t* bulk_out_ed;
    ohci_td_t* bulk_in_td;
    ohci_td_t* bulk_in_tail;
    ohci_td_t* bulk_out_td;
    ohci_td_t* bulk_out_tail;

    ohci_async_in_t* async_list;
} ohci_ctrl_t;

void ohci_pci_attach(uint32_t mmio_base, uint8_t irq_line);
void ohci_rescan_all_ports(bool reset_addr_allocator);
void ohci_poll_changes(void);
bool ohci_take_rescan_pending(void);

bool ohci_control_transfer(ohci_ctrl_t* hc, uint8_t addr, uint8_t ep,
                           uint16_t mps, bool low_speed,
                           const void* setup8, void* data, uint16_t len);

bool ohci_bulk_transfer(ohci_ctrl_t* hc, uint8_t addr, uint8_t ep, bool in,
                        uint16_t mps, bool low_speed, uint8_t start_toggle,
                        void* data, uint16_t len);
