#include "uhci.h"
#include "../screen.h"
#include "../keyboard.h"
#include "../mouse.h"
#include "../../mm/mem.h"
#include "../../mm/paging.h"
#include "../../cpu/timer.h"
#include "../../libc/string.h"
#include "../hal.h"
#include "../../kernel/proc/workqueue.h"
#include "../../kernel/log.h"

#define UHCI_MAX_CONTROLLERS 4
static int controller_count = 0;
static volatile bool uhci_rescan_pending = false;
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

static void uhci_rescan_work(void* ctx) {
    (void)ctx;
    if (uhci_take_rescan_pending()) {
        uhci_rescan_all_ports();
    }
}

static void uhci_queue_rescan(void) {
    bool enqueue = false;
    uint32_t flags = irq_save();
    if (!uhci_rescan_pending) {
        uhci_rescan_pending = true;
        enqueue = true;
    }
    irq_restore(flags);

    if (enqueue) {
        (void)workqueue_enqueue(uhci_rescan_work, NULL);
    }
}

typedef struct uhci_td {
    uint32_t link;
    uint32_t status;
    uint32_t token;
    uint32_t buffer;
} __attribute__((packed, aligned(16))) uhci_td_t;

typedef struct uhci_qh {
    uint32_t head;
    uint32_t elem;
} __attribute__((packed, aligned(16))) uhci_qh_t;

typedef struct uhci_ctrl {
    uint16_t io;
    uint8_t irq_line;

    uint32_t* frame_list;   // 1024 entries, 4K aligned
    uhci_qh_t* sched_qh;    // always scheduled
    uhci_qh_t* tail_qh;     // last in QH chain
    uint8_t next_addr;
} uhci_ctrl_t;

static uhci_ctrl_t controllers[UHCI_MAX_CONTROLLERS];

// I/O register offsets
#define UHCI_USBCMD     0x00
#define UHCI_USBSTS     0x02
#define UHCI_USBINTR    0x04
#define UHCI_FRNUM      0x06
#define UHCI_FLBASEADD  0x08
#define UHCI_SOFMOD     0x0C
#define UHCI_PORTSC1    0x10
#define UHCI_PORTSC2    0x12

#define UHCI_HID_IDLE_RATE_4MS 10u

// USBCMD bits
#define CMD_RS          (1u << 0)
#define CMD_HCRESET     (1u << 1)
#define CMD_GRESET      (1u << 2)
#define CMD_CF          (1u << 6)
#define CMD_MAXP        (1u << 7)

// USBSTS bits
#define STS_HCHALTED    (1u << 5)

// PORTSC bits (subset)
#define PORT_CCS        (1u << 0)
#define PORT_CSC        (1u << 1)
#define PORT_PED        (1u << 2)
#define PORT_PEDC       (1u << 3)
#define PORT_PRS        (1u << 9)
#define PORT_LSDA       (1u << 12)

// Link pointers
#define UHCI_PTR_TERM   0x00000001u
#define UHCI_PTR_QH     0x00000002u
#define UHCI_PTR_DF     0x00000004u

// TD status bits
#define TD_STS_ACTIVE   (1u << 23)
#define TD_STS_STALL    (1u << 22)
#define TD_STS_DBE      (1u << 21)
#define TD_STS_BABBLE   (1u << 20)
#define TD_STS_NAK      (1u << 19)
#define TD_STS_CRC_TO   (1u << 18)
#define TD_STS_BITSTUFF (1u << 17)

#define TD_CTL_IOC      (1u << 24)
#define TD_CTL_ISO      (1u << 25)
#define TD_CTL_LS       (1u << 26)
#define TD_CTL_CERR_SHIFT 27
#define TD_CTL_SPD      (1u << 29)

// Token encoding
#define TOK_PID_SHIFT   0
#define TOK_DEV_SHIFT   8
#define TOK_EP_SHIFT    15
#define TOK_DT_SHIFT    19
#define TOK_MAXLEN_SHIFT 21

#define PID_OUT   0xE1u
#define PID_IN    0x69u
#define PID_SETUP 0x2Du

static inline uint32_t phys_addr(void* p) {
    uint32_t phys;
    if (vmm_virt_to_phys((uint32_t)p, &phys) == 0) return phys;
    return (uint32_t)p;
}

static inline uint16_t rd16(uint16_t io, uint16_t off) {
    return hal_in16((uint16_t)(io + off));
}
static inline void wr16(uint16_t io, uint16_t off, uint16_t v) {
    hal_out16((uint16_t)(io + off), v);
}
static inline uint32_t rd32(uint16_t io, uint16_t off) {
    return hal_in32((uint16_t)(io + off));
}
static inline void wr32(uint16_t io, uint16_t off, uint32_t v) {
    hal_out32((uint16_t)(io + off), v);
}

static void delay_ms(uint32_t ms) {
    uint32_t start = tick;
    uint32_t ticks_needed = (ms + 9) / 10;
    if (ticks_needed == 0) ticks_needed = 1;
    while ((tick - start) < ticks_needed) hal_wait_for_interrupt();
}

static uint32_t td_token(uint8_t pid, uint8_t dev, uint8_t ep, uint8_t toggle, uint16_t len) {
    uint32_t maxlen = (len == 0) ? 0x7FFu : (uint32_t)(len - 1) & 0x7FFu;
    return ((uint32_t)pid << TOK_PID_SHIFT) |
           ((uint32_t)dev << TOK_DEV_SHIFT) |
           ((uint32_t)ep << TOK_EP_SHIFT) |
           ((uint32_t)(toggle & 1u) << TOK_DT_SHIFT) |
           (maxlen << TOK_MAXLEN_SHIFT);
}

static void td_init(uhci_td_t* td, uint32_t link, bool low_speed,
                    uint8_t pid, uint8_t dev, uint8_t ep, uint8_t toggle,
                    void* buf, uint16_t len, bool ioc) {
    td->link = link;
    td->status = TD_STS_ACTIVE |
                 (3u << TD_CTL_CERR_SHIFT) |
                 (low_speed ? TD_CTL_LS : 0) |
                 TD_CTL_SPD |
                 (ioc ? TD_CTL_IOC : 0);
    td->token = td_token(pid, dev, ep, toggle, len);
    td->buffer = (buf && len) ? phys_addr(buf) : 0;
}

static bool td_wait_done(uhci_td_t* td, uint32_t timeout_ms) {
    uint32_t start = tick;
    uint32_t timeout_ticks = (timeout_ms + 9) / 10;
    if (timeout_ticks == 0) timeout_ticks = 1;
    while (td->status & TD_STS_ACTIVE) {
        if ((tick - start) > timeout_ticks) return false;
        hal_wait_for_interrupt();
    }
    if (td->status & (TD_STS_STALL | TD_STS_DBE | TD_STS_BABBLE | TD_STS_CRC_TO | TD_STS_BITSTUFF)) return false;
    return true;
}

static uint16_t td_actual_len(uhci_td_t* td) {
    uint32_t al = td->status & 0x7FFu;
    if (al == 0x7FFu) return 0;
    return (uint16_t)(al + 1);
}

