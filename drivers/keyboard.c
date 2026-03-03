#include "keyboard.h"
#include "hal.h"
#include "../cpu/isr.h"
#include "screen.h"
#include "../libc/string.h"
#include "../mm/mem.h"
#include "../libc/function.h"
#include "../kernel/kernel.h"
#include "../kernel/tty.h"
#include "../kernel/input_queue.h"
#include "../kernel/io/console_lock.h"
#include "../kernel/ipc/gui_ipc.h"
#include "../kernel/proc/proc.h"
#include "../kernel/log.h"
#include "../cpu/timer.h"
#include <stdint.h>
#include <stdbool.h>

#define BACKSPACE       0x0E
#define ENTER           0x1C
#define CAPSLOCK        0x3A
#define LSHIFT_MAKE     0x2A
#define RSHIFT_MAKE     0x36
#define LSHIFT_BREAK    0xAA
#define RSHIFT_BREAK    0xB6
#define ALT_MAKE        0x38
#define ALT_BREAK       0xB8
#define CTRL_MAKE  0x1D
#define CTRL_BREAK 0x9D

#define KBD_E0_PREFIX   0xE0
#define KBD_F0_PREFIX   0xF0
#define KEY_LEFT_MAKE   0x4B
#define KEY_RIGHT_MAKE  0x4D
#define KEY_UP_MAKE     0x48
#define KEY_DOWN_MAKE   0x50
#define KEY_HOME_MAKE   0x47
#define KEY_END_MAKE    0x4F
#define KEY_DELETE_MAKE 0x53
#define ESCAPE          0x01

#define MAX_LINE     256
#define MAX_HISTORY  16
#define SC_MAX       57
#define NUMLOCK_MAKE    0x45
#define NUMLOCK_BREAK   0xC5
#define KEY_PGUP_MAKE   0x49
#define KEY_PGDN_MAKE   0x51
#define KEY_F1_MAKE     0x3B
#define KEY_F2_MAKE     0x3C
#define KEY_F3_MAKE     0x3D
#define KEY_F4_MAKE     0x3E

// ──────────────────────────────────────────────
// 전역 상태
// ──────────────────────────────────────────────
static char key_buffer[MAX_LINE];
static int in_len = 0;
static int cur_ix = 0;
int prompt_row = 0;
int prompt_col = 0;

static int last_drawn_len = 0;
static char hist[MAX_HISTORY][MAX_LINE];
static int hist_head = 0;
static int hist_size = 0;
static int hist_view = -1;
static char edit_scratch[MAX_LINE];
static int saved_edit = 0;

static int  kbd_e0 = 0;
static int  kbd_f0 = 0;
static bool shift_pressed = false;
static bool capslock_on = false;
static bool alt_left_pressed = false;
static bool alt_right_pressed = false;
static bool alt_right_toggle_candidate = false;
static bool numlock_on = false;
static bool scrolllock_on = false;
static bool ctrl_pressed = false;

// note 모드 / getch용
volatile bool g_key_pressed = false;
volatile uint8_t last_ascii = 0;

static inline void note_keybuf_clear_unsafe(void) {
    input_queue_clear();
    g_key_pressed = false;
    last_ascii = 0;
}

static input_event_t keyboard_make_input_event(uint8_t code) {
    input_event_t event = {
        .code = code,
        .modifiers = 0,
        .toggles = 0,
        .reserved = 0,
    };
    if (shift_pressed) {
        event.modifiers |= INPUT_MOD_SHIFT;
    }
    if (ctrl_pressed) {
        event.modifiers |= INPUT_MOD_CTRL;
    }
    if (alt_left_pressed || alt_right_pressed) {
        event.modifiers |= INPUT_MOD_ALT;
    }
    if (capslock_on) {
        event.toggles |= INPUT_TOGGLE_CAPS;
    }
    if (numlock_on) {
        event.toggles |= INPUT_TOGGLE_NUM;
    }
    if (scrolllock_on) {
        event.toggles |= INPUT_TOGGLE_SCROLL;
    }
    if (keyboard_korean_ime_enabled) {
        event.toggles |= INPUT_TOGGLE_KOREAN;
    }
    return event;
}

