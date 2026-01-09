#include "ohci.h"
#include "usb.h"
#include "../hal.h"
#include "../screen.h"
#include "../../mm/mem.h"
#include "../../mm/paging.h"
#include "../../cpu/timer.h"
#include "../../libc/string.h"
#include "../../kernel/proc/workqueue.h"
#include "../../kernel/log.h"

#define OHCI_MAX_CONTROLLERS 2

static ohci_ctrl_t controllers[OHCI_MAX_CONTROLLERS];
static usb_hc_t usbhc_wrappers[OHCI_MAX_CONTROLLERS];
static int controller_count = 0;
static volatile bool ohci_rescan_pending = false;
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

static void ohci_rescan_work(void* ctx) {
    (void)ctx;
    if (ohci_take_rescan_pending()) {
        ohci_rescan_all_ports(true);
    }
}

static void ohci_queue_rescan(void) {
    bool enqueue = false;
    uint32_t flags = irq_save();
    if (!ohci_rescan_pending) {
        ohci_rescan_pending = true;
        enqueue = true;
    }
    irq_restore(flags);

    if (enqueue) {
        (void)workqueue_enqueue(ohci_rescan_work, NULL);
    }
}

// Register offsets
#define HC_REVISION          0x00
#define HC_CONTROL           0x04
#define HC_COMMAND_STATUS    0x08
#define HC_INT_STATUS        0x0C
#define HC_INT_ENABLE        0x10
#define HC_INT_DISABLE       0x14
#define HC_HCCA              0x18
#define HC_CONTROL_HEAD_ED   0x20
#define HC_BULK_HEAD_ED      0x28
#define HC_DONE_HEAD         0x30
#define HC_FM_INTERVAL       0x34
#define HC_PERIODIC_START    0x40
#define HC_LS_THRESHOLD      0x44
#define HC_RH_DESC_A         0x48
#define HC_RH_STATUS         0x50
#define HC_RH_PORT_STATUS(n) (0x54 + ((n) * 4))

// HcControl bits
#define CTRL_PLE   (1u << 2)
#define CTRL_CLE   (1u << 4)
#define CTRL_BLE   (1u << 5)
#define CTRL_HCFS_MASK (3u << 6)
#define CTRL_HCFS_OPERATIONAL (2u << 6)

// HcCommandStatus bits
#define CMD_HCR   (1u << 0)
#define CMD_CLF   (1u << 1)
#define CMD_BLF   (1u << 2)

// Root hub port bits
#define RHPS_CCS   (1u << 0)
#define RHPS_PES   (1u << 1)
#define RHPS_PRS   (1u << 4)
#define RHPS_PPS   (1u << 8)
#define RHPS_LSDA  (1u << 9)
#define RHPS_CSC   (1u << 16)
#define RHPS_PRSC  (1u << 20)

// ED flags
#define ED_FA_SHIFT 0
#define ED_EN_SHIFT 7
#define ED_D_SHIFT  11
#define ED_S_SHIFT  13
#define ED_K_SHIFT  14
#define ED_F_SHIFT  15
#define ED_MPS_SHIFT 16

#define ED_D_FROM_TD (0u << ED_D_SHIFT)
#define ED_D_OUT     (1u << ED_D_SHIFT)
#define ED_D_IN      (2u << ED_D_SHIFT)

// TD flags
#define TD_R        (1u << 18)
#define TD_DP_SHIFT 19
#define TD_T_SHIFT  24
#define TD_CC_SHIFT 28
#define TD_CC_MASK  (0xFu << TD_CC_SHIFT)
#define TD_CC_NOACCESS (0xFu << TD_CC_SHIFT)

#define TD_DP_SETUP (0u << TD_DP_SHIFT)
#define TD_DP_OUT   (1u << TD_DP_SHIFT)
#define TD_DP_IN    (2u << TD_DP_SHIFT)

#define TD_T_DATA0  (1u << TD_T_SHIFT)
#define TD_T_DATA1  (2u << TD_T_SHIFT)

