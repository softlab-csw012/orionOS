#include "hid_boot_kbd.h"
#include "../keyboard.h"
#include "../screen.h"
#include "../../cpu/timer.h"
#include "../../libc/string.h"

#define HID_KBD_MAX_DEVS 4
#define HID_KBD_REPEAT_DELAY_TICKS 35u
#define HID_KBD_REPEAT_RATE_TICKS 5u
#define HID_KBD_IDLE_RATE_4MS 10u
#define HID_KBD_REPORT_TIMEOUT_TICKS 100u

typedef struct {
    usb_hc_t* hc;
    uint32_t dev;
    usb_speed_t speed;
    uint8_t tt_hub_addr;
    uint8_t tt_port;
    uint8_t iface_num;
    uint8_t ep;
    uint16_t mps;
    uint8_t interval;

    usb_async_in_t in;
    uint8_t buf[64];
    uint16_t buf_len;
    uint8_t prev_report[8];

    bool active;
    bool repeat_active;
    uint8_t repeat_hid;
    uint8_t repeat_prefix;
    uint8_t repeat_sc;
    uint32_t repeat_next_tick;
    uint32_t last_report_tick;
} hid_kbd_dev_t;

static hid_kbd_dev_t kbds[HID_KBD_MAX_DEVS];
static int kbd_count = 0;

static void hid_boot_kbd_refresh_ps2_ignore(void) {
    bool any_active = false;
    for (int i = 0; i < HID_KBD_MAX_DEVS; i++) {
        if (kbds[i].active) {
            any_active = true;
            break;
        }
    }
    keyboard_set_ignore_ps2(any_active);
}

static int hid_boot_kbd_find_free_slot(void) {
    for (int i = 0; i < HID_KBD_MAX_DEVS; i++) {
        if (!kbds[i].active) return i;
    }
    return -1;
}

static void hid_boot_kbd_deactivate(hid_kbd_dev_t* dev) {
    if (!dev || !dev->active) return;
    if (dev->hc && dev->hc->ops && dev->hc->ops->async_in_cancel) {
        dev->hc->ops->async_in_cancel(&dev->in);
    }
    dev->active = false;
    dev->repeat_active = false;
    dev->repeat_hid = 0;
    memset(dev->prev_report, 0, sizeof(dev->prev_report));
    if (kbd_count > 0) kbd_count--;
    hid_boot_kbd_refresh_ps2_ignore();
}