static inline void note_key_emit(uint8_t code) {
    input_event_t event = keyboard_make_input_event(code);
    input_queue_push_event(&event);
    last_ascii = code;
    g_key_pressed = true;
}

static bool note_keybuf_pop(uint8_t* out) {
    bool ok = input_queue_pop(out);
    g_key_pressed = input_queue_has_data();
    if (!g_key_pressed)
        last_ascii = 0;
    return ok;
}

static bool note_keybuf_pop_event(input_event_t* out) {
    bool ok = input_queue_pop_event(out);
    g_key_pressed = input_queue_has_data();
    if (!g_key_pressed) {
        last_ascii = 0;
    } else if (ok) {
        last_ascii = out->code;
    }
    return ok;
}

static volatile bool ignore_ps2_scancodes = false;

static bool has_name_suffix(const char* name, const char* suffix) {
    if (!name || !suffix) {
        return false;
    }
    size_t nlen = strlen(name);
    size_t slen = strlen(suffix);
    if (nlen < slen) {
        return false;
    }
    return strcasecmp(name + (nlen - slen), suffix) == 0;
}

static bool keyboard_should_scroll_history(void) {
    if (keyboard_input_enabled) {
        return true;
    }
    process_t* fg = proc_lookup(tty_get_foreground());
    if (!fg) {
        return false;
    }
    return has_name_suffix(fg->name, "shell") || has_name_suffix(fg->name, "login");
}

// 모드 전환 플래그 (true = shell, false = note),  스크립트 종료 플래그
bool keyboard_input_enabled = false; // 모드 전환 플래그
bool keyboard_korean_ime_enabled = false;
volatile int g_break_script = 0; // 스크립트 종료 플래그 
static volatile bool kbd_led_update_pending = false;

static bool keyboard_post_alt_tab_event(bool reverse) {
    uint32_t gui_pid = gui_ipc_server_pid_get();
    if (gui_pid == 0) {
        return false;
    }
    gui_ipc_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = GUI_MSG_SYS_ALT_TAB;
    msg.a = reverse ? 1 : 0;
    uint32_t flags = console_lock_acquire();
    bool ok = gui_ipc_queue_push(&msg);
    console_lock_release(flags);
    return ok;
}

static bool keyboard_should_use_korean_ime(void) {
    if (!keyboard_korean_ime_enabled || keyboard_input_enabled) {
        return false;
    }
    process_t* fg = proc_lookup(tty_get_foreground());
    if (!fg) {
        return false;
    }
    return has_name_suffix(fg->name, "shell") || has_name_suffix(fg->name, "login");
}

static bool keyboard_is_korean_shift_key(char ch) {
    return ch == 'q' || ch == 'w' || ch == 'e' || ch == 'r' ||
           ch == 't' || ch == 'o' || ch == 'p';
}

extern int  get_cursor_row(void);
extern int  get_cursor_col(void);
extern void set_cursor(int row, int col);

// ──────────────────────────────────────────────
// PS/2 컨트롤러 유틸 (LED 동기화용)
// ──────────────────────────────────────────────
static bool ps2_wait_write(void) {
    for (int i = 0; i < 100000; i++) {
        if (!(hal_in8(0x64) & 0x02))
            return true;
    }
    return false; // 아직 busy
}
static bool ps2_wait_read(void) {
    for (int i = 0; i < 100000; i++) {
        if (hal_in8(0x64) & 0x01)
            return true;
    }
    return false; // 데이터 없음
}
static bool kbd_write(uint8_t val) {
    if (!ps2_wait_write())
        return false;
    hal_out8(0x60, val);
    return true;
}
static bool kbd_read(uint8_t* out) {
    if (!ps2_wait_read())
        return false;
    *out = hal_in8(0x60);
    return true;
}