struct ohci_async_in {
    struct ohci_async_in* next;
    ohci_ctrl_t* hc;

    ohci_ed_t* ed;
    ohci_td_t* td;
    ohci_td_t* tail;

    uint8_t addr;
    uint8_t ep;
    uint16_t mps;
    bool low_speed;
    uint8_t toggle;

    void* buf;
    uint32_t buf_phys;
    uint16_t len;
};

static inline uint32_t phys_addr(void* p) {
    uint32_t phys;
    if (vmm_virt_to_phys((uint32_t)p, &phys) == 0)
        return phys;
    return (uint32_t)p;
}

static inline uint32_t rd_reg(ohci_ctrl_t* hc, uint32_t off) {
    return hc->regs[off / 4];
}

static inline void wr_reg(ohci_ctrl_t* hc, uint32_t off, uint32_t v) {
    hc->regs[off / 4] = v;
}

static void invlpg(uint32_t addr) {
    hal_invlpg((const void*)(uintptr_t)addr);
}

static void map_mmio(uint32_t base) {
    uint32_t start = base & ~0xFFFu;
    for (uint32_t addr = start; addr < start + 0x1000u; addr += 0x1000u) {
        map_page(page_directory, addr, addr, PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT);
        invlpg(addr);
    }
}

static void delay_ticks(uint32_t ticks) {
    uint32_t start = tick;
    while ((tick - start) < ticks) {
        hal_wait_for_interrupt();
    }
}

static void delay_ms(uint32_t ms) {
    uint32_t ticks_needed = (ms + 9) / 10; // PIT 100Hz => 10ms per tick
    if (ticks_needed == 0) ticks_needed = 1;
    delay_ticks(ticks_needed);
}

static bool wait_td_done(ohci_td_t* td, uint32_t timeout_ms) {
    uint32_t start = tick;
    uint32_t timeout_ticks = (timeout_ms + 9) / 10;
    if (timeout_ticks == 0) timeout_ticks = 1;
    while (((td->flags & TD_CC_MASK) == TD_CC_NOACCESS)) {
        if ((tick - start) > timeout_ticks) return false;
        hal_wait_for_interrupt();
    }
    return ((td->flags >> TD_CC_SHIFT) & 0xF) == 0;
}

static void init_td(ohci_td_t* td, uint32_t dp_flags, uint32_t toggle_flags,
                    void* buf, uint32_t len, bool rounding) {
    td->flags = TD_CC_NOACCESS | (rounding ? TD_R : 0) | dp_flags | toggle_flags;
    if (len == 0 || buf == NULL) {
        td->cbp = 0;
        td->be = 0;
    } else {
        td->cbp = phys_addr(buf);
        td->be = phys_addr((uint8_t*)buf + len - 1);
    }
    td->next_td = 0;
}

static void init_ed(ohci_ed_t* ed, uint8_t addr, uint8_t ep, uint16_t mps,
                    bool low_speed, uint32_t dir_flags) {
    ed->flags = ((uint32_t)addr << ED_FA_SHIFT) |
                ((uint32_t)ep << ED_EN_SHIFT) |
                dir_flags |
                (low_speed ? (1u << ED_S_SHIFT) : 0) |
                (0u << ED_K_SHIFT) |
                (0u << ED_F_SHIFT) |
                ((uint32_t)mps << ED_MPS_SHIFT);
    ed->next_ed = 0;
}

static void ohci_update_interrupt_table(ohci_ctrl_t* hc) {
    if (!hc || !hc->hcca) return;

    uint32_t head_phys = 0;
    ohci_async_in_t* a = hc->async_list;
    while (a) {
        ohci_async_in_t* next = a->next;
        a->ed->next_ed = next ? phys_addr(next->ed) : 0;
        a = next;
    }
    if (hc->async_list) head_phys = phys_addr(hc->async_list->ed);

    for (int i = 0; i < 32; i++) {
        hc->hcca->interrupt_table[i] = head_phys;
    }
}