#pragma pack(push,1)
typedef struct {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_pkt_t;
#pragma pack(pop)

static bool usb_control(usb_hc_t* hc, uint32_t dev, uint8_t ep0_mps,
                        usb_speed_t speed, uint8_t tt_hub_addr, uint8_t tt_port,
                        usb_setup_pkt_t* setup, void* data, uint16_t len) {
    if (!hc || !hc->ops || !hc->ops->control_transfer) return false;
    return hc->ops->control_transfer(hc, dev, 0, ep0_mps, speed, tt_hub_addr, tt_port, setup, data, len);
}

static bool hid_set_protocol(usb_hc_t* hc, uint32_t dev, uint8_t ep0_mps,
                             usb_speed_t speed, uint8_t tt_hub_addr, uint8_t tt_port,
                             uint8_t iface_num, uint16_t protocol) {
    usb_setup_pkt_t setup;
    setup.bmRequestType = 0x21;
    setup.bRequest = 0x0B;
    setup.wValue = protocol;
    setup.wIndex = iface_num;
    setup.wLength = 0;
    return usb_control(hc, dev, ep0_mps, speed, tt_hub_addr, tt_port, &setup, NULL, 0);
}

static bool hid_set_idle(usb_hc_t* hc, uint32_t dev, uint8_t ep0_mps,
                         usb_speed_t speed, uint8_t tt_hub_addr, uint8_t tt_port,
                         uint8_t iface_num, uint8_t duration, uint8_t report_id) {
    usb_setup_pkt_t setup;
    setup.bmRequestType = 0x21;
    setup.bRequest = 0x0A;
    setup.wValue = (uint16_t)(((uint16_t)duration << 8) | report_id);
    setup.wIndex = iface_num;
    setup.wLength = 0;
    return usb_control(hc, dev, ep0_mps, speed, tt_hub_addr, tt_port, &setup, NULL, 0);
}

typedef struct {
    uint8_t usage;
    uint8_t prefix;
    uint8_t sc;
} hid_to_set1_t;

static bool hid_usage_to_set1(uint8_t usage, uint8_t* out_prefix, uint8_t* out_sc) {
    static const hid_to_set1_t map[] = {
        { 0x04, 0x00, 0x1E }, // A
        { 0x05, 0x00, 0x30 }, // B
        { 0x06, 0x00, 0x2E }, // C
        { 0x07, 0x00, 0x20 }, // D
        { 0x08, 0x00, 0x12 }, // E
        { 0x09, 0x00, 0x21 }, // F
        { 0x0A, 0x00, 0x22 }, // G
        { 0x0B, 0x00, 0x23 }, // H
        { 0x0C, 0x00, 0x17 }, // I
        { 0x0D, 0x00, 0x24 }, // J
        { 0x0E, 0x00, 0x25 }, // K
        { 0x0F, 0x00, 0x26 }, // L
        { 0x10, 0x00, 0x32 }, // M
        { 0x11, 0x00, 0x31 }, // N
        { 0x12, 0x00, 0x18 }, // O
        { 0x13, 0x00, 0x19 }, // P
        { 0x14, 0x00, 0x10 }, // Q
        { 0x15, 0x00, 0x13 }, // R
        { 0x16, 0x00, 0x1F }, // S
        { 0x17, 0x00, 0x14 }, // T
        { 0x18, 0x00, 0x16 }, // U
        { 0x19, 0x00, 0x2F }, // V
        { 0x1A, 0x00, 0x11 }, // W
        { 0x1B, 0x00, 0x2D }, // X
        { 0x1C, 0x00, 0x15 }, // Y
        { 0x1D, 0x00, 0x2C }, // Z
        { 0x1E, 0x00, 0x02 }, // 1
        { 0x1F, 0x00, 0x03 }, // 2
        { 0x20, 0x00, 0x04 }, // 3
        { 0x21, 0x00, 0x05 }, // 4
        { 0x22, 0x00, 0x06 }, // 5
        { 0x23, 0x00, 0x07 }, // 6
        { 0x24, 0x00, 0x08 }, // 7
        { 0x25, 0x00, 0x09 }, // 8
        { 0x26, 0x00, 0x0A }, // 9
        { 0x27, 0x00, 0x0B }, // 0
        { 0x28, 0x00, 0x1C }, // Enter
        { 0x29, 0x00, 0x01 }, // Escape
        { 0x2A, 0x00, 0x0E }, // Backspace
        { 0x2B, 0x00, 0x0F }, // Tab
        { 0x2C, 0x00, 0x39 }, // Space
        { 0x2D, 0x00, 0x0C }, // -
        { 0x2E, 0x00, 0x0D }, // =
        { 0x2F, 0x00, 0x1A }, // [
        { 0x30, 0x00, 0x1B }, // ]
        { 0x31, 0x00, 0x2B }, // backslash
        { 0x32, 0x00, 0x56 }, // ISO #
        { 0x33, 0x00, 0x27 }, // ;
        { 0x34, 0x00, 0x28 }, // '
        { 0x35, 0x00, 0x29 }, // `
        { 0x36, 0x00, 0x33 }, // ,
        { 0x37, 0x00, 0x34 }, // .
        { 0x38, 0x00, 0x35 }, // /
        { 0x39, 0x00, 0x3A }, // CapsLock
        { 0x3A, 0x00, 0x3B }, // F1
        { 0x3B, 0x00, 0x3C }, // F2
        { 0x3C, 0x00, 0x3D }, // F3
        { 0x3D, 0x00, 0x3E }, // F4
        { 0x3E, 0x00, 0x3F }, // F5
        { 0x3F, 0x00, 0x40 }, // F6
        { 0x40, 0x00, 0x41 }, // F7
        { 0x41, 0x00, 0x42 }, // F8
        { 0x42, 0x00, 0x43 }, // F9
        { 0x43, 0x00, 0x44 }, // F10
        { 0x44, 0x00, 0x57 }, // F11
        { 0x45, 0x00, 0x58 }, // F12
        { 0x47, 0x00, 0x46 }, // ScrollLock
        { 0x49, 0xE0, 0x52 }, // Insert
        { 0x4A, 0xE0, 0x47 }, // Home
        { 0x4B, 0xE0, 0x49 }, // PageUp
        { 0x4C, 0xE0, 0x53 }, // Delete
        { 0x4D, 0xE0, 0x4F }, // End
        { 0x4E, 0xE0, 0x51 }, // PageDown
        { 0x4F, 0xE0, 0x4D }, // Right
        { 0x50, 0xE0, 0x4B }, // Left
        { 0x51, 0xE0, 0x50 }, // Down
        { 0x52, 0xE0, 0x48 }, // Up
        { 0x53, 0x00, 0x45 }, // NumLock
        { 0x54, 0xE0, 0x35 }, // Keypad /
        { 0x55, 0x00, 0x37 }, // Keypad *
        { 0x56, 0x00, 0x4A }, // Keypad -
        { 0x57, 0x00, 0x4E }, // Keypad +
        { 0x58, 0xE0, 0x1C }, // Keypad Enter
        { 0x59, 0x00, 0x4F }, // Keypad 1
        { 0x5A, 0x00, 0x50 }, // Keypad 2
        { 0x5B, 0x00, 0x51 }, // Keypad 3
        { 0x5C, 0x00, 0x4B }, // Keypad 4
        { 0x5D, 0x00, 0x4C }, // Keypad 5
        { 0x5E, 0x00, 0x4D }, // Keypad 6
        { 0x5F, 0x00, 0x47 }, // Keypad 7
        { 0x60, 0x00, 0x48 }, // Keypad 8
        { 0x61, 0x00, 0x49 }, // Keypad 9
        { 0x62, 0x00, 0x52 }, // Keypad 0
        { 0x63, 0x00, 0x53 }, // Keypad .
        { 0x64, 0x00, 0x56 }, // ISO backslash
        { 0x65, 0xE0, 0x5D }, // Application/Menu
    };

    if (!out_prefix || !out_sc) return false;
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (map[i].usage == usage) {
            *out_prefix = map[i].prefix;
            *out_sc = map[i].sc;
            return true;
        }
    }
    return false;
}