// ★ 정식 LED 세팅 (ACK까지 확인)
void kbd_set_leds(bool caps, bool num, bool scroll) {
    uint8_t val = (scroll ? 1 : 0) | (num ? 2 : 0) | (caps ? 4 : 0);
    uint8_t ack;

    // Step 1: LED 명령
    if (!kbd_write(0xED))
        return;
    if (!kbd_read(&ack) || ack != 0xFA)
        return;

    // Step 2: LED 값
    if (!kbd_write(val))
        return;
    (void)kbd_read(&ack); // 마지막 ACK 소비
}

// shift, alt, E0 플래그만 리셋
void reset_modifiers(void) {
    shift_pressed     = false;
    alt_left_pressed  = false;
    alt_right_pressed = false;
    alt_right_toggle_candidate = false;
    kbd_e0            = 0;
    kbd_f0            = 0;
    // capslock_on, numlock_on, scrolllock_on 은 유지
}

// ──────────────────────────────────────────────
// 스캔코드 테이블
// ──────────────────────────────────────────────
const char sc_ascii[] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,
    'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,
    '\\',
    'z','x','c','v','b','n','m',',','.','/', 
    0,
    '*',
    0,
    ' ',
    0,
};

const char sc_ascii_shift[] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t',
    0,0,0,0,0,0,0,0,0,0, '{','}','\n',
    0,
    0,0,0,0,0,0,0,0,0, ':','"','~',
    0,
    '|',
    0,0,0,0,0,0,0, '<','>','?',
    0,
    '*',
    0,
    ' ',
    0,
};

// ──────────────────────────────────────────────
// 셸 모드 유틸
// ──────────────────────────────────────────────
static void redraw_line(void) {
    set_cursor(prompt_row, prompt_col);
    for (int i = 0; i < in_len; i++) {
        kprint_char(key_buffer[i]);
    }
    int tail = last_drawn_len - in_len;
    if (tail > 0) {
        for (int i = 0; i < tail; i++) kprint_char(' ');
    }
    set_cursor(prompt_row, prompt_col + cur_ix);
    last_drawn_len = in_len;
}

static void log_input_line(void) {
    klog_add(key_buffer);
    klog_add("\n");
}

static void hist_push(const char* s) {
    if (!s || !s[0]) return;
    if (hist_size > 0) {
        int last = (hist_head - 1 + MAX_HISTORY) % MAX_HISTORY;
        if (strcmp(hist[last], s) == 0) return;
    }
    strncpy(hist[hist_head], s, MAX_LINE - 1);
    hist[hist_head][MAX_LINE - 1] = '\0';
    if (hist_size < MAX_HISTORY) hist_size++;
    hist_head = (hist_head + 1) % MAX_HISTORY;
}

static void load_history_view(void) {
    set_cursor_offset(get_offset(prompt_col, prompt_row));
    clear_input_line();
    int idx = (hist_head - 1 - hist_view + MAX_HISTORY) % MAX_HISTORY;
    strncpy(key_buffer, hist[idx], MAX_LINE - 1);
    key_buffer[MAX_LINE - 1] = '\0';
    in_len = (int)strlen(key_buffer);
    cur_ix = in_len;
    redraw_line();
    key_buffer[in_len] = '\0';
    memset(&key_buffer[in_len], 0, MAX_LINE - in_len);
}

static char translate_numpad(uint8_t sc, bool e0) {
    if (e0) {
        if (sc == 0x35) return '/';
        return 0;
    }
    switch (sc) {
        case 0x37: return '*';
        case 0x4A: return '-';
        case 0x4E: return '+';
        case 0x52: return numlock_on ? '0' : 0;
        case 0x53: return numlock_on ? '.' : 0;
        case 0x4F: return numlock_on ? '1' : 0;
        case 0x50: return numlock_on ? '2' : 0;
        case 0x51: return numlock_on ? '3' : 0;
        case 0x4B: return numlock_on ? '4' : 0;
        case 0x4C: return numlock_on ? '5' : 0;
        case 0x4D: return numlock_on ? '6' : 0;
        case 0x47: return numlock_on ? '7' : 0;
        case 0x48: return numlock_on ? '8' : 0;
        case 0x49: return numlock_on ? '9' : 0;
        default:   return 0;
    }
}