static bool uhci_reset_controller(uhci_ctrl_t* hc) {
    wr16(hc->io, UHCI_USBCMD, 0);
    delay_ms(2);

    wr16(hc->io, UHCI_USBCMD, CMD_GRESET);
    delay_ms(50);
    wr16(hc->io, UHCI_USBCMD, 0);
    delay_ms(2);

    wr16(hc->io, UHCI_USBCMD, CMD_HCRESET);
    for (int i = 0; i < 1000; i++) {
        if ((rd16(hc->io, UHCI_USBCMD) & CMD_HCRESET) == 0) break;
        delay_ms(1);
    }
    if (rd16(hc->io, UHCI_USBCMD) & CMD_HCRESET) {
        kprint("[UHCI] HCRESET timeout\n");
        return false;
    }

    // Clear status and disable interrupts (polled).
    wr16(hc->io, UHCI_USBINTR, 0);
    wr16(hc->io, UHCI_USBSTS, 0xFFFFu);

    // Frame list base + start at frame 0.
    wr32(hc->io, UHCI_FLBASEADD, phys_addr(hc->frame_list));
    wr16(hc->io, UHCI_FRNUM, 0);
    hal_out8((uint16_t)(hc->io + UHCI_SOFMOD), 64);

    // Run
    wr16(hc->io, UHCI_USBCMD, CMD_RS | CMD_CF | CMD_MAXP);
    for (int i = 0; i < 1000; i++) {
        if ((rd16(hc->io, UHCI_USBSTS) & STS_HCHALTED) == 0) break;
        delay_ms(1);
    }
    return true;
}

static bool uhci_port_reset(uhci_ctrl_t* hc, int port, bool* out_low_speed) {
    uint16_t off = (port == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
    uint16_t ps = rd16(hc->io, off);
    if (!(ps & PORT_CCS)) return false;

    // Reset
    wr16(hc->io, off, (uint16_t)(ps | PORT_PRS));
    delay_ms(50);
    ps = rd16(hc->io, off);
    wr16(hc->io, off, (uint16_t)(ps & ~PORT_PRS));
    delay_ms(10);

    ps = rd16(hc->io, off);
    // Enable
    wr16(hc->io, off, (uint16_t)(ps | PORT_PED));
    delay_ms(10);
    ps = rd16(hc->io, off);
    if ((ps & PORT_PED) == 0) return false;

    if (out_low_speed) *out_low_speed = (ps & PORT_LSDA) != 0;
    return true;
}

// Very small subset of USB requests for HID boot devices.
#pragma pack(push, 1)
typedef struct {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_pkt_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} usb_device_desc_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
} usb_config_desc_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} usb_interface_desc_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} usb_endpoint_desc_t;
#pragma pack(pop)

#define USB_DESC_DEVICE 1
#define USB_DESC_CONFIG 2
#define USB_DESC_INTERFACE 4
#define USB_DESC_ENDPOINT 5
#define USB_DESC_HID 0x21
#define USB_DESC_HID_REPORT 0x22

#define USB_REQ_GET_DESCRIPTOR 6
#define USB_REQ_SET_ADDRESS 5
#define USB_REQ_SET_CONFIGURATION 9

static bool uhci_control_transfer(uhci_ctrl_t* hc, bool low_speed,
                                  uint8_t addr, uint8_t ep0_mps,
                                  usb_setup_pkt_t* setup, void* data, uint16_t len) {
    if (ep0_mps == 0) ep0_mps = 8;

    uhci_qh_t* qh = (uhci_qh_t*)kmalloc_aligned(sizeof(uhci_qh_t), 16);
    uhci_td_t* td_setup = (uhci_td_t*)kmalloc_aligned(sizeof(uhci_td_t), 16);
    uhci_td_t* td_status = (uhci_td_t*)kmalloc_aligned(sizeof(uhci_td_t), 16);
    if (!qh || !td_setup || !td_status) return false;

    memset(qh, 0, sizeof(*qh));
    memset(td_setup, 0, sizeof(*td_setup));
    memset(td_status, 0, sizeof(*td_status));

    bool has_data = (len > 0 && data != NULL);
    bool data_in = has_data && (setup->bmRequestType & 0x80);

    // Data TDs (one packet per TD)
    uhci_td_t* data_tds = NULL;
    uint16_t data_td_count = 0;
    if (has_data) {
        data_td_count = (uint16_t)((len + ep0_mps - 1) / ep0_mps);
        data_tds = (uhci_td_t*)kmalloc_aligned((size_t)data_td_count * sizeof(uhci_td_t), 16);
        if (!data_tds) {
            kfree(qh);
            kfree(td_setup);
            kfree(td_status);
            return false;
        }
        memset(data_tds, 0, (size_t)data_td_count * sizeof(uhci_td_t));
    }

    // Setup TD (DATA0)
    td_init(td_setup,
            has_data ? (phys_addr(&data_tds[0]) | UHCI_PTR_DF) : (phys_addr(td_status) | UHCI_PTR_DF),
            low_speed,
            PID_SETUP, addr, 0, 0, setup, 8, false);

    if (has_data) {
        uint8_t pid = data_in ? PID_IN : PID_OUT;
        uint8_t toggle = 1;
        uint16_t remaining = len;
        uint8_t* p = (uint8_t*)data;
        for (uint16_t i = 0; i < data_td_count; i++) {
            uint16_t chunk = remaining;
            if (chunk > ep0_mps) chunk = ep0_mps;
            remaining = (uint16_t)(remaining - chunk);

            uint32_t next = (i + 1 < data_td_count) ? phys_addr(&data_tds[i + 1]) : phys_addr(td_status);
            next |= UHCI_PTR_DF;
            td_init(&data_tds[i], next, low_speed, pid, addr, 0, toggle, p, chunk, false);
            toggle ^= 1u;
            p += chunk;
        }
    }

    // Status TD (DATA1, opposite dir or IN if no data)
    uint8_t status_pid = data_in ? PID_OUT : PID_IN;
    td_init(td_status, UHCI_PTR_TERM, low_speed,
            status_pid, addr, 0, 1, NULL, 0, true);

    qh->head = UHCI_PTR_TERM;
    qh->elem = phys_addr(td_setup);

    // Temporarily insert QH at head of schedule.
    uint32_t old_head = hc->sched_qh->head;
    hc->sched_qh->head = phys_addr(qh) | UHCI_PTR_QH | UHCI_PTR_DF;
    qh->head = old_head;

    bool ok = td_wait_done(td_status, 2000);

    // Remove from schedule (restore).
    hc->sched_qh->head = old_head;

    if (data_tds) kfree(data_tds);
    kfree(qh);
    kfree(td_setup);
    kfree(td_status);

    return ok;
}

static bool uhci_get_desc(uhci_ctrl_t* hc, bool low_speed, uint8_t addr, uint8_t ep0_mps,
                          uint8_t type, uint8_t index, void* buf, uint16_t len) {
    usb_setup_pkt_t setup;
    setup.bmRequestType = 0x80;
    setup.bRequest = USB_REQ_GET_DESCRIPTOR;
    setup.wValue = (uint16_t)((type << 8) | index);
    setup.wIndex = 0;
    setup.wLength = len;
    return uhci_control_transfer(hc, low_speed, addr, ep0_mps, &setup, buf, len);
}

static bool uhci_get_report_desc(uhci_ctrl_t* hc, bool low_speed, uint8_t addr, uint8_t ep0_mps,
                                 uint8_t iface, void* buf, uint16_t len) {
    usb_setup_pkt_t setup;
    setup.bmRequestType = 0x81;
    setup.bRequest = USB_REQ_GET_DESCRIPTOR;
    setup.wValue = (uint16_t)(USB_DESC_HID_REPORT << 8);
    setup.wIndex = iface;
    setup.wLength = len;
    return uhci_control_transfer(hc, low_speed, addr, ep0_mps, &setup, buf, len);
}