static bool hid_report_has_rollover(const uint8_t* rep) {
    if (!rep) return false;
    for (int i = 2; i < 8; i++) {
        if (rep[i] >= 0x01 && rep[i] <= 0x03) return true;
    }
    return false;
}

static bool hid_usage_present(const uint8_t* rep, uint8_t usage) {
    if (!rep || usage == 0) return false;
    for (int i = 2; i < 8; i++) {
        if (rep[i] == usage) return true;
    }
    return false;
}

static void send_scancode(uint8_t prefix, uint8_t sc, bool make) {
    if (prefix) keyboard_inject_scancode(prefix);
    keyboard_inject_scancode(make ? sc : (uint8_t)(sc | 0x80u));
}

static void process_report(hid_kbd_dev_t* dev, uint16_t actual) {
    if (!dev || actual < 8) return;

    const uint8_t* rep = dev->buf;
    const uint8_t* prev = dev->prev_report;

    if (hid_report_has_rollover(rep)) return;

    uint8_t mod = rep[0];
    uint8_t prev_mod = prev[0];
    uint8_t changed = (uint8_t)(mod ^ prev_mod);

    struct mod_map { uint8_t bit; uint8_t prefix; uint8_t sc; };
    static const struct mod_map mods[] = {
        { 0, 0x00, 0x1D },
        { 1, 0x00, 0x2A },
        { 2, 0x00, 0x38 },
        { 3, 0xE0, 0x5B },
        { 4, 0xE0, 0x1D },
        { 5, 0x00, 0x36 },
        { 6, 0xE0, 0x38 },
        { 7, 0xE0, 0x5C },
    };

    for (unsigned i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
        uint8_t mask = (uint8_t)(1u << mods[i].bit);
        if (!(changed & mask)) continue;
        bool make = (mod & mask) != 0;
        send_scancode(mods[i].prefix, mods[i].sc, make);
    }

    uint8_t new_repeat_hid = 0;
    uint8_t new_repeat_prefix = 0;
    uint8_t new_repeat_sc = 0;

    for (int i = 2; i < 8; i++) {
        uint8_t key = prev[i];
        if (key == 0) continue;
        if (!hid_usage_present(rep, key)) {
            uint8_t prefix, sc;
            if (hid_usage_to_set1(key, &prefix, &sc)) {
                send_scancode(prefix, sc, false);
                if (dev->repeat_active && dev->repeat_hid == key) {
                    dev->repeat_active = false;
                }
            }
        }
    }

    for (int i = 2; i < 8; i++) {
        uint8_t key = rep[i];
        if (key == 0 || key <= 0x03) continue;
        if (!hid_usage_present(prev, key)) {
            uint8_t prefix, sc;
            if (hid_usage_to_set1(key, &prefix, &sc)) {
                send_scancode(prefix, sc, true);
                if (new_repeat_hid == 0) {
                    new_repeat_hid = key;
                    new_repeat_prefix = prefix;
                    new_repeat_sc = sc;
                }
            }
        }
    }

    if (new_repeat_hid != 0) {
        dev->repeat_active = true;
        dev->repeat_hid = new_repeat_hid;
        dev->repeat_prefix = new_repeat_prefix;
        dev->repeat_sc = new_repeat_sc;
        dev->repeat_next_tick = tick + HID_KBD_REPEAT_DELAY_TICKS;
    } else if (dev->repeat_active) {
        if (!hid_usage_present(rep, dev->repeat_hid)) {
            dev->repeat_active = false;
        }
    }

    memcpy(dev->prev_report, dev->buf, 8);
}

