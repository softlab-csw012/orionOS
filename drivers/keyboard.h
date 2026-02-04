#include <stdbool.h>
#include <stdint.h>

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

extern bool keyboard_input_enabled;
extern bool alt_pressed;
extern volatile int g_break_script; // 스크립트 종료 플래그 

void kbd_set_leds(bool caps, bool num, bool scroll);
void reset_modifiers(void);
void init_keyboard();
void wait_for_keypress();
int getkey(void);
int getkey_nonblock(void);
void keyboard_flush(void);
void keyboard_note_debounce(void);

// Inject a PS/2 Set-1 scancode byte (including 0xE0 prefix if needed).
void keyboard_inject_scancode(uint8_t sc);

// When enabled, still drains port 0x60 but ignores PS/2 scancodes (useful when USB HID is active).
void keyboard_set_ignore_ps2(bool ignore);