// ──────────────────────────────────────────────
// 키보드 인터럽트 핸들러
// ──────────────────────────────────────────────
static void keyboard_handle_scancode(uint8_t sc) {
    char base = 0;

    if (sc == KBD_E0_PREFIX) { kbd_e0 = 1; goto done; }
    if (sc == KBD_F0_PREFIX) { kbd_f0 = 1; goto done; }

    if (kbd_f0) {
        if (kbd_e0) {
            if (sc == 0x11) {
                alt_right_pressed = false;
                alt_right_toggle_candidate = false;
            } else if (sc == 0x14) {
                ctrl_pressed = false;
            }
            kbd_e0 = 0;
            kbd_f0 = 0;
            goto done;
        }

        if (sc == 0x12 || sc == 0x59) {
            shift_pressed = false;
        } else if (sc == 0x11) {
            alt_left_pressed = false;
        } else if (sc == 0x14) {
            ctrl_pressed = false;
        }
        kbd_f0 = 0;
        goto done;
    }

    if (sc & 0x80) {
        if (kbd_e0) {
            if (sc == ALT_BREAK) {
                alt_right_pressed = false;
                if (alt_right_toggle_candidate) {
                    keyboard_korean_ime_enabled = !keyboard_korean_ime_enabled;
                }
                alt_right_toggle_candidate = false;
            }
            kbd_e0 = 0;
            goto done;
        }
        if (sc == CTRL_BREAK)  ctrl_pressed = false;
        if (sc == ALT_BREAK) alt_left_pressed = false;
        if (sc == LSHIFT_BREAK || sc == RSHIFT_BREAK) shift_pressed = false;
        goto done;
    }

    if (sc == ALT_MAKE) { 
        if (kbd_e0) {
            keyboard_korean_ime_enabled = !keyboard_korean_ime_enabled;
            alt_right_pressed = false;
            alt_right_toggle_candidate = false;
            kbd_e0 = 0;
        } else {
            alt_left_pressed = true;
        }
        goto done;
    }
    if (alt_right_pressed) alt_right_toggle_candidate = false;
    if ((alt_left_pressed || alt_right_pressed) &&
        (sc == KEY_F1_MAKE || sc == KEY_F2_MAKE || sc == KEY_F3_MAKE || sc == KEY_F4_MAKE)) {
        tty_request_vc_switch((uint8_t)(sc - KEY_F1_MAKE));
        reset_modifiers();
        goto done;
    }
    if (sc == LSHIFT_MAKE || sc == RSHIFT_MAKE) { shift_pressed = true; goto done; }
    
    // CAPSLOCK 이벤트 처리
    if (sc == CAPSLOCK) {
        capslock_on = !capslock_on;
        kbd_led_update_pending = true;
        goto done;
    }

    // NUMLOCK 이벤트 처리
    if (sc == NUMLOCK_MAKE) {
        numlock_on = !numlock_on;
        kbd_led_update_pending = true;
        goto done;
    }

    // ctrl 이벤트 처리
    if (sc == CTRL_MAKE) { ctrl_pressed = true; goto done; }
    if (sc == CTRL_BREAK) { ctrl_pressed = false; goto done; }

    if (sc == 0x0F && (alt_left_pressed || alt_right_pressed)) {
        (void)keyboard_post_alt_tab_event(shift_pressed);
        goto done;
    }

    if (ctrl_pressed && sc == 0x12) {
        if (!tty_signal_int()) {
            g_break_script = 1;
        }
        ctrl_pressed = false;
        reset_modifiers();
        goto done;
    }

    if (in_len == 0 && cur_ix == 0) {
        prompt_row = get_cursor_row();
        prompt_col = get_cursor_col();
    }

    if (keyboard_input_enabled && screen_is_scrolled()) {
        if (sc != KEY_PGUP_MAKE && sc != KEY_PGDN_MAKE) {
            screen_scroll_to_bottom();
            redraw_line();
        }
    }

    if (kbd_e0) {
        // ★ 넘패드 엔터 (E0 1C) → 일반 엔터와 동일 처리
        if (sc == 0x1C) {
            if (keyboard_input_enabled) {
                kprint_char('\n');
                log_input_line();
                hist_push(key_buffer);
                user_input(key_buffer);
                key_buffer[0] = '\0'; in_len = 0; cur_ix = 0;
                hist_view = -1; saved_edit = 0; last_drawn_len = 0;
                reset_modifiers();
            } else {
                note_key_emit('\n');
            }
            kbd_e0 = 0;
            goto done;
        }

        // 넘패드 '/' 같은 E0 프리픽스 키
        char np = translate_numpad(sc, true);
        if (np) { base = np; kbd_e0 = 0; goto insert_char; }

        // 방향키 / 페이지 이동
        if (keyboard_input_enabled) {
            if (sc == KEY_LEFT_MAKE && cur_ix > 0) { cur_ix--; redraw_line(); reset_modifiers(); }
            else if (sc == KEY_RIGHT_MAKE && cur_ix < in_len) { cur_ix++; redraw_line(); reset_modifiers(); }
            else if (sc == KEY_UP_MAKE && hist_size > 0) {
                if (hist_view == -1) {
                    strncpy(edit_scratch, key_buffer, MAX_LINE - 1);
                    edit_scratch[MAX_LINE - 1] = '\0';
                    saved_edit = 1;
                    hist_view = 0;
                } else if (hist_view < hist_size - 1) {
                    hist_view++;
                }
                load_history_view();
                reset_modifiers();
            }
            else if (sc == KEY_DOWN_MAKE && hist_view != -1) {
                if (hist_view > 0) {
                    hist_view--;
                    load_history_view();
                } else {
                    hist_view = -1;
                    if (saved_edit) strncpy(key_buffer, edit_scratch, MAX_LINE - 1);
                    else key_buffer[0] = '\0';
                    in_len = (int)strlen(key_buffer);
                    cur_ix = in_len;
                    redraw_line();
                }
                reset_modifiers();
            }
            else if (sc == KEY_PGUP_MAKE) { scroll_up_screen(); reset_modifiers(); }
            else if (sc == KEY_PGDN_MAKE) { scroll_down_screen(); reset_modifiers(); }
        } else {
            if (sc == KEY_LEFT_MAKE)  { note_key_emit(NOTE_KEY_LEFT);  reset_modifiers(); }
            if (sc == KEY_RIGHT_MAKE) { note_key_emit(NOTE_KEY_RIGHT); reset_modifiers(); }
            if (sc == KEY_UP_MAKE)    { note_key_emit(NOTE_KEY_UP);    reset_modifiers(); }
            if (sc == KEY_DOWN_MAKE)  { note_key_emit(NOTE_KEY_DOWN);  reset_modifiers(); }
            if (sc == KEY_PGUP_MAKE)  { note_key_emit(NOTE_KEY_PGUP);  reset_modifiers(); }
            if (sc == KEY_PGDN_MAKE)  { note_key_emit(NOTE_KEY_PGDN);  reset_modifiers(); }
            if (sc == KEY_HOME_MAKE)  { note_key_emit(NOTE_KEY_HOME);  reset_modifiers(); }
            if (sc == KEY_END_MAKE)   { note_key_emit(NOTE_KEY_END);   reset_modifiers(); }
            if (sc == KEY_DELETE_MAKE){ note_key_emit(NOTE_KEY_DEL);   reset_modifiers(); }
        }

        kbd_e0 = 0;
        goto done;
    }

    if (sc == BACKSPACE) {
        if (keyboard_input_enabled) {
            if (cur_ix > 0) {
                memmove(&key_buffer[cur_ix - 1], &key_buffer[cur_ix], in_len - cur_ix);
                in_len--; cur_ix--; key_buffer[in_len] = '\0'; redraw_line();
            }
        } else { note_key_emit('\b'); }
        goto done;
    }
    if (sc == ENTER) {
        if (keyboard_input_enabled) {
            kprint_char('\n');
            log_input_line();
            hist_push(key_buffer);
            user_input(key_buffer);
            key_buffer[0] = '\0'; in_len = 0; cur_ix = 0;
            hist_view = -1; saved_edit = 0; last_drawn_len = 0;
            reset_modifiers();   // ✅ 엔터 시 전체 리셋
        } else { note_key_emit('\n'); }
        goto done;
    }
    if (sc == ESCAPE) {
        if (!keyboard_input_enabled) { note_key_emit(27); }
        reset_modifiers();
        goto done;
    }

    {
        char np = translate_numpad(sc, false);
        if (np) { base = np; goto insert_char; }
    }

    if (!numlock_on) {
        if (sc == KEY_LEFT_MAKE || sc == KEY_RIGHT_MAKE || sc == KEY_UP_MAKE || sc == KEY_DOWN_MAKE) {
            if (keyboard_input_enabled) {
                if (sc == KEY_LEFT_MAKE && cur_ix > 0) { cur_ix--; redraw_line(); reset_modifiers(); }
                else if (sc == KEY_RIGHT_MAKE && cur_ix < in_len) { cur_ix++; redraw_line(); reset_modifiers(); }
                else if (sc == KEY_UP_MAKE && hist_size > 0) {
                    if (hist_view == -1) {
                        strncpy(edit_scratch, key_buffer, MAX_LINE - 1);
                        edit_scratch[MAX_LINE - 1] = '\0';
                        saved_edit = 1;
                        hist_view = 0;
                    } else if (hist_view < hist_size - 1) {
                        hist_view++;
                    }
                    load_history_view();
                    reset_modifiers();
                }
                else if (sc == KEY_DOWN_MAKE && hist_view != -1) {
                    if (hist_view > 0) {
                        hist_view--;
                        load_history_view();
                    } else {
                        hist_view = -1;
                        if (saved_edit) strncpy(key_buffer, edit_scratch, MAX_LINE - 1);
                        else key_buffer[0] = '\0';
                        in_len = (int)strlen(key_buffer);
                        cur_ix = in_len;
                        redraw_line();
                    }
                    reset_modifiers();
                }
            } else {
                if (sc == KEY_LEFT_MAKE)  { note_key_emit(NOTE_KEY_LEFT);  reset_modifiers(); }
                if (sc == KEY_RIGHT_MAKE) { note_key_emit(NOTE_KEY_RIGHT); reset_modifiers(); }
                if (sc == KEY_UP_MAKE)    { note_key_emit(NOTE_KEY_UP);    reset_modifiers(); }
                if (sc == KEY_DOWN_MAKE)  { note_key_emit(NOTE_KEY_DOWN);  reset_modifiers(); }
                if (sc == KEY_HOME_MAKE)  { note_key_emit(NOTE_KEY_HOME);  reset_modifiers(); }
                if (sc == KEY_END_MAKE)   { note_key_emit(NOTE_KEY_END);   reset_modifiers(); }
                if (sc == KEY_DELETE_MAKE){ note_key_emit(NOTE_KEY_DEL);   reset_modifiers(); }
            }
            goto done;
        }
        if (sc == KEY_PGUP_MAKE) {
            if (keyboard_should_scroll_history()) scroll_up_screen();
            else note_key_emit(NOTE_KEY_PGUP);
            reset_modifiers();
            goto done;
        }
        if (sc == KEY_PGDN_MAKE) {
            if (keyboard_should_scroll_history()) scroll_down_screen();
            else note_key_emit(NOTE_KEY_PGDN);
            reset_modifiers();
            goto done;
        }
    }

    if (sc > SC_MAX) goto done;
    if (alt_left_pressed) {
        base = (char)(sc + 255);
    } 
    else if (alt_right_pressed) {
        base = (char)(sc + 126);
    } 
    else {
        base = sc_ascii[sc];
        if (base >= 'a' && base <= 'z') {
            if (keyboard_should_use_korean_ime()) {
                if (shift_pressed && keyboard_is_korean_shift_key(base)) {
                    base = (char)(base - 'a' + 'A');
                }
            } else if (shift_pressed ^ capslock_on) {
                base = (char)(base - 'a' + 'A');
            }
        } else {
            if (shift_pressed) base = sc_ascii_shift[sc];
        }
    }

insert_char:
    if (base != 0) {
        bool korean_ime_key = keyboard_should_use_korean_ime() &&
                              ((base >= 'a' && base <= 'z') || (base >= 'A' && base <= 'Z'));
        if (keyboard_input_enabled) {
            if (in_len < MAX_LINE - 1) {
                memmove(&key_buffer[cur_ix + 1], &key_buffer[cur_ix], in_len - cur_ix);
                key_buffer[cur_ix] = base;
                in_len++; cur_ix++;
                key_buffer[in_len] = '\0';
                redraw_line();
            }
        } else {
            if (ctrl_pressed && ((base >= 'a' && base <= 'z') || (base >= 'A' && base <= 'Z'))) {
                uint8_t ctrl_code = (uint8_t)(base & 0x1F);
                if (shift_pressed) {
                    note_key_emit((uint8_t)(0xA0u | ctrl_code));
                } else {
                    note_key_emit(ctrl_code);
                }
                reset_modifiers();
            } else {
                note_key_emit((uint8_t)base);
            }
        }
        if (korean_ime_key) {
            shift_pressed = false;
        }
    }

done:
    return;
}