static bool uhci_set_address(uhci_ctrl_t* hc, bool low_speed, uint8_t new_addr, uint8_t ep0_mps) {
    usb_setup_pkt_t setup;
    setup.bmRequestType = 0x00;
    setup.bRequest = USB_REQ_SET_ADDRESS;
    setup.wValue = new_addr;
    setup.wIndex = 0;
    setup.wLength = 0;
    if (!uhci_control_transfer(hc, low_speed, 0, ep0_mps, &setup, NULL, 0)) return false;
    delay_ms(20);
    return true;
}

static bool uhci_set_configuration(uhci_ctrl_t* hc, bool low_speed, uint8_t addr, uint8_t ep0_mps, uint8_t cfg_value) {
    usb_setup_pkt_t setup;
    setup.bmRequestType = 0x00;
    setup.bRequest = USB_REQ_SET_CONFIGURATION;
    setup.wValue = cfg_value;
    setup.wIndex = 0;
    setup.wLength = 0;
    return uhci_control_transfer(hc, low_speed, addr, ep0_mps, &setup, NULL, 0);
}

static bool uhci_hid_set_protocol(uhci_ctrl_t* hc, bool low_speed, uint8_t addr, uint8_t ep0_mps,
                                  uint8_t iface_num, uint16_t protocol) {
    usb_setup_pkt_t setup;
    setup.bmRequestType = 0x21;
    setup.bRequest = 0x0B;
    setup.wValue = protocol;
    setup.wIndex = iface_num;
    setup.wLength = 0;
    return uhci_control_transfer(hc, low_speed, addr, ep0_mps, &setup, NULL, 0);
}

static bool uhci_hid_set_idle(uhci_ctrl_t* hc, bool low_speed, uint8_t addr, uint8_t ep0_mps,
                              uint8_t iface_num, uint8_t duration, uint8_t report_id) {
    usb_setup_pkt_t setup;
    setup.bmRequestType = 0x21;
    setup.bRequest = 0x0A;
    setup.wValue = (uint16_t)(((uint16_t)duration << 8) | report_id);
    setup.wIndex = iface_num;
    setup.wLength = 0;
    return uhci_control_transfer(hc, low_speed, addr, ep0_mps, &setup, NULL, 0);
}

#define HID_USAGE_PAGE_GENERIC 0x01
#define HID_USAGE_PAGE_KBD 0x07
#define HID_USAGE_PAGE_BUTTON 0x09
#define HID_USAGE_X 0x30
#define HID_USAGE_Y 0x31
#define HID_USAGE_WHEEL 0x38

#define HID_REPORT_MAX_TRACKED 4
#define UHCI_HID_MAX_KEYS 16

typedef struct {
    bool used;
    uint8_t report_id;
    uint16_t bit_off;
    uint16_t report_bits;

    bool has_mods;
    uint16_t mod_bit_off;
    uint8_t mod_bit_count;

    bool has_keys;
    uint16_t keys_bit_off;
    uint8_t keys_count;
    uint8_t keys_size;

    bool has_buttons;
    uint16_t buttons_bit_off;
    uint8_t buttons_count;

    bool has_x;
    uint16_t x_bit_off;
    uint8_t x_size;
    bool x_rel;

    bool has_y;
    uint16_t y_bit_off;
    uint8_t y_size;
    bool y_rel;

    bool has_wheel;
    uint16_t wheel_bit_off;
    uint8_t wheel_size;
    bool wheel_rel;
} hid_report_info_t;

typedef struct {
    uint16_t usage_page;
    uint8_t report_size;
    uint8_t report_count;
    uint8_t report_id;
} hid_global_t;

typedef struct {
    uint16_t usages[16];
    uint8_t usage_count;
    uint16_t usage_min;
    uint16_t usage_max;
    bool has_usage_minmax;
} hid_local_t;

static hid_report_info_t* hid_get_report_info(hid_report_info_t* infos, size_t count, uint8_t report_id) {
    for (size_t i = 0; i < count; i++) {
        if (infos[i].used && infos[i].report_id == report_id) return &infos[i];
    }
    for (size_t i = 0; i < count; i++) {
        if (!infos[i].used) {
            memset(&infos[i], 0, sizeof(infos[i]));
            infos[i].used = true;
            infos[i].report_id = report_id;
            return &infos[i];
        }
    }
    return NULL;
}

static void hid_local_reset(hid_local_t* l) {
    memset(l, 0, sizeof(*l));
}

static uint16_t hid_local_usage(const hid_local_t* l, uint8_t idx) {
    if (idx < l->usage_count) return l->usages[idx];
    if (l->has_usage_minmax && l->usage_min <= l->usage_max) {
        uint16_t usage = (uint16_t)(l->usage_min + idx);
        if (usage <= l->usage_max) return usage;
    }
    return 0;
}

static uint32_t hid_get_bits(const uint8_t* buf, uint16_t bit_off, uint8_t bit_len) {
    uint32_t v = 0;
    for (uint8_t i = 0; i < bit_len; i++) {
        uint16_t b = (uint16_t)(bit_off + i);
        uint8_t byte = buf[b >> 3];
        uint8_t bit = (uint8_t)((byte >> (b & 7u)) & 1u);
        v |= ((uint32_t)bit << i);
    }
    return v;
}

static int32_t hid_get_bits_signed(const uint8_t* buf, uint16_t bit_off, uint8_t bit_len) {
    if (bit_len == 0) return 0;
    uint32_t v = hid_get_bits(buf, bit_off, bit_len);
    if (bit_len < 32 && (v & (1u << (bit_len - 1u)))) {
        v |= ~((1u << bit_len) - 1u);
    }
    return (int32_t)v;
}