static void ohci_async_prep_in_td(ohci_async_in_t* a) {
    if (!a || !a->td) return;
    uint32_t toggle = a->toggle ? TD_T_DATA1 : TD_T_DATA0;
    a->td->flags = TD_CC_NOACCESS | TD_R | TD_DP_IN | toggle;
    if (a->len == 0 || !a->buf) {
        a->td->cbp = 0;
        a->td->be = 0;
    } else {
        a->td->cbp = a->buf_phys;
        a->td->be = a->buf_phys + (uint32_t)a->len - 1u;
    }
    a->td->next_td = a->tail ? phys_addr(a->tail) : 0;
}

static uint16_t ohci_async_actual_len(ohci_async_in_t* a) {
    if (!a || !a->td) return 0;
    if (a->len == 0) return 0;
    uint32_t cbp = a->td->cbp;
    if (cbp == 0) return a->len;
    if (cbp <= a->buf_phys) return 0;
    uint32_t diff = cbp - a->buf_phys;
    if (diff > a->len) diff = a->len;
    return (uint16_t)diff;
}

static bool ohci_reset_controller(ohci_ctrl_t* hc) {
    uint32_t rev = rd_reg(hc, HC_REVISION);
    kprintf("[OHCI] Revision %x\n", rev & 0xFF);

    wr_reg(hc, HC_INT_DISABLE, 0xFFFFFFFFu);
    wr_reg(hc, HC_INT_STATUS,  0xFFFFFFFFu);

    // Host controller reset
    wr_reg(hc, HC_COMMAND_STATUS, CMD_HCR);
    for (int i = 0; i < 1000; i++) {
        if (!(rd_reg(hc, HC_COMMAND_STATUS) & CMD_HCR)) break;
        delay_ms(1);
    }
    if (rd_reg(hc, HC_COMMAND_STATUS) & CMD_HCR) {
        kprint("[OHCI] HCR timeout\n");
        return false;
    }

    // Restore timing registers (use current hardware defaults)
    uint32_t fm = rd_reg(hc, HC_FM_INTERVAL);
    wr_reg(hc, HC_FM_INTERVAL, fm);
    uint32_t fi = fm & 0x3FFFu;
    wr_reg(hc, HC_PERIODIC_START, (fi * 9) / 10);
    wr_reg(hc, HC_LS_THRESHOLD, 0x0628);

    // Program HCCA and lists
    wr_reg(hc, HC_HCCA, phys_addr(hc->hcca));
    wr_reg(hc, HC_CONTROL_HEAD_ED, 0);
    wr_reg(hc, HC_BULK_HEAD_ED, 0);

    // Operational with control/bulk enabled
    uint32_t ctrl = rd_reg(hc, HC_CONTROL);
    ctrl &= ~CTRL_HCFS_MASK;
    ctrl |= CTRL_PLE | CTRL_CLE | CTRL_BLE | CTRL_HCFS_OPERATIONAL;
    wr_reg(hc, HC_CONTROL, ctrl);

    return true;
}

static bool ohci_reset_port(ohci_ctrl_t* hc, int port, bool* low_speed_out) {
    uint32_t ps = rd_reg(hc, HC_RH_PORT_STATUS(port));
    if (!(ps & RHPS_CCS)) return false;

    // Power on port
    wr_reg(hc, HC_RH_PORT_STATUS(port), RHPS_PPS);
    delay_ms(20);

    // Reset port
    wr_reg(hc, HC_RH_PORT_STATUS(port), RHPS_PRS);
    delay_ms(60);

    // Wait for reset complete
    for (int i = 0; i < 200; i++) {
        ps = rd_reg(hc, HC_RH_PORT_STATUS(port));
        if (!(ps & RHPS_PRS)) break;
        delay_ms(1);
    }

    // Clear change bits
    wr_reg(hc, HC_RH_PORT_STATUS(port), RHPS_CSC | RHPS_PRSC);

    // Enable port
    wr_reg(hc, HC_RH_PORT_STATUS(port), RHPS_PES);
    delay_ms(10);

    ps = rd_reg(hc, HC_RH_PORT_STATUS(port));
    if (low_speed_out) *low_speed_out = (ps & RHPS_LSDA) != 0;
    return (ps & RHPS_CCS) != 0;
}

