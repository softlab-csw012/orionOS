#include "xhci.h"
#include "usb.h"
#include "hid_boot_kbd.h"
#include "../hal.h"
#include "../pci.h"
#include "../screen.h"
#include "../../mm/mem.h"
#include "../../mm/paging.h"
#include "../../cpu/timer.h"
#include "../../libc/string.h"
#include "../../kernel/proc/workqueue.h"

#define XHCI_MAX_CONTROLLERS 2
#define XHCI_MAX_SLOTS 32
#define XHCI_MAX_DCI 32

typedef struct {
    uint32_t param_lo;
    uint32_t param_hi;
    uint32_t status;
    uint32_t control;
} __attribute__((packed, aligned(16))) xhci_trb_t;

typedef struct {
    uint32_t seg_addr_lo;
    uint32_t seg_addr_hi;
    uint32_t seg_size;
    uint32_t rsvd;
} __attribute__((packed, aligned(16))) xhci_erst_t;

typedef struct {
    xhci_trb_t* trbs;
    uint32_t trb_count;
    uint32_t enqueue;
    uint8_t cycle;
    uint32_t trbs_phys;
} xhci_ring_t;

typedef struct xhci_ctrl xhci_ctrl_t;

typedef struct xhci_async {
    xhci_ctrl_t* ctrl;
    uint8_t slot_id;
    uint8_t dci;
    uint64_t expected_trb;
    uint32_t buf_phys;
    uint16_t requested_len;
    uint16_t actual;
    int status; // 0=pending, 1=ok, -1=err
    struct xhci_async* next;
} xhci_async_t;

typedef struct {
    bool used;
    uint8_t slot_id;
    uint8_t root_port;
    usb_speed_t speed;
    uint8_t usb_addr;
    uint8_t ctx_size;
    uint8_t context_entries;

    void* dc;       // output device context
    uint32_t dc_phys;
    void* ic;       // input context
    uint32_t ic_phys;

    xhci_ring_t ep_rings[XHCI_MAX_DCI];
} xhci_dev_t;

struct xhci_ctrl {
    uint32_t base;
    volatile uint8_t* cap;
    volatile uint32_t* op;
    volatile uint32_t* rt;
    volatile uint32_t* db;

    uint8_t cap_len;
    uint8_t max_ports;
    uint8_t max_slots;
    uint8_t ctx_size;

    uint32_t* dcbaa;
    uint32_t dcbaa_phys;

    xhci_ring_t cmd_ring;
    xhci_ring_t evt_ring;
    xhci_erst_t* erst;
    uint32_t erst_phys;

    xhci_dev_t devs[XHCI_MAX_SLOTS + 1];
    xhci_async_t* async_list;

    uint8_t next_addr;

    struct {
        bool waiting;
        uint64_t expected_trb;
        uint8_t completion_code;
        uint32_t remaining;
        uint8_t slot_id;
    } wait_xfer, wait_cmd;

    usb_hc_t usbhc;
};

static xhci_ctrl_t controllers[XHCI_MAX_CONTROLLERS];
static int controller_count = 0;
static volatile bool xhci_rescan_pending = false;
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

static void xhci_rescan_work(void* ctx) {
    (void)ctx;
    if (xhci_take_rescan_pending()) {
        xhci_rescan_all_ports(false, false);
    }
}

static void xhci_queue_rescan(void) {
    bool enqueue = false;
    uint32_t flags = irq_save();
    if (!xhci_rescan_pending) {
        xhci_rescan_pending = true;
        enqueue = true;
    }
    irq_restore(flags);

    if (enqueue) {
        (void)workqueue_enqueue(xhci_rescan_work, NULL);
    }
}

static inline uint32_t phys_addr32(void* p) {
    uint32_t phys;
    if (vmm_virt_to_phys((uint32_t)p, &phys) == 0) return phys;
    return (uint32_t)p;
}

static inline void mmio_wr32(volatile uint32_t* base, uint32_t off, uint32_t v) {
    base[off / 4] = v;
}
static inline uint32_t mmio_rd32(volatile uint32_t* base, uint32_t off) {
    return base[off / 4];
}

static inline void mmio_wr64(volatile uint32_t* base, uint32_t off, uint64_t v) {
    base[off / 4] = (uint32_t)(v & 0xFFFFFFFFu);
    base[off / 4 + 1] = (uint32_t)(v >> 32);
}

static inline uint64_t mmio_rd64(volatile uint32_t* base, uint32_t off) {
    uint64_t lo = base[off / 4];
    uint64_t hi = base[off / 4 + 1];
    return lo | (hi << 32);
}

static void invlpg(uint32_t addr) {
    hal_invlpg((const void*)(uintptr_t)addr);
}