static bool hid_parse_report_desc(const uint8_t* desc, uint16_t len, bool is_mouse,
                                  hid_report_info_t* out) {
    hid_report_info_t infos[HID_REPORT_MAX_TRACKED];
    memset(infos, 0, sizeof(infos));

    hid_global_t g;
    memset(&g, 0, sizeof(g));
    hid_local_t l;
    hid_local_reset(&l);

    uint16_t i = 0;
    while (i < len) {
        uint8_t b = desc[i++];
        if (b == 0xFE) {
            if (i + 1 >= len) break;
            uint8_t data_size = desc[i];
            i = (uint16_t)(i + 2 + data_size);
            continue;
        }
        uint8_t size_code = (uint8_t)(b & 0x3u);
        uint8_t item_size = (size_code == 3) ? 4u : size_code;
        uint8_t type = (uint8_t)((b >> 2) & 0x3u);
        uint8_t tag = (uint8_t)((b >> 4) & 0xFu);
        uint32_t data = 0;
        for (uint8_t j = 0; j < item_size && i < len; j++) {
            data |= ((uint32_t)desc[i++] << (8u * j));
        }

        if (type == 1) {
            switch (tag) {
            case 0x0: g.usage_page = (uint16_t)data; break;
            case 0x7: g.report_size = (uint8_t)data; break;
            case 0x8: g.report_id = (uint8_t)data; break;
            case 0x9: g.report_count = (uint8_t)data; break;
            default: break;
            }
        } else if (type == 2) {
            switch (tag) {
            case 0x0:
                if (l.usage_count < (uint8_t)(sizeof(l.usages) / sizeof(l.usages[0]))) {
                    l.usages[l.usage_count++] = (uint16_t)data;
                }
                break;
            case 0x1: l.usage_min = (uint16_t)data; l.has_usage_minmax = true; break;
            case 0x2: l.usage_max = (uint16_t)data; l.has_usage_minmax = true; break;
            default: break;
            }
        } else if (type == 0) {
            if (tag == 0x8) {
                hid_report_info_t* info = hid_get_report_info(infos, HID_REPORT_MAX_TRACKED, g.report_id);
                if (!info) {
                    hid_local_reset(&l);
                    continue;
                }

                bool is_const = (data & 0x01u) != 0;
                bool is_var = (data & 0x02u) != 0;
                bool is_rel = (data & 0x04u) != 0;
                uint8_t count = g.report_count;
                uint8_t size = g.report_size;
                uint16_t bit_off = info->bit_off;

                if (size != 0 && count != 0) {
                    if (!is_const) {
                        for (uint8_t idx = 0; idx < count; idx++) {
                            uint16_t usage = hid_local_usage(&l, idx);
                            uint16_t elem_off = (uint16_t)(bit_off + (uint16_t)idx * size);
                            if (!is_mouse) {
                                if (g.usage_page == HID_USAGE_PAGE_KBD) {
                                    if (is_var && size == 1 && usage >= 0xE0 && usage <= 0xE7) {
                                        if (!info->has_mods) {
                                            info->has_mods = true;
                                            info->mod_bit_off = elem_off;
                                            info->mod_bit_count = (count > 8) ? 8 : count;
                                        }
                                    } else if (!is_var && size == 8 && !info->has_keys) {
                                        info->has_keys = true;
                                        info->keys_bit_off = bit_off;
                                        info->keys_count = count;
                                        info->keys_size = size;
                                    }
                                }
                            } else {
                                if (g.usage_page == HID_USAGE_PAGE_BUTTON && is_var && size == 1) {
                                    if (!info->has_buttons) {
                                        info->has_buttons = true;
                                        info->buttons_bit_off = elem_off;
                                        info->buttons_count = count;
                                    }
                                } else if (g.usage_page == HID_USAGE_PAGE_GENERIC && is_var) {
                                    if (usage == HID_USAGE_X && !info->has_x) {
                                        info->has_x = true;
                                        info->x_bit_off = elem_off;
                                        info->x_size = size;
                                        info->x_rel = is_rel;
                                    } else if (usage == HID_USAGE_Y && !info->has_y) {
                                        info->has_y = true;
                                        info->y_bit_off = elem_off;
                                        info->y_size = size;
                                        info->y_rel = is_rel;
                                    } else if (usage == HID_USAGE_WHEEL && !info->has_wheel) {
                                        info->has_wheel = true;
                                        info->wheel_bit_off = elem_off;
                                        info->wheel_size = size;
                                        info->wheel_rel = is_rel;
                                    }
                                }
                            }
                        }
                    }
                    info->bit_off = (uint16_t)(bit_off + (uint16_t)count * size);
                    if (info->bit_off > info->report_bits) info->report_bits = info->bit_off;
                }
                hid_local_reset(&l);
            } else {
                hid_local_reset(&l);
            }
        }
    }

    hid_report_info_t* best = NULL;
    for (size_t idx = 0; idx < HID_REPORT_MAX_TRACKED; idx++) {
        hid_report_info_t* info = &infos[idx];
        if (!info->used) continue;
        if (!is_mouse) {
            if (info->has_keys && info->has_mods) { best = info; break; }
            if (!best && info->has_keys) best = info;
        } else {
            if (info->has_x && info->has_y && info->has_buttons) { best = info; break; }
            if (!best && info->has_x && info->has_y) best = info;
        }
    }
    if (!best) return false;
    *out = *best;
    return true;
}

static bool hid_key_to_set1(uint8_t key, uint8_t* out_prefix, uint8_t* out_sc) {
    if (!out_prefix || !out_sc) return false;
    *out_prefix = 0;
    *out_sc = 0;

    switch (key) {
    case 0x04: *out_sc = 0x1E; return true; // a
    case 0x05: *out_sc = 0x30; return true; // b
    case 0x06: *out_sc = 0x2E; return true; // c
    case 0x07: *out_sc = 0x20; return true; // d
    case 0x08: *out_sc = 0x12; return true; // e
    case 0x09: *out_sc = 0x21; return true; // f
    case 0x0A: *out_sc = 0x22; return true; // g
    case 0x0B: *out_sc = 0x23; return true; // h
    case 0x0C: *out_sc = 0x17; return true; // i
    case 0x0D: *out_sc = 0x24; return true; // j
    case 0x0E: *out_sc = 0x25; return true; // k
    case 0x0F: *out_sc = 0x26; return true; // l
    case 0x10: *out_sc = 0x32; return true; // m
    case 0x11: *out_sc = 0x31; return true; // n
    case 0x12: *out_sc = 0x18; return true; // o
    case 0x13: *out_sc = 0x19; return true; // p
    case 0x14: *out_sc = 0x10; return true; // q
    case 0x15: *out_sc = 0x13; return true; // r
    case 0x16: *out_sc = 0x1F; return true; // s
    case 0x17: *out_sc = 0x14; return true; // t
    case 0x18: *out_sc = 0x16; return true; // u
    case 0x19: *out_sc = 0x2F; return true; // v
    case 0x1A: *out_sc = 0x11; return true; // w
    case 0x1B: *out_sc = 0x2D; return true; // x
    case 0x1C: *out_sc = 0x15; return true; // y
    case 0x1D: *out_sc = 0x2C; return true; // z
    case 0x1E: *out_sc = 0x02; return true; // 1
    case 0x1F: *out_sc = 0x03; return true; // 2
    case 0x20: *out_sc = 0x04; return true; // 3
    case 0x21: *out_sc = 0x05; return true; // 4
    case 0x22: *out_sc = 0x06; return true; // 5
    case 0x23: *out_sc = 0x07; return true; // 6
    case 0x24: *out_sc = 0x08; return true; // 7
    case 0x25: *out_sc = 0x09; return true; // 8
    case 0x26: *out_sc = 0x0A; return true; // 9
    case 0x27: *out_sc = 0x0B; return true; // 0
    case 0x28: *out_sc = 0x1C; return true; // Enter
    case 0x29: *out_sc = 0x01; return true; // Esc
    case 0x2A: *out_sc = 0x0E; return true; // Backspace
    case 0x2B: *out_sc = 0x0F; return true; // Tab
    case 0x2C: *out_sc = 0x39; return true; // Space
    case 0x2D: *out_sc = 0x0C; return true; // -
    case 0x2E: *out_sc = 0x0D; return true; // =
    case 0x2F: *out_sc = 0x1A; return true; // [
    case 0x30: *out_sc = 0x1B; return true; // ]
    case 0x31: *out_sc = 0x2B; return true; // backslash
    case 0x33: *out_sc = 0x27; return true; // ;
    case 0x34: *out_sc = 0x28; return true; // '
    case 0x35: *out_sc = 0x29; return true; // `
    case 0x36: *out_sc = 0x33; return true; // ,
    case 0x37: *out_sc = 0x34; return true; // .
    case 0x38: *out_sc = 0x35; return true; // /
    case 0x39: *out_sc = 0x3A; return true; // CapsLock
    case 0x47: *out_sc = 0x46; return true; // ScrollLock
    case 0x53: *out_sc = 0x45; return true; // NumLock

    case 0x54: *out_prefix = 0xE0; *out_sc = 0x35; return true; // Keypad /
    case 0x55: *out_sc = 0x37; return true; // Keypad *
    case 0x56: *out_sc = 0x4A; return true; // Keypad -
    case 0x57: *out_sc = 0x4E; return true; // Keypad +
    case 0x58: *out_prefix = 0xE0; *out_sc = 0x1C; return true; // Keypad Enter
    case 0x59: *out_sc = 0x4F; return true; // Keypad 1
    case 0x5A: *out_sc = 0x50; return true; // Keypad 2
    case 0x5B: *out_sc = 0x51; return true; // Keypad 3
    case 0x5C: *out_sc = 0x4B; return true; // Keypad 4
    case 0x5D: *out_sc = 0x4C; return true; // Keypad 5
    case 0x5E: *out_sc = 0x4D; return true; // Keypad 6
    case 0x5F: *out_sc = 0x47; return true; // Keypad 7
    case 0x60: *out_sc = 0x48; return true; // Keypad 8
    case 0x61: *out_sc = 0x49; return true; // Keypad 9
    case 0x62: *out_sc = 0x52; return true; // Keypad 0
    case 0x63: *out_sc = 0x53; return true; // Keypad .
    case 0x65: *out_prefix = 0xE0; *out_sc = 0x5D; return true; // Application/Menu

    case 0x4B: *out_prefix = 0xE0; *out_sc = 0x49; return true; // PageUp
    case 0x4E: *out_prefix = 0xE0; *out_sc = 0x51; return true; // PageDown
    case 0x4F: *out_prefix = 0xE0; *out_sc = 0x4D; return true; // Right
    case 0x50: *out_prefix = 0xE0; *out_sc = 0x4B; return true; // Left
    case 0x51: *out_prefix = 0xE0; *out_sc = 0x50; return true; // Down
    case 0x52: *out_prefix = 0xE0; *out_sc = 0x48; return true; // Up

    default:
        return false;
    }
}

