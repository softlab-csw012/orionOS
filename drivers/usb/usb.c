#include "usb.h"
#include "../hal.h"
#include "../screen.h"
#include "../mouse.h"
#include "hid_boot_kbd.h"
#include "ehci.h"
#include "ohci.h"
#include "xhci.h"
#include "../../fs/disk.h"
#include "../../mm/mem.h"
#include "../../libc/string.h"
#include "../../cpu/timer.h"

#pragma pack(push,1)
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

typedef struct {
    uint32_t dCBWSignature;
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t bmCBWFlags;
    uint8_t bCBWLUN;
    uint8_t bCBWCBLength;
    uint8_t CBWCB[16];
} msc_cbw_t;

typedef struct {
    uint32_t dCSWSignature;
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t bCSWStatus;
} msc_csw_t;

typedef struct {
    uint16_t wPortStatus;
    uint16_t wPortChange;
} usb_hub_port_status_t;
#pragma pack(pop)

enum {
    USB_DESC_DEVICE = 1,
    USB_DESC_CONFIG = 2,
    USB_DESC_INTERFACE = 4,
    USB_DESC_ENDPOINT = 5,
    USB_DESC_HID = 0x21,
    USB_DESC_HID_REPORT = 0x22,
    USB_DESC_HUB = 0x29,
};

enum {
    USB_REQ_GET_STATUS = 0,
    USB_REQ_CLEAR_FEATURE = 1,
    USB_REQ_SET_FEATURE = 3,
    USB_REQ_SET_ADDRESS = 5,
    USB_REQ_GET_DESCRIPTOR = 6,
    USB_REQ_SET_CONFIGURATION = 9,
    USB_REQ_SET_INTERFACE = 0x0B,
};

enum {
    USB_CLASS_HID = 0x03,
    USB_CLASS_MSC = 0x08,
    USB_CLASS_HUB = 0x09,
};

enum {
    USB_HID_SUBCLASS_BOOT = 0x01,
    USB_HID_PROTO_KBD = 0x01,
    USB_HID_PROTO_MOUSE = 0x02,
};

enum {
    HID_REQ_SET_IDLE = 0x0A,
    HID_REQ_SET_PROTOCOL = 0x0B,
};

#define HID_USAGE_PAGE_GENERIC 0x01
#define HID_USAGE_PAGE_KBD 0x07
#define HID_USAGE_PAGE_BUTTON 0x09
#define HID_USAGE_X 0x30
#define HID_USAGE_Y 0x31
#define HID_USAGE_WHEEL 0x38

#define HID_REPORT_MAX_TRACKED 4