bool ohci_control_transfer(ohci_ctrl_t* hc, uint8_t addr, uint8_t ep,
                           uint16_t mps, bool low_speed,
                           const void* setup8, void* data, uint16_t len) {
    init_ed(hc->ctrl_ed, addr, ep, mps, low_speed, ED_D_FROM_TD);

    // Tail TD (dummy)
    init_td(hc->ctrl_td_tail, TD_DP_OUT, TD_T_DATA0, NULL, 0, false);

    // Setup stage
    init_td(hc->ctrl_td_setup, TD_DP_SETUP, TD_T_DATA0, (void*)setup8, 8, false);

    // Data stage (optional)
    if (len > 0 && data != NULL) {
        bool in = (((const uint8_t*)setup8)[0] & 0x80) != 0;
        init_td(hc->ctrl_td_data, in ? TD_DP_IN : TD_DP_OUT, TD_T_DATA1, data, len, in);
        hc->ctrl_td_setup->next_td = phys_addr(hc->ctrl_td_data);
        hc->ctrl_td_data->next_td = phys_addr(hc->ctrl_td_status);
    } else {
        hc->ctrl_td_setup->next_td = phys_addr(hc->ctrl_td_status);
    }

    // Status stage
    bool status_in = (len > 0 && data != NULL) ? ((((const uint8_t*)setup8)[0] & 0x80) == 0) : true;
    init_td(hc->ctrl_td_status, status_in ? TD_DP_IN : TD_DP_OUT, TD_T_DATA1, NULL, 0, false);
    hc->ctrl_td_status->next_td = phys_addr(hc->ctrl_td_tail);

    hc->ctrl_ed->head_td = phys_addr(hc->ctrl_td_setup);
    hc->ctrl_ed->tail_td = phys_addr(hc->ctrl_td_tail);

    wr_reg(hc, HC_CONTROL_HEAD_ED, phys_addr(hc->ctrl_ed));
    wr_reg(hc, HC_COMMAND_STATUS, CMD_CLF);

    bool ok = wait_td_done(hc->ctrl_td_status, 2000);

    wr_reg(hc, HC_CONTROL_HEAD_ED, 0);
    return ok;
}

bool ohci_bulk_transfer(ohci_ctrl_t* hc, uint8_t addr, uint8_t ep, bool in,
                        uint16_t mps, bool low_speed, uint8_t start_toggle,
                        void* data, uint16_t len) {
    ohci_ed_t* ed = in ? hc->bulk_in_ed : hc->bulk_out_ed;
    ohci_td_t* td = in ? hc->bulk_in_td : hc->bulk_out_td;
    ohci_td_t* tail = in ? hc->bulk_in_tail : hc->bulk_out_tail;

    init_ed(ed, addr, ep, mps, low_speed, in ? ED_D_IN : ED_D_OUT);
    init_td(tail, TD_DP_OUT, TD_T_DATA0, NULL, 0, false);

    uint32_t dp = in ? TD_DP_IN : TD_DP_OUT;
    uint32_t toggle = (start_toggle == 0) ? TD_T_DATA0 : TD_T_DATA1;
    init_td(td, dp, toggle, data, len, in);
    td->next_td = phys_addr(tail);

    ed->head_td = phys_addr(td);
    ed->tail_td = phys_addr(tail);

    wr_reg(hc, HC_BULK_HEAD_ED, phys_addr(ed));
    wr_reg(hc, HC_COMMAND_STATUS, CMD_BLF);

    bool ok = wait_td_done(td, 2000);

    wr_reg(hc, HC_BULK_HEAD_ED, 0);
    return ok;
}