static void send_scancode(uint8_t prefix, uint8_t sc, bool make) {
    if (prefix) keyboard_inject_scancode(prefix);
    keyboard_inject_scancode(make ? sc : (uint8_t)(sc | 0x80u));
}

#define UHCI_MAX_HID 2
typedef struct {
    uhci_ctrl_t* hc;
    bool low_speed;
    uint8_t addr;
    uint8_t iface;
    uint8_t ep;
    uint16_t mps;
    bool is_mouse;

    uhci_qh_t* qh;
    uhci_td_t* td;
    uint8_t buf[64];
    uint16_t poll_len;
    uint8_t toggle;

    uint8_t prev_kbd[8];
    bool first_read;  // 첫 번째 읽기 이후 플래그
    bool report_proto;
    hid_report_info_t report;
    uint8_t prev_mod;
    uint8_t prev_keys[UHCI_HID_MAX_KEYS];
    uint8_t prev_keys_count;
    bool repeat_active;
    uint8_t repeat_key_hid;
    uint8_t repeat_prefix;
    uint8_t repeat_sc;
    uint32_t repeat_next_tick;
} uhci_hid_dev_t;

static uhci_hid_dev_t hid_devs[UHCI_MAX_HID];
static int hid_dev_count = 0;

static bool kbd_key_present(const uint8_t* keys, uint8_t count, uint8_t key) {
    for (uint8_t i = 0; i < count; i++) {
        if (keys[i] == key) return true;
    }
    return false;
}

static void kbd_process_boot(uhci_hid_dev_t* dev, uint16_t actual) {
    if (actual < 8) return;

    if (!dev->first_read) {
        dev->first_read = true;
        memcpy(dev->prev_kbd, dev->buf, 8);
        return;
    }

    const uint8_t* rep = dev->buf;
    const uint8_t* prev = dev->prev_kbd;

    uint8_t mod = rep[0];
    uint8_t prev_mod = prev[0];
    uint8_t changed = (uint8_t)(mod ^ prev_mod);

    struct mod_map { uint8_t bit; uint8_t prefix; uint8_t sc; };
    static const struct mod_map mods[] = {
        { 0, 0x00, 0x1D }, // LCTRL
        { 1, 0x00, 0x2A }, // LSHIFT
        { 2, 0x00, 0x38 }, // LALT
        { 4, 0xE0, 0x1D }, // RCTRL
        { 5, 0x00, 0x36 }, // RSHIFT
        { 6, 0xE0, 0x38 }, // RALT
    };

    for (unsigned i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
        uint8_t mask = (uint8_t)(1u << mods[i].bit);
        if (!(changed & mask)) continue;
        bool make = (mod & mask) != 0;
        send_scancode(mods[i].prefix, mods[i].sc, make);
    }

    for (int i = 2; i < 8; i++) {
        uint8_t key = prev[i];
        if (key == 0) continue;
        bool still_down = false;
        for (int j = 2; j < 8; j++) if (rep[j] == key) { still_down = true; break; }
        if (!still_down) {
            uint8_t prefix, sc;
            if (hid_key_to_set1(key, &prefix, &sc)) {
                send_scancode(prefix, sc, false);
                if (dev->repeat_active && dev->repeat_key_hid == key) dev->repeat_active = false;
            }
        }
    }

    for (int i = 2; i < 8; i++) {
        uint8_t key = rep[i];
        if (key == 0 || key <= 0x03) continue;
        bool was_down = false;
        for (int j = 2; j < 8; j++) if (prev[j] == key) { was_down = true; break; }
        if (!was_down) {
            uint8_t prefix, sc;
            if (hid_key_to_set1(key, &prefix, &sc)) {
                send_scancode(prefix, sc, true);
                dev->repeat_active = true;
                dev->repeat_key_hid = key;
                dev->repeat_prefix = prefix;
                dev->repeat_sc = sc;
                dev->repeat_next_tick = tick + 35u;
            }
        }
    }

    memcpy(dev->prev_kbd, dev->buf, 8);
}

static bool kbd_report_extract(uhci_hid_dev_t* dev, uint16_t actual,
                               uint8_t* mod, uint8_t* keys, uint8_t* key_count) {
    const hid_report_info_t* r = &dev->report;
    if (!r->has_keys || r->keys_size != 8) return false;
    if (r->report_id != 0) {
        if (actual < 1 || dev->buf[0] != r->report_id) return false;
    }
    uint16_t base = (r->report_id != 0) ? 8u : 0u;
    uint16_t need_bits = (uint16_t)(r->keys_bit_off + (uint16_t)r->keys_count * r->keys_size);
    uint16_t mod_bits = (uint16_t)(r->mod_bit_off + r->mod_bit_count);
    uint16_t total_bits = (need_bits > mod_bits) ? need_bits : mod_bits;
    if ((uint32_t)base + total_bits > (uint32_t)actual * 8u) return false;

    uint8_t count = r->keys_count;
    if (count > UHCI_HID_MAX_KEYS) count = UHCI_HID_MAX_KEYS;
    for (uint8_t i = 0; i < count; i++) {
        uint16_t off = (uint16_t)(base + r->keys_bit_off + (uint16_t)i * r->keys_size);
        keys[i] = (uint8_t)hid_get_bits(dev->buf, off, r->keys_size);
    }
    *key_count = count;
    if (r->has_mods) {
        uint8_t mod_count = r->mod_bit_count;
        if (mod_count > 8) mod_count = 8;
        uint16_t off = (uint16_t)(base + r->mod_bit_off);
        *mod = (uint8_t)hid_get_bits(dev->buf, off, mod_count);
    } else {
        *mod = 0;
    }
    return true;
}

