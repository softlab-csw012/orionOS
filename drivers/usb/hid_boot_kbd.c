#include "hid_boot_kbd.h"
#include "../keyboard.h"
#include "../screen.h"
#include "../../cpu/timer.h"
#include "../../libc/string.h"
#include "../../mm/mem.h"

#define HID_KBD_MAX_DEVS 4
#define HID_KBD_REPEAT_DELAY_TICKS 35u
#define HID_KBD_REPEAT_RATE_TICKS 5u
#define HID_KBD_IDLE_RATE_4MS 10u
#define HID_KBD_REPORT_TIMEOUT_TICKS 100u
#define HID_KBD_MAX_KEYS 16

#define HID_USAGE_PAGE_KBD 0x07
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
    bool report_proto;
    hid_report_info_t report;
    uint8_t prev_mod;
    uint8_t prev_keys[HID_KBD_MAX_KEYS];
    uint8_t prev_keys_count;

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
    dev->report_proto = false;
    memset(&dev->report, 0, sizeof(dev->report));
    dev->prev_mod = 0;
    dev->prev_keys_count = 0;
    memset(dev->prev_keys, 0, sizeof(dev->prev_keys));
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

static bool usb_get_report_desc(usb_hc_t* hc, uint32_t dev, uint8_t ep0_mps,
                                usb_speed_t speed, uint8_t tt_hub_addr, uint8_t tt_port,
                                uint8_t iface_num, void* buf, uint16_t len) {
    usb_setup_pkt_t setup;
    setup.bmRequestType = 0x81;
    setup.bRequest = 0x06;
    setup.wValue = (uint16_t)(0x22u << 8);
    setup.wIndex = iface_num;
    setup.wLength = len;
    return usb_control(hc, dev, ep0_mps, speed, tt_hub_addr, tt_port, &setup, buf, len);
}

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