static void repeat_tick(hid_kbd_dev_t* dev) {
    if (!dev || !dev->repeat_active) return;
    if (tick < dev->repeat_next_tick) return;

    bool still_down = false;
    for (int i = 2; i < 8; i++) {
        if (dev->prev_report[i] == dev->repeat_hid) {
            still_down = true;
            break;
        }
    }
    if (!still_down) {
        dev->repeat_active = false;
        return;
    }

    send_scancode(dev->repeat_prefix, dev->repeat_sc, true);
    dev->repeat_next_tick = tick + HID_KBD_REPEAT_RATE_TICKS;
}

void hid_boot_kbd_init(void) {
    for (int i = 0; i < HID_KBD_MAX_DEVS; i++) {
        if (kbds[i].hc && kbds[i].hc->ops && kbds[i].hc->ops->async_in_cancel) {
            kbds[i].hc->ops->async_in_cancel(&kbds[i].in);
        }
    }
    memset(kbds, 0, sizeof(kbds));
    kbd_count = 0;
    hid_boot_kbd_refresh_ps2_ignore();
}

bool hid_boot_kbd_add_device(usb_hc_t* hc, uint32_t dev, uint8_t ep0_mps,
                             usb_speed_t speed, uint8_t tt_hub_addr, uint8_t tt_port,
                             uint8_t iface_num, uint8_t ep, uint16_t mps, uint8_t interval) {
    if (!hc || !hc->ops) return false;
    if (ep == 0) return false;

    int slot = hid_boot_kbd_find_free_slot();
    if (slot < 0) return false;

    if (hc->ops->configure_endpoint) {
        if (!hc->ops->configure_endpoint(hc, dev, ep, true, USB_EP_INTERRUPT, mps ? mps : 8, interval)) {
            kprint("[USB] HID keyboard: endpoint config failed\n");
            return false;
        }
    }

    hid_kbd_dev_t* k = &kbds[slot];
    memset(k, 0, sizeof(*k));
    k->hc = hc;
    k->dev = dev;
    k->speed = speed;
    k->tt_hub_addr = tt_hub_addr;
    k->tt_port = tt_port;
    k->iface_num = iface_num;
    k->ep = ep & 0x0F;
    k->mps = mps ? mps : 8;
    k->interval = interval;
    k->buf_len = k->mps;
    if (k->buf_len < 8) k->buf_len = 8;
    if (k->buf_len > sizeof(k->buf)) k->buf_len = sizeof(k->buf);

    uint8_t idle = HID_KBD_IDLE_RATE_4MS;
    (void)hid_set_idle(hc, dev, ep0_mps, speed, tt_hub_addr, tt_port, iface_num, idle, 0);
    (void)hid_set_protocol(hc, dev, ep0_mps, speed, tt_hub_addr, tt_port, iface_num, 0);

    bool ok = hc->ops->async_in_init &&
              hc->ops->async_in_init(hc, &k->in, dev, k->ep, k->mps,
                                     speed, tt_hub_addr, tt_port,
                                     0, k->buf, k->buf_len);
    if (!ok) {
        memset(k, 0, sizeof(*k));
        return false;
    }

    k->active = true;
    k->last_report_tick = tick;
    kbd_count++;
    hid_boot_kbd_refresh_ps2_ignore();
    kprintf("[USB] HID keyboard dev=%u ep=%u mps=%u\n", (uint32_t)dev, k->ep, k->mps);
    return true;
}