static void kbd_process_report(uhci_hid_dev_t* dev, uint16_t actual) {
    uint8_t keys[UHCI_HID_MAX_KEYS];
    uint8_t key_count = 0;
    uint8_t mod = 0;
    if (!kbd_report_extract(dev, actual, &mod, keys, &key_count)) return;

    if (!dev->first_read) {
        dev->first_read = true;
        dev->prev_mod = mod;
        dev->prev_keys_count = key_count;
        memset(dev->prev_keys, 0, sizeof(dev->prev_keys));
        if (key_count) memcpy(dev->prev_keys, keys, key_count);
        return;
    }

    uint8_t changed = (uint8_t)(mod ^ dev->prev_mod);

    struct mod_map { uint8_t bit; uint8_t prefix; uint8_t sc; };
    static const struct mod_map mods[] = {
        { 0, 0x00, 0x1D }, // LCTRL
        { 1, 0x00, 0x2A }, // LSHIFT
        { 2, 0x00, 0x38 }, // LALT
        { 4, 0xE0, 0x1D }, // RCTRL
        { 5, 0x00, 0x36 }, // RSHIFT
        { 6, 0xE0, 0x38 }, // RALT
    };

    for (unsigned i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
        uint8_t mask = (uint8_t)(1u << mods[i].bit);
        if (!(changed & mask)) continue;
        bool make = (mod & mask) != 0;
        send_scancode(mods[i].prefix, mods[i].sc, make);
    }

    for (uint8_t i = 0; i < dev->prev_keys_count; i++) {
        uint8_t key = dev->prev_keys[i];
        if (key == 0) continue;
        if (!kbd_key_present(keys, key_count, key)) {
            uint8_t prefix, sc;
            if (hid_key_to_set1(key, &prefix, &sc)) {
                send_scancode(prefix, sc, false);
                if (dev->repeat_active && dev->repeat_key_hid == key) dev->repeat_active = false;
            }
        }
    }

    for (uint8_t i = 0; i < key_count; i++) {
        uint8_t key = keys[i];
        if (key == 0 || key <= 0x03) continue;
        if (!kbd_key_present(dev->prev_keys, dev->prev_keys_count, key)) {
            uint8_t prefix, sc;
            if (hid_key_to_set1(key, &prefix, &sc)) {
                send_scancode(prefix, sc, true);
                dev->repeat_active = true;
                dev->repeat_key_hid = key;
                dev->repeat_prefix = prefix;
                dev->repeat_sc = sc;
                dev->repeat_next_tick = tick + 35u;
            }
        }
    }

    dev->prev_mod = mod;
    dev->prev_keys_count = key_count;
    memset(dev->prev_keys, 0, sizeof(dev->prev_keys));
    if (key_count) memcpy(dev->prev_keys, keys, key_count);
}

static void kbd_process(uhci_hid_dev_t* dev, uint16_t actual) {
    if (dev->report_proto) kbd_process_report(dev, actual);
    else kbd_process_boot(dev, actual);
}

static void mouse_process_boot(uhci_hid_dev_t* dev, uint16_t actual) {
    if (actual < 3) return;
    uint8_t buttons = dev->buf[0];
    int dx = (int8_t)dev->buf[1];
    int dy = (int8_t)dev->buf[2];
    int wheel = 0;
    if (actual >= 4) wheel = (int8_t)dev->buf[3];
    mouse_inject(dx, dy, wheel, buttons);
}

static void mouse_process_report(uhci_hid_dev_t* dev, uint16_t actual) {
    const hid_report_info_t* r = &dev->report;
    if (!r->has_x || !r->has_y) return;
    if (r->report_id != 0) {
        if (actual < 1 || dev->buf[0] != r->report_id) return;
    }
    uint16_t base = (r->report_id != 0) ? 8u : 0u;
    uint16_t max_bits = (uint16_t)(r->x_bit_off + r->x_size);
    uint16_t y_bits = (uint16_t)(r->y_bit_off + r->y_size);
    if (y_bits > max_bits) max_bits = y_bits;
    if (r->has_buttons) {
        uint16_t b_bits = (uint16_t)(r->buttons_bit_off + r->buttons_count);
        if (b_bits > max_bits) max_bits = b_bits;
    }
    if (r->has_wheel) {
        uint16_t w_bits = (uint16_t)(r->wheel_bit_off + r->wheel_size);
        if (w_bits > max_bits) max_bits = w_bits;
    }
    if ((uint32_t)base + max_bits > (uint32_t)actual * 8u) return;
    if (r->x_size > 16 || r->y_size > 16) return;

    int dx = r->x_rel ? hid_get_bits_signed(dev->buf, (uint16_t)(base + r->x_bit_off), r->x_size)
                      : (int)hid_get_bits(dev->buf, (uint16_t)(base + r->x_bit_off), r->x_size);
    int dy = r->y_rel ? hid_get_bits_signed(dev->buf, (uint16_t)(base + r->y_bit_off), r->y_size)
                      : (int)hid_get_bits(dev->buf, (uint16_t)(base + r->y_bit_off), r->y_size);
    int wheel = 0;
    if (r->has_wheel && r->wheel_size <= 16) {
        wheel = r->wheel_rel ? hid_get_bits_signed(dev->buf, (uint16_t)(base + r->wheel_bit_off), r->wheel_size)
                             : (int)hid_get_bits(dev->buf, (uint16_t)(base + r->wheel_bit_off), r->wheel_size);
    }

    int buttons = 0;
    if (r->has_buttons) {
        uint8_t bcount = r->buttons_count;
        if (bcount > 8) bcount = 8;
        buttons = (int)hid_get_bits(dev->buf, (uint16_t)(base + r->buttons_bit_off), bcount);
    }

    mouse_inject(dx, dy, wheel, buttons);
}

static void mouse_process(uhci_hid_dev_t* dev, uint16_t actual) {
    if (dev->report_proto) mouse_process_report(dev, actual);
    else mouse_process_boot(dev, actual);
}

static void kbd_repeat_tick(uhci_hid_dev_t* dev) {
    if (!dev->repeat_active) return;
    if (tick < dev->repeat_next_tick) return;
    bool still_down = false;
    if (dev->report_proto) {
        still_down = kbd_key_present(dev->prev_keys, dev->prev_keys_count, dev->repeat_key_hid);
    } else {
        for (int i = 2; i < 8; i++) if (dev->prev_kbd[i] == dev->repeat_key_hid) { still_down = true; break; }
    }
    if (!still_down) { dev->repeat_active = false; return; }
    send_scancode(dev->repeat_prefix, dev->repeat_sc, true);
    dev->repeat_next_tick = tick + 5u;
}

static void uhci_hid_schedule(uhci_ctrl_t* hc, uhci_hid_dev_t* dev) {
    // Insert at end of QH chain: tail_qh->head -> new, new->head -> TERM
    hc->tail_qh->head = phys_addr(dev->qh) | UHCI_PTR_QH | UHCI_PTR_DF;
    dev->qh->head = UHCI_PTR_TERM;
    hc->tail_qh = dev->qh;
}

