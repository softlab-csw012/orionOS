#include "ehci.h"
#include "usb.h"
#include "../hal.h"
#include "../pci.h"
#include "../screen.h"
#include "../../mm/mem.h"
#include "../../mm/paging.h"
#include "../../cpu/timer.h"
#include "../../libc/string.h"
#include "../../kernel/proc/workqueue.h"

#define EHCI_MAX_CONTROLLERS 2
static ehci_ctrl_t controllers[EHCI_MAX_CONTROLLERS];
static usb_hc_t usbhc_wrappers[EHCI_MAX_CONTROLLERS];
static int controller_count = 0;
static volatile bool ehci_rescan_pending = false;
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

static void ehci_rescan_work(void* ctx) {
    (void)ctx;
    if (ehci_take_rescan_pending()) {
        ehci_rescan_all_ports(true);
    }
}

static void ehci_queue_rescan(void) {
    bool enqueue = false;
    uint32_t flags = irq_save();
    if (!ehci_rescan_pending) {
        ehci_rescan_pending = true;
        enqueue = true;
    }
    irq_restore(flags);

    if (enqueue) {
        (void)workqueue_enqueue(ehci_rescan_work, NULL);
    }
}

static void delay_ms(uint32_t ms);

typedef struct {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) usb_setup_pkt_t;

static inline ehci_speed_t ehci_speed_from_usb(usb_speed_t s) {
    switch (s) {
    case USB_SPEED_LOW:  return EHCI_SPEED_LOW;
    case USB_SPEED_FULL: return EHCI_SPEED_FULL;
    case USB_SPEED_HIGH:
    case USB_SPEED_SUPER:
    default:             return EHCI_SPEED_HIGH;
    }
}

static bool ehci_usbhc_control(usb_hc_t* hc, uint32_t dev, uint8_t ep,
                              uint16_t mps, usb_speed_t speed,
                              uint8_t tt_hub_addr, uint8_t tt_port,
                              const void* setup8, void* data, uint16_t len) {
    if (!hc || !hc->impl) return false;
    return ehci_control_transfer((ehci_ctrl_t*)hc->impl, (uint8_t)dev, ep, mps,
                                 ehci_speed_from_usb(speed),
                                 tt_hub_addr, tt_port,
                                 setup8, data, len);
}

static bool ehci_usbhc_bulk(usb_hc_t* hc, uint32_t dev, uint8_t ep, bool in,
                            uint16_t mps, usb_speed_t speed,
                            uint8_t tt_hub_addr, uint8_t tt_port,
                            uint8_t start_toggle,
                            void* data, uint16_t len) {
    if (!hc || !hc->impl) return false;
    return ehci_bulk_transfer((ehci_ctrl_t*)hc->impl, (uint8_t)dev, ep, in,
                              mps, ehci_speed_from_usb(speed),
                              tt_hub_addr, tt_port,
                              start_toggle, data, len);
}

static bool ehci_usbhc_async_in_init(usb_hc_t* hc, usb_async_in_t* x,
                                    uint32_t dev, uint8_t ep, uint16_t mps,
                                    usb_speed_t speed,
                                    uint8_t tt_hub_addr, uint8_t tt_port,
                                    uint8_t start_toggle,
                                    void* buf, uint16_t len) {
    if (!hc || !hc->impl || !x) return false;
    ehci_async_in_t* impl = (ehci_async_in_t*)kmalloc(sizeof(*impl), 0, NULL);
    if (!impl) return false;
    memset(impl, 0, sizeof(*impl));
    if (!ehci_async_in_init((ehci_ctrl_t*)hc->impl, impl, (uint8_t)dev, ep, mps,
                            ehci_speed_from_usb(speed),
                            tt_hub_addr, tt_port,
                            start_toggle, buf, len)) {
        return false;
    }
    x->hc = hc;
    x->impl = impl;
    return true;
}

static int ehci_usbhc_async_in_check(usb_async_in_t* x, uint16_t* out_actual) {
    if (!x || !x->impl) return -1;
    return ehci_async_in_check((ehci_async_in_t*)x->impl, out_actual);
}

static void ehci_usbhc_async_in_rearm(usb_async_in_t* x) {
    if (!x || !x->impl) return;
    ehci_async_in_rearm((ehci_async_in_t*)x->impl);
}