static bool ohci_usbhc_control_transfer(usb_hc_t* hc, uint32_t dev, uint8_t ep,
                                       uint16_t mps, usb_speed_t speed,
                                       uint8_t tt_hub_addr, uint8_t tt_port,
                                       const void* setup8, void* data, uint16_t len) {
    (void)tt_hub_addr;
    (void)tt_port;
    if (!hc || !hc->impl) return false;
    return ohci_control_transfer((ohci_ctrl_t*)hc->impl, (uint8_t)dev, ep, mps,
                                 speed == USB_SPEED_LOW, setup8, data, len);
}

static bool ohci_usbhc_bulk_transfer(usb_hc_t* hc, uint32_t dev, uint8_t ep, bool in,
                                     uint16_t mps, usb_speed_t speed,
                                     uint8_t tt_hub_addr, uint8_t tt_port,
                                     uint8_t start_toggle,
                                     void* data, uint16_t len) {
    (void)tt_hub_addr;
    (void)tt_port;
    if (!hc || !hc->impl) return false;
    return ohci_bulk_transfer((ohci_ctrl_t*)hc->impl, (uint8_t)dev, ep, in, mps,
                              speed == USB_SPEED_LOW, start_toggle, data, len);
}

static bool ohci_usbhc_async_in_init(usb_hc_t* hc, usb_async_in_t* x,
                                    uint32_t dev, uint8_t ep, uint16_t mps,
                                    usb_speed_t speed,
                                    uint8_t tt_hub_addr, uint8_t tt_port,
                                    uint8_t start_toggle,
                                    void* buf, uint16_t len) {
    (void)tt_hub_addr;
    (void)tt_port;
    if (!hc || !hc->impl || !x || !buf || len == 0) return false;
    ohci_ctrl_t* ctrl = (ohci_ctrl_t*)hc->impl;

    ohci_async_in_t* a = (ohci_async_in_t*)kmalloc(sizeof(*a), 0, NULL);
    if (!a) return false;
    memset(a, 0, sizeof(*a));

    a->hc = ctrl;
    a->addr = (uint8_t)dev;
    a->ep = ep;
    a->mps = mps;
    a->low_speed = (speed == USB_SPEED_LOW);
    a->toggle = start_toggle & 1u;
    a->buf = buf;
    a->buf_phys = phys_addr(buf);
    a->len = len;

    a->ed = (ohci_ed_t*)kmalloc_aligned(sizeof(ohci_ed_t), 16);
    a->td = (ohci_td_t*)kmalloc_aligned(sizeof(ohci_td_t), 16);
    a->tail = (ohci_td_t*)kmalloc_aligned(sizeof(ohci_td_t), 16);
    if (!a->ed || !a->td || !a->tail) return false;
    memset(a->ed, 0, sizeof(*a->ed));
    memset(a->td, 0, sizeof(*a->td));
    memset(a->tail, 0, sizeof(*a->tail));

    init_td(a->tail, TD_DP_OUT, TD_T_DATA0, NULL, 0, false);

    init_ed(a->ed, a->addr, a->ep, a->mps, a->low_speed, ED_D_IN);
    ohci_async_prep_in_td(a);

    a->ed->head_td = phys_addr(a->td);
    a->ed->tail_td = phys_addr(a->tail);
    a->ed->next_ed = 0;

    a->next = ctrl->async_list;
    ctrl->async_list = a;
    ohci_update_interrupt_table(ctrl);

    x->hc = hc;
    x->impl = a;
    return true;
}

static int ohci_usbhc_async_in_check(usb_async_in_t* x, uint16_t* out_actual) {
    if (!x || !x->impl) return -1;
    ohci_async_in_t* a = (ohci_async_in_t*)x->impl;

    uint32_t flags = a->td->flags;
    if ((flags & TD_CC_MASK) == TD_CC_NOACCESS) return 0;

    uint32_t cc = (flags >> TD_CC_SHIFT) & 0xFu;
    if (cc != 0) return -1;

    if (out_actual) *out_actual = ohci_async_actual_len(a);
    return 1;
}