static bool uhci_hid_init(uhci_ctrl_t* hc, bool low_speed, uint8_t addr,
                          uint8_t iface, uint8_t ep, uint16_t mps, bool is_mouse,
                          uint8_t ep0_mps, uint16_t report_len) {
    if (hid_dev_count >= UHCI_MAX_HID) return false;
    bool verbose = bootlog_enabled;
    uhci_hid_dev_t* dev = &hid_devs[hid_dev_count++];
    memset(dev, 0, sizeof(*dev));
    dev->hc = hc;
    dev->low_speed = low_speed;
    dev->addr = addr;
    dev->iface = iface;
    dev->ep = ep;
    dev->mps = mps;
    dev->is_mouse = is_mouse;
    dev->toggle = 0;
    dev->poll_len = mps;

    dev->qh = (uhci_qh_t*)kmalloc_aligned(sizeof(uhci_qh_t), 16);
    dev->td = (uhci_td_t*)kmalloc_aligned(sizeof(uhci_td_t), 16);
    if (!dev->qh || !dev->td) return false;
    memset(dev->qh, 0, sizeof(*dev->qh));
    memset(dev->td, 0, sizeof(*dev->td));

    dev->report_proto = false;
    memset(&dev->report, 0, sizeof(dev->report));
    dev->prev_mod = 0;
    dev->prev_keys_count = 0;
    memset(dev->prev_keys, 0, sizeof(dev->prev_keys));

    if (report_len > 0 && report_len <= 1024) {
        uint8_t* report_desc = (uint8_t*)kmalloc(report_len, 0, NULL);
        if (report_desc) {
            if (uhci_get_report_desc(hc, low_speed, addr, ep0_mps, iface, report_desc, report_len) &&
                hid_parse_report_desc(report_desc, report_len, is_mouse, &dev->report)) {
                uint16_t rpt_bytes = (uint16_t)((dev->report.report_bits + 7u) / 8u);
                if (dev->report.report_id != 0) rpt_bytes = (uint16_t)(rpt_bytes + 1);
                if (rpt_bytes > 0) {
                    dev->report_proto = true;
                    dev->poll_len = rpt_bytes;
                }
            }
            kfree(report_desc);
        }
    }

    if (dev->poll_len > dev->mps) dev->poll_len = dev->mps;
    if (dev->poll_len > sizeof(dev->buf)) dev->poll_len = sizeof(dev->buf);
    if (dev->poll_len == 0) dev->poll_len = mps;

    uint8_t idle = is_mouse ? 0 : UHCI_HID_IDLE_RATE_4MS;
    (void)uhci_hid_set_idle(hc, low_speed, addr, ep0_mps, iface, idle, 0);
    (void)uhci_hid_set_protocol(hc, low_speed, addr, ep0_mps, iface, dev->report_proto ? 1 : 0);

    // 초기화: prev_kbd/prev_mouse를 0으로 초기화하여 첫 읽기를 모두 press로 처리하지 않음
    memset(dev->prev_kbd, 0, sizeof(dev->prev_kbd));

    uint16_t blen = dev->poll_len;
    if (blen > sizeof(dev->buf)) blen = sizeof(dev->buf);
    td_init(dev->td, UHCI_PTR_TERM, low_speed, PID_IN, addr, ep, dev->toggle, dev->buf, blen, false);
    dev->qh->elem = phys_addr(dev->td);
    dev->qh->head = UHCI_PTR_TERM;

    uhci_hid_schedule(hc, dev);

    if (verbose) {
        kprintf("[UHCI] HID %s addr=%u ep=%u mps=%u low=%d\n",
                is_mouse ? "mouse" : "kbd", addr, ep, mps, low_speed ? 1 : 0);
    }
    if (is_mouse) mouse_set_ignore_ps2(true);
    return true;
}

static void uhci_enumerate_port(uhci_ctrl_t* hc, bool low_speed) {
    if (hc->next_addr == 0 || hc->next_addr >= 127) return;

    uint8_t dev_desc8[8] = {0};
    if (!uhci_get_desc(hc, low_speed, 0, 8, USB_DESC_DEVICE, 0, dev_desc8, 8)) {
        kprint("[UHCI] GET_DESC8 failed\n");
        return;
    }

    uint8_t ep0_mps = dev_desc8[7];
    if (ep0_mps == 0) ep0_mps = 8;

    uint8_t addr = hc->next_addr++;
    if (!uhci_set_address(hc, low_speed, addr, ep0_mps)) {
        kprint("[UHCI] SET_ADDRESS failed\n");
        return;
    }

    usb_device_desc_t dev_desc;
    memset(&dev_desc, 0, sizeof(dev_desc));
    if (!uhci_get_desc(hc, low_speed, addr, ep0_mps, USB_DESC_DEVICE, 0, &dev_desc, sizeof(dev_desc))) {
        kprint("[UHCI] GET_DEVICE_DESC failed\n");
        return;
    }

    usb_config_desc_t cfg_hdr;
    memset(&cfg_hdr, 0, sizeof(cfg_hdr));
    if (!uhci_get_desc(hc, low_speed, addr, ep0_mps, USB_DESC_CONFIG, 0, &cfg_hdr, 9)) {
        kprint("[UHCI] GET_CONFIG_HDR failed\n");
        return;
    }

    uint16_t total_len = cfg_hdr.wTotalLength;
    if (total_len < 9 || total_len > 512) total_len = 512;
    uint8_t* cfg_buf = (uint8_t*)kmalloc(total_len, 0, NULL);
    if (!cfg_buf) return;
    if (!uhci_get_desc(hc, low_speed, addr, ep0_mps, USB_DESC_CONFIG, 0, cfg_buf, total_len)) {
        kprint("[UHCI] GET_CONFIG failed\n");
        kfree(cfg_buf);
        return;
    }

    uint8_t hid_kbd_iface = 0, hid_mouse_iface = 0;
    uint8_t hid_kbd_ep = 0, hid_mouse_ep = 0;
    uint16_t hid_kbd_mps = 0, hid_mouse_mps = 0;
    uint16_t hid_kbd_report_len = 0, hid_mouse_report_len = 0;
    bool in_kbd = false;
    bool in_mouse = false;

    for (uint16_t off = 0; off + 2 <= total_len; ) {
        uint8_t len = cfg_buf[off];
        uint8_t type = cfg_buf[off + 1];
        if (len < 2) break;
        if (off + len > total_len) break;

        if (type == USB_DESC_INTERFACE && len >= sizeof(usb_interface_desc_t)) {
            usb_interface_desc_t* ifd = (usb_interface_desc_t*)(cfg_buf + off);
            in_kbd = (ifd->bInterfaceClass == 0x03 && ifd->bInterfaceSubClass == 0x01 && ifd->bInterfaceProtocol == 0x01);
            in_mouse = (ifd->bInterfaceClass == 0x03 && ifd->bInterfaceSubClass == 0x01 && ifd->bInterfaceProtocol == 0x02);
            if (in_kbd) hid_kbd_iface = ifd->bInterfaceNumber;
            if (in_mouse) hid_mouse_iface = ifd->bInterfaceNumber;
        } else if ((in_kbd || in_mouse) && type == USB_DESC_HID && len >= 9) {
            uint8_t num_desc = cfg_buf[off + 5];
            uint16_t desc_off = (uint16_t)(off + 6);
            for (uint8_t d = 0; d < num_desc; d++) {
                if (desc_off + 2 >= off + len) break;
                uint8_t desc_type = cfg_buf[desc_off];
                uint16_t desc_len = (uint16_t)(cfg_buf[desc_off + 1] | (cfg_buf[desc_off + 2] << 8));
                if (desc_type == USB_DESC_HID_REPORT) {
                    if (in_kbd) hid_kbd_report_len = desc_len;
                    if (in_mouse) hid_mouse_report_len = desc_len;
                }
                desc_off = (uint16_t)(desc_off + 3);
            }
        } else if ((in_kbd || in_mouse) && type == USB_DESC_ENDPOINT && len >= sizeof(usb_endpoint_desc_t)) {
            usb_endpoint_desc_t* epd = (usb_endpoint_desc_t*)(cfg_buf + off);
            if ((epd->bmAttributes & 0x3) == 0x3) {
                uint8_t ep_addr = epd->bEndpointAddress;
                uint16_t mps = epd->wMaxPacketSize & 0x7FF;
                if ((ep_addr & 0x80) != 0) {
                    if (in_kbd && hid_kbd_ep == 0) { hid_kbd_ep = ep_addr & 0x0F; hid_kbd_mps = mps ? mps : 8; }
                    if (in_mouse && hid_mouse_ep == 0) { hid_mouse_ep = ep_addr & 0x0F; hid_mouse_mps = mps ? mps : 4; }
                }
            }
        }
        off += len;
    }

    if (!uhci_set_configuration(hc, low_speed, addr, ep0_mps, cfg_hdr.bConfigurationValue)) {
        kprint("[UHCI] SET_CONFIGURATION failed\n");
        kfree(cfg_buf);
        return;
    }

    if (hid_kbd_ep) (void)uhci_hid_init(hc, low_speed, addr, hid_kbd_iface, hid_kbd_ep,
                                        hid_kbd_mps, false, ep0_mps, hid_kbd_report_len);
    if (hid_mouse_ep) (void)uhci_hid_init(hc, low_speed, addr, hid_mouse_iface, hid_mouse_ep,
                                          hid_mouse_mps, true, ep0_mps, hid_mouse_report_len);

    kfree(cfg_buf);
}