static void ehci_usbhc_async_in_cancel(usb_async_in_t* x) {
    if (!x || !x->impl) return;
    ehci_async_in_t* impl = (ehci_async_in_t*)x->impl;
    if (impl->qh) impl->qh->next_qtd = 1u;
    if (impl->qtd) impl->qtd->token = 0;
}

static bool ehci_usbhc_configure_endpoint(usb_hc_t* hc, uint32_t dev,
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

static bool ehci_usbhc_enum_open(usb_hc_t* hc, uint8_t root_port, usb_speed_t speed,
                                uint32_t* out_dev) {
    (void)hc;
    (void)root_port;
    (void)speed;
    if (!out_dev) return false;
    *out_dev = 0;
    return true;
}

static bool ehci_usbhc_enum_set_address(usb_hc_t* hc, uint32_t dev_default, uint8_t ep0_mps,
                                       usb_speed_t speed, uint8_t tt_hub_addr, uint8_t tt_port,
                                       uint8_t desired_addr,
                                       uint32_t* inout_dev) {
    (void)dev_default;
    if (!hc || !hc->impl || !inout_dev || desired_addr == 0 || desired_addr >= 127) return false;
    usb_setup_pkt_t setup;
    setup.bmRequestType = 0x00;
    setup.bRequest = 5; // SET_ADDRESS
    setup.wValue = desired_addr;
    setup.wIndex = 0;
    setup.wLength = 0;
    if (!ehci_control_transfer((ehci_ctrl_t*)hc->impl, 0, 0, ep0_mps,
                               ehci_speed_from_usb(speed),
                               tt_hub_addr, tt_port,
                               &setup, NULL, 0)) {
        return false;
    }
    delay_ms(20);
    *inout_dev = desired_addr;
    return true;
}

static void ehci_usbhc_enum_close(usb_hc_t* hc, uint32_t dev) {
    (void)hc;
    (void)dev;
}

static uint8_t ehci_usbhc_alloc_address(usb_hc_t* hc) {
    if (!hc || !hc->impl) return 0;
    ehci_ctrl_t* ehc = (ehci_ctrl_t*)hc->impl;
    if (ehc->next_addr == 0 || ehc->next_addr >= 127) return 0;
    return ehc->next_addr++;
}

static void ehci_usbhc_reset_address_allocator(usb_hc_t* hc) {
    if (!hc || !hc->impl) return;
    ((ehci_ctrl_t*)hc->impl)->next_addr = 1;
}

static const usb_hc_ops_t ehci_usbhc_ops = {
    .control_transfer = ehci_usbhc_control,
    .bulk_transfer = ehci_usbhc_bulk,
    .async_in_init = ehci_usbhc_async_in_init,
    .async_in_check = ehci_usbhc_async_in_check,
    .async_in_rearm = ehci_usbhc_async_in_rearm,
    .async_in_cancel = ehci_usbhc_async_in_cancel,
    .configure_endpoint = ehci_usbhc_configure_endpoint,
    .enum_open = ehci_usbhc_enum_open,
    .enum_set_address = ehci_usbhc_enum_set_address,
    .enum_close = ehci_usbhc_enum_close,
    .alloc_address = ehci_usbhc_alloc_address,
    .reset_address_allocator = ehci_usbhc_reset_address_allocator,
};

// Capability offsets
#define CAP_CAPLENGTH   0x00
#define CAP_HCSPARAMS   0x04
#define CAP_HCCPARAMS   0x08

// Operational offsets
#define OP_USBCMD       0x00
#define OP_USBSTS       0x04
#define OP_USBINTR      0x08
#define OP_FRINDEX      0x0C
#define OP_CTRLDSSEG    0x10
#define OP_PERIODICLIST 0x14
#define OP_ASYNCLIST    0x18
#define OP_CONFIGFLAG   0x40
#define OP_PORTSC(n)    (0x44 + ((n) * 4))

// USBCMD bits
#define CMD_RS          (1u << 0)
#define CMD_HCRESET     (1u << 1)
#define CMD_PSE         (1u << 4)
#define CMD_ASE         (1u << 5)

// USBSTS bits
#define STS_HCHALTED    (1u << 12)
#define STS_ASS         (1u << 15)

// PORTSC bits
#define PORT_CCS        (1u << 0)
#define PORT_CSC        (1u << 1)
#define PORT_PED        (1u << 2)
#define PORT_PEC        (1u << 3)
#define PORT_OCC        (1u << 5)
#define PORT_PR         (1u << 8)
#define PORT_PP         (1u << 12)
#define PORT_OWNER      (1u << 13)
#define PORT_CHANGE_BITS (PORT_CSC | PORT_PEC | PORT_OCC)

// qTD token bits
#define QTD_STATUS_ACTIVE (1u << 7)
#define QTD_STATUS_HALTED (1u << 6)
#define QTD_PID_SHIFT     8
#define QTD_CERR_SHIFT    10
#define QTD_IOC           (1u << 15)
#define QTD_BYTES_SHIFT   16
#define QTD_TOGGLE        (1u << 31)

#define QTD_PID_OUT   (0u << QTD_PID_SHIFT)
#define QTD_PID_IN    (1u << QTD_PID_SHIFT)
#define QTD_PID_SETUP (2u << QTD_PID_SHIFT)

// QH fields
#define QH_EPS_SHIFT   12
#define QH_DTC         (1u << 14)
#define QH_H           (1u << 15)
#define QH_MPS_SHIFT   16
#define QH_RL_SHIFT    28
#define QH_C           (1u << 27)
#define QH_EPS_HIGH    (2u << QH_EPS_SHIFT)

#define EHCI_PTR_TERM  1u
#define EHCI_PTR_QH    (1u << 1) // type=QH

static inline uint32_t phys_addr(void* p) {
    uint32_t phys;
    if (vmm_virt_to_phys((uint32_t)p, &phys) == 0)
        return phys;
    kprintf("[EHCI] v2p failed for %08x\n", (uint32_t)p);
    return 0;
}

static void* ehci_dma_alloc(size_t size) {
    // EHCI descriptors must not cross a 4KB boundary; use page-aligned allocations to be safe.
    void* p = kmalloc(size, 1, NULL);
    if (!p)
        return NULL;
    if ((((uintptr_t)p) & 0xFFFu) + size > 0x1000u) {
        kfree(p);
        return NULL;
    }
    memset(p, 0, size);
    return p;
}

static inline uint32_t cap_rd(ehci_ctrl_t* hc, uint32_t off) {
    return hc->cap_regs[off / 4];
}
static inline uint32_t op_rd(ehci_ctrl_t* hc, uint32_t off) {
    return hc->op_regs[off / 4];
}
static inline void op_wr(ehci_ctrl_t* hc, uint32_t off, uint32_t v) {
    hc->op_regs[off / 4] = v;
}

static void invlpg(uint32_t addr) {
    hal_invlpg((const void*)(uintptr_t)addr);
}

static void map_mmio(uint32_t base) {
    uint32_t start = base & ~0xFFFu;
    for (uint32_t addr = start; addr < start + 0x3000u; addr += 0x1000u) {
        vmm_map_page(addr, addr, PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT);
        invlpg(addr);
    }
}

static void delay_ms(uint32_t ms) {
    uint32_t start = tick;
    uint32_t ticks_needed = (ms + 9) / 10;
    if (ticks_needed == 0) ticks_needed = 1;
    while ((tick - start) < ticks_needed) {
        hal_wait_for_interrupt();
    }
}

static bool qtd_fill_bufs(ehci_qtd_t* qtd, void* buf, uint32_t len) {
    for (int i = 0; i < 5; i++) {
        qtd->buf[i] = 0;
        qtd->buf_hi[i] = 0;
    }

    if (!buf || len == 0)
        return true;

    uint32_t virt = (uint32_t)buf;
    uint32_t offset = virt & 0xFFFu;
    uint32_t total = offset + len;
    uint32_t pages = (total + 0xFFFu) >> 12;
    if (pages > 5) {
        kprintf("[EHCI] buffer spans too many pages (len=%u)\n", len);
        return false;
    }

    uint32_t phys;
    if (vmm_virt_to_phys(virt, &phys) != 0) {
        kprintf("[EHCI] v2p failed for buffer %08x\n", virt);
        return false;
    }
    qtd->buf[0] = phys;

    uint32_t virt_page = virt & ~0xFFFu;
    for (uint32_t i = 1; i < pages; i++) {
        uint32_t page_phys;
        uint32_t page_virt = virt_page + i * 0x1000u;
        if (vmm_virt_to_phys(page_virt, &page_phys) != 0) {
            kprintf("[EHCI] v2p failed for buffer page %08x\n", page_virt);
            return false;
        }
        qtd->buf[i] = page_phys;
    }
    return true;
}

static bool qtd_init(ehci_qtd_t* qtd, uint32_t pid, uint32_t toggle, void* buf, uint32_t len, bool ioc) {
    qtd->next = EHCI_PTR_TERM;
    qtd->alt_next = EHCI_PTR_TERM;
    qtd->token = QTD_STATUS_ACTIVE |
                 (3u << QTD_CERR_SHIFT) |
                 pid |
                 (ioc ? QTD_IOC : 0) |
                 (len << QTD_BYTES_SHIFT) |
                 (toggle ? QTD_TOGGLE : 0);
    return qtd_fill_bufs(qtd, buf, len);
}

static uint32_t qh_eps_bits(ehci_speed_t speed) {
    switch (speed) {
    case EHCI_SPEED_LOW:  return (1u << QH_EPS_SHIFT);
    case EHCI_SPEED_HIGH: return QH_EPS_HIGH;
    case EHCI_SPEED_FULL:
    default:              return (0u << QH_EPS_SHIFT);
    }
}

static void qh_init_ep(ehci_qh_t* qh, uint8_t addr, uint8_t ep, uint16_t mps,
                       ehci_speed_t speed, bool is_control,
                       uint8_t tt_hub_addr, uint8_t tt_port) {
    qh->ep_char = ((uint32_t)addr) |
                  ((uint32_t)ep << 8) |
                  qh_eps_bits(speed) |
                  QH_DTC |
                  ((uint32_t)mps << QH_MPS_SHIFT) |
                  (0u << QH_RL_SHIFT);
    if (is_control && speed != EHCI_SPEED_HIGH) qh->ep_char |= QH_C;

    if (speed != EHCI_SPEED_HIGH && tt_hub_addr != 0 && tt_port != 0) {
        qh->ep_cap = (1u << 0) |
                     (0xFCu << 8) |
                     ((uint32_t)tt_hub_addr << 16) |
                     ((uint32_t)tt_port << 23);
    } else {
        qh->ep_cap = 0;
    }
    qh->current_qtd = 0;
    qh->next_qtd = EHCI_PTR_TERM;
    qh->alt_next_qtd = EHCI_PTR_TERM;
    qh->token = 0;
    for (int i = 0; i < 5; i++) qh->buf[i] = 0;
    for (int i = 0; i < 5; i++) qh->buf_hi[i] = 0;
}

static bool wait_qtd_done(ehci_qtd_t* qtd, uint32_t timeout_ms) {
    uint32_t start = tick;
    uint32_t timeout_ticks = (timeout_ms + 9) / 10;
    if (timeout_ticks == 0) timeout_ticks = 1;
    while (qtd->token & QTD_STATUS_ACTIVE) {
        if ((tick - start) > timeout_ticks) return false;
        hal_wait_for_interrupt();
    }
    if (qtd->token & QTD_STATUS_HALTED) return false;
    return true;
}

bool ehci_control_transfer(ehci_ctrl_t* hc, uint8_t addr, uint8_t ep,
                           uint16_t mps, ehci_speed_t speed,
                           uint8_t tt_hub_addr, uint8_t tt_port,
                           const void* setup8, void* data, uint16_t len) {
    qh_init_ep(hc->ctrl_qh, addr, ep, mps, speed, true, tt_hub_addr, tt_port);

    if (!qtd_init(hc->ctrl_qtd_setup, QTD_PID_SETUP, 0, (void*)setup8, 8, false))
        return false;

    if (len > 0 && data != NULL) {
        bool in = (((const uint8_t*)setup8)[0] & 0x80) != 0;
        if (!qtd_init(hc->ctrl_qtd_data, in ? QTD_PID_IN : QTD_PID_OUT, 1, data, len, false))
            return false;
        hc->ctrl_qtd_setup->next = phys_addr(hc->ctrl_qtd_data);
        hc->ctrl_qtd_data->next = phys_addr(hc->ctrl_qtd_status);
    } else {
        hc->ctrl_qtd_setup->next = phys_addr(hc->ctrl_qtd_status);
    }

    bool status_in = (len > 0 && data != NULL) ? ((((const uint8_t*)setup8)[0] & 0x80) == 0) : true;
    if (!qtd_init(hc->ctrl_qtd_status, status_in ? QTD_PID_IN : QTD_PID_OUT, 1, NULL, 0, true))
        return false;
    hc->ctrl_qtd_status->next = EHCI_PTR_TERM;

    hc->ctrl_qh->next_qtd = phys_addr(hc->ctrl_qtd_setup);

    bool ok = wait_qtd_done(hc->ctrl_qtd_status, EHCI_CTRL_TIMEOUT_MS);
    return ok;
}

bool ehci_bulk_transfer(ehci_ctrl_t* hc, uint8_t addr, uint8_t ep, bool in,
                        uint16_t mps, ehci_speed_t speed,
                        uint8_t tt_hub_addr, uint8_t tt_port,
                        uint8_t start_toggle,
                        void* data, uint16_t len) {
    ehci_qh_t* qh = in ? hc->bulk_in_qh : hc->bulk_out_qh;
    ehci_qtd_t* qtd = in ? hc->bulk_in_qtd : hc->bulk_out_qtd;

    qh_init_ep(qh, addr, ep, mps, speed, false, tt_hub_addr, tt_port);

    uint32_t pid = in ? QTD_PID_IN : QTD_PID_OUT;
    if (!qtd_init(qtd, pid, start_toggle ? 1u : 0u, data, len, true))
        return false;
    qh->next_qtd = phys_addr(qtd);

    bool ok = wait_qtd_done(qtd, EHCI_BULK_TIMEOUT_MS);
    return ok;
}

static bool ehci_reset_controller(ehci_ctrl_t* hc) {
    uint32_t cap0 = cap_rd(hc, CAP_CAPLENGTH);
    hc->cap_len = (uint8_t)(cap0 & 0xFF);
    hc->op_regs = (volatile uint32_t*)((uint32_t)hc->cap_regs + hc->cap_len);

    uint16_t ver = (uint16_t)((cap0 >> 16) & 0xFFFF);
    kprintf("[EHCI] Version %x caplen=%u\n", ver, hc->cap_len);

    // Stop controller
    op_wr(hc, OP_USBCMD, 0);
    for (int i = 0; i < 1000; i++) {
        if (op_rd(hc, OP_USBSTS) & STS_HCHALTED) break;
        delay_ms(1);
    }

    // Reset
    op_wr(hc, OP_USBCMD, CMD_HCRESET);
    for (int i = 0; i < 1000; i++) {
        if (!(op_rd(hc, OP_USBCMD) & CMD_HCRESET)) break;
        delay_ms(1);
    }
    if (op_rd(hc, OP_USBCMD) & CMD_HCRESET) {
        kprint("[EHCI] HCR timeout\n");
        return false;
    }

    // Clear status, disable intr
    op_wr(hc, OP_USBINTR, 0);
    op_wr(hc, OP_USBSTS, 0x3Fu);

    op_wr(hc, OP_CTRLDSSEG, 0);
    if (hc->periodic_list) op_wr(hc, OP_PERIODICLIST, phys_addr(hc->periodic_list));
    op_wr(hc, OP_ASYNCLIST, phys_addr(hc->async_head));
    op_wr(hc, OP_CONFIGFLAG, 1);

    // Run + async enable
    op_wr(hc, OP_USBCMD, CMD_RS | CMD_ASE | (hc->periodic_list ? CMD_PSE : 0));
    for (int i = 0; i < 1000; i++) {
        if (op_rd(hc, OP_USBSTS) & STS_ASS) break;
        delay_ms(1);
    }

    return true;
}

static void ehci_scan_ports(ehci_ctrl_t* hc) {
    uint32_t hcs = cap_rd(hc, CAP_HCSPARAMS);
    uint32_t n_ports = hcs & 0xFu;
    kprintf("[EHCI] Root hub ports=%u\n", n_ports);

    for (uint32_t p = 0; p < n_ports; p++) {
        uint32_t ps = op_rd(hc, OP_PORTSC(p));
        if (!(ps & PORT_CCS)) continue;

        // Power on
        ps |= PORT_PP;
        op_wr(hc, OP_PORTSC(p), ps);
        delay_ms(20);

        // Reset
        ps |= PORT_PR;
        op_wr(hc, OP_PORTSC(p), ps);
        delay_ms(50);
        ps &= ~PORT_PR;
        op_wr(hc, OP_PORTSC(p), ps);
        delay_ms(20);

        ps = op_rd(hc, OP_PORTSC(p));

        // 🔑 EHCI가 직접 다루는 건 HS만
        // LS/FS device면 companion으로 강제 handoff
        if (!(ps & PORT_PED)) {
            ps |= PORT_OWNER;
            op_wr(hc, OP_PORTSC(p), ps);
            delay_ms(5);

            kprintf("[EHCI] Port %u forced to companion (OWNER=1)\n", p + 1);
            continue;
        }

        kprintf("[EHCI] High-speed device on port %u\n", p + 1);

        if (hc->usbhc) {
            usb_port_connected(
                hc->usbhc,
                USB_SPEED_HIGH,
                (uint8_t)(p + 1),
                0, 0
            );
        }
    }
}

static bool ehci_legacy_handoff(ehci_ctrl_t* hc, uint8_t bus, uint8_t dev, uint8_t func) {
    uint32_t hcc = cap_rd(hc, CAP_HCCPARAMS);
    uint8_t eecp = (uint8_t)((hcc >> 8) & 0xFF);
    if (eecp == 0) return true;

    for (int hops = 0; hops < 32 && eecp >= 0x40; hops++) {
        uint32_t cap = pci_read_dword(bus, dev, func, eecp);
        uint8_t cap_id = (uint8_t)(cap & 0xFF);
        uint8_t next = (uint8_t)((cap >> 8) & 0xFF);

        if (cap_id == 0x01) { // USB Legacy Support Capability
            const uint32_t BIOS_OWNED = (1u << 16);
            const uint32_t OS_OWNED = (1u << 24);

            uint8_t legsup_off = eecp;
            uint32_t legsup = cap;
            if ((legsup & OS_OWNED) == 0) {
                pci_write_dword(bus, dev, func, eecp, legsup | OS_OWNED);
                legsup |= OS_OWNED;
            }

            if (legsup & BIOS_OWNED) {
                for (int i = 0; i < 200; i++) {
                    delay_ms(1);
                    legsup = pci_read_dword(bus, dev, func, legsup_off);
                    if ((legsup & BIOS_OWNED) == 0) break;
                }
            }

            legsup = pci_read_dword(bus, dev, func, legsup_off);
            if (legsup & BIOS_OWNED) {
                kprint("[EHCI] BIOS owned semaphore stuck; skipping controller\n");
                return false;
            }

            // Disable legacy SMI generation (best-effort).
            pci_write_dword(bus, dev, func, (uint8_t)(legsup_off + 4), 0);
            return true;
        }

        if (next == 0 || next == eecp) break;
        eecp = next;
    }
    return true;
}

void ehci_pci_attach(uint8_t bus, uint8_t dev, uint8_t func,
                     uint32_t mmio_base, uint8_t irq_line) {
    if (controller_count >= EHCI_MAX_CONTROLLERS) return;

    map_mmio(mmio_base);

    int idx = controller_count;
    ehci_ctrl_t* hc = &controllers[idx];
    memset(hc, 0, sizeof(*hc));
    hc->base = mmio_base;
    hc->cap_regs = (volatile uint32_t*)mmio_base;
    hc->irq_line = irq_line;
    hc->next_addr = 1;
    hc->usbhc = &usbhc_wrappers[idx];
    usbhc_wrappers[idx].ops = &ehci_usbhc_ops;
    usbhc_wrappers[idx].impl = hc;

    if (!ehci_legacy_handoff(hc, bus, dev, func)) return;

    hc->async_head = (ehci_qh_t*)ehci_dma_alloc(sizeof(ehci_qh_t));
    hc->periodic_list = (uint32_t*)ehci_dma_alloc(1024 * sizeof(uint32_t));
    hc->periodic_head = (ehci_qh_t*)ehci_dma_alloc(sizeof(ehci_qh_t));
    hc->ctrl_qh = (ehci_qh_t*)ehci_dma_alloc(sizeof(ehci_qh_t));
    hc->bulk_in_qh = (ehci_qh_t*)ehci_dma_alloc(sizeof(ehci_qh_t));
    hc->bulk_out_qh = (ehci_qh_t*)ehci_dma_alloc(sizeof(ehci_qh_t));

    hc->ctrl_qtd_setup = (ehci_qtd_t*)ehci_dma_alloc(sizeof(ehci_qtd_t));
    hc->ctrl_qtd_data = (ehci_qtd_t*)ehci_dma_alloc(sizeof(ehci_qtd_t));
    hc->ctrl_qtd_status = (ehci_qtd_t*)ehci_dma_alloc(sizeof(ehci_qtd_t));
    hc->bulk_in_qtd = (ehci_qtd_t*)ehci_dma_alloc(sizeof(ehci_qtd_t));
    hc->bulk_out_qtd = (ehci_qtd_t*)ehci_dma_alloc(sizeof(ehci_qtd_t));

    if (!hc->async_head || !hc->ctrl_qh || !hc->bulk_in_qh || !hc->bulk_out_qh ||
        !hc->ctrl_qtd_setup || !hc->ctrl_qtd_data || !hc->ctrl_qtd_status ||
        !hc->bulk_in_qtd || !hc->bulk_out_qtd) {
        return;
    }

    if (!hc->periodic_list || !hc->periodic_head) {
        hc->periodic_list = NULL;
        hc->periodic_head = NULL;
    }

    memset(hc->async_head, 0, sizeof(ehci_qh_t));
    // Async head must have a terminated overlay before enabling ASE,
    // otherwise the controller may chase a bogus qTD at address 0.
    hc->async_head->ep_char = QH_H | QH_EPS_HIGH | QH_DTC | (64u << QH_MPS_SHIFT);
    hc->async_head->ep_cap = 0;
    hc->async_head->next_qtd = EHCI_PTR_TERM;
    hc->async_head->alt_next_qtd = EHCI_PTR_TERM;
    hc->async_head->token = 0;
    for (int i = 0; i < 5; i++) hc->async_head->buf[i] = 0;
    for (int i = 0; i < 5; i++) hc->async_head->buf_hi[i] = 0;

    memset(hc->ctrl_qh, 0, sizeof(ehci_qh_t));
    memset(hc->bulk_in_qh, 0, sizeof(ehci_qh_t));
    memset(hc->bulk_out_qh, 0, sizeof(ehci_qh_t));
    if (hc->periodic_head) memset(hc->periodic_head, 0, sizeof(ehci_qh_t));

    // Permanent async ring: head -> ctrl -> bulk_in -> bulk_out -> head
    hc->async_head->hlp = phys_addr(hc->ctrl_qh) | EHCI_PTR_QH;
    hc->ctrl_qh->hlp = phys_addr(hc->bulk_in_qh) | EHCI_PTR_QH;
    hc->bulk_in_qh->hlp = phys_addr(hc->bulk_out_qh) | EHCI_PTR_QH;
    hc->bulk_out_qh->hlp = phys_addr(hc->async_head) | EHCI_PTR_QH;

    // Make sure the controller never chases a bogus qTD before we schedule a transfer.
    // (These QHs are always present in the ring.)
    qh_init_ep(hc->ctrl_qh, 0, 0, 64, EHCI_SPEED_HIGH, true, 0, 0);
    qh_init_ep(hc->bulk_in_qh, 0, 0, 64, EHCI_SPEED_HIGH, false, 0, 0);
    qh_init_ep(hc->bulk_out_qh, 0, 0, 64, EHCI_SPEED_HIGH, false, 0, 0);

    // Periodic schedule: one dummy head QH referenced by every frame entry.
    if (hc->periodic_list && hc->periodic_head) {
        qh_init_ep(hc->periodic_head, 0, 0, 64, EHCI_SPEED_HIGH, false, 0, 0);
        hc->periodic_head->hlp = EHCI_PTR_TERM;
        uint32_t head_ptr = phys_addr(hc->periodic_head) | EHCI_PTR_QH;
        for (int i = 0; i < 1024; i++) hc->periodic_list[i] = head_ptr;
    }

    if (!ehci_reset_controller(hc)) return;

    ehci_scan_ports(hc);
    controller_count++;
}

void ehci_rescan_all_ports(bool reset_addr_allocator) {
    for (int i = 0; i < controller_count; i++) {
        ehci_ctrl_t* hc = &controllers[i];
        if (!hc->cap_regs || !hc->op_regs) continue;
        if (hc->usbhc) usb_drop_controller_devices(hc->usbhc);
        if (reset_addr_allocator) hc->next_addr = 1;
        ehci_scan_ports(hc);
    }
}

void ehci_poll_changes(void) {
    if (ehci_rescan_pending) return;
    for (int i = 0; i < controller_count; i++) {
        ehci_ctrl_t* hc = &controllers[i];
        if (!hc->cap_regs || !hc->op_regs) continue;
        uint32_t hcs = cap_rd(hc, CAP_HCSPARAMS);
        uint32_t n_ports = hcs & 0xFu;
        for (uint32_t p = 0; p < n_ports; p++) {
            uint32_t ps = op_rd(hc, OP_PORTSC(p));
            if (ps & PORT_CHANGE_BITS) {
                op_wr(hc, OP_PORTSC(p), ps | PORT_CHANGE_BITS);
                ehci_queue_rescan();
                return;
            }
        }
    }
}

bool ehci_take_rescan_pending(void) {
    bool pending = false;
    hal_disable_interrupts();
    pending = ehci_rescan_pending;
    ehci_rescan_pending = false;
    hal_enable_interrupts();
    return pending;
}

static void ehci_async_insert_qh(ehci_ctrl_t* hc, ehci_qh_t* qh) {
    ehci_qh_t* after = hc->bulk_out_qh ? hc->bulk_out_qh : hc->async_head;
    if (!after) return;

    uint32_t next = after->hlp;
    after->hlp = phys_addr(qh) | EHCI_PTR_QH;
    qh->hlp = next;
}

static void ehci_periodic_insert_qh(ehci_ctrl_t* hc, ehci_qh_t* qh) {
    if (!hc || !hc->periodic_head) return;
    uint32_t next = hc->periodic_head->hlp;
    hc->periodic_head->hlp = phys_addr(qh) | EHCI_PTR_QH;
    qh->hlp = next;
}

bool ehci_async_in_init(ehci_ctrl_t* hc, ehci_async_in_t* x,
                        uint8_t addr, uint8_t ep, uint16_t mps,
                        ehci_speed_t speed, uint8_t tt_hub_addr, uint8_t tt_port,
                        uint8_t start_toggle, void* buf, uint16_t len) {
    if (!hc || !x || !buf || len == 0) return false;

    ehci_qh_t* qh = (ehci_qh_t*)ehci_dma_alloc(sizeof(ehci_qh_t));
    ehci_qtd_t* qtd = (ehci_qtd_t*)ehci_dma_alloc(sizeof(ehci_qtd_t));
    if (!qh || !qtd) return false;

    memset(qh, 0, sizeof(*qh));
    memset(qtd, 0, sizeof(*qtd));

    qh_init_ep(qh, addr, ep, mps, speed, false, tt_hub_addr, tt_port);
    // Interrupt QHs must have a non-zero S-mask to be scheduled by EHCI.
    if (speed == EHCI_SPEED_HIGH) {
        qh->ep_cap = 0x01u; // S-mask: microframe 0
    }

    x->qh = qh;
    x->qtd = qtd;
    x->buf = buf;
    x->len = len;
    x->toggle = start_toggle & 1u;

    if (!qtd_init(qtd, QTD_PID_IN, x->toggle ? 1u : 0u, buf, len, false))
        return false;
    qh->next_qtd = phys_addr(qtd);

    // EHCI interrupt endpoints should live on the periodic schedule.
    // Without a periodic frame list + PSE, FS/LS HID (split transactions) won't complete.
    if (hc->periodic_head && hc->periodic_list) ehci_periodic_insert_qh(hc, qh);
    else ehci_async_insert_qh(hc, qh);
    return true;
}

int ehci_async_in_check(ehci_async_in_t* x, uint16_t* out_actual) {
    if (!x || !x->qtd) return -1;

    uint32_t token = x->qtd->token;
    if (token & QTD_STATUS_ACTIVE) return 0;
    if (token & QTD_STATUS_HALTED) return -1;

    uint32_t rem = (token >> QTD_BYTES_SHIFT) & 0x7FFFu;
    uint32_t actual = 0;
    if (x->len >= rem) actual = (uint32_t)x->len - rem;
    if (actual > 0xFFFFu) actual = 0xFFFFu;
    if (out_actual) *out_actual = (uint16_t)actual;
    return 1;
}

void ehci_async_in_rearm(ehci_async_in_t* x) {
    if (!x || !x->qh || !x->qtd || !x->buf || x->len == 0) return;

    x->toggle ^= 1u;
    if (!qtd_init(x->qtd, QTD_PID_IN, x->toggle ? 1u : 0u, x->buf, x->len, false))
        return;

    x->qh->current_qtd = 0;
    x->qh->next_qtd = phys_addr(x->qtd);
}