static void ohci_usbhc_async_in_rearm(usb_async_in_t* x) {
    if (!x || !x->impl) return;
    ohci_async_in_t* a = (ohci_async_in_t*)x->impl;

    a->toggle ^= 1u;
    ohci_async_prep_in_td(a);

    a->ed->head_td = phys_addr(a->td);
    a->ed->tail_td = phys_addr(a->tail);
    a->ed->flags &= ~(1u << ED_K_SHIFT);
}

static void ohci_usbhc_async_in_cancel(usb_async_in_t* x) {
    if (!x || !x->impl) return;
    ohci_async_in_t* a = (ohci_async_in_t*)x->impl;
    ohci_ctrl_t* hc = a->hc;

    // Stop the endpoint immediately.
    if (a->ed) {
        a->ed->flags |= (1u << ED_K_SHIFT);
        a->ed->head_td = a->ed->tail_td;
    }

    // Remove from controller interrupt list.
    if (hc) {
        ohci_async_in_t** pp = &hc->async_list;
        while (*pp) {
            if (*pp == a) {
                *pp = a->next;
                break;
            }
            pp = &(*pp)->next;
        }
        ohci_update_interrupt_table(hc);
    }

    x->impl = NULL;
}

static bool ohci_usbhc_configure_endpoint(usb_hc_t* hc, uint32_t dev,
                                         uint8_t ep, bool in,
                                         usb_ep_type_t type,
                                         uint16_t mps, uint8_t interval) {
    (void)hc;
    (void)dev;
    (void)ep;
    (void)in;
    (void)type;
    (void)mps;
    (void)interval;
    return true;
}

static bool ohci_usbhc_enum_open(usb_hc_t* hc, uint8_t root_port, usb_speed_t speed,
                                uint32_t* out_dev) {
    (void)hc;
    (void)root_port;
    (void)speed;
    if (!out_dev) return false;
    *out_dev = 0;
    return true;
}

typedef struct {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) usb_setup_pkt_t;

static bool ohci_usbhc_enum_set_address(usb_hc_t* hc, uint32_t dev_default, uint8_t ep0_mps,
                                       usb_speed_t speed, uint8_t tt_hub_addr, uint8_t tt_port,
                                       uint8_t desired_addr,
                                       uint32_t* inout_dev) {
    (void)dev_default;
    (void)tt_hub_addr;
    (void)tt_port;
    if (!hc || !hc->impl || !inout_dev || desired_addr == 0 || desired_addr >= 127) return false;
    ohci_ctrl_t* o = (ohci_ctrl_t*)hc->impl;

    usb_setup_pkt_t setup;
    setup.bmRequestType = 0x00;
    setup.bRequest = 5; // SET_ADDRESS
    setup.wValue = desired_addr;
    setup.wIndex = 0;
    setup.wLength = 0;

    if (!ohci_control_transfer(o, 0, 0, ep0_mps, speed == USB_SPEED_LOW, &setup, NULL, 0))
        return false;
    delay_ms(20);
    *inout_dev = desired_addr;
    return true;
}

static void ohci_usbhc_enum_close(usb_hc_t* hc, uint32_t dev) {
    (void)hc;
    (void)dev;
}

static uint8_t ohci_usbhc_alloc_address(usb_hc_t* hc) {
    if (!hc || !hc->impl) return 0;
    ohci_ctrl_t* o = (ohci_ctrl_t*)hc->impl;
    if (o->next_addr == 0 || o->next_addr >= 127) return 0;
    return o->next_addr++;
}

static void ohci_usbhc_reset_address_allocator(usb_hc_t* hc) {
    if (!hc || !hc->impl) return;
    ((ohci_ctrl_t*)hc->impl)->next_addr = 1;
}