static void keyboard_callback(registers_t* regs) {
    uint8_t sc = hal_in8(0x60);

    // ----- PS/2 응답 바이트 필터 -----
    if (sc == 0xFA || sc == 0xFE || sc == 0xEE) {
        UNUSED(regs);
        return; // ACK/RESEND/SELFTEST → 소비만 하고 무시
    }

    if (!ignore_ps2_scancodes)
        keyboard_handle_scancode(sc);

    UNUSED(regs);
}

// ──────────────────────────────────────────────
// 외부 함수
// ──────────────────────────────────────────────
void allow_keyboard_only() {
    // Enable IRQ0 (timer) + IRQ1 (keyboard) + IRQ2 (cascade for PIC2) while keeping others masked.
    // Timer must stay enabled so USB keyboard input (polled from the timer ISR) works in wait_for_keypress/getkey.
    hal_out8(0x21, 0xF8);
    // PIC2 must stay enabled for the mouse IRQ (IRQ12) so the PS/2 buffer continues to drain
    hal_out8(0xA1, 0xEF);
}

void allow_all_irqs() {
    hal_out8(0x21, 0x00);
    hal_out8(0xA1, 0x00);
}

static void wait_for_note_key(void) {
    hal_enable_interrupts();
    while (!input_queue_has_data()) {
        hal_halt();
    }
    g_key_pressed = true;
}