typedef struct hid_report_info {
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

enum {
    USB_MSC_SUBCLASS_SCSI = 0x06,
    USB_MSC_PROTO_BULK_ONLY = 0x50,
};

#define USB_MAX_HID_DEVS 4

typedef enum {
    USB_HID_BOOT_KBD = 1,
    USB_HID_BOOT_MOUSE = 2,
} usb_hid_kind_t;

enum {
    USB_HUB_PORT_FEAT_RESET = 4,
    USB_HUB_PORT_FEAT_POWER = 8,
    USB_HUB_PORT_FEAT_C_CONNECTION = 16,
    USB_HUB_PORT_FEAT_C_ENABLE = 17,
    USB_HUB_PORT_FEAT_C_SUSPEND = 18,
    USB_HUB_PORT_FEAT_C_OVER_CURRENT = 19,
    USB_HUB_PORT_FEAT_C_RESET = 20,
};

enum {
    USB_HUB_PORT_STAT_CONNECTION = 0x0001,
    USB_HUB_PORT_STAT_ENABLE = 0x0002,
    USB_HUB_PORT_STAT_RESET = 0x0010,
    USB_HUB_PORT_STAT_POWER = 0x0100,
    USB_HUB_PORT_STAT_LOW_SPEED = 0x0200,
    USB_HUB_PORT_STAT_HIGH_SPEED = 0x0400,
};

enum {
    MSC_REQ_RESET = 0xFF,
    MSC_REQ_GET_MAX_LUN = 0xFE,
};

#define MSC_CBW_SIGNATURE 0x43425355u
#define MSC_CSW_SIGNATURE 0x53425355u

enum {
    SCSI_OP_TEST_UNIT_READY = 0x00,
    SCSI_OP_REQUEST_SENSE = 0x03,
    SCSI_OP_READ_CAPACITY10 = 0x25,
    SCSI_OP_READ_CAPACITY16 = 0x9E,
    SCSI_SA_READ_CAPACITY16 = 0x10,
    SCSI_OP_READ10 = 0x28,
    SCSI_OP_WRITE10 = 0x2A,
    SCSI_OP_SYNC_CACHE10 = 0x35,
};

typedef struct {
    usb_hc_t* hc;
    uint32_t dev;
    usb_speed_t speed;
    uint8_t tt_hub_addr;
    uint8_t tt_port;

    uint8_t ep0_mps;
    uint8_t interface_num;
    uint8_t bulk_in_ep;
    uint8_t bulk_out_ep;
    uint16_t bulk_in_mps;
    uint16_t bulk_out_mps;

    uint8_t bulk_in_toggle;
    uint8_t bulk_out_toggle;

    uint32_t block_size;
    uint32_t block_count;
    uint8_t drive_id;
    uint8_t max_lun;
} usb_msc_dev_t;

typedef struct {
    usb_hc_t* hc;
    uint32_t dev;
    usb_speed_t speed;
    uint8_t tt_hub_addr;
    uint8_t tt_port;

    usb_hid_kind_t kind;
    uint8_t iface_num;
    uint8_t intr_in_ep;
    uint16_t intr_in_mps;
    uint8_t intr_in_interval;

    bool report_proto;
    hid_report_info_t report;

    usb_async_in_t in;
    uint8_t buf[64];
    uint16_t buf_len;
} usb_hid_dev_t;

typedef struct {
    bool is_hub;

    bool msc_iface_present;
    bool msc_iface_found;
    uint8_t msc_iface_num;
    uint8_t msc_alt_setting;
    uint8_t bulk_in_ep;
    uint8_t bulk_out_ep;
    uint16_t bulk_in_mps;
    uint16_t bulk_out_mps;

    uint8_t hid_kbd_iface;
    uint8_t hid_kbd_ep;
    uint16_t hid_kbd_mps;
    uint8_t hid_kbd_interval;
    uint16_t hid_kbd_report_len;

    uint8_t hid_mouse_iface;
    uint8_t hid_mouse_ep;
    uint16_t hid_mouse_mps;
    uint8_t hid_mouse_interval;
    uint16_t hid_mouse_report_len;

    uint8_t last_iface_class;
    uint8_t last_iface_sub;
    uint8_t last_iface_proto;
    uint8_t last_alt;
} usb_parse_result_t;

static usb_msc_dev_t storage_devs[USB_MAX_STORAGE_DEVS];
static int storage_dev_count = 0;
static uint32_t msc_tag = 1;

static usb_hid_dev_t hid_devs[USB_MAX_HID_DEVS];
static int hid_dev_count = 0;

static void usb_enumerate_default(usb_hc_t* hc, usb_speed_t speed,
                                  uint8_t root_port,
                                  uint8_t tt_hub_addr, uint8_t tt_port,
                                  int depth);

static void usb_handle_hub(usb_hc_t* hc, uint32_t hub_dev, uint8_t ep0_mps,
                           usb_speed_t speed, uint8_t tt_hub_addr, uint8_t tt_port,
                           int depth);

static void delay_ms(uint32_t ms) {
    uint32_t start = tick;
    uint32_t ticks_needed = (ms + 9) / 10;
    if (ticks_needed == 0) ticks_needed = 1;
    while ((tick - start) < ticks_needed) {
        hal_wait_for_interrupt();
    }
}

static inline uint32_t ticks_to_ms(uint32_t t) {
    return t * 10u;
}

static bool usb_control(usb_hc_t* hc, uint32_t dev, uint8_t ep0_mps,
                        usb_speed_t speed, uint8_t tt_hub_addr, uint8_t tt_port,
                        const usb_setup_pkt_t* setup, void* data, uint16_t len) {
    if (!hc || !hc->ops || !hc->ops->control_transfer) return false;
    return hc->ops->control_transfer(hc, dev, 0, ep0_mps, speed,
                                     tt_hub_addr, tt_port, setup, data, len);
}

static bool usb_get_desc(usb_hc_t* hc, uint32_t dev, uint8_t ep0_mps,
                         usb_speed_t speed, uint8_t tt_hub_addr, uint8_t tt_port,
                         uint8_t type, uint8_t index, void* buf, uint16_t len) {
    usb_setup_pkt_t setup;
    setup.bmRequestType = 0x80;
    setup.bRequest = USB_REQ_GET_DESCRIPTOR;
    setup.wValue = (uint16_t)((type << 8) | index);
    setup.wIndex = 0;
    setup.wLength = len;
    return usb_control(hc, dev, ep0_mps, speed, tt_hub_addr, tt_port, &setup, buf, len);
}

static bool usb_get_report_desc(usb_hc_t* hc, uint32_t dev, uint8_t ep0_mps,
                                usb_speed_t speed, uint8_t tt_hub_addr, uint8_t tt_port,
                                uint8_t iface_num, void* buf, uint16_t len) {
    usb_setup_pkt_t setup;
    setup.bmRequestType = 0x81;
    setup.bRequest = USB_REQ_GET_DESCRIPTOR;
    setup.wValue = (uint16_t)(USB_DESC_HID_REPORT << 8);
    setup.wIndex = iface_num;
    setup.wLength = len;
    return usb_control(hc, dev, ep0_mps, speed, tt_hub_addr, tt_port, &setup, buf, len);
}

static bool usb_set_configuration(usb_hc_t* hc, uint32_t dev, uint8_t ep0_mps,
                                  usb_speed_t speed, uint8_t tt_hub_addr, uint8_t tt_port,
                                  uint8_t cfg_value) {
    usb_setup_pkt_t setup;
    setup.bmRequestType = 0x00;
    setup.bRequest = USB_REQ_SET_CONFIGURATION;
    setup.wValue = cfg_value;
    setup.wIndex = 0;
    setup.wLength = 0;
    return usb_control(hc, dev, ep0_mps, speed, tt_hub_addr, tt_port, &setup, NULL, 0);
}

static bool usb_set_interface(usb_hc_t* hc, uint32_t dev, uint8_t ep0_mps,
                              usb_speed_t speed, uint8_t tt_hub_addr, uint8_t tt_port,
                              uint8_t iface, uint8_t alt) {
    usb_setup_pkt_t setup;
    setup.bmRequestType = 0x01;
    setup.bRequest = USB_REQ_SET_INTERFACE;
    setup.wValue = alt;
    setup.wIndex = iface;
    setup.wLength = 0;
    return usb_control(hc, dev, ep0_mps, speed, tt_hub_addr, tt_port, &setup, NULL, 0);
}

static bool usb_hid_set_protocol(usb_hc_t* hc, uint32_t dev, uint8_t ep0_mps,
                                 usb_speed_t speed, uint8_t tt_hub_addr, uint8_t tt_port,
                                 uint8_t iface_num, uint16_t protocol) {
    usb_setup_pkt_t setup;
    setup.bmRequestType = 0x21;
    setup.bRequest = HID_REQ_SET_PROTOCOL;
    setup.wValue = protocol;
    setup.wIndex = iface_num;
    setup.wLength = 0;
    return usb_control(hc, dev, ep0_mps, speed, tt_hub_addr, tt_port, &setup, NULL, 0);
}

static bool usb_hid_set_idle(usb_hc_t* hc, uint32_t dev, uint8_t ep0_mps,
                             usb_speed_t speed, uint8_t tt_hub_addr, uint8_t tt_port,
                             uint8_t iface_num, uint8_t duration, uint8_t report_id) {
    usb_setup_pkt_t setup;
    setup.bmRequestType = 0x21;
    setup.bRequest = HID_REQ_SET_IDLE;
    setup.wValue = (uint16_t)(((uint16_t)duration << 8) | report_id);
    setup.wIndex = iface_num;
    setup.wLength = 0;
    return usb_control(hc, dev, ep0_mps, speed, tt_hub_addr, tt_port, &setup, NULL, 0);
}

#define HID_USAGE_PAGE_GENERIC 0x01
#define HID_USAGE_PAGE_KBD 0x07
#define HID_USAGE_PAGE_BUTTON 0x09
#define HID_USAGE_X 0x30
#define HID_USAGE_Y 0x31
#define HID_USAGE_WHEEL 0x38

#define HID_REPORT_MAX_TRACKED 4

typedef struct hid_report_info {
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

static void usb_hid_mouse_process(usb_hid_dev_t* dev, uint16_t actual) {
    if (!dev) return;
    if (dev->report_proto) {
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
        return;
    }

    if (actual < 3) return;
    uint8_t buttons = dev->buf[0];
    int dx = (int8_t)dev->buf[1];
    int dy = (int8_t)dev->buf[2];
    int wheel = 0;
    if (actual >= 4) wheel = (int8_t)dev->buf[3];
    mouse_inject(dx, dy, wheel, buttons);
}

static void usb_hid_attach(usb_hc_t* hc, uint32_t dev_handle, uint8_t ep0_mps,
                           usb_speed_t speed, uint8_t tt_hub_addr, uint8_t tt_port,
                           usb_hid_kind_t kind, uint8_t iface_num,
                           uint8_t intr_in_ep, uint16_t intr_in_mps, uint8_t intr_in_interval,
                           uint16_t report_len) {
    if (hid_dev_count >= USB_MAX_HID_DEVS) return;
    if (intr_in_ep == 0 || intr_in_mps == 0) return;

    if (hc && hc->ops && hc->ops->configure_endpoint) {
        if (!hc->ops->configure_endpoint(hc, dev_handle, intr_in_ep, true,
                                         USB_EP_INTERRUPT, intr_in_mps, intr_in_interval)) {
            kprint("[USB] HID: configure endpoint failed\n");
            return;
        }
    }

    usb_hid_dev_t* hid = &hid_devs[hid_dev_count++];
    memset(hid, 0, sizeof(*hid));
    hid->hc = hc;
    hid->dev = dev_handle;
    hid->speed = speed;
    hid->tt_hub_addr = tt_hub_addr;
    hid->tt_port = tt_port;
    hid->kind = kind;
    hid->iface_num = iface_num;
    hid->intr_in_ep = intr_in_ep;
    hid->intr_in_mps = intr_in_mps;
    hid->intr_in_interval = intr_in_interval;

    hid->report_proto = false;
    memset(&hid->report, 0, sizeof(hid->report));

    if (report_len > 0 && report_len <= 1024) {
        uint8_t* report_desc = (uint8_t*)kmalloc(report_len, 0, NULL);
        if (report_desc) {
            if (usb_get_report_desc(hc, dev_handle, ep0_mps, speed, tt_hub_addr, tt_port,
                                    iface_num, report_desc, report_len) &&
                hid_parse_report_desc(report_desc, report_len,
                                      kind == USB_HID_BOOT_MOUSE, &hid->report)) {
                uint16_t rpt_bytes = (uint16_t)((hid->report.report_bits + 7u) / 8u);
                if (hid->report.report_id != 0) rpt_bytes = (uint16_t)(rpt_bytes + 1);
                if (rpt_bytes > 0 && rpt_bytes <= intr_in_mps && rpt_bytes <= sizeof(hid->buf)) {
                    hid->report_proto = true;
                    hid->buf_len = rpt_bytes;
                }
            }
            kfree(report_desc);
        }
    }

    if (!hid->report_proto) {
        hid->buf_len = intr_in_mps;
        if (hid->buf_len < 8) hid->buf_len = 8;
        if (hid->buf_len > sizeof(hid->buf)) hid->buf_len = sizeof(hid->buf);
    }

    (void)usb_hid_set_idle(hc, dev_handle, ep0_mps, speed, tt_hub_addr, tt_port,
                           iface_num, 0, 0);
    (void)usb_hid_set_protocol(hc, dev_handle, ep0_mps, speed, tt_hub_addr, tt_port,
                               iface_num, hid->report_proto ? 1 : 0);

    bool ok = hc && hc->ops && hc->ops->async_in_init &&
              hc->ops->async_in_init(hc, &hid->in, dev_handle, intr_in_ep, intr_in_mps,
                                     speed, tt_hub_addr, tt_port,
                                     0, hid->buf, hid->buf_len);
    if (!ok) {
        hid_dev_count--;
        memset(hid, 0, sizeof(*hid));
        return;
    }

    if (kind == USB_HID_BOOT_MOUSE) {
        mouse_set_ignore_ps2(true);
        kprintf("[USB] HID mouse dev=%u ep=%u mps=%u\n",
                (uint32_t)dev_handle, intr_in_ep, intr_in_mps);
    }
}

static bool usb_hub_get_descriptor(usb_hc_t* hc, uint32_t hub_dev, uint8_t ep0_mps,
                                   usb_speed_t speed, uint8_t tt_hub_addr, uint8_t tt_port,
                                   void* buf, uint16_t len) {
    usb_setup_pkt_t setup;
    setup.bmRequestType = 0xA0;
    setup.bRequest = USB_REQ_GET_DESCRIPTOR;
    setup.wValue = (uint16_t)(USB_DESC_HUB << 8);
    setup.wIndex = 0;
    setup.wLength = len;
    return usb_control(hc, hub_dev, ep0_mps, speed, tt_hub_addr, tt_port, &setup, buf, len);
}

static bool usb_hub_port_set_feature(usb_hc_t* hc, uint32_t hub_dev, uint8_t ep0_mps,
                                     usb_speed_t speed, uint8_t tt_hub_addr, uint8_t tt_port,
                                     uint16_t feature, uint8_t port) {
    usb_setup_pkt_t setup;
    setup.bmRequestType = 0x23;
    setup.bRequest = USB_REQ_SET_FEATURE;
    setup.wValue = feature;
    setup.wIndex = port;
    setup.wLength = 0;
    return usb_control(hc, hub_dev, ep0_mps, speed, tt_hub_addr, tt_port, &setup, NULL, 0);
}

static bool usb_hub_port_clear_feature(usb_hc_t* hc, uint32_t hub_dev, uint8_t ep0_mps,
                                       usb_speed_t speed, uint8_t tt_hub_addr, uint8_t tt_port,
                                       uint16_t feature, uint8_t port) {
    usb_setup_pkt_t setup;
    setup.bmRequestType = 0x23;
    setup.bRequest = USB_REQ_CLEAR_FEATURE;
    setup.wValue = feature;
    setup.wIndex = port;
    setup.wLength = 0;
    return usb_control(hc, hub_dev, ep0_mps, speed, tt_hub_addr, tt_port, &setup, NULL, 0);
}

static bool usb_hub_port_get_status(usb_hc_t* hc, uint32_t hub_dev, uint8_t ep0_mps,
                                    usb_speed_t speed, uint8_t tt_hub_addr, uint8_t tt_port,
                                    uint8_t port, usb_hub_port_status_t* st) {
    usb_setup_pkt_t setup;
    setup.bmRequestType = 0xA3;
    setup.bRequest = USB_REQ_GET_STATUS;
    setup.wValue = 0;
    setup.wIndex = port;
    setup.wLength = 4;
    return usb_control(hc, hub_dev, ep0_mps, speed, tt_hub_addr, tt_port, &setup, st, 4);
}

static bool msc_clear_halt(usb_msc_dev_t* dev, uint8_t ep_addr) {
    usb_setup_pkt_t setup;
    setup.bmRequestType = 0x02;
    setup.bRequest = USB_REQ_CLEAR_FEATURE;
    setup.wValue = 0;
    setup.wIndex = ep_addr;
    setup.wLength = 0;
    return usb_control(dev->hc, dev->dev, dev->ep0_mps, dev->speed,
                       dev->tt_hub_addr, dev->tt_port, &setup, NULL, 0);
}

static bool msc_bulk_only_reset(usb_msc_dev_t* dev) {
    usb_setup_pkt_t setup;
    setup.bmRequestType = 0x21;
    setup.bRequest = MSC_REQ_RESET;
    setup.wValue = 0;
    setup.wIndex = dev->interface_num;
    setup.wLength = 0;
    return usb_control(dev->hc, dev->dev, dev->ep0_mps, dev->speed,
                       dev->tt_hub_addr, dev->tt_port, &setup, NULL, 0);
}

static void msc_reset_recovery(usb_msc_dev_t* dev) {
    if (!dev) return;
    (void)msc_bulk_only_reset(dev);
    (void)msc_clear_halt(dev, (uint8_t)(0x80 | dev->bulk_in_ep));
    (void)msc_clear_halt(dev, dev->bulk_out_ep);
    dev->bulk_in_toggle = 0;
    dev->bulk_out_toggle = 0;
}

static bool msc_bulk_in(usb_msc_dev_t* dev, void* data, uint16_t len) {
    if (!dev || !dev->hc || !dev->hc->ops || !dev->hc->ops->bulk_transfer) return false;
    bool ok = dev->hc->ops->bulk_transfer(dev->hc, dev->dev, dev->bulk_in_ep, true,
                                          dev->bulk_in_mps, dev->speed,
                                          dev->tt_hub_addr, dev->tt_port,
                                          dev->bulk_in_toggle, data, len);
    if (ok && len > 0) {
        dev->bulk_in_toggle ^= 1;
    }
    return ok;
}

static bool msc_bulk_out(usb_msc_dev_t* dev, const void* data, uint16_t len) {
    if (!dev || !dev->hc || !dev->hc->ops || !dev->hc->ops->bulk_transfer) return false;
    bool ok = dev->hc->ops->bulk_transfer(dev->hc, dev->dev, dev->bulk_out_ep, false,
                                          dev->bulk_out_mps, dev->speed,
                                          dev->tt_hub_addr, dev->tt_port,
                                          dev->bulk_out_toggle, (void*)data, len);
    if (ok && len > 0) {
        dev->bulk_out_toggle ^= 1;
    }
    return ok;
}

static bool msc_bot_cmd(usb_msc_dev_t* dev, uint8_t lun,
                        const uint8_t* cdb, uint8_t cdb_len,
                        bool data_in, void* data, uint32_t data_len,
                        uint8_t* csw_status_out) {
    uint8_t attempts = USB_MSC_BOT_RETRIES;
    if (attempts == 0) attempts = 1;
    for (uint8_t attempt = 0; attempt < attempts; attempt++) {
        msc_cbw_t cbw;
        memset(&cbw, 0, sizeof(cbw));
        cbw.dCBWSignature = MSC_CBW_SIGNATURE;
        cbw.dCBWTag = msc_tag++;
        cbw.dCBWDataTransferLength = data_len;
        cbw.bmCBWFlags = data_in ? 0x80 : 0x00;
        cbw.bCBWLUN = lun;
        cbw.bCBWCBLength = cdb_len;
        memcpy(cbw.CBWCB, cdb, cdb_len);

        bool ok = msc_bulk_out(dev, &cbw, (uint16_t)sizeof(cbw));
        if (!ok) goto retry;

        if (data_len > 0 && data != NULL) {
            if (data_in) {
                ok = msc_bulk_in(dev, data, (uint16_t)data_len);
                if (!ok) goto retry;
            } else {
                ok = msc_bulk_out(dev, data, (uint16_t)data_len);
                if (!ok) goto retry;
            }
        }

        msc_csw_t csw;
        ok = msc_bulk_in(dev, &csw, (uint16_t)sizeof(csw));
        if (!ok) goto retry;

        if (csw.dCSWSignature != MSC_CSW_SIGNATURE || csw.dCSWTag != cbw.dCBWTag) {
            kprint("[MSC] Bad CSW\n");
            goto retry;
        }
        if (csw_status_out) *csw_status_out = csw.bCSWStatus;
        if (csw.bCSWStatus == 0) return true;
        if (csw.bCSWStatus == 2) goto retry;
        return false;

retry:
        msc_reset_recovery(dev);
        if (USB_MSC_BOT_RETRY_DELAY_MS) {
            delay_ms(USB_MSC_BOT_RETRY_DELAY_MS);
        }
    }
    return false;
}

static bool msc_scsi_test_unit_ready(usb_msc_dev_t* dev) {
    uint8_t cdb[6] = {0};
    cdb[0] = SCSI_OP_TEST_UNIT_READY;
    return msc_bot_cmd(dev, 0, cdb, 6, false, NULL, 0, NULL);
}

static bool msc_scsi_request_sense(usb_msc_dev_t* dev, uint8_t* key, uint8_t* asc, uint8_t* ascq) {
    uint8_t cdb[6] = {0};
    cdb[0] = SCSI_OP_REQUEST_SENSE;
    cdb[4] = 18;
    uint8_t buf[18] = {0};
    if (!msc_bot_cmd(dev, 0, cdb, 6, true, buf, sizeof(buf), NULL)) return false;
    if (key) *key = buf[2] & 0x0F;
    if (asc) *asc = buf[12];
    if (ascq) *ascq = buf[13];
    return true;
}

static bool msc_scsi_read_capacity10(usb_msc_dev_t* dev, uint32_t* out_last_lba, uint32_t* out_blksz) {
    uint8_t cdb[10] = {0};
    cdb[0] = SCSI_OP_READ_CAPACITY10;
    uint8_t buf[8] = {0};
    uint8_t status = 0;
    if (!msc_bot_cmd(dev, 0, cdb, 10, true, buf, sizeof(buf), &status)) return false;
    uint32_t last_lba = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
    uint32_t blksz = (buf[4] << 24) | (buf[5] << 16) | (buf[6] << 8) | buf[7];
    if (out_last_lba) *out_last_lba = last_lba;
    if (out_blksz) *out_blksz = blksz;
    return blksz != 0;
}

static bool msc_scsi_read_capacity16(usb_msc_dev_t* dev, uint64_t* out_last_lba, uint32_t* out_blksz) {
    uint8_t cdb[16] = {0};
    cdb[0] = SCSI_OP_READ_CAPACITY16;
    cdb[1] = SCSI_SA_READ_CAPACITY16;
    cdb[10] = 0;
    cdb[11] = 0;
    cdb[12] = 0;
    cdb[13] = 32;
    uint8_t buf[32] = {0};
    if (!msc_bot_cmd(dev, 0, cdb, 16, true, buf, sizeof(buf), NULL)) return false;
    uint64_t last_lba = ((uint64_t)buf[0] << 56) | ((uint64_t)buf[1] << 48) |
                        ((uint64_t)buf[2] << 40) | ((uint64_t)buf[3] << 32) |
                        ((uint64_t)buf[4] << 24) | ((uint64_t)buf[5] << 16) |
                        ((uint64_t)buf[6] << 8)  | (uint64_t)buf[7];
    uint32_t blksz = (buf[8] << 24) | (buf[9] << 16) | (buf[10] << 8) | buf[11];
    if (out_last_lba) *out_last_lba = last_lba;
    if (out_blksz) *out_blksz = blksz;
    return blksz != 0;
}

static bool msc_scsi_sync_cache(usb_msc_dev_t* dev) {
    uint8_t cdb[10] = {0};
    cdb[0] = SCSI_OP_SYNC_CACHE10;
    return msc_bot_cmd(dev, 0, cdb, 10, false, NULL, 0, NULL);
}

static void msc_wait_ready(usb_msc_dev_t* dev) {
    uint8_t key = 0;
    uint8_t asc = 0;
    uint8_t ascq = 0;

    uint8_t attempts = USB_MSC_TUR_RETRIES;
    if (attempts == 0) attempts = 1;
    for (uint8_t i = 0; i < attempts; i++) {
        if (msc_scsi_test_unit_ready(dev)) return;
        if (msc_scsi_request_sense(dev, &key, &asc, &ascq)) {
            if (key == 0x02 || key == 0x06) {
                if (USB_MSC_TUR_NOT_READY_DELAY_MS) {
                    delay_ms(USB_MSC_TUR_NOT_READY_DELAY_MS);
                }
                continue;
            }
        }
        if (USB_MSC_TUR_FAIL_DELAY_MS) {
            delay_ms(USB_MSC_TUR_FAIL_DELAY_MS);
        }
    }
}

static bool msc_scsi_read_capacity(usb_msc_dev_t* dev) {
    msc_wait_ready(dev);

    uint32_t last_lba = 0;
    uint32_t blksz = 0;

    uint8_t attempts = USB_MSC_READ_CAPACITY_RETRIES;
    if (attempts == 0) attempts = 1;
    for (uint8_t i = 0; i < attempts; i++) {
        if (msc_scsi_read_capacity10(dev, &last_lba, &blksz)) {
            if (last_lba == 0xFFFFFFFFu) {
                uint64_t last_lba64 = 0;
                if (!msc_scsi_read_capacity16(dev, &last_lba64, &blksz)) return false;
                if (last_lba64 >= 0xFFFFFFFFu) {
                    dev->block_count = 0xFFFFFFFFu;
                } else {
                    dev->block_count = (uint32_t)(last_lba64 + 1);
                }
                dev->block_size = blksz;
                kprintf("[MSC] Capacity blocks=%u size=%u\n", dev->block_count, dev->block_size);
                return blksz != 0;
            }
            dev->block_count = last_lba + 1;
            dev->block_size = blksz;
            kprintf("[MSC] Capacity blocks=%u size=%u\n", dev->block_count, dev->block_size);
            return blksz != 0;
        }

        uint8_t key = 0, asc = 0, ascq = 0;
        if (msc_scsi_request_sense(dev, &key, &asc, &ascq)) {
            if (key == 0x02 || key == 0x06) {
                if (USB_MSC_READ_CAPACITY_NOT_READY_DELAY_MS) {
                    delay_ms(USB_MSC_READ_CAPACITY_NOT_READY_DELAY_MS);
                }
                continue;
            }
        }
        if (USB_MSC_READ_CAPACITY_FAIL_DELAY_MS) {
            delay_ms(USB_MSC_READ_CAPACITY_FAIL_DELAY_MS);
        }
    }
    return false;
}

static bool msc_scsi_read10(usb_msc_dev_t* dev, uint32_t lba, uint16_t blocks, void* out) {
    uint8_t cdb[10] = {0};
    cdb[0] = SCSI_OP_READ10;
    cdb[2] = (lba >> 24) & 0xFF;
    cdb[3] = (lba >> 16) & 0xFF;
    cdb[4] = (lba >> 8) & 0xFF;
    cdb[5] = lba & 0xFF;
    cdb[7] = (blocks >> 8) & 0xFF;
    cdb[8] = blocks & 0xFF;
    uint32_t len = (uint32_t)blocks * dev->block_size;
    return msc_bot_cmd(dev, 0, cdb, 10, true, out, len, NULL);
}

static bool msc_scsi_write10(usb_msc_dev_t* dev, uint32_t lba, uint16_t blocks, const void* inbuf) {
    uint8_t cdb[10] = {0};
    cdb[0] = SCSI_OP_WRITE10;
    cdb[2] = (lba >> 24) & 0xFF;
    cdb[3] = (lba >> 16) & 0xFF;
    cdb[4] = (lba >> 8) & 0xFF;
    cdb[5] = lba & 0xFF;
    cdb[7] = (blocks >> 8) & 0xFF;
    cdb[8] = blocks & 0xFF;
    uint32_t len = (uint32_t)blocks * dev->block_size;
    return msc_bot_cmd(dev, 0, cdb, 10, false, (void*)inbuf, len, NULL);
}

static bool msc_get_max_lun(usb_msc_dev_t* dev, uint8_t ep0_mps, uint8_t* out_maxlun) {
    usb_setup_pkt_t setup;
    setup.bmRequestType = 0xA1;
    setup.bRequest = MSC_REQ_GET_MAX_LUN;
    setup.wValue = 0;
    setup.wIndex = dev->interface_num;
    setup.wLength = 1;
    uint8_t maxlun = 0;
    uint8_t attempts = USB_MSC_GET_MAX_LUN_RETRIES;
    if (attempts == 0) attempts = 1;
    for (uint8_t i = 0; i < attempts; i++) {
        if (usb_control(dev->hc, dev->dev, ep0_mps, dev->speed,
                        dev->tt_hub_addr, dev->tt_port, &setup, &maxlun, 1)) {
            if (out_maxlun) *out_maxlun = maxlun;
            return true;
        }
        if (USB_MSC_GET_MAX_LUN_RETRY_DELAY_MS) {
            delay_ms(USB_MSC_GET_MAX_LUN_RETRY_DELAY_MS);
        }
    }
    return false;
}

static usb_msc_dev_t* find_dev_by_drive(uint8_t drive) {
    for (int i = 0; i < storage_dev_count; i++) {
        if (storage_devs[i].drive_id == drive) return &storage_devs[i];
    }
    return NULL;
}

static void usb_parse_config(const uint8_t* cfg, uint16_t total_len, usb_parse_result_t* out) {
    memset(out, 0, sizeof(*out));
    if (!cfg || total_len < 2) return;

    bool in_msc_iface = false;
    bool in_hid_kbd_iface = false;
    bool in_hid_mouse_iface = false;

    uint8_t primary_msc_iface = 0xFF;
    uint8_t cur_iface_num = 0;
    uint8_t cur_alt_setting = 0;
    uint8_t cur_ep_count = 0;
    uint8_t cur_bulk_in = 0;
    uint8_t cur_bulk_out = 0;
    uint16_t cur_bulk_in_mps = 0;
    uint16_t cur_bulk_out_mps = 0;

    bool best_valid = false;
    uint8_t best_iface_num = 0;
    uint8_t best_alt_setting = 0;
    uint8_t best_bulk_in = 0;
    uint8_t best_bulk_out = 0;
    uint16_t best_bulk_in_mps = 0;
    uint16_t best_bulk_out_mps = 0;
    uint8_t best_ep_count = 0;
    uint32_t best_mps_sum = 0;

    for (uint16_t off = 0; off + 2 <= total_len; ) {
        uint8_t len = cfg[off];
        uint8_t type = cfg[off + 1];
        if (len < 2) break;
        if (off + len > total_len) break;

        if (type == USB_DESC_INTERFACE && len >= sizeof(usb_interface_desc_t)) {
            if (in_msc_iface && cur_bulk_in != 0 && cur_bulk_out != 0) {
                uint32_t cur_mps_sum = (uint32_t)cur_bulk_in_mps + (uint32_t)cur_bulk_out_mps;
                bool better = !best_valid;
                if (!better) {
                    if (cur_ep_count > best_ep_count) {
                        better = true;
                    } else if (cur_ep_count == best_ep_count && cur_mps_sum > best_mps_sum) {
                        better = true;
                    } else if (cur_ep_count == best_ep_count && cur_mps_sum == best_mps_sum &&
                               cur_bulk_in_mps > best_bulk_in_mps) {
                        better = true;
                    }
                }
                if (better) {
                    best_valid = true;
                    best_iface_num = cur_iface_num;
                    best_alt_setting = cur_alt_setting;
                    best_bulk_in = cur_bulk_in;
                    best_bulk_out = cur_bulk_out;
                    best_bulk_in_mps = cur_bulk_in_mps;
                    best_bulk_out_mps = cur_bulk_out_mps;
                    best_ep_count = cur_ep_count;
                    best_mps_sum = cur_mps_sum;
                }
            }

            usb_interface_desc_t* ifd = (usb_interface_desc_t*)(cfg + off);
            out->last_alt = ifd->bAlternateSetting;
            out->last_iface_class = ifd->bInterfaceClass;
            out->last_iface_sub = ifd->bInterfaceSubClass;
            out->last_iface_proto = ifd->bInterfaceProtocol;

            if (ifd->bInterfaceClass == USB_CLASS_HUB) out->is_hub = true;

            bool is_msc = (ifd->bInterfaceClass == USB_CLASS_MSC &&
                           ifd->bInterfaceSubClass == USB_MSC_SUBCLASS_SCSI &&
                           ifd->bInterfaceProtocol == USB_MSC_PROTO_BULK_ONLY);
            if (is_msc) {
                out->msc_iface_present = true;
                if (primary_msc_iface == 0xFF) primary_msc_iface = ifd->bInterfaceNumber;
            }
            in_msc_iface = is_msc && ifd->bInterfaceNumber == primary_msc_iface;
            if (in_msc_iface) {
                cur_iface_num = ifd->bInterfaceNumber;
                cur_alt_setting = ifd->bAlternateSetting;
                cur_ep_count = ifd->bNumEndpoints;
                cur_bulk_in = 0;
                cur_bulk_out = 0;
                cur_bulk_in_mps = 0;
                cur_bulk_out_mps = 0;
            }

            in_hid_kbd_iface = (ifd->bInterfaceClass == USB_CLASS_HID &&
                                ifd->bInterfaceSubClass == USB_HID_SUBCLASS_BOOT &&
                                ifd->bInterfaceProtocol == USB_HID_PROTO_KBD);
            if (in_hid_kbd_iface) out->hid_kbd_iface = ifd->bInterfaceNumber;

            in_hid_mouse_iface = (ifd->bInterfaceClass == USB_CLASS_HID &&
                                  ifd->bInterfaceSubClass == USB_HID_SUBCLASS_BOOT &&
                                  ifd->bInterfaceProtocol == USB_HID_PROTO_MOUSE);
            if (in_hid_mouse_iface) out->hid_mouse_iface = ifd->bInterfaceNumber;
        } else if ((in_hid_kbd_iface || in_hid_mouse_iface) && type == USB_DESC_HID && len >= 9) {
            uint8_t num_desc = cfg[off + 5];
            uint16_t desc_off = (uint16_t)(off + 6);
            for (uint8_t d = 0; d < num_desc; d++) {
                if (desc_off + 2 >= off + len) break;
                uint8_t desc_type = cfg[desc_off];
                uint16_t desc_len = (uint16_t)(cfg[desc_off + 1] | (cfg[desc_off + 2] << 8));
                if (desc_type == USB_DESC_HID_REPORT) {
                    if (in_hid_kbd_iface) out->hid_kbd_report_len = desc_len;
                    if (in_hid_mouse_iface) out->hid_mouse_report_len = desc_len;
                }
                desc_off = (uint16_t)(desc_off + 3);
            }
        } else if (type == USB_DESC_ENDPOINT && len >= sizeof(usb_endpoint_desc_t)) {
            usb_endpoint_desc_t* epd = (usb_endpoint_desc_t*)(cfg + off);
            uint8_t ep_addr = epd->bEndpointAddress;
            uint16_t mps = epd->wMaxPacketSize & 0x7FF;

            if (in_msc_iface) {
                if ((epd->bmAttributes & 0x3) == USB_EP_BULK) {
                    if (ep_addr & 0x80) {
                        cur_bulk_in = ep_addr & 0x0F;
                        cur_bulk_in_mps = mps;
                    } else {
                        cur_bulk_out = ep_addr & 0x0F;
                        cur_bulk_out_mps = mps;
                    }
                }
            } else if (in_hid_kbd_iface || in_hid_mouse_iface) {
                if ((epd->bmAttributes & 0x3) == USB_EP_INTERRUPT) {
                    if ((ep_addr & 0x80) != 0) {
                        if (in_hid_kbd_iface && out->hid_kbd_ep == 0) {
                            out->hid_kbd_ep = ep_addr & 0x0F;
                            out->hid_kbd_mps = mps;
                            out->hid_kbd_interval = epd->bInterval;
                        }
                        if (in_hid_mouse_iface && out->hid_mouse_ep == 0) {
                            out->hid_mouse_ep = ep_addr & 0x0F;
                            out->hid_mouse_mps = mps;
                            out->hid_mouse_interval = epd->bInterval;
                        }
                    }
                }
            }
        }
        off += len;
    }

    if (in_msc_iface && cur_bulk_in != 0 && cur_bulk_out != 0) {
        uint32_t cur_mps_sum = (uint32_t)cur_bulk_in_mps + (uint32_t)cur_bulk_out_mps;
        bool better = !best_valid;
        if (!better) {
            if (cur_ep_count > best_ep_count) {
                better = true;
            } else if (cur_ep_count == best_ep_count && cur_mps_sum > best_mps_sum) {
                better = true;
            } else if (cur_ep_count == best_ep_count && cur_mps_sum == best_mps_sum &&
                       cur_bulk_in_mps > best_bulk_in_mps) {
                better = true;
            }
        }
        if (better) {
            best_valid = true;
            best_iface_num = cur_iface_num;
            best_alt_setting = cur_alt_setting;
            best_bulk_in = cur_bulk_in;
            best_bulk_out = cur_bulk_out;
            best_bulk_in_mps = cur_bulk_in_mps;
            best_bulk_out_mps = cur_bulk_out_mps;
            best_ep_count = cur_ep_count;
            best_mps_sum = cur_mps_sum;
        }
    }

    if (best_valid) {
        out->msc_iface_found = true;
        out->msc_iface_num = best_iface_num;
        out->msc_alt_setting = best_alt_setting;
        out->bulk_in_ep = best_bulk_in;
        out->bulk_out_ep = best_bulk_out;
        out->bulk_in_mps = best_bulk_in_mps;
        out->bulk_out_mps = best_bulk_out_mps;
    }
}

static void usb_enumerate_default(usb_hc_t* hc, usb_speed_t speed,
                                  uint8_t root_port,
                                  uint8_t tt_hub_addr, uint8_t tt_port,
                                  int depth) {
    if (storage_dev_count >= USB_MAX_STORAGE_DEVS && hid_dev_count >= USB_MAX_HID_DEVS) return;
    if (!hc || !hc->ops || !hc->ops->enum_open || !hc->ops->enum_set_address || !hc->ops->alloc_address) return;

    uint32_t dev_default = 0;
    if (!hc->ops->enum_open(hc, root_port, speed, &dev_default)) return;
    uint32_t dev_handle = dev_default;
    uint8_t* cfg_buf = NULL;

    uint8_t dev_desc8[8] = {0};
    usb_setup_pkt_t setup;
    setup.bmRequestType = 0x80;
    setup.bRequest = USB_REQ_GET_DESCRIPTOR;
    setup.wValue = (USB_DESC_DEVICE << 8);
    setup.wIndex = 0;
    setup.wLength = 8;

    bool got_desc8 = false;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (usb_control(hc, dev_handle, 8, speed, tt_hub_addr, tt_port, &setup, dev_desc8, 8)) {
            got_desc8 = true;
            break;
        }
        delay_ms(50);
    }
    if (!got_desc8) {
        kprint("[USB] GET_DESC8 failed\n");
        goto fail;
    }

    uint8_t ep0_mps = dev_desc8[7];
    if (ep0_mps == 0) ep0_mps = 8;

    uint8_t desired_addr = hc->ops->alloc_address(hc);
    if (!hc->ops->enum_set_address(hc, dev_default, ep0_mps, speed,
                                   tt_hub_addr, tt_port, desired_addr, &dev_handle)) {
        kprint("[USB] SET_ADDRESS failed\n");
        goto fail;
    }

    usb_device_desc_t dev_desc;
    memset(&dev_desc, 0, sizeof(dev_desc));
    if (!usb_get_desc(hc, dev_handle, ep0_mps, speed, tt_hub_addr, tt_port,
                      USB_DESC_DEVICE, 0, &dev_desc, sizeof(dev_desc))) {
        kprint("[USB] GET_DEVICE_DESC failed\n");
        goto fail;
    }
    kprintf("[USB] Dev %04x:%04x class=%02x/%02x/%02x ep0=%u\n",
            dev_desc.idVendor, dev_desc.idProduct,
            dev_desc.bDeviceClass, dev_desc.bDeviceSubClass, dev_desc.bDeviceProtocol,
            ep0_mps);

    usb_config_desc_t cfg_hdr;
    memset(&cfg_hdr, 0, sizeof(cfg_hdr));
    if (!usb_get_desc(hc, dev_handle, ep0_mps, speed, tt_hub_addr, tt_port,
                      USB_DESC_CONFIG, 0, &cfg_hdr, 9)) {
        kprint("[USB] GET_CONFIG_HDR failed\n");
        goto fail;
    }

    uint16_t total_len = cfg_hdr.wTotalLength;
    if (total_len < 9 || total_len > 512) total_len = 512;

    cfg_buf = (uint8_t*)kmalloc(total_len, 0, NULL);
    if (!cfg_buf) goto fail;
    if (!usb_get_desc(hc, dev_handle, ep0_mps, speed, tt_hub_addr, tt_port,
                      USB_DESC_CONFIG, 0, cfg_buf, total_len)) {
        kprint("[USB] GET_CONFIG failed\n");
        goto fail;
    }

    usb_parse_result_t parsed;
    usb_parse_config(cfg_buf, total_len, &parsed);

    bool is_hub = (dev_desc.bDeviceClass == USB_CLASS_HUB) || parsed.is_hub;

    if (!usb_set_configuration(hc, dev_handle, ep0_mps, speed, tt_hub_addr, tt_port,
                               cfg_hdr.bConfigurationValue)) {
        kprint("[USB] SET_CONFIGURATION failed\n");
        goto fail;
    }

    if (parsed.msc_iface_found && parsed.msc_alt_setting != 0) {
        if (!usb_set_interface(hc, dev_handle, ep0_mps, speed, tt_hub_addr, tt_port,
                               parsed.msc_iface_num, parsed.msc_alt_setting)) {
            kprintf("[USB] SET_INTERFACE iface=%u alt=%u failed\n",
                    parsed.msc_iface_num, parsed.msc_alt_setting);
            goto fail;
        }
    }

    if (is_hub) {
        usb_handle_hub(hc, dev_handle, ep0_mps, speed, tt_hub_addr, tt_port, depth);
        if (hc && hc->ops && hc->ops->enum_close) hc->ops->enum_close(hc, dev_handle);
        goto out;
    }

    if (parsed.bulk_in_ep == 0 || parsed.bulk_out_ep == 0) {
        if (!parsed.msc_iface_present) {
            if (parsed.hid_kbd_ep != 0) {
                (void)hid_boot_kbd_add_device(hc, dev_handle, ep0_mps, speed, tt_hub_addr, tt_port,
                                              parsed.hid_kbd_iface,
                                              parsed.hid_kbd_ep,
                                              parsed.hid_kbd_mps ? parsed.hid_kbd_mps : 8,
                                              parsed.hid_kbd_interval,
                                              parsed.hid_kbd_report_len);
            }
            if (parsed.hid_mouse_ep != 0) {
                usb_hid_attach(hc, dev_handle, ep0_mps, speed, tt_hub_addr, tt_port,
                               USB_HID_BOOT_MOUSE, parsed.hid_mouse_iface,
                               parsed.hid_mouse_ep,
                               parsed.hid_mouse_mps ? parsed.hid_mouse_mps : 4,
                               parsed.hid_mouse_interval,
                               parsed.hid_mouse_report_len);
            }
            if (parsed.hid_kbd_ep == 0 && parsed.hid_mouse_ep == 0) {
                kprintf("[USB] No bulk endpoints (last iface %02x/%02x/%02x alt=%u)\n",
                        parsed.last_iface_class, parsed.last_iface_sub,
                        parsed.last_iface_proto, parsed.last_alt);
            }
        } else {
            kprint("[USB] MSC interface present, HID ignored\n");
        }
        goto out;
    }

    if (storage_dev_count >= USB_MAX_STORAGE_DEVS) {
        kprint("[USB] MSC device ignored (storage slots full)\n");
        goto out;
    }

    if (hc->ops->configure_endpoint) {
        if (!hc->ops->configure_endpoint(hc, dev_handle, parsed.bulk_out_ep, false,
                                         USB_EP_BULK, parsed.bulk_out_mps ? parsed.bulk_out_mps : 64, 0) ||
            !hc->ops->configure_endpoint(hc, dev_handle, parsed.bulk_in_ep, true,
                                         USB_EP_BULK, parsed.bulk_in_mps ? parsed.bulk_in_mps : 64, 0)) {
            kprint("[USB] MSC: configure endpoints failed\n");
            goto fail;
        }
    }

    usb_msc_dev_t* msc = &storage_devs[storage_dev_count];
    memset(msc, 0, sizeof(*msc));
    msc->hc = hc;
    msc->dev = dev_handle;
    msc->speed = speed;
    msc->tt_hub_addr = tt_hub_addr;
    msc->tt_port = tt_port;
    msc->ep0_mps = ep0_mps;
    msc->interface_num = parsed.msc_iface_num;
    msc->bulk_in_ep = parsed.bulk_in_ep;
    msc->bulk_out_ep = parsed.bulk_out_ep;
    msc->bulk_in_mps = parsed.bulk_in_mps ? parsed.bulk_in_mps : 64;
    msc->bulk_out_mps = parsed.bulk_out_mps ? parsed.bulk_out_mps : 64;
    msc->bulk_in_toggle = 0;
    msc->bulk_out_toggle = 0;
    msc->drive_id = (uint8_t)(USB_DRIVE_BASE + storage_dev_count);

    if (USB_STORAGE_SETTLE_DELAY_MS) {
        delay_ms(USB_STORAGE_SETTLE_DELAY_MS);
    }

    msc->max_lun = 0;
    if (parsed.msc_iface_found) {
        uint8_t max_lun = 0;
        if (msc_get_max_lun(msc, ep0_mps, &max_lun)) {
            msc->max_lun = max_lun;
        } else {
            msc->max_lun = 0;
        }
    }
    if (USB_MSC_POST_MAX_LUN_DELAY_MS) {
        delay_ms(USB_MSC_POST_MAX_LUN_DELAY_MS);
    }
    if (!msc_scsi_read_capacity(msc)) {
        uint8_t key = 0, asc = 0, ascq = 0;
        if (msc_scsi_request_sense(msc, &key, &asc, &ascq)) {
            kprintf("[USB] READ_CAPACITY failed: sense key=%02x asc=%02x ascq=%02x\n",
                    key, asc, ascq);
        } else {
            kprint("[USB] READ_CAPACITY failed: sense unavailable\n");
        }
        delay_ms(500);
        goto fail;
    }

    storage_dev_count++;
    kprintf("[USB] MSC device dev=%u drive=%d\n", (uint32_t)msc->dev, msc->drive_id);
    disk_request_rescan();
    goto out;

fail:
    if (cfg_buf) kfree(cfg_buf);
    if (hc && hc->ops && hc->ops->enum_close) hc->ops->enum_close(hc, dev_default);
    return;

out:
    if (cfg_buf) kfree(cfg_buf);
    return;
}

static void usb_handle_hub(usb_hc_t* hc, uint32_t hub_dev, uint8_t ep0_mps,
                           usb_speed_t speed, uint8_t tt_hub_addr, uint8_t tt_port,
                           int depth) {
    if (depth > 4) {
        kprint("[USB] Hub depth limit\n");
        return;
    }

    uint8_t hub_desc8[8] = {0};
    if (!usb_hub_get_descriptor(hc, hub_dev, ep0_mps, speed, tt_hub_addr, tt_port,
                                hub_desc8, sizeof(hub_desc8))) {
        kprint("[USB] HUB_DESC failed\n");
        return;
    }

    uint8_t n_ports = hub_desc8[2];
    uint8_t pwr2good = hub_desc8[5];
    if (n_ports == 0 || n_ports > 32) {
        kprintf("[USB] Hub ports=%u unsupported\n", n_ports);
        return;
    }

    kprintf("[USB] Hub dev=%u ports=%u\n", (uint32_t)hub_dev, n_ports);

    for (uint8_t port = 1; port <= n_ports; port++) {
        (void)usb_hub_port_set_feature(hc, hub_dev, ep0_mps, speed, tt_hub_addr, tt_port,
                                       USB_HUB_PORT_FEAT_POWER, port);
    }

    uint32_t wait_ms = (uint32_t)pwr2good * 2;
    if (wait_ms < 20) wait_ms = 20;
    kprintf("[USB] Hub dev=%u power wait %u ms\n", (uint32_t)hub_dev, wait_ms);
    delay_ms(wait_ms);

    for (uint8_t port = 1; port <= n_ports; port++) {
        usb_hub_port_status_t st;
        memset(&st, 0, sizeof(st));
        if (!usb_hub_port_get_status(hc, hub_dev, ep0_mps, speed, tt_hub_addr, tt_port, port, &st)) {
            kprintf("[USB] Hub port %u: GET_STATUS failed\n", port);
            continue;
        }

        if ((st.wPortStatus & USB_HUB_PORT_STAT_CONNECTION) == 0) continue;

        kprintf("[USB] Hub port %u status=%04x change=%04x\n",
                port, st.wPortStatus, st.wPortChange);

        if (st.wPortChange) {
            if (st.wPortChange & 0x0001) (void)usb_hub_port_clear_feature(hc, hub_dev, ep0_mps, speed, tt_hub_addr, tt_port, USB_HUB_PORT_FEAT_C_CONNECTION, port);
            if (st.wPortChange & 0x0002) (void)usb_hub_port_clear_feature(hc, hub_dev, ep0_mps, speed, tt_hub_addr, tt_port, USB_HUB_PORT_FEAT_C_ENABLE, port);
            if (st.wPortChange & 0x0004) (void)usb_hub_port_clear_feature(hc, hub_dev, ep0_mps, speed, tt_hub_addr, tt_port, USB_HUB_PORT_FEAT_C_SUSPEND, port);
            if (st.wPortChange & 0x0008) (void)usb_hub_port_clear_feature(hc, hub_dev, ep0_mps, speed, tt_hub_addr, tt_port, USB_HUB_PORT_FEAT_C_OVER_CURRENT, port);
            if (st.wPortChange & 0x0010) (void)usb_hub_port_clear_feature(hc, hub_dev, ep0_mps, speed, tt_hub_addr, tt_port, USB_HUB_PORT_FEAT_C_RESET, port);
        }

        uint32_t reset_start = tick;
        if (!usb_hub_port_set_feature(hc, hub_dev, ep0_mps, speed, tt_hub_addr, tt_port,
                                      USB_HUB_PORT_FEAT_RESET, port))
            continue;
        delay_ms(60);

        int tries = 0;
        for (tries = 0; tries < 50; tries++) {
            if (!usb_hub_port_get_status(hc, hub_dev, ep0_mps, speed, tt_hub_addr, tt_port, port, &st)) break;
            if ((st.wPortStatus & USB_HUB_PORT_STAT_RESET) == 0) break;
            delay_ms(10);
        }
        kprintf("[USB] Hub port %u reset wait %u ms (tries=%d)\n",
                port, ticks_to_ms(tick - reset_start), tries);

        (void)usb_hub_port_get_status(hc, hub_dev, ep0_mps, speed, tt_hub_addr, tt_port, port, &st);
        if ((st.wPortStatus & USB_HUB_PORT_STAT_CONNECTION) == 0) continue;

        bool child_low = (st.wPortStatus & USB_HUB_PORT_STAT_LOW_SPEED) != 0;
        bool child_high = (st.wPortStatus & USB_HUB_PORT_STAT_HIGH_SPEED) != 0;
        if ((st.wPortStatus & USB_HUB_PORT_STAT_ENABLE) == 0) {
            kprintf("[USB] Hub port %u: not enabled\n", port);
            continue;
        }

        usb_speed_t child_speed = child_high ? USB_SPEED_HIGH : (child_low ? USB_SPEED_LOW : USB_SPEED_FULL);
        kprintf("[USB] Hub port %u: enumerating\n", port);
        delay_ms(100);
        usb_enumerate_default(hc, child_speed, 0, (uint8_t)hub_dev, port, depth + 1);

        (void)usb_hub_port_clear_feature(hc, hub_dev, ep0_mps, speed, tt_hub_addr, tt_port,
                                         USB_HUB_PORT_FEAT_C_RESET, port);
    }
}

void usb_storage_reset(void) {
    storage_dev_count = 0;
    msc_tag = 1;
    memset(storage_devs, 0, sizeof(storage_devs));
}

uint32_t usb_storage_device_count(void) {
    return (uint32_t)storage_dev_count;
}

void usb_hid_reset(void) {
    mouse_set_ignore_ps2(false);
    for (int i = 0; i < hid_dev_count; i++) {
        usb_hid_dev_t* dev = &hid_devs[i];
        if (dev->hc && dev->hc->ops && dev->hc->ops->async_in_cancel) {
            dev->hc->ops->async_in_cancel(&dev->in);
        }
    }
    hid_dev_count = 0;
    memset(hid_devs, 0, sizeof(hid_devs));
    hid_boot_kbd_init();
}

void usb_hid_drop_device(usb_hc_t* hc, uint32_t dev) {
    if (!hc) return;
    for (int i = 0; i < hid_dev_count; ) {
        usb_hid_dev_t* d = &hid_devs[i];
        if (d->hc == hc && d->dev == dev) {
            if (d->hc && d->hc->ops && d->hc->ops->async_in_cancel) {
                d->hc->ops->async_in_cancel(&d->in);
            }
            int last = hid_dev_count - 1;
            if (i != last) {
                hid_devs[i] = hid_devs[last];
            }
            memset(&hid_devs[last], 0, sizeof(hid_devs[last]));
            hid_dev_count--;
            continue;
        }
        i++;
    }

    bool any_mouse = false;
    for (int i = 0; i < hid_dev_count; i++) {
        if (hid_devs[i].kind == USB_HID_BOOT_MOUSE) {
            any_mouse = true;
            break;
        }
    }
    mouse_set_ignore_ps2(any_mouse);
}

void usb_drop_controller_devices(usb_hc_t* hc) {
    if (!hc) return;
    hid_boot_kbd_drop_controller(hc);

    for (int i = 0; i < hid_dev_count; ) {
        usb_hid_dev_t* d = &hid_devs[i];
        if (d->hc == hc) {
            uint32_t dev = d->dev;
            usb_hid_drop_device(hc, dev);
            continue;
        }
        i++;
    }

    for (int i = 0; i < storage_dev_count; ) {
        usb_msc_dev_t* d = &storage_devs[i];
        if (d->hc == hc) {
            uint32_t dev = d->dev;
            usb_storage_drop_device(hc, dev);
            continue;
        }
        i++;
    }
}

void usb_storage_drop_device(usb_hc_t* hc, uint32_t dev) {
    if (!hc) return;
    bool changed = false;
    for (int i = 0; i < storage_dev_count; ) {
        usb_msc_dev_t* m = &storage_devs[i];
        if (m->hc == hc && m->dev == dev) {
            int last = storage_dev_count - 1;
            if (i != last) {
                storage_devs[i] = storage_devs[last];
                storage_devs[i].drive_id = (uint8_t)(USB_DRIVE_BASE + i);
            }
            memset(&storage_devs[last], 0, sizeof(storage_devs[last]));
            storage_dev_count--;
            changed = true;
            continue;
        }
        i++;
    }
    if (changed) {
        disk_request_rescan();
    }
}

void usb_port_connected(usb_hc_t* hc, usb_speed_t speed, uint8_t root_port,
                        uint8_t tt_hub_addr, uint8_t tt_port) {
    usb_enumerate_default(hc, speed, root_port, tt_hub_addr, tt_port, 0);
}

void usb_poll(void) {
    hid_boot_kbd_poll();
    ehci_poll_changes();
    ohci_poll_changes();
    xhci_poll_changes();
    for (int i = 0; i < hid_dev_count; i++) {
        usb_hid_dev_t* dev = &hid_devs[i];

        if (!dev->hc || !dev->hc->ops || !dev->hc->ops->async_in_check || !dev->hc->ops->async_in_rearm) continue;
        for (;;) {
            uint16_t actual = 0;
            int rc = dev->hc->ops->async_in_check(&dev->in, &actual);
            if (rc == 0) break;
            if (rc < 0) break;
            if (actual > 0) {
                if (dev->kind == USB_HID_BOOT_MOUSE) usb_hid_mouse_process(dev, actual);
            }
            dev->hc->ops->async_in_rearm(&dev->in);
        }
    }
}

bool usb_storage_read_sectors(uint8_t drive, uint32_t lba, uint16_t count, uint8_t* buffer) {
    usb_msc_dev_t* dev = find_dev_by_drive(drive);
    if (!dev || dev->block_size != 512) return false;

    uint32_t done = 0;
    while (done < count) {
        uint16_t chunk = (uint16_t)(count - done);
        if (chunk > 32) chunk = 32;
        if (!msc_scsi_read10(dev, lba + done, chunk, buffer + done * 512)) return false;
        done += chunk;
    }
    return true;
}

bool usb_storage_write_sectors(uint8_t drive, uint32_t lba, uint16_t count, const uint8_t* buffer) {
    usb_msc_dev_t* dev = find_dev_by_drive(drive);
    if (!dev || dev->block_size != 512) return false;

    uint32_t done = 0;
    while (done < count) {
        uint16_t chunk = (uint16_t)(count - done);
        if (chunk > 32) chunk = 32;
        if (!msc_scsi_write10(dev, lba + done, chunk, buffer + done * 512)) return false;
        done += chunk;
    }
    return true;
}

uint32_t usb_storage_get_sector_count(uint8_t drive) {
    usb_msc_dev_t* dev = find_dev_by_drive(drive);
    return dev ? dev->block_count : 0;
}

uint32_t usb_storage_get_sector_size(uint8_t drive) {
    usb_msc_dev_t* dev = find_dev_by_drive(drive);
    return dev ? dev->block_size : 0;
}

bool usb_storage_sync(uint8_t drive) {
    usb_msc_dev_t* dev = find_dev_by_drive(drive);
    if (!dev) return false;
    return msc_scsi_sync_cache(dev);
}