static const usb_hc_ops_t ohci_usbhc_ops = {
    .control_transfer = ohci_usbhc_control_transfer,
    .bulk_transfer = ohci_usbhc_bulk_transfer,
    .async_in_init = ohci_usbhc_async_in_init,
    .async_in_check = ohci_usbhc_async_in_check,
    .async_in_rearm = ohci_usbhc_async_in_rearm,
    .async_in_cancel = ohci_usbhc_async_in_cancel,
    .configure_endpoint = ohci_usbhc_configure_endpoint,
    .enum_open = ohci_usbhc_enum_open,
    .enum_set_address = ohci_usbhc_enum_set_address,
    .enum_close = ohci_usbhc_enum_close,
    .alloc_address = ohci_usbhc_alloc_address,
    .reset_address_allocator = ohci_usbhc_reset_address_allocator,
};

static void ohci_scan_ports(ohci_ctrl_t* hc) {
    if (!hc) return;
    bool verbose = bootlog_enabled;
    uint32_t rha = rd_reg(hc, HC_RH_DESC_A);
    uint32_t ndp = rha & 0xFFu;
    if (verbose) kprintf("[OHCI] Root hub ports=%u\n", ndp);

    for (uint32_t p = 0; p < ndp; p++) {
        bool low_speed = false;
        if (!ohci_reset_port(hc, (int)p, &low_speed)) continue;
        usb_speed_t spd = low_speed ? USB_SPEED_LOW : USB_SPEED_FULL;
        if (verbose) kprintf("[OHCI] Device on port %u (speed=%s)\n", p + 1, low_speed ? "LS" : "FS");
        if (hc->usbhc) usb_port_connected(hc->usbhc, spd, (uint8_t)(p + 1), 0, 0);
    }
}

void ohci_rescan_all_ports(bool reset_addr_allocator) {
    for (int i = 0; i < controller_count; i++) {
        ohci_ctrl_t* hc = &controllers[i];
        if (!hc->regs) continue;
        if (hc->usbhc) usb_drop_controller_devices(hc->usbhc);
        if (reset_addr_allocator) hc->next_addr = 1;
        ohci_scan_ports(hc);
    }
}

void ohci_poll_changes(void) {
    if (ohci_rescan_pending) return;
    for (int i = 0; i < controller_count; i++) {
        ohci_ctrl_t* hc = &controllers[i];
        if (!hc->regs) continue;
        uint32_t rha = rd_reg(hc, HC_RH_DESC_A);
        uint32_t ndp = rha & 0xFFu;
        for (uint32_t p = 0; p < ndp; p++) {
            uint32_t ps = rd_reg(hc, HC_RH_PORT_STATUS(p));
            uint32_t change = ps & (RHPS_CSC | RHPS_PRSC);
            if (!change) continue;
            wr_reg(hc, HC_RH_PORT_STATUS(p), change);
            // PRSC can be raised by our own reset; only rescan on connect changes.
            if (ps & RHPS_CSC) {
                ohci_queue_rescan();
                return;
            }
        }
    }
}

bool ohci_take_rescan_pending(void) {
    bool pending = false;
    hal_disable_interrupts();
    pending = ohci_rescan_pending;
    ohci_rescan_pending = false;
    hal_enable_interrupts();
    return pending;
}