void keyboard_note_debounce(void) {
    hal_disable_interrupts();
    note_keybuf_clear_unsafe();
    hal_enable_interrupts();

    uint32_t start = tick;
    while ((tick - start) < 3u) { // ~30ms at 100Hz
        hal_halt();
        hal_disable_interrupts();
        note_keybuf_clear_unsafe();
        hal_enable_interrupts();
    }
}

void wait_for_keypress() {
    hal_disable_interrupts();
    note_keybuf_clear_unsafe();
    allow_keyboard_only();
    hal_enable_interrupts();

    // Debounce/flush: ignore any immediately-pending key event (e.g. the Enter that triggered the command).
    uint32_t start = tick;
    while ((tick - start) < 3u) { // ~30ms at 100Hz
        hal_halt();
        hal_disable_interrupts();
        note_keybuf_clear_unsafe();
        hal_enable_interrupts();
    }

    while (!input_queue_has_data()) hal_halt();
    g_key_pressed = true;
    allow_all_irqs();
}

int getkey(void) {
    uint8_t c = 0;
    do {
        if (!note_keybuf_pop(&c)) {
            wait_for_note_key();
            continue;
        }
    } while (c == 0);
    return (int)c;
}

int getkey_nonblock(void) {
    uint8_t c = 0;
    if (!note_keybuf_pop(&c)) {
        return 0;
    }
    return (int)c;
}