static void uhci_scan_ports(uhci_ctrl_t* hc) {
    bool verbose = bootlog_enabled;
    for (int p = 0; p < 2; p++) {
        bool low = false;
        if (uhci_port_reset(hc, p, &low)) {
            if (verbose) {
                kprintf("[UHCI] Device on port %d (low=%d)\n", p + 1, low ? 1 : 0);
            }
            uhci_enumerate_port(hc, low);
        }
    }
}

void uhci_pci_attach(uint8_t bus, uint8_t dev, uint8_t func,
                     uint16_t io_base, uint8_t irq_line) {
    (void)bus; (void)dev; (void)func;
    if (controller_count >= UHCI_MAX_CONTROLLERS) return;
    if (io_base == 0) return;
    bool verbose = bootlog_enabled;

    uhci_ctrl_t* hc = &controllers[controller_count++];
    memset(hc, 0, sizeof(*hc));
    hc->io = io_base;
    hc->irq_line = irq_line;
    hc->next_addr = 1;

    hc->frame_list = (uint32_t*)kmalloc_aligned(1024 * sizeof(uint32_t), 4096);
    hc->sched_qh = (uhci_qh_t*)kmalloc_aligned(sizeof(uhci_qh_t), 16);
    hc->tail_qh = hc->sched_qh;
    if (!hc->frame_list || !hc->sched_qh) return;
    memset(hc->frame_list, 0, 1024 * sizeof(uint32_t));
    memset(hc->sched_qh, 0, sizeof(*hc->sched_qh));

    hc->sched_qh->head = UHCI_PTR_TERM;
    hc->sched_qh->elem = UHCI_PTR_TERM;

    uint32_t qh_ptr = phys_addr(hc->sched_qh) | UHCI_PTR_QH | UHCI_PTR_DF;
    for (int i = 0; i < 1024; i++) hc->frame_list[i] = qh_ptr;

    if (!uhci_reset_controller(hc)) return;

    if (verbose) kprintf("[UHCI] Attached io=%x irq=%u\n", hc->io, hc->irq_line);
    uhci_scan_ports(hc);
}

void uhci_rescan_all_ports(void) {
    mouse_set_ignore_ps2(false);
    hid_dev_count = 0;
    memset(hid_devs, 0, sizeof(hid_devs));
    for (int i = 0; i < controller_count; i++) {
        controllers[i].next_addr = 1;
        controllers[i].tail_qh = controllers[i].sched_qh;
        controllers[i].sched_qh->head = UHCI_PTR_TERM;
        controllers[i].sched_qh->elem = UHCI_PTR_TERM;
        uhci_scan_ports(&controllers[i]);
    }
}

void uhci_poll_changes(void) {
    if (uhci_rescan_pending) return;
    for (int i = 0; i < controller_count; i++) {
        uhci_ctrl_t* hc = &controllers[i];
        for (int p = 0; p < 2; p++) {
            uint16_t off = (p == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
            uint16_t ps = rd16(hc->io, off);
            if (ps & (PORT_CSC | PORT_PEDC)) {
                wr16(hc->io, off, (uint16_t)(PORT_CSC | PORT_PEDC));
                uhci_queue_rescan();
                return;
            }
        }
    }
}

bool uhci_take_rescan_pending(void) {
    bool pending = false;
    hal_disable_interrupts();
    pending = uhci_rescan_pending;
    uhci_rescan_pending = false;
    hal_enable_interrupts();
    return pending;
}

void uhci_poll(void) {
    uhci_poll_changes();
    for (int i = 0; i < hid_dev_count; i++) {
        uhci_hid_dev_t* dev = &hid_devs[i];

        if (!dev->is_mouse) kbd_repeat_tick(dev);

        uhci_td_t* td = dev->td;
        if (!td) continue;
        if (td->status & TD_STS_ACTIVE) continue;

        uint16_t actual = td_actual_len(td);
        
        // 에러 감지: 토글 비트 동기화 오류 없이 에러 처리
        bool has_error = (td->status & (TD_STS_STALL | TD_STS_DBE | TD_STS_BABBLE | TD_STS_CRC_TO | TD_STS_BITSTUFF)) != 0;
        if (has_error) {
            // 에러 상황: actual = 0, 하지만 toggle은 성공한 전송처럼 진행
            actual = 0;
        }

        if (actual > 0) {
            // 데이터 수신 성공: 토글 비트를 한 번 더함
            if (dev->is_mouse) mouse_process(dev, actual);
            else kbd_process(dev, actual);
            dev->toggle ^= 1u;
        } else if (has_error) {
            // 에러 발생: 토글 비트를 반전시켜서 재시도
            dev->toggle ^= 1u;
        }
        // else: actual == 0 이고 에러 없음 (ZLP or timeout) - 토글 비트 유지

        // Rearm: 현재 토글 비트로 다음 전송 준비
        uint16_t blen = dev->poll_len;
        if (blen > sizeof(dev->buf)) blen = sizeof(dev->buf);
        td_init(td, UHCI_PTR_TERM, dev->low_speed, PID_IN, dev->addr, dev->ep, dev->toggle, dev->buf, blen, false);
        dev->qh->elem = phys_addr(td);
    }
}