static void ohci_legacy_handoff(ohci_ctrl_t* hc) {
    if (!hc) return;

    // 1️⃣ 컨트롤러 완전 정지 (BIOS 스케줄 완전 차단)
    wr_reg(hc, HC_CONTROL, 0);
    delay_ms(10);

    // 2️⃣ 모든 인터럽트 비활성화 + 클리어
    wr_reg(hc, HC_INT_DISABLE, 0xFFFFFFFFu);
    wr_reg(hc, HC_INT_STATUS,  0xFFFFFFFFu);

    // 3️⃣ Host Controller Reset (BIOS 잔재 제거)
    wr_reg(hc, HC_COMMAND_STATUS, CMD_HCR);
    for (int i = 0; i < 1000; i++) {
        if (!(rd_reg(hc, HC_COMMAND_STATUS) & CMD_HCR))
            break;
        delay_ms(1);
    }

    // HCR 안 풀리면 포기 (BIOS가 이상한 상태)
    if (rd_reg(hc, HC_COMMAND_STATUS) & CMD_HCR) {
        kprint("[OHCI] Legacy handoff: HCR timeout\n");
        return;
    }

    // 4️⃣ 다시 한번 인터럽트 싹 정리
    wr_reg(hc, HC_INT_DISABLE, 0xFFFFFFFFu);
    wr_reg(hc, HC_INT_STATUS,  0xFFFFFFFFu);

    // 5️⃣ 리스트 포인터 제거 (BIOS 스케줄 완전 제거)
    wr_reg(hc, HC_CONTROL_HEAD_ED, 0);
    wr_reg(hc, HC_BULK_HEAD_ED, 0);
    wr_reg(hc, HC_DONE_HEAD, 0);

    // 6️⃣ OS가 소유한 HCCA로 교체
    wr_reg(hc, HC_HCCA, phys_addr(hc->hcca));

    // 7️⃣ HCFS를 명시적으로 OPERATIONAL로 진입
    uint32_t ctrl = rd_reg(hc, HC_CONTROL);
    ctrl &= ~CTRL_HCFS_MASK;
    ctrl |= CTRL_HCFS_OPERATIONAL;
    wr_reg(hc, HC_CONTROL, ctrl);
    delay_ms(10);

    kprint("[OHCI] Legacy handoff complete\n");
}

void ohci_pci_attach(uint32_t mmio_base, uint8_t irq_line) {
    if (controller_count >= OHCI_MAX_CONTROLLERS) return;

    map_mmio(mmio_base);

    int idx = controller_count++;
    ohci_ctrl_t* hc = &controllers[idx];
    memset(hc, 0, sizeof(*hc));
    hc->base = mmio_base;
    hc->regs = (volatile uint32_t*)mmio_base;
    hc->irq_line = irq_line;
    hc->next_addr = 1;
    hc->usbhc = &usbhc_wrappers[idx];
    usbhc_wrappers[idx].ops = &ohci_usbhc_ops;
    usbhc_wrappers[idx].impl = hc;

    hc->hcca = (ohci_hcca_t*)kmalloc_aligned(sizeof(ohci_hcca_t), 256);
    memset(hc->hcca, 0, sizeof(*hc->hcca));

    hc->ctrl_ed = (ohci_ed_t*)kmalloc_aligned(sizeof(ohci_ed_t), 16);
    hc->ctrl_td_setup  = (ohci_td_t*)kmalloc_aligned(sizeof(ohci_td_t), 16);
    hc->ctrl_td_data   = (ohci_td_t*)kmalloc_aligned(sizeof(ohci_td_t), 16);
    hc->ctrl_td_status = (ohci_td_t*)kmalloc_aligned(sizeof(ohci_td_t), 16);
    hc->ctrl_td_tail   = (ohci_td_t*)kmalloc_aligned(sizeof(ohci_td_t), 16);

    hc->bulk_in_ed   = (ohci_ed_t*)kmalloc_aligned(sizeof(ohci_ed_t), 16);
    hc->bulk_out_ed  = (ohci_ed_t*)kmalloc_aligned(sizeof(ohci_ed_t), 16);
    hc->bulk_in_td   = (ohci_td_t*)kmalloc_aligned(sizeof(ohci_td_t), 16);
    hc->bulk_in_tail = (ohci_td_t*)kmalloc_aligned(sizeof(ohci_td_t), 16);
    hc->bulk_out_td  = (ohci_td_t*)kmalloc_aligned(sizeof(ohci_td_t), 16);
    hc->bulk_out_tail= (ohci_td_t*)kmalloc_aligned(sizeof(ohci_td_t), 16);

    ohci_legacy_handoff(hc);
    if (!ohci_reset_controller(hc)) return;
    ohci_update_interrupt_table(hc);
    ohci_scan_ports(hc);
}