int getkey_event(input_event_t* event) {
    if (!event) {
        return 0;
    }
    do {
        if (!note_keybuf_pop_event(event)) {
            wait_for_note_key();
            continue;
        }
    } while (event->code == 0);
    return 1;
}

int getkey_event_nonblock(input_event_t* event) {
    if (!event) {
        return 0;
    }
    if (!note_keybuf_pop_event(event)) {
        return 0;
    }
    return event->code != 0 ? 1 : 0;
}

void keyboard_flush(void) {
    hal_disable_interrupts();
    note_keybuf_clear_unsafe();
    hal_enable_interrupts();
    reset_modifiers();
}

void keyboard_inject_scancode(uint8_t sc) {
    keyboard_handle_scancode(sc);
}

void keyboard_set_ignore_ps2(bool ignore) {
    ignore_ps2_scancodes = ignore;
}

uint32_t keyboard_get_modifiers(void) {
    uint32_t mods = 0;
    if (shift_pressed) {
        mods |= KEYBOARD_MOD_SHIFT;
    }
    if (ctrl_pressed) {
        mods |= KEYBOARD_MOD_CTRL;
    }
    if (alt_left_pressed || alt_right_pressed) {
        mods |= KEYBOARD_MOD_ALT;
    }
    return mods;
}

