#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct usb_hc usb_hc_t;

#ifndef EHCI_CTRL_TIMEOUT_MS
#define EHCI_CTRL_TIMEOUT_MS 1000u
#endif
#ifndef EHCI_BULK_TIMEOUT_MS
#define EHCI_BULK_TIMEOUT_MS 1000u
#endif

typedef struct ehci_qtd {
    uint32_t next;
    uint32_t alt_next;
    uint32_t token;
    uint32_t buf[5];
    uint32_t buf_hi[5];
} __attribute__((packed, aligned(32))) ehci_qtd_t;

typedef struct ehci_qh {
    uint32_t hlp;
    uint32_t ep_char;
    uint32_t ep_cap;
    uint32_t current_qtd;
    uint32_t next_qtd;
    uint32_t alt_next_qtd;
    uint32_t token;
    uint32_t buf[5];
    uint32_t buf_hi[5];
} __attribute__((packed, aligned(32))) ehci_qh_t;

typedef struct ehci_ctrl {
    uint32_t base;
    volatile uint32_t* cap_regs;
    volatile uint32_t* op_regs;
    uint8_t cap_len;
    uint8_t irq_line;
    uint8_t next_addr;
    usb_hc_t* usbhc;

    ehci_qh_t* async_head;
    uint32_t* periodic_list;
    ehci_qh_t* periodic_head;
    ehci_qh_t* ctrl_qh;
    ehci_qtd_t* ctrl_qtd_setup;
    ehci_qtd_t* ctrl_qtd_data;
    ehci_qtd_t* ctrl_qtd_status;

    ehci_qh_t* bulk_in_qh;
    ehci_qh_t* bulk_out_qh;
    ehci_qtd_t* bulk_in_qtd;
    ehci_qtd_t* bulk_out_qtd;
} ehci_ctrl_t;

typedef enum {
    EHCI_SPEED_FULL = 0,
    EHCI_SPEED_LOW  = 1,
    EHCI_SPEED_HIGH = 2,
} ehci_speed_t;

typedef struct {
    ehci_qh_t* qh;
    ehci_qtd_t* qtd;
    void* buf;
    uint16_t len;
    uint8_t toggle;
} ehci_async_in_t;

void ehci_pci_attach(uint8_t bus, uint8_t dev, uint8_t func,
                     uint32_t mmio_base, uint8_t irq_line);

void ehci_rescan_all_ports(bool reset_addr_allocator);
void ehci_poll_changes(void);
bool ehci_take_rescan_pending(void);

bool ehci_async_in_init(ehci_ctrl_t* hc, ehci_async_in_t* x,
                        uint8_t addr, uint8_t ep, uint16_t mps,
                        ehci_speed_t speed, uint8_t tt_hub_addr, uint8_t tt_port,
                        uint8_t start_toggle, void* buf, uint16_t len);

// Returns: 0=no completion yet, 1=completed OK (out_actual valid), -1=halted/error.
int ehci_async_in_check(ehci_async_in_t* x, uint16_t* out_actual);
void ehci_async_in_rearm(ehci_async_in_t* x);

bool ehci_control_transfer(ehci_ctrl_t* hc, uint8_t addr, uint8_t ep,
                           uint16_t mps, ehci_speed_t speed,
                           uint8_t tt_hub_addr, uint8_t tt_port,
                           const void* setup8, void* data, uint16_t len);

bool ehci_bulk_transfer(ehci_ctrl_t* hc, uint8_t addr, uint8_t ep, bool in,
                        uint16_t mps, ehci_speed_t speed,
                        uint8_t tt_hub_addr, uint8_t tt_port,
                        uint8_t start_toggle,
                        void* data, uint16_t len);
