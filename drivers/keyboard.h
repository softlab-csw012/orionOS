#include <stdbool.h>
#include <stdint.h>
#include "../kernel/input_queue.h"

#define LSHIFT_MAKE  0x2A
#define RSHIFT_MAKE  0x36
#define LSHIFT_BREAK 0xAA
#define RSHIFT_BREAK 0xB6
#define PGUP 0x49
#define PGDN 0x51
#define CAPSLOCK     0x3A
#define SCROLLBACK_PAGES 100
#define SCROLLBACK_LINES (MAX_ROWS * SCROLLBACK_PAGES)
#define NOTE_KEY_LEFT   0x90
#define NOTE_KEY_RIGHT  0x91
#define NOTE_KEY_UP     0x92
#define NOTE_KEY_DOWN   0x93
#define NOTE_KEY_PGUP   0x94
#define NOTE_KEY_PGDN   0x95
#define NOTE_KEY_HOME   0x96
#define NOTE_KEY_END    0x97
#define NOTE_KEY_DEL    0x98

extern bool keyboard_input_enabled;
extern bool keyboard_korean_ime_enabled;
extern bool alt_pressed;
extern volatile int g_break_script; // 스크립트 종료 플래그 

enum {
    KEYBOARD_MOD_SHIFT = 1u << 0,
    KEYBOARD_MOD_CTRL  = 1u << 1,
    KEYBOARD_MOD_ALT   = 1u << 2,
};

void kbd_set_leds(bool caps, bool num, bool scroll);
void reset_modifiers(void);
void init_keyboard();
void wait_for_keypress();
int getkey(void);
int getkey_nonblock(void);
int getkey_event(input_event_t* event);
int getkey_event_nonblock(input_event_t* event);
void keyboard_flush(void);
void keyboard_note_debounce(void);

// Inject a PS/2 Set-1 scancode byte (including 0xE0 prefix if needed).
void keyboard_inject_scancode(uint8_t sc);

// When enabled, still drains port 0x60 but ignores PS/2 scancodes (useful when USB HID is active).
void keyboard_set_ignore_ps2(bool ignore);
uint32_t keyboard_get_modifiers(void);
