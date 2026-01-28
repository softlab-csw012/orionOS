#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "usbhc.h"

// HID Boot Keyboard Report Format
#pragma pack(push, 1)
typedef struct {
    uint8_t modifier;           // Modifier keys (Ctrl, Shift, Alt, GUI)
    uint8_t reserved;           // Reserved (always 0)
    uint8_t keycode[6];         // Up to 6 simultaneous key presses
} hid_boot_kbd_report_t;
#pragma pack(pop)

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
} hid_boot_kbd_dev_t;

// Modifier key bits
#define HID_MOD_LCTRL    0x01
#define HID_MOD_LSHIFT   0x02
#define HID_MOD_LALT     0x04
#define HID_MOD_LGUI     0x08
#define HID_MOD_RCTRL    0x10
#define HID_MOD_RSHIFT   0x20
#define HID_MOD_RALT     0x40
#define HID_MOD_RGUI     0x80

// Common HID Keycodes (Boot protocol)
typedef enum {
    HID_KEY_NONE            = 0x00,
    HID_KEY_ERRORROLLOVER   = 0x01,
    HID_KEY_POSTFAIL        = 0x02,
    HID_KEY_ERRORUNDEFINED  = 0x03,
    HID_KEY_A               = 0x04,
    HID_KEY_B               = 0x05,
    HID_KEY_C               = 0x06,
    HID_KEY_D               = 0x07,
    HID_KEY_E               = 0x08,
    HID_KEY_F               = 0x09,
    HID_KEY_G               = 0x0A,
    HID_KEY_H               = 0x0B,
    HID_KEY_I               = 0x0C,
    HID_KEY_J               = 0x0D,
    HID_KEY_K               = 0x0E,
    HID_KEY_L               = 0x0F,
    HID_KEY_M               = 0x10,
    HID_KEY_N               = 0x11,
    HID_KEY_O               = 0x12,
    HID_KEY_P               = 0x13,
    HID_KEY_Q               = 0x14,
    HID_KEY_R               = 0x15,
    HID_KEY_S               = 0x16,
    HID_KEY_T               = 0x17,
    HID_KEY_U               = 0x18,
    HID_KEY_V               = 0x19,
    HID_KEY_W               = 0x1A,
    HID_KEY_X               = 0x1B,
    HID_KEY_Y               = 0x1C,
    HID_KEY_Z               = 0x1D,
    HID_KEY_1               = 0x1E,
    HID_KEY_2               = 0x1F,
    HID_KEY_3               = 0x20,
    HID_KEY_4               = 0x21,
    HID_KEY_5               = 0x22,
    HID_KEY_6               = 0x23,
    HID_KEY_7               = 0x24,
    HID_KEY_8               = 0x25,
    HID_KEY_9               = 0x26,
    HID_KEY_0               = 0x27,
    HID_KEY_ENTER           = 0x28,
    HID_KEY_ESCAPE          = 0x29,
    HID_KEY_BACKSPACE       = 0x2A,
    HID_KEY_TAB             = 0x2B,
    HID_KEY_SPACE           = 0x2C,
    HID_KEY_MINUS           = 0x2D,
    HID_KEY_EQUAL           = 0x2E,
    HID_KEY_LBRACKET        = 0x2F,
    HID_KEY_RBRACKET        = 0x30,
    HID_KEY_BACKSLASH       = 0x31,
    HID_KEY_SEMICOLON       = 0x33,
    HID_KEY_APOSTROPHE      = 0x34,
    HID_KEY_GRAVE           = 0x35,
    HID_KEY_COMMA           = 0x36,
    HID_KEY_PERIOD          = 0x37,
    HID_KEY_SLASH           = 0x38,
    HID_KEY_CAPSLOCK        = 0x39,
    HID_KEY_F1              = 0x3A,
    HID_KEY_F2              = 0x3B,
    HID_KEY_F3              = 0x3C,
    HID_KEY_F4              = 0x3D,
    HID_KEY_F5              = 0x3E,
    HID_KEY_F6              = 0x3F,
    HID_KEY_F7              = 0x40,
    HID_KEY_F8              = 0x41,
    HID_KEY_F9              = 0x42,
    HID_KEY_F10             = 0x43,
    HID_KEY_F11             = 0x44,
    HID_KEY_F12             = 0x45,
    HID_KEY_PRINTSCREEN     = 0x46,
    HID_KEY_SCROLLLOCK      = 0x47,
    HID_KEY_PAUSE           = 0x48,
    HID_KEY_INSERT          = 0x49,
    HID_KEY_HOME            = 0x4A,
    HID_KEY_PAGEUP          = 0x4B,
    HID_KEY_DELETE          = 0x4C,
    HID_KEY_END             = 0x4D,
    HID_KEY_PAGEDOWN        = 0x4E,
    HID_KEY_RIGHT           = 0x4F,
    HID_KEY_LEFT            = 0x50,
    HID_KEY_DOWN            = 0x51,
    HID_KEY_UP              = 0x52,
    HID_KEY_NUMLOCK         = 0x53,
    HID_KEY_KP_SLASH        = 0x54,
    HID_KEY_KP_ASTERISK     = 0x55,
    HID_KEY_KP_MINUS        = 0x56,
    HID_KEY_KP_PLUS         = 0x57,
    HID_KEY_KP_ENTER        = 0x58,
    HID_KEY_KP_1            = 0x59,
    HID_KEY_KP_2            = 0x5A,
    HID_KEY_KP_3            = 0x5B,
    HID_KEY_KP_4            = 0x5C,
    HID_KEY_KP_5            = 0x5D,
    HID_KEY_KP_6            = 0x5E,
    HID_KEY_KP_7            = 0x5F,
    HID_KEY_KP_8            = 0x60,
    HID_KEY_KP_9            = 0x61,
    HID_KEY_KP_0            = 0x62,
    HID_KEY_KP_PERIOD       = 0x63,
} hid_keycode_t;

void hid_boot_kbd_init(void);
bool hid_boot_kbd_add_device(usb_hc_t* hc, uint32_t dev, uint8_t ep0_mps,
                             usb_speed_t speed, uint8_t tt_hub_addr, uint8_t tt_port,
                             uint8_t iface_num, uint8_t ep, uint16_t mps, uint8_t interval,
                             uint16_t report_len);
void hid_boot_kbd_poll(void);
void hid_boot_kbd_drop_device(usb_hc_t* hc, uint32_t dev);
void hid_boot_kbd_drop_controller(usb_hc_t* hc);

// HID 키코드를 PS/2 스캔코드로 변환
uint8_t hid_keycode_to_ps2(uint8_t hid_code, bool shifted);