static void map_mmio(uint32_t base, uint32_t size) {
    uint32_t start = base & ~0xFFFu;
    uint32_t end = (base + size + 0xFFFu) & ~0xFFFu;
    for (uint32_t addr = start; addr < end; addr += 0x1000u) {
        map_page(page_directory, addr, addr, PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT);
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

static bool xhci_legacy_handoff(xhci_ctrl_t* x, uint32_t hcc1) {
    uint32_t xecp = (hcc1 >> 16) & 0xFFFFu;
    if (xecp == 0) return true;

    for (int hops = 0; hops < 64 && xecp >= 0x40; hops++) {
        volatile uint32_t* cap = (volatile uint32_t*)(x->base + (xecp * 4u));
        uint32_t v = *cap;
        uint8_t cap_id = (uint8_t)(v & 0xFFu);
        uint32_t next = (v >> 8) & 0xFFu;

        if (cap_id == 0x01) { // USB Legacy Support Capability
            const uint32_t BIOS_OWNED = (1u << 16);
            const uint32_t OS_OWNED = (1u << 24);

            uint32_t legsup = v;
            if ((legsup & OS_OWNED) == 0) {
                *cap = legsup | OS_OWNED;
                legsup |= OS_OWNED;
            }

            if (legsup & BIOS_OWNED) {
                for (int i = 0; i < 500; i++) {
                    delay_ms(1);
                    legsup = *cap;
                    if ((legsup & BIOS_OWNED) == 0) break;
                }
            }

            legsup = *cap;
            if (legsup & BIOS_OWNED) {
                kprint("[xHCI] BIOS owned semaphore stuck; skipping controller\n");
                return false;
            }

            // Disable legacy SMI generation (best-effort).
            cap[1] = 0;
            return true;
        }

        if (next == 0 || next == xecp) break;
        xecp = next;
    }
    return true;
}

// Capability regs offsets
#define XHCI_CAPLENGTH    0x00
#define XHCI_HCSPARAMS1   0x04
#define XHCI_HCSPARAMS2   0x08
#define XHCI_HCCPARAMS1   0x10
#define XHCI_DBOFF        0x14
#define XHCI_RTSOFF       0x18

// Operational regs offsets (from op base)
#define XHCI_USBCMD       0x00
#define XHCI_USBSTS       0x04
#define XHCI_PAGESIZE     0x08
#define XHCI_CRCR         0x18
#define XHCI_DCBAAP       0x30
#define XHCI_CONFIG       0x38
#define XHCI_PORTSC(n)    (0x400u + ((uint32_t)(n) * 0x10u))

// Runtime regs offsets (from rt base)
#define XHCI_IR0_BASE     0x20
#define XHCI_IMAN         0x00
#define XHCI_IMOD         0x04
#define XHCI_ERSTSZ       0x08
#define XHCI_ERSTBA       0x10
#define XHCI_ERDP         0x18

// USBCMD bits
#define CMD_RS            (1u << 0)
#define CMD_HCRST         (1u << 1)
#define CMD_INTE          (1u << 2)

// USBSTS bits
#define STS_HCH           (1u << 0)
#define STS_CNR           (1u << 11)

// PORTSC bits (subset)
#define PORT_CCS          (1u << 0)
#define PORT_PED          (1u << 1)
#define PORT_PR           (1u << 4)
#define PORT_PP           (1u << 9)
#define PORT_SPEED_SHIFT  10
#define PORT_CSC          (1u << 17)
#define PORT_PEC          (1u << 18)
#define PORT_WRC          (1u << 19)
#define PORT_OCC          (1u << 20)
#define PORT_PRC          (1u << 21)
#define PORT_PLC          (1u << 22)
#define PORT_CEC          (1u << 23)
#define PORT_CHANGE_BITS  (PORT_CSC | PORT_PEC | PORT_WRC | PORT_OCC | PORT_PRC | PORT_PLC | PORT_CEC)

// TRB bits
#define TRB_CYCLE         (1u << 0)
#define TRB_CHAIN         (1u << 4)
#define TRB_IOC           (1u << 5)
#define TRB_IDT           (1u << 6)
#define TRB_TYPE_SHIFT    10
#define TRB_TYPE_MASK     (0x3Fu << TRB_TYPE_SHIFT)

#define TRB_TYPE_NORMAL         1
#define TRB_TYPE_SETUP_STAGE    2
#define TRB_TYPE_DATA_STAGE     3
#define TRB_TYPE_STATUS_STAGE   4
#define TRB_TYPE_LINK           6
#define TRB_TYPE_ENABLE_SLOT    9
#define TRB_TYPE_DISABLE_SLOT   10
#define TRB_TYPE_ADDRESS_DEVICE 11
#define TRB_TYPE_CONFIG_EP      12

#define TRB_TYPE_TRANSFER_EVENT 32
#define TRB_TYPE_CMD_CMPLT_EVT  33

// Completion codes (subset)
#define CC_SUCCESS 1
#define CC_SHORT_PACKET 13

static inline uint32_t trb_type(uint32_t control) {
    return (control >> TRB_TYPE_SHIFT) & 0x3Fu;
}

static void ring_init(xhci_ring_t* r, uint32_t trb_count) {
    memset(r, 0, sizeof(*r));
    r->trb_count = trb_count;
    // xHCI rings are DMA'd by the controller and must be physically contiguous.
    // Our heap maps pages on-demand and may not give contiguous physical pages, so keep
    // rings page-aligned to ensure a 4K ring stays within a single physical page.
    r->trbs = (xhci_trb_t*)kmalloc_aligned(sizeof(xhci_trb_t) * trb_count, 0x1000);
    memset(r->trbs, 0, sizeof(xhci_trb_t) * trb_count);
    r->trbs_phys = phys_addr32(r->trbs);
    r->enqueue = 0;
    r->cycle = 1;

    // Link TRB at end -> start, with TC=1 (bit1) so HW toggles cycle at wrap.
    xhci_trb_t* link = &r->trbs[trb_count - 1];
    uint64_t ptr = (uint64_t)r->trbs_phys;
    link->param_lo = (uint32_t)(ptr & 0xFFFFFFFFu);
    link->param_hi = (uint32_t)(ptr >> 32);
    link->status = 0;
    link->control = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | (1u << 1) | TRB_CYCLE;
}

static void event_ring_init(xhci_ring_t* r, uint32_t trb_count) {
    memset(r, 0, sizeof(*r));
    r->trb_count = trb_count;
    // Same DMA contiguity requirement as transfer rings.
    r->trbs = (xhci_trb_t*)kmalloc_aligned(sizeof(xhci_trb_t) * trb_count, 0x1000);
    memset(r->trbs, 0, sizeof(xhci_trb_t) * trb_count);
    r->trbs_phys = phys_addr32(r->trbs);
    r->enqueue = 0; // dequeue index
    r->cycle = 1;
}

static uint64_t ring_enqueue_trb(xhci_ring_t* r, const xhci_trb_t* in, bool ioc, bool chain) {
    if (!r || !r->trbs || r->trb_count < 2) return 0;

    uint32_t idx = r->enqueue;
    if (idx >= r->trb_count - 1) {
        idx = 0;
        r->enqueue = 0;
    }

    // Keep a stable copy of the current Producer Cycle State (PCS) for this TRB.
    // The Link TRB at the end of the ring must use the same PCS for the hardware to
    // consume it and toggle the Consumer Cycle State (CCS). Do not write the post-toggle
    // PCS into the Link TRB, or the ring can stall at wraparound.
    uint8_t pcs = r->cycle;

    xhci_trb_t trb = *in;
    trb.control &= ~TRB_CYCLE;
    trb.control |= (pcs ? TRB_CYCLE : 0);
    if (ioc) trb.control |= TRB_IOC;
    if (chain) trb.control |= TRB_CHAIN;

    r->trbs[idx] = trb;

    uint64_t trb_phys = (uint64_t)r->trbs_phys + (uint64_t)idx * sizeof(xhci_trb_t);

    idx++;
    if (idx >= r->trb_count - 1) {
        // Wrap: mark Link TRB valid for the current PCS, then advance to 0 and toggle PCS
        // (Link TRB has TC=1 so HW toggles CCS when it reaches it).
        r->enqueue = 0;
        r->trbs[r->trb_count - 1].control &= ~TRB_CYCLE;
        r->trbs[r->trb_count - 1].control |= (pcs ? TRB_CYCLE : 0);
        r->cycle ^= 1u;
    } else {
        r->enqueue = idx;
    }

    return trb_phys;
}

static volatile uint32_t* ir0_regs(xhci_ctrl_t* x) {
    return (volatile uint32_t*)((volatile uint8_t*)x->rt + XHCI_IR0_BASE);
}

static void xhci_update_erdp(xhci_ctrl_t* x) {
    volatile uint32_t* ir0 = ir0_regs(x);
    uint64_t ptr = (uint64_t)x->evt_ring.trbs_phys + (uint64_t)x->evt_ring.enqueue * sizeof(xhci_trb_t);
    mmio_wr64(ir0, XHCI_ERDP, ptr | 0x8u);
}

static void xhci_handle_transfer_event(xhci_ctrl_t* x, const xhci_trb_t* ev) {
    uint64_t ptr = ((uint64_t)ev->param_hi << 32) | ev->param_lo;
    uint32_t remaining = ev->status & 0x00FFFFFFu;
    uint8_t cc = (uint8_t)((ev->status >> 24) & 0xFFu);
    uint8_t slot_id = (uint8_t)((ev->control >> 24) & 0xFFu);

    if (x->wait_xfer.waiting && x->wait_xfer.expected_trb == ptr) {
        x->wait_xfer.waiting = false;
        x->wait_xfer.completion_code = cc;
        x->wait_xfer.remaining = remaining;
        x->wait_xfer.slot_id = slot_id;
        return;
    }

    for (xhci_async_t* a = x->async_list; a; a = a->next) {
        if (a->expected_trb == ptr) {
            a->status = (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) ? 1 : -1;
            uint32_t req = a->requested_len;
            if (remaining <= req) {
                uint32_t act = req - remaining;
                if (act > 0xFFFFu) act = 0xFFFFu;
                a->actual = (uint16_t)act;
            }
            return;
        }
    }
}

static void xhci_handle_cmd_complete(xhci_ctrl_t* x, const xhci_trb_t* ev) {
    uint64_t ptr = ((uint64_t)ev->param_hi << 32) | ev->param_lo;
    uint8_t cc = (uint8_t)((ev->status >> 24) & 0xFFu);
    uint8_t slot_id = (uint8_t)((ev->control >> 24) & 0xFFu);

    if (x->wait_cmd.waiting && x->wait_cmd.expected_trb == ptr) {
        x->wait_cmd.waiting = false;
        x->wait_cmd.completion_code = cc;
        x->wait_cmd.slot_id = slot_id;
    }
}

static void xhci_poll_events(xhci_ctrl_t* x) {
    if (!x || !x->evt_ring.trbs) return;

    while (1) {
        uint32_t idx = x->evt_ring.enqueue;
        xhci_trb_t* ev = &x->evt_ring.trbs[idx];
        uint8_t c = (uint8_t)(ev->control & 1u);
        if (c != x->evt_ring.cycle) break;

        uint32_t t = trb_type(ev->control);
        if (t == TRB_TYPE_TRANSFER_EVENT) xhci_handle_transfer_event(x, ev);
        else if (t == TRB_TYPE_CMD_CMPLT_EVT) xhci_handle_cmd_complete(x, ev);

        idx++;
        if (idx >= x->evt_ring.trb_count) {
            idx = 0;
            x->evt_ring.cycle ^= 1u;
        }
        x->evt_ring.enqueue = idx;
        xhci_update_erdp(x);
    }
}

static void xhci_arm_wait_cmd(xhci_ctrl_t* x, uint64_t expected_trb) {
    x->wait_cmd.waiting = true;
    x->wait_cmd.expected_trb = expected_trb;
    x->wait_cmd.completion_code = 0;
    x->wait_cmd.slot_id = 0;
}

static bool xhci_wait_cmd_armed(xhci_ctrl_t* x, uint8_t* out_slot_id) {
    if (!x || !x->wait_cmd.waiting) return false;

    uint32_t start = tick;
    while (x->wait_cmd.waiting) {
        xhci_poll_events(x);
        if ((tick - start) > 200) break;
    }
    if (x->wait_cmd.waiting) return false;
    if (out_slot_id) *out_slot_id = x->wait_cmd.slot_id;
    return x->wait_cmd.completion_code == CC_SUCCESS;
}

static void xhci_arm_wait_xfer(xhci_ctrl_t* x, uint64_t expected_trb) {
    x->wait_xfer.waiting = true;
    x->wait_xfer.expected_trb = expected_trb;
    x->wait_xfer.completion_code = 0;
    x->wait_xfer.remaining = 0;
    x->wait_xfer.slot_id = 0;
}

static bool xhci_wait_xfer_armed(xhci_ctrl_t* x, uint32_t* out_remaining) {
    if (!x || !x->wait_xfer.waiting) return false;

    uint32_t start = tick;
    while (x->wait_xfer.waiting) {
        xhci_poll_events(x);
        if ((tick - start) > 200) break;
    }
    if (x->wait_xfer.waiting) return false;
    if (out_remaining) *out_remaining = x->wait_xfer.remaining;
    return x->wait_xfer.completion_code == CC_SUCCESS ||
           x->wait_xfer.completion_code == CC_SHORT_PACKET;
}

static uint32_t xhci_speed_id(usb_speed_t s) {
    switch (s) {
    case USB_SPEED_LOW:   return 2;
    case USB_SPEED_HIGH:  return 3;
    case USB_SPEED_SUPER: return 4;
    case USB_SPEED_FULL:
    default:              return 1;
    }
}

static uint16_t xhci_default_ep0_mps(usb_speed_t s) {
    switch (s) {
    case USB_SPEED_LOW:
    case USB_SPEED_FULL:  return 8;
    case USB_SPEED_HIGH:  return 64;
    case USB_SPEED_SUPER: return 512;
    default:              return 8;
    }
}

static inline uint32_t* ctx_at(void* base, uint8_t ctx_size, uint32_t index) {
    return (uint32_t*)((uint8_t*)base + (uint32_t)ctx_size * index);
}

static void xhci_fill_slot_ctx(xhci_ctrl_t* x, xhci_dev_t* d, uint32_t* slot_ctx, uint8_t context_entries) {
    (void)x;
    memset(slot_ctx, 0, d->ctx_size);
    uint32_t speed = xhci_speed_id(d->speed);
    slot_ctx[0] = (speed << 20) | ((uint32_t)context_entries << 27);
    slot_ctx[1] = ((uint32_t)d->root_port << 16);
    slot_ctx[3] = d->usb_addr;
}

static void xhci_fill_ep_ctx(xhci_dev_t* d, uint32_t* ep_ctx, uint8_t ep_type, uint16_t mps,
	                            uint8_t interval, uint64_t tr_deq, uint8_t dcs) {
	    memset(ep_ctx, 0, d->ctx_size);
	    // dword0: Interval field in bits 16..23.
	    ep_ctx[0] = ((uint32_t)interval << 16);
	    // dword1: CErr in bits 1..2 (use 3 retries for non-isoch), ep type in bits 3..5,
	    // max packet size in bits 16..31
	    uint32_t cerr = 3u;
	    ep_ctx[1] = (cerr << 1) | ((uint32_t)ep_type << 3) | ((uint32_t)mps << 16);
    // TR Dequeue Pointer (bits 4..), low includes DCS in bit0
    uint64_t ptr = (tr_deq & ~0xFULL) | (uint64_t)(dcs & 1u);
    ep_ctx[2] = (uint32_t)(ptr & 0xFFFFFFFFu);
    ep_ctx[3] = (uint32_t)(ptr >> 32);
    // average TRB length
	    ep_ctx[4] = mps;
}

static uint8_t xhci_encode_interval(usb_speed_t speed, usb_ep_type_t type, uint8_t bInterval) {
    // xHCI Endpoint Context Interval encoding depends on device speed.
    // We approximate using the standard guidance:
    // - HS/SS periodic endpoints: Interval = bInterval - 1 (bInterval is 1..16)
    // - FS/LS periodic endpoints: Interval is log2(bInterval) + 3 (1 frame = 8 microframes)
    if (type != USB_EP_INTERRUPT && type != USB_EP_ISOCH) return 0;
    if (bInterval == 0) bInterval = 1;

    if (speed == USB_SPEED_HIGH || speed == USB_SPEED_SUPER) {
        if (bInterval > 16) bInterval = 16;
        return (uint8_t)(bInterval - 1);
    }

    // FS/LS: floor(log2(bInterval)) + 3
    uint8_t v = bInterval;
    uint8_t log2 = 0;
    while (v > 1) {
        v >>= 1;
        log2++;
    }
    uint8_t interval = (uint8_t)(log2 + 3);
    // Clamp to a reasonable range (field is 8-bit but hardware commonly supports up to 0..15).
    if (interval > 15) interval = 15;
    return interval;
}

static uint8_t xhci_ep_type_code(usb_ep_type_t type, bool in) {
	    switch (type) {
	    case USB_EP_CONTROL: return 4;
	    case USB_EP_BULK: return in ? 6 : 2;
    case USB_EP_INTERRUPT: return in ? 7 : 3;
    case USB_EP_ISOCH: return in ? 5 : 1;
    default: return 0;
    }
}

static uint8_t xhci_dci_for_ep(uint8_t ep, bool in) {
    if (ep == 0) return 1;
    return (uint8_t)(2u * ep + (in ? 1u : 0u));
}

static xhci_dev_t* xhci_get_dev(xhci_ctrl_t* x, uint32_t dev_handle) {
    uint8_t slot = (uint8_t)dev_handle;
    if (slot == 0 || slot > x->max_slots) return NULL;
    if (!x->devs[slot].used) return NULL;
    return &x->devs[slot];
}

static int xhci_find_slot_by_port(xhci_ctrl_t* x, uint8_t root_port) {
    for (uint32_t s = 1; s <= x->max_slots; s++) {
        if (x->devs[s].used && x->devs[s].root_port == root_port)
            return (int)s;
    }
    return 0;
}

static void xhci_async_cancel_slot(xhci_ctrl_t* x, uint8_t slot_id) {
    if (!x) return;
    xhci_async_t** pp = &x->async_list;
    while (*pp) {
        xhci_async_t* cur = *pp;
        if (cur->slot_id == slot_id) {
            cur->status = -1;
            *pp = cur->next;
            continue;
        }
        pp = &(*pp)->next;
    }
}

static bool xhci_submit_cmd(xhci_ctrl_t* x, const xhci_trb_t* trb, uint8_t* out_slot_id) {
    uint64_t phys = ring_enqueue_trb(&x->cmd_ring, trb, false, false);
    if (!phys) return false;

    // Arm wait before ringing the doorbell so polling from the timer ISR can't
    // consume the completion event and lose it.
    xhci_arm_wait_cmd(x, phys);

    // Ring doorbell 0 (command ring)
    x->db[0] = 0;
    return xhci_wait_cmd_armed(x, out_slot_id);
}

static bool xhci_cmd_enable_slot(xhci_ctrl_t* x, uint8_t* out_slot_id) {
    xhci_trb_t trb;
    memset(&trb, 0, sizeof(trb));
    trb.control = (TRB_TYPE_ENABLE_SLOT << TRB_TYPE_SHIFT);
    return xhci_submit_cmd(x, &trb, out_slot_id);
}

static bool xhci_cmd_disable_slot(xhci_ctrl_t* x, uint8_t slot_id) {
    xhci_trb_t trb;
    memset(&trb, 0, sizeof(trb));
    trb.control = (TRB_TYPE_DISABLE_SLOT << TRB_TYPE_SHIFT) |
                  ((uint32_t)slot_id << 24);
    uint8_t got_slot = 0;
    return xhci_submit_cmd(x, &trb, &got_slot) && got_slot == slot_id;
}

static bool xhci_cmd_address_device(xhci_ctrl_t* x, uint8_t slot_id, uint32_t ic_phys, bool bsr) {
    xhci_trb_t trb;
    memset(&trb, 0, sizeof(trb));
    trb.param_lo = ic_phys;
    trb.param_hi = 0;
    trb.control = (TRB_TYPE_ADDRESS_DEVICE << TRB_TYPE_SHIFT) |
                  (bsr ? (1u << 9) : 0) |
                  ((uint32_t)slot_id << 24);
    uint8_t got_slot = 0;
    return xhci_submit_cmd(x, &trb, &got_slot) && got_slot == slot_id;
}

static bool xhci_cmd_configure_ep(xhci_ctrl_t* x, uint8_t slot_id, uint32_t ic_phys) {
    xhci_trb_t trb;
    memset(&trb, 0, sizeof(trb));
    trb.param_lo = ic_phys;
    trb.param_hi = 0;
    trb.control = (TRB_TYPE_CONFIG_EP << TRB_TYPE_SHIFT) |
                  ((uint32_t)slot_id << 24);
    uint8_t got_slot = 0;
    return xhci_submit_cmd(x, &trb, &got_slot) && got_slot == slot_id;
}

static void xhci_release_slot(xhci_ctrl_t* x, uint8_t slot_id) {
    if (!x || slot_id == 0 || slot_id > x->max_slots) return;
    xhci_dev_t* d = &x->devs[slot_id];
    if (!d->used) return;

    (void)xhci_cmd_disable_slot(x, slot_id);
    xhci_async_cancel_slot(x, slot_id);
    usb_hid_drop_device(&x->usbhc, slot_id);
    usb_storage_drop_device(&x->usbhc, slot_id);
    hid_boot_kbd_drop_device(&x->usbhc, slot_id);

    x->dcbaa[slot_id * 2] = 0;
    x->dcbaa[slot_id * 2 + 1] = 0;

    for (int i = 0; i < XHCI_MAX_DCI; i++) {
        if (d->ep_rings[i].trbs) {
            kfree(d->ep_rings[i].trbs);
        }
        memset(&d->ep_rings[i], 0, sizeof(d->ep_rings[i]));
    }

    if (d->dc) kfree(d->dc);
    if (d->ic) kfree(d->ic);

    memset(d, 0, sizeof(*d));
}

static bool xhci_ring_transfer(xhci_ctrl_t* x, xhci_dev_t* d, uint8_t dci,
                               xhci_ring_t* ring, uint8_t trb_type,
                               bool in_dir, const void* setup8,
                               void* data, uint16_t len,
                               uint32_t* out_actual_len) {
    (void)setup8;
    if (!x || !d || !ring) return false;
    if (!ring->trbs) return false;

    uint32_t remaining = 0;
    uint64_t last_trb_phys = 0;

    if (trb_type == TRB_TYPE_NORMAL) {
        // Split at page boundaries (best-effort).
        uint32_t off = 0;
        while (off < len) {
            uint32_t virt = (uint32_t)((uint8_t*)data + off);
            uint32_t phys;
            if (vmm_virt_to_phys(virt, &phys) != 0) phys = virt;
            uint32_t page_off = phys & 0xFFFu;
            uint32_t chunk = 0x1000u - page_off;
            uint32_t left = (uint32_t)len - off;
            if (chunk > left) chunk = left;

            xhci_trb_t trb;
            memset(&trb, 0, sizeof(trb));
            trb.param_lo = phys;
            trb.param_hi = 0;
            trb.status = (chunk & 0x1FFFFu);
            trb.control = (TRB_TYPE_NORMAL << TRB_TYPE_SHIFT);

            bool is_last = (off + chunk) >= len;
            last_trb_phys = ring_enqueue_trb(ring, &trb, is_last, !is_last);
            if (!last_trb_phys) return false;

            off += chunk;
        }
    } else if (trb_type == TRB_TYPE_SETUP_STAGE) {
        xhci_trb_t trb;
        memset(&trb, 0, sizeof(trb));
        // Immediate data setup packet (8 bytes)
        const uint32_t* s = (const uint32_t*)setup8;
        trb.param_lo = s[0];
        trb.param_hi = s[1];
        trb.status = 8;
        uint32_t trt = 0;
        if (len == 0) trt = 0;
        else trt = in_dir ? 3u : 2u;
        trb.control = (TRB_TYPE_SETUP_STAGE << TRB_TYPE_SHIFT) | TRB_IDT | (trt << 16);
        last_trb_phys = ring_enqueue_trb(ring, &trb, false, true);
    } else if (trb_type == TRB_TYPE_DATA_STAGE) {
        xhci_trb_t trb;
        memset(&trb, 0, sizeof(trb));
        uint32_t phys = phys_addr32(data);
        trb.param_lo = phys;
        trb.param_hi = 0;
        trb.status = len;
        trb.control = (TRB_TYPE_DATA_STAGE << TRB_TYPE_SHIFT) | (in_dir ? (1u << 16) : 0);
        last_trb_phys = ring_enqueue_trb(ring, &trb, false, true);
    } else if (trb_type == TRB_TYPE_STATUS_STAGE) {
        xhci_trb_t trb;
        memset(&trb, 0, sizeof(trb));
        bool status_in = (len == 0) ? true : !in_dir;
        trb.control = (TRB_TYPE_STATUS_STAGE << TRB_TYPE_SHIFT) | (status_in ? (1u << 16) : 0);
        last_trb_phys = ring_enqueue_trb(ring, &trb, true, false);
    }

    if (!last_trb_phys) return false;

    // Ring doorbell for slot with target = DCI
    // Arm wait before ringing the doorbell to avoid a race with usb_poll() calling
    // xhci_poll_events() from the timer ISR.
    xhci_arm_wait_xfer(x, last_trb_phys);

    x->db[d->slot_id] = dci;

    if (!xhci_wait_xfer_armed(x, &remaining)) return false;
    if (out_actual_len) {
        uint32_t actual = (uint32_t)len;
        if (remaining <= actual) actual -= remaining;
        *out_actual_len = actual;
    }
    return true;
}

static bool xhci_usbhc_control_transfer(usb_hc_t* hc, uint32_t dev, uint8_t ep,
                                       uint16_t mps, usb_speed_t speed,
                                       uint8_t tt_hub_addr, uint8_t tt_port,
                                       const void* setup8, void* data, uint16_t len) {
    (void)ep;
    (void)mps;
    (void)speed;
    (void)tt_hub_addr;
    (void)tt_port;
    xhci_ctrl_t* x = (xhci_ctrl_t*)hc->impl;
    xhci_dev_t* d = xhci_get_dev(x, dev);
    if (!d) return false;

    uint8_t bmRequestType = ((const uint8_t*)setup8)[0];
    bool in_dir = (bmRequestType & 0x80u) != 0;

    xhci_ring_t* ring = &d->ep_rings[1]; // DCI 1 (control)
    if (!ring->trbs) return false;

    // Queue Setup + optional Data + Status, then ring once and wait for Status completion.
    uint64_t last_trb_phys = 0;

    xhci_trb_t setup_trb;
    memset(&setup_trb, 0, sizeof(setup_trb));
    const uint32_t* s = (const uint32_t*)setup8;
    setup_trb.param_lo = s[0];
    setup_trb.param_hi = s[1];
    setup_trb.status = 8;
    uint32_t trt = 0;
    if (len == 0) trt = 0;
    else trt = in_dir ? 3u : 2u;
    setup_trb.control = (TRB_TYPE_SETUP_STAGE << TRB_TYPE_SHIFT) | TRB_IDT | (trt << 16);
    last_trb_phys = ring_enqueue_trb(ring, &setup_trb, false, true);
    if (!last_trb_phys) return false;

    if (len && data) {
        // Best-effort: split at page boundaries using multiple Data Stage TRBs.
        uint32_t off = 0;
        while (off < len) {
            uint32_t virt = (uint32_t)((uint8_t*)data + off);
            uint32_t phys;
            if (vmm_virt_to_phys(virt, &phys) != 0) phys = virt;
            uint32_t page_off = phys & 0xFFFu;
            uint32_t chunk = 0x1000u - page_off;
            uint32_t left = (uint32_t)len - off;
            if (chunk > left) chunk = left;

            xhci_trb_t data_trb;
            memset(&data_trb, 0, sizeof(data_trb));
            data_trb.param_lo = phys;
            data_trb.param_hi = 0;
            data_trb.status = chunk;
            data_trb.control = (TRB_TYPE_DATA_STAGE << TRB_TYPE_SHIFT) | (in_dir ? (1u << 16) : 0);

            bool last_data = (off + chunk) >= len;
            last_trb_phys = ring_enqueue_trb(ring, &data_trb, false, true);
            if (!last_trb_phys) return false;
            off += chunk;
            (void)last_data;
        }
    }

    xhci_trb_t status_trb;
    memset(&status_trb, 0, sizeof(status_trb));
    bool status_in = (len == 0) ? true : !in_dir;
    status_trb.control = (TRB_TYPE_STATUS_STAGE << TRB_TYPE_SHIFT) | (status_in ? (1u << 16) : 0);
    last_trb_phys = ring_enqueue_trb(ring, &status_trb, true, false);
    if (!last_trb_phys) return false;

    // Arm wait before ringing the doorbell to avoid losing the completion event if
    // usb_poll() polls xHCI events concurrently.
    xhci_arm_wait_xfer(x, last_trb_phys);
    x->db[d->slot_id] = 1;
    return xhci_wait_xfer_armed(x, NULL);
}

static bool xhci_usbhc_bulk_transfer(usb_hc_t* hc, uint32_t dev, uint8_t ep, bool in,
                                     uint16_t mps, usb_speed_t speed,
                                     uint8_t tt_hub_addr, uint8_t tt_port,
                                     uint8_t start_toggle,
                                     void* data, uint16_t len) {
    (void)mps;
    (void)speed;
    (void)tt_hub_addr;
    (void)tt_port;
    (void)start_toggle;
    xhci_ctrl_t* x = (xhci_ctrl_t*)hc->impl;
    xhci_dev_t* d = xhci_get_dev(x, dev);
    if (!d) return false;

    uint8_t dci = xhci_dci_for_ep(ep, in);
    if (dci >= XHCI_MAX_DCI) return false;
    xhci_ring_t* ring = &d->ep_rings[dci];
    if (!ring->trbs) return false;

    uint32_t actual = 0;
    return xhci_ring_transfer(x, d, dci, ring, TRB_TYPE_NORMAL, in, NULL, data, len, &actual);
}

static bool xhci_usbhc_async_in_init(usb_hc_t* hc, usb_async_in_t* x,
                                     uint32_t dev, uint8_t ep, uint16_t mps,
                                     usb_speed_t speed,
                                     uint8_t tt_hub_addr, uint8_t tt_port,
                                     uint8_t start_toggle,
                                     void* buf, uint16_t len) {
    (void)mps;
    (void)speed;
    (void)tt_hub_addr;
    (void)tt_port;
    (void)start_toggle;
    if (!hc || !hc->impl || !x) return false;
    xhci_ctrl_t* ctrl = (xhci_ctrl_t*)hc->impl;
    xhci_dev_t* d = xhci_get_dev(ctrl, dev);
    if (!d) return false;

    uint8_t dci = xhci_dci_for_ep(ep, true);
    if (dci >= XHCI_MAX_DCI) return false;
    xhci_ring_t* ring = &d->ep_rings[dci];
    if (!ring->trbs) return false;

    xhci_async_t* a = (xhci_async_t*)kmalloc(sizeof(*a), 0, NULL);
    if (!a) return false;
    memset(a, 0, sizeof(*a));
    a->ctrl = ctrl;
    a->slot_id = d->slot_id;
    a->dci = dci;
    a->status = 0;
    a->buf_phys = phys_addr32(buf);
    a->requested_len = len;
    a->actual = 0;

    // queue one normal TRB (IOC)
    xhci_trb_t trb;
    memset(&trb, 0, sizeof(trb));
    trb.param_lo = a->buf_phys;
    trb.param_hi = 0;
    trb.status = len;
    trb.control = (TRB_TYPE_NORMAL << TRB_TYPE_SHIFT);
    a->expected_trb = ring_enqueue_trb(ring, &trb, true, false);
    if (!a->expected_trb) return false;

    // insert into async list
    a->next = ctrl->async_list;
    ctrl->async_list = a;

    ctrl->db[d->slot_id] = dci;
    x->hc = hc;
    x->impl = a;
    return true;
}

static int xhci_usbhc_async_in_check(usb_async_in_t* x, uint16_t* out_actual) {
    if (!x || !x->impl) return -1;
    xhci_async_t* a = (xhci_async_t*)x->impl;
    xhci_poll_events(a->ctrl);
    if (a->status == 0) return 0;
    if (a->status < 0) return -1;
    if (out_actual) *out_actual = a->actual;
    return 1;
}

static void xhci_usbhc_async_in_rearm(usb_async_in_t* x) {
    if (!x || !x->impl) return;
    xhci_async_t* a = (xhci_async_t*)x->impl;
    xhci_ctrl_t* ctrl = a->ctrl;
    xhci_dev_t* d = &ctrl->devs[a->slot_id];
    if (!d->used) return;
    xhci_ring_t* ring = &d->ep_rings[a->dci];
    if (!ring->trbs) return;

    // Re-queue a fresh IN transfer to the same buffer address as last time.
    a->status = 0;
    a->actual = 0;

    xhci_trb_t trb;
    memset(&trb, 0, sizeof(trb));
    trb.param_lo = a->buf_phys;
    trb.param_hi = 0;
    trb.status = a->requested_len;
    trb.control = (TRB_TYPE_NORMAL << TRB_TYPE_SHIFT);

    a->expected_trb = ring_enqueue_trb(ring, &trb, true, false);
    if (!a->expected_trb) {
        a->status = -1;
        return;
    }
    ctrl->db[d->slot_id] = a->dci;
}

static void xhci_usbhc_async_in_cancel(usb_async_in_t* x) {
    if (!x || !x->impl) return;
    xhci_async_t* a = (xhci_async_t*)x->impl;
    xhci_ctrl_t* ctrl = a->ctrl;
    xhci_async_t** pp = &ctrl->async_list;
    while (*pp) {
        if (*pp == a) {
            *pp = a->next;
            break;
        }
        pp = &(*pp)->next;
    }
    a->status = -1;
    x->impl = NULL;
}

static bool xhci_usbhc_configure_endpoint(usb_hc_t* hc, uint32_t dev,
	                                          uint8_t ep, bool in,
	                                          usb_ep_type_t type,
	                                          uint16_t mps, uint8_t interval) {
	    xhci_ctrl_t* x = (xhci_ctrl_t*)hc->impl;
	    xhci_dev_t* d = xhci_get_dev(x, dev);
	    if (!d) return false;

    uint8_t dci = xhci_dci_for_ep(ep, in);
    if (dci >= XHCI_MAX_DCI) return false;
    if (!d->ep_rings[dci].trbs) {
        ring_init(&d->ep_rings[dci], 256);
    }

    uint32_t* icc = ctx_at(d->ic, d->ctx_size, 0);
    uint32_t* islot = ctx_at(d->ic, d->ctx_size, 1);
    uint32_t* iep = ctx_at(d->ic, d->ctx_size, 1u + dci);
    memset(d->ic, 0, (uint32_t)d->ctx_size * 33u);

    // Drop=0, Add flags
    icc[0] = 0;
    icc[1] = (1u << 0) | (1u << dci);

	    if (dci > d->context_entries) d->context_entries = dci;
	    xhci_fill_slot_ctx(x, d, islot, d->context_entries);

	    uint8_t ep_type = xhci_ep_type_code(type, in);
	    uint8_t enc_interval = xhci_encode_interval(d->speed, type, interval);
	    xhci_fill_ep_ctx(d, iep, ep_type, mps, enc_interval,
	                     (uint64_t)d->ep_rings[dci].trbs_phys, d->ep_rings[dci].cycle);

	    return xhci_cmd_configure_ep(x, d->slot_id, d->ic_phys);
}

static bool xhci_usbhc_enum_open(usb_hc_t* hc, uint8_t root_port, usb_speed_t speed,
                                 uint32_t* out_dev) {
    if (!hc || !hc->impl || !out_dev) return false;
    if (root_port == 0) return false; // hubs not supported yet
    xhci_ctrl_t* x = (xhci_ctrl_t*)hc->impl;

    uint8_t slot_id = 0;
    if (!xhci_cmd_enable_slot(x, &slot_id)) return false;
    if (slot_id == 0 || slot_id > x->max_slots) return false;

    xhci_dev_t* d = &x->devs[slot_id];
    memset(d, 0, sizeof(*d));
    d->used = true;
    d->slot_id = slot_id;
    d->root_port = root_port;
    d->speed = speed;
    d->usb_addr = 0;
    d->ctx_size = x->ctx_size;
    d->context_entries = 1;

    // Device/Input contexts are DMA'd; keep them within a single physical page.
    d->dc = kmalloc_aligned((uint32_t)d->ctx_size * 32u, 0x1000);
    d->ic = kmalloc_aligned((uint32_t)d->ctx_size * 33u, 0x1000);
    if (!d->dc || !d->ic) return false;
    memset(d->dc, 0, (uint32_t)d->ctx_size * 32u);
    memset(d->ic, 0, (uint32_t)d->ctx_size * 33u);
    d->dc_phys = phys_addr32(d->dc);
    d->ic_phys = phys_addr32(d->ic);

    // Update DCBAA slot pointer.
    x->dcbaa[slot_id * 2] = d->dc_phys;
    x->dcbaa[slot_id * 2 + 1] = 0;

    // EP0 ring
    ring_init(&d->ep_rings[1], 256);

    uint32_t* icc = ctx_at(d->ic, d->ctx_size, 0);
    uint32_t* islot = ctx_at(d->ic, d->ctx_size, 1);
    uint32_t* iep0 = ctx_at(d->ic, d->ctx_size, 2);

    icc[0] = 0;
    icc[1] = (1u << 0) | (1u << 1);

    xhci_fill_slot_ctx(x, d, islot, 1);

    uint16_t mps = xhci_default_ep0_mps(speed);
    xhci_fill_ep_ctx(d, iep0, 4, mps, 0, (uint64_t)d->ep_rings[1].trbs_phys, d->ep_rings[1].cycle);

    if (!xhci_cmd_address_device(x, slot_id, d->ic_phys, true)) return false;

    *out_dev = slot_id;
    return true;
}

static bool xhci_usbhc_enum_set_address(usb_hc_t* hc, uint32_t dev_default, uint8_t ep0_mps,
                                        usb_speed_t speed, uint8_t tt_hub_addr, uint8_t tt_port,
                                        uint8_t desired_addr, uint32_t* inout_dev) {
    (void)speed;
    (void)tt_hub_addr;
    (void)tt_port;
    if (!hc || !hc->impl || !inout_dev) return false;
    xhci_ctrl_t* x = (xhci_ctrl_t*)hc->impl;
    xhci_dev_t* d = xhci_get_dev(x, dev_default);
    if (!d) return false;

    d->usb_addr = desired_addr;

    // Update EP0 MPS, then Address Device (BSR=0) to move to Addressed state.
    memset(d->ic, 0, (uint32_t)d->ctx_size * 33u);
    uint32_t* icc = ctx_at(d->ic, d->ctx_size, 0);
    uint32_t* islot = ctx_at(d->ic, d->ctx_size, 1);
    uint32_t* iep0 = ctx_at(d->ic, d->ctx_size, 2);

    icc[0] = 0;
    icc[1] = (1u << 0) | (1u << 1);
    xhci_fill_slot_ctx(x, d, islot, 1);
    xhci_fill_ep_ctx(d, iep0, 4, ep0_mps, 0, (uint64_t)d->ep_rings[1].trbs_phys, d->ep_rings[1].cycle);

    if (!xhci_cmd_address_device(x, d->slot_id, d->ic_phys, false)) return false;
    *inout_dev = d->slot_id;
    return true;
}

static void xhci_usbhc_enum_close(usb_hc_t* hc, uint32_t dev) {
    (void)hc;
    (void)dev;
    // Not implemented (slots remain allocated).
}

static uint8_t xhci_usbhc_alloc_address(usb_hc_t* hc) {
    xhci_ctrl_t* x = (xhci_ctrl_t*)hc->impl;
    if (!x) return 0;
    if (x->next_addr == 0 || x->next_addr >= 127) return 0;
    return x->next_addr++;
}

static void xhci_usbhc_reset_address_allocator(usb_hc_t* hc) {
    xhci_ctrl_t* x = (xhci_ctrl_t*)hc->impl;
    if (!x) return;
    x->next_addr = 1;
}

static const usb_hc_ops_t xhci_usbhc_ops = {
    .control_transfer = xhci_usbhc_control_transfer,
    .bulk_transfer = xhci_usbhc_bulk_transfer,
    .async_in_init = xhci_usbhc_async_in_init,
    .async_in_check = xhci_usbhc_async_in_check,
    .async_in_rearm = xhci_usbhc_async_in_rearm,
    .async_in_cancel = xhci_usbhc_async_in_cancel,
    .configure_endpoint = xhci_usbhc_configure_endpoint,
    .enum_open = xhci_usbhc_enum_open,
    .enum_set_address = xhci_usbhc_enum_set_address,
    .enum_close = xhci_usbhc_enum_close,
    .alloc_address = xhci_usbhc_alloc_address,
    .reset_address_allocator = xhci_usbhc_reset_address_allocator,
};

static bool xhci_reset_controller(xhci_ctrl_t* x) {
    // Stop controller
    uint32_t cmd = mmio_rd32(x->op, XHCI_USBCMD);
    cmd &= ~CMD_RS;
    mmio_wr32(x->op, XHCI_USBCMD, cmd);
    for (int i = 0; i < 200; i++) {
        if (mmio_rd32(x->op, XHCI_USBSTS) & STS_HCH) break;
        delay_ms(1);
    }

    // Reset
    cmd = mmio_rd32(x->op, XHCI_USBCMD);
    cmd |= CMD_HCRST;
    mmio_wr32(x->op, XHCI_USBCMD, cmd);
    for (int i = 0; i < 500; i++) {
        if ((mmio_rd32(x->op, XHCI_USBCMD) & CMD_HCRST) == 0) break;
        delay_ms(1);
    }

    for (int i = 0; i < 200; i++) {
        if (mmio_rd32(x->op, XHCI_USBSTS) & STS_HCH) break;
        delay_ms(1);
    }

    for (int i = 0; i < 200; i++) {
        if ((mmio_rd32(x->op, XHCI_USBSTS) & STS_CNR) == 0) break;
        delay_ms(1);
    }
    return true;
}

static usb_speed_t xhci_usb_speed_from_portsc(uint32_t ps) {
    uint32_t spd = (ps >> PORT_SPEED_SHIFT) & 0xFu;
    switch (spd) {
    case 1: return USB_SPEED_FULL;
    case 2: return USB_SPEED_LOW;
    case 3: return USB_SPEED_HIGH;
    case 4: return USB_SPEED_SUPER;
    case 5: return USB_SPEED_SUPER; // treat SS+ as SuperSpeed for now
    default: return USB_SPEED_FULL;
    }
}

static void xhci_clear_port_changes(xhci_ctrl_t* x, uint32_t p) {
    uint32_t ps = mmio_rd32(x->op, XHCI_PORTSC(p));
    mmio_wr32(x->op, XHCI_PORTSC(p), ps | PORT_CHANGE_BITS);
}

static bool xhci_reset_port(xhci_ctrl_t* x, uint32_t p, uint32_t* out_ps) {
    uint32_t ps = mmio_rd32(x->op, XHCI_PORTSC(p));

    if ((ps & PORT_PP) == 0) {
        mmio_wr32(x->op, XHCI_PORTSC(p), ps | PORT_PP | PORT_CHANGE_BITS);
        delay_ms(20);
        ps = mmio_rd32(x->op, XHCI_PORTSC(p));
    }

    if ((ps & PORT_CCS) == 0) {
        if (out_ps) *out_ps = ps;
        return false;
    }

    xhci_clear_port_changes(x, p);

    for (int attempt = 0; attempt < 2; attempt++) {
        mmio_wr32(x->op, XHCI_PORTSC(p), ps | PORT_PR | PORT_CHANGE_BITS);
        for (int i = 0; i < 100; i++) {
            delay_ms(5);
            ps = mmio_rd32(x->op, XHCI_PORTSC(p));
            if ((ps & PORT_PR) == 0) break;
        }

        for (int i = 0; i < 100; i++) {
            ps = mmio_rd32(x->op, XHCI_PORTSC(p));
            if (ps & PORT_PED) break;
            delay_ms(5);
        }

        if ((ps & PORT_CCS) && (ps & PORT_PED)) {
            if (out_ps) *out_ps = ps;
            return true;
        }

        xhci_clear_port_changes(x, p);
    }

    if (out_ps) *out_ps = ps;
    return false;
}

static void xhci_scan_ports(xhci_ctrl_t* x, bool verbose) {
    if (verbose) kprintf("[xHCI] Root hub ports=%u\n", x->max_ports);

    for (uint32_t p = 0; p < x->max_ports; p++) {
        uint8_t root_port = (uint8_t)(p + 1);
        int slot = xhci_find_slot_by_port(x, root_port);
        uint32_t ps = mmio_rd32(x->op, XHCI_PORTSC(p));
        bool conn_change = (ps & PORT_CSC) != 0;

        if ((ps & PORT_CCS) == 0) {
            if (slot) {
                if (verbose) kprintf("[xHCI] Port %u disconnected\n", p + 1);
                xhci_release_slot(x, (uint8_t)slot);
            }
            xhci_clear_port_changes(x, p);
            continue;
        }

        if (slot) {
            if (conn_change) {
                if (verbose) kprintf("[xHCI] Port %u connection change\n", p + 1);
                xhci_release_slot(x, (uint8_t)slot);
                slot = 0;
            } else {
                xhci_clear_port_changes(x, p);
                continue;
            }
        }

        if (slot) {
            xhci_clear_port_changes(x, p);
            continue;
        }

        ps = 0;
        if (!xhci_reset_port(x, p, &ps)) {
            if (ps & PORT_CCS)
                if (verbose) kprintf("[xHCI] Port %u not enabled\n", p + 1);
            continue;
        }

        usb_speed_t speed = xhci_usb_speed_from_portsc(ps);
        if (verbose) kprintf("[xHCI] Device on port %u speed=%u\n", p + 1, (uint32_t)speed);
        usb_port_connected(&x->usbhc, speed, root_port, 0, 0);
        xhci_clear_port_changes(x, p);
    }
}

static bool xhci_init(xhci_ctrl_t* x) {
    map_mmio(x->base, 0x20000u);

    x->cap = (volatile uint8_t*)x->base;
    x->cap_len = x->cap[XHCI_CAPLENGTH];
    uint32_t hcs1 = *(volatile uint32_t*)(x->base + XHCI_HCSPARAMS1);
    uint32_t hcc1 = *(volatile uint32_t*)(x->base + XHCI_HCCPARAMS1);
    uint32_t dboff = *(volatile uint32_t*)(x->base + XHCI_DBOFF);
    uint32_t rtsoff = *(volatile uint32_t*)(x->base + XHCI_RTSOFF);

    if (!xhci_legacy_handoff(x, hcc1))
        return false;

    x->max_slots = (uint8_t)(hcs1 & 0xFFu);
    if (x->max_slots > XHCI_MAX_SLOTS) x->max_slots = XHCI_MAX_SLOTS;
    x->max_ports = (uint8_t)((hcs1 >> 24) & 0xFFu);
    x->ctx_size = (hcc1 & (1u << 2)) ? 64 : 32;

    x->op = (volatile uint32_t*)(x->base + x->cap_len);
    x->db = (volatile uint32_t*)(x->base + (dboff & ~0x3u));
    x->rt = (volatile uint32_t*)(x->base + (rtsoff & ~0x1Fu));

    kprintf("[xHCI] caplen=%u max_slots=%u max_ports=%u ctx=%u\n",
            x->cap_len, x->max_slots, x->max_ports, x->ctx_size);

    if (!xhci_reset_controller(x)) return false;

    // Use 4K page size for DMA structures (bit0 = 4K).
    mmio_wr32(x->op, XHCI_PAGESIZE, 1u);

    // Set max slots enabled
    mmio_wr32(x->op, XHCI_CONFIG, x->max_slots);

    // DCBAA (max_slots+1 entries, each 64-bit -> 2 dwords)
    // DMA'd by the controller; keep within a single physical page.
    x->dcbaa = (uint32_t*)kmalloc_aligned((uint32_t)(x->max_slots + 1u) * 8u, 0x1000);
    memset(x->dcbaa, 0, (uint32_t)(x->max_slots + 1u) * 8u);
    x->dcbaa_phys = phys_addr32(x->dcbaa);
    mmio_wr64(x->op, XHCI_DCBAAP, (uint64_t)x->dcbaa_phys);

    // Command ring
    ring_init(&x->cmd_ring, 256);
    mmio_wr64(x->op, XHCI_CRCR, ((uint64_t)x->cmd_ring.trbs_phys) | (uint64_t)x->cmd_ring.cycle);

    // Event ring + ERST (1 segment)
    event_ring_init(&x->evt_ring, 256);

    x->erst = (xhci_erst_t*)kmalloc_aligned(sizeof(xhci_erst_t), 0x1000);
    memset(x->erst, 0, sizeof(*x->erst));
    x->erst_phys = phys_addr32(x->erst);
    x->erst[0].seg_addr_lo = x->evt_ring.trbs_phys;
    x->erst[0].seg_addr_hi = 0;
    x->erst[0].seg_size = x->evt_ring.trb_count;

    volatile uint32_t* ir0 = ir0_regs(x);
    // We run xHCI purely by polling (no IRQ handler yet). Enabling interrupts here can
    // cause an INTx storm and freeze the system.
    mmio_wr32(ir0, XHCI_IMAN, 1u); // clear IP (best-effort), keep IE=0
    mmio_wr32(ir0, XHCI_IMOD, 0);
    mmio_wr32(ir0, XHCI_ERSTSZ, 1);
    mmio_wr64(ir0, XHCI_ERSTBA, (uint64_t)x->erst_phys);
    mmio_wr64(ir0, XHCI_ERDP, ((uint64_t)x->evt_ring.trbs_phys) | 0x8u);

    // Run (interrupts disabled; polling only)
    uint32_t cmd = mmio_rd32(x->op, XHCI_USBCMD);
    cmd |= CMD_RS;
    cmd &= ~CMD_INTE;
    mmio_wr32(x->op, XHCI_USBCMD, cmd);

    for (int i = 0; i < 200; i++) {
        uint32_t sts = mmio_rd32(x->op, XHCI_USBSTS);
        if ((sts & STS_HCH) == 0 && (sts & STS_CNR) == 0) break;
        delay_ms(1);
    }

    x->next_addr = 1;
    x->usbhc.ops = &xhci_usbhc_ops;
    x->usbhc.impl = x;

    xhci_scan_ports(x, true);
    return true;
}

void xhci_pci_attach(uint8_t bus, uint8_t dev, uint8_t func,
                     uint32_t mmio_base, uint8_t irq_line) {
    (void)bus;
    (void)dev;
    (void)func;
    (void)irq_line;
    if (controller_count >= XHCI_MAX_CONTROLLERS) return;
    if (mmio_base == 0) return;

    xhci_ctrl_t* x = &controllers[controller_count++];
    memset(x, 0, sizeof(*x));
    x->base = mmio_base;

    if (!xhci_init(x)) {
        kprint("[xHCI] init failed\n");
    }
}

void xhci_rescan_all_ports(bool reset_addr_allocator, bool verbose) {
    for (int i = 0; i < controller_count; i++) {
        xhci_ctrl_t* x = &controllers[i];
        if (reset_addr_allocator) x->next_addr = 1;
        xhci_scan_ports(x, verbose);
    }
}

void xhci_poll_changes(void) {
    if (xhci_rescan_pending) return;
    for (int i = 0; i < controller_count; i++) {
        xhci_ctrl_t* x = &controllers[i];
        if (!x->op || x->max_ports == 0) continue;
        for (uint32_t p = 0; p < x->max_ports; p++) {
            uint32_t ps = mmio_rd32(x->op, XHCI_PORTSC(p));
            if (ps & PORT_CHANGE_BITS) {
                xhci_queue_rescan();
                return;
            }
        }
    }
}

bool xhci_take_rescan_pending(void) {
    bool pending = false;
    hal_disable_interrupts();
    pending = xhci_rescan_pending;
    xhci_rescan_pending = false;
    hal_enable_interrupts();
    return pending;
}