static bool hid_parse_report_desc(const uint8_t* desc, uint16_t len, hid_report_info_t* out) {
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
                uint8_t count = g.report_count;
                uint8_t size = g.report_size;
                uint16_t bit_off = info->bit_off;

                if (size != 0 && count != 0) {
                    if (!is_const && g.usage_page == HID_USAGE_PAGE_KBD) {
                        for (uint8_t idx = 0; idx < count; idx++) {
                            uint16_t usage = hid_local_usage(&l, idx);
                            uint16_t elem_off = (uint16_t)(bit_off + (uint16_t)idx * size);
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
        if (info->has_keys && info->has_mods) { best = info; break; }
        if (!best && info->has_keys) best = info;
    }
    if (!best) return false;
    *out = *best;
    return true;
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

static bool kbd_key_present(const uint8_t* keys, uint8_t count, uint8_t key) {
    for (uint8_t i = 0; i < count; i++) {
        if (keys[i] == key) return true;
    }
    return false;
}

static void process_report_boot(hid_kbd_dev_t* dev, uint16_t actual) {
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

static bool kbd_report_extract(hid_kbd_dev_t* dev, uint16_t actual,
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
    if (count > HID_KBD_MAX_KEYS) count = HID_KBD_MAX_KEYS;
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

static void process_report_report(hid_kbd_dev_t* dev, uint16_t actual) {
    uint8_t keys[HID_KBD_MAX_KEYS];
    uint8_t key_count = 0;
    uint8_t mod = 0;
    if (!kbd_report_extract(dev, actual, &mod, keys, &key_count)) return;

    uint8_t changed = (uint8_t)(mod ^ dev->prev_mod);

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

    for (uint8_t i = 0; i < dev->prev_keys_count; i++) {
        uint8_t key = dev->prev_keys[i];
        if (key == 0) continue;
        if (!kbd_key_present(keys, key_count, key)) {
            uint8_t prefix, sc;
            if (hid_usage_to_set1(key, &prefix, &sc)) {
                send_scancode(prefix, sc, false);
                if (dev->repeat_active && dev->repeat_hid == key) {
                    dev->repeat_active = false;
                }
            }
        }
    }

    for (uint8_t i = 0; i < key_count; i++) {
        uint8_t key = keys[i];
        if (key == 0 || key <= 0x03) continue;
        if (!kbd_key_present(dev->prev_keys, dev->prev_keys_count, key)) {
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
        if (!kbd_key_present(keys, key_count, dev->repeat_hid)) {
            dev->repeat_active = false;
        }
    }

    dev->prev_mod = mod;
    dev->prev_keys_count = key_count;
    memset(dev->prev_keys, 0, sizeof(dev->prev_keys));
    if (key_count) memcpy(dev->prev_keys, keys, key_count);
}

static void repeat_tick(hid_kbd_dev_t* dev) {
    if (!dev || !dev->repeat_active) return;
    if (tick < dev->repeat_next_tick) return;

    bool still_down = false;
    if (dev->report_proto) {
        still_down = kbd_key_present(dev->prev_keys, dev->prev_keys_count, dev->repeat_hid);
    } else {
        for (int i = 2; i < 8; i++) {
            if (dev->prev_report[i] == dev->repeat_hid) {
                still_down = true;
                break;
            }
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
                             uint8_t iface_num, uint8_t ep, uint16_t mps, uint8_t interval,
                             uint16_t report_len) {
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
    k->report_proto = false;
    memset(&k->report, 0, sizeof(k->report));
    k->prev_mod = 0;
    k->prev_keys_count = 0;
    memset(k->prev_keys, 0, sizeof(k->prev_keys));

    if (report_len > 0 && report_len <= 1024) {
        uint8_t* report_desc = (uint8_t*)kmalloc(report_len, 0, NULL);
        if (report_desc) {
            if (usb_get_report_desc(hc, dev, ep0_mps, speed, tt_hub_addr, tt_port,
                                    iface_num, report_desc, report_len) &&
                hid_parse_report_desc(report_desc, report_len, &k->report)) {
                uint16_t rpt_bytes = (uint16_t)((k->report.report_bits + 7u) / 8u);
                if (k->report.report_id != 0) rpt_bytes = (uint16_t)(rpt_bytes + 1);
                if (rpt_bytes > 0 && rpt_bytes <= k->mps && rpt_bytes <= sizeof(k->buf)) {
                    k->report_proto = true;
                    k->buf_len = rpt_bytes;
                }
            }
            kfree(report_desc);
        }
    }

    if (!k->report_proto) {
        k->buf_len = k->mps;
        if (k->buf_len < 8) k->buf_len = 8;
        if (k->buf_len > sizeof(k->buf)) k->buf_len = sizeof(k->buf);
    }

    uint8_t idle = HID_KBD_IDLE_RATE_4MS;
    (void)hid_set_idle(hc, dev, ep0_mps, speed, tt_hub_addr, tt_port, iface_num, idle, 0);
    (void)hid_set_protocol(hc, dev, ep0_mps, speed, tt_hub_addr, tt_port, iface_num,
                           k->report_proto ? 1 : 0);

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
            dev->prev_mod = 0;
            dev->prev_keys_count = 0;
            memset(dev->prev_keys, 0, sizeof(dev->prev_keys));
            dev->last_report_tick = tick;
        }

        repeat_tick(dev);

        if (!dev->hc || !dev->hc->ops || !dev->hc->ops->async_in_check || !dev->hc->ops->async_in_rearm) continue;

        for (;;) {
            uint16_t actual = 0;
            int rc = dev->hc->ops->async_in_check(&dev->in, &actual);
            if (rc == 0) break;
            if (rc < 0) {
                kprint("[USB] HID keyboard: transfer error\n");
                hid_boot_kbd_deactivate(dev);
                break;
            }
            if (actual > 0) {
                if (dev->report_proto) process_report_report(dev, actual);
                else process_report_boot(dev, actual);
            }
            dev->last_report_tick = tick;
            dev->hc->ops->async_in_rearm(&dev->in);
        }
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