static inline void kbd_wait_input() {
    while (hal_in8(0x64) & 0x02);
}

static inline void kbd_wait_output() {
    while (!(hal_in8(0x64) & 0x01));
}

static inline void kbd_drain_output(void) {
    while (hal_in8(0x64) & 0x01) {
        (void)hal_in8(0x60);
    }
}

void init_keyboard() {
    // IRQ 핸들러 먼저 등록
    register_interrupt_handler(IRQ1, keyboard_callback);

    // 키보드 포트 비활성화
    kbd_wait_input();
    hal_out8(0x64, 0xAD);

    // 출력 버퍼 flush (VBox 필수)
    kbd_drain_output();

    // Command byte 읽기
    kbd_wait_input();
    hal_out8(0x64, 0x20);
    kbd_wait_output();
    uint8_t cmd = hal_in8(0x60);

    // IRQ1 enable.
    // Keep the controller's native mode and handle VMware's odd Shift
    // make/break sequences in software. Forcing translation caused
    // stray PS/2 key events on some VMware setups.
    cmd |= 0x01;

    // Command byte 쓰기
    kbd_wait_input();
    hal_out8(0x64, 0x60);
    kbd_wait_input();
    hal_out8(0x60, cmd);

    // 키보드 enable
    kbd_wait_input();
    hal_out8(0x64, 0xAE);

    // 스캔 활성화 (ACK 반드시 처리)
    kbd_wait_input();
    hal_out8(0x60, 0xF4);
    kbd_wait_output();
    hal_in8(0x60); // ACK(0xFA)

    // LED 끄기 (ACK 2번 처리)
    kbd_wait_input();
    hal_out8(0x60, 0xED);
    kbd_wait_output();
    hal_in8(0x60); // ACK

    kbd_wait_input();
    hal_out8(0x60, 0x00);
    kbd_wait_output();
    hal_in8(0x60); // ACK

    // modifier 상태 초기화
    capslock_on = numlock_on = scrolllock_on = false;
    reset_modifiers();
    note_keybuf_clear_unsafe();

    // Some QEMU/firmware setups leave one translated make code pending
    // after scan enable/LED sync. Drain it before IRQ1 is unmasked so it
    // cannot appear as a phantom first key (for example, '2').
    kbd_drain_output();

    // 마지막에 IRQ1 unmask
    uint8_t mask = hal_in8(0x21);
    mask &= ~(1 << 1);
    hal_out8(0x21, mask);
}