void hid_boot_kbd_poll(void) {
    for (int i = 0; i < HID_KBD_MAX_DEVS; i++) {
        hid_kbd_dev_t* dev = &kbds[i];
        if (!dev->active) continue;

        if ((tick - dev->last_report_tick) > HID_KBD_REPORT_TIMEOUT_TICKS) {
            dev->repeat_active = false;
            dev->repeat_hid = 0;
            memset(dev->prev_report, 0, sizeof(dev->prev_report));
            dev->last_report_tick = tick;
        }

        repeat_tick(dev);

        if (!dev->hc || !dev->hc->ops || !dev->hc->ops->async_in_check || !dev->hc->ops->async_in_rearm) continue;

        uint16_t actual = 0;
        int rc = dev->hc->ops->async_in_check(&dev->in, &actual);
        if (rc == 0) continue;
        if (rc < 0) {
            kprint("[USB] HID keyboard: transfer error\n");
            hid_boot_kbd_deactivate(dev);
            continue;
        }
        if (rc == 1 && actual > 0) {
            process_report(dev, actual);
            dev->last_report_tick = tick;
        } else if (rc == 1) {
            dev->last_report_tick = tick;
        }
        dev->hc->ops->async_in_rearm(&dev->in);
    }
}

void hid_boot_kbd_drop_device(usb_hc_t* hc, uint32_t dev) {
    if (!hc) return;
    for (int i = 0; i < HID_KBD_MAX_DEVS; i++) {
        hid_kbd_dev_t* k = &kbds[i];
        if (k->active && k->hc == hc && k->dev == dev) {
            hid_boot_kbd_deactivate(k);
        }
    }
}

void hid_boot_kbd_drop_controller(usb_hc_t* hc) {
    if (!hc) return;
    for (int i = 0; i < HID_KBD_MAX_DEVS; i++) {
        hid_kbd_dev_t* k = &kbds[i];
        if (k->active && k->hc == hc) {
            hid_boot_kbd_deactivate(k);
        }
    }
}

// HID 키코드를 PS/2 스캔코드로 변환 (확장키는 0 반환)
uint8_t hid_keycode_to_ps2(uint8_t hid_code, bool shifted __attribute__((unused))) {
    static const uint8_t hid_to_ps2_map[0x66] = {
        0x00, 0x00, 0x00, 0x00,
        0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26,
        0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D,
        0x15, 0x2C, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
        0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x00, 0x27,
        0x28, 0x29, 0x33, 0x34, 0x35, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40,
        0x41, 0x42, 0x43, 0x44, 0x57, 0x58, 0x00, 0x46, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x56, 0x00
    };

    (void)shifted;
    if (hid_code < sizeof(hid_to_ps2_map)) return hid_to_ps2_map[hid_code];
    return 0;
}
