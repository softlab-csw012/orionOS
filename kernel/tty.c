#include "tty.h"
#include "proc/proc.h"
#include "input_queue.h"
#include "../drivers/hal.h"
#include "../drivers/screen.h"
#include "io/console.h"
#include "../libc/string.h"
#include <stdbool.h>

#ifndef TTY_SIG_FAST_KILL
#define TTY_SIG_FAST_KILL 1
#endif

#define TTY_COOKED_LINE_MAX 256u

static inline uint32_t tty_irq_save(void) {
    uint32_t flags = 0;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void tty_irq_restore(uint32_t flags) {
    if (flags & 0x200u) {
        __asm__ volatile("sti" ::: "memory");
    }
}

static bool has_process_suffix(const char* name, const char* suffix) {
    if (!name) {
        return false;
    }
    size_t nlen = strlen(name);
    size_t slen = strlen(suffix);
    if (nlen < slen) {
        return false;
    }
    return strcasecmp(name + (nlen - slen), suffix) == 0;
}

static bool is_ctrl_e_protected_process(const char* name) {
    return has_process_suffix(name, "shell") ||
           has_process_suffix(name, "init.sys");
}

typedef enum {
    TTY_ANSI_TEXT = 0,
    TTY_ANSI_ESC,
    TTY_ANSI_CSI,
} tty_ansi_state_t;

typedef struct {
    tty_ansi_state_t state;
    int params[8];
    int param_count;
    bool have_digits;
    uint8_t fg;
    uint8_t bg;
    uint8_t saved_fg;
    uint8_t saved_bg;
    int saved_row;
    int saved_col;
    bool has_saved_cursor;
    bool bold;
    bool faint;
    bool inverse;
} tty_ansi_t;

static tty_ansi_t g_tty_ansi = {
    .state = TTY_ANSI_TEXT,
    .params = {0, 0, 0, 0, 0, 0, 0, 0},
    .param_count = 0,
    .have_digits = false,
    .fg = 7,
    .bg = 0,
    .saved_fg = 7,
    .saved_bg = 0,
    .saved_row = 0,
    .saved_col = 0,
    .has_saved_cursor = false,
    .bold = false,
    .faint = false,
    .inverse = false,
};

static char g_tty_line_build[TTY_COOKED_LINE_MAX];
static uint32_t g_tty_line_build_len = 0;
static char g_tty_line_ready[TTY_COOKED_LINE_MAX];
static uint32_t g_tty_line_ready_len = 0;
static uint32_t g_tty_line_ready_off = 0;

static bool tty_is_cooked_input_char(uint8_t key) {
    char ch = (char)key;
    if (ch == '\n' || ch == '\r' || ch == '\b' || ch == 0x7f) {
        return true;
    }
    if (ch == '\t') {
        return true;
    }
    return (ch >= 32 && ch <= 126);
}

static void tty_cooked_build_line_blocking(void) {
    while (g_tty_line_ready_off >= g_tty_line_ready_len) {
        uint8_t key = 0;
        if (!input_queue_pop(&key)) {
            hal_enable_interrupts();
            hal_halt();
            continue;
        }

        if (!tty_is_cooked_input_char(key)) {
            continue;
        }

        char ch = (char)key;
        if (ch == '\r') {
            ch = '\n';
        }

        if (ch == '\b' || ch == 0x7f) {
            if (g_tty_line_build_len > 0) {
                g_tty_line_build_len--;
                kprint_char('\b');
            }
            continue;
        }

        if (ch == '\n') {
            if (g_tty_line_build_len < TTY_COOKED_LINE_MAX - 1u) {
                g_tty_line_build[g_tty_line_build_len++] = '\n';
            } else {
                g_tty_line_build[TTY_COOKED_LINE_MAX - 1u] = '\n';
                g_tty_line_build_len = TTY_COOKED_LINE_MAX;
            }

            memcpy(g_tty_line_ready, g_tty_line_build, g_tty_line_build_len);
            g_tty_line_ready_len = g_tty_line_build_len;
            g_tty_line_ready_off = 0;
            g_tty_line_build_len = 0;
            kprint_char('\n');
            return;
        }

        if (g_tty_line_build_len < TTY_COOKED_LINE_MAX - 1u) {
            g_tty_line_build[g_tty_line_build_len++] = ch;
            kprint_char(ch);
        }
    }
}

static uint8_t tty_ansi_map_fg(int code, uint8_t cur_fg) {
    if (code >= 30 && code <= 37) return (uint8_t)(code - 30);
    if (code == 39) return 7;
    if (code >= 90 && code <= 97) return (uint8_t)(8 + (code - 90));
    return cur_fg;
}

static uint8_t tty_ansi_map_bg(int code, uint8_t cur_bg) {
    if (code >= 40 && code <= 47) return (uint8_t)(code - 40);
    if (code == 49) return 0;
    if (code >= 100 && code <= 107) return (uint8_t)(8 + (code - 100));
    return cur_bg;
}

static inline int tty_clamp(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline int tty_ansi_param(const tty_ansi_t* a, int idx, int def) {
    if (!a || idx < 0 || idx >= a->param_count) {
        return def;
    }
    return a->params[idx];
}

static void tty_ansi_apply_current_color(tty_ansi_t* a) {
    uint8_t fg = a->fg;
    uint8_t bg = a->bg;
    if (a->bold && fg < 8) {
        fg = (uint8_t)(fg + 8);
    }
    if (a->faint && fg >= 8) {
        fg = (uint8_t)(fg - 8);
    }
    if (a->inverse) {
        uint8_t t = fg;
        fg = bg;
        bg = t;
    }
    set_color(fg, bg);
}

static void tty_ansi_apply_sgr(tty_ansi_t* a) {
    int count = a->param_count;
    if (count == 0) {
        count = 1;
        a->params[0] = 0;
    }
    for (int i = 0; i < count; i++) {
        int code = a->params[i];
        if (code == 0) {
            a->fg = 7;
            a->bg = 0;
            a->bold = false;
            a->faint = false;
            a->inverse = false;
            continue;
        }
        if (code == 1) {
            a->bold = true;
            a->faint = false;
            continue;
        }
        if (code == 2) {
            a->faint = true;
            a->bold = false;
            continue;
        }
        if (code == 7) {
            a->inverse = true;
            continue;
        }
        if (code == 22) {
            a->bold = false;
            a->faint = false;
            continue;
        }
        if (code == 27) {
            a->inverse = false;
            continue;
        }
        a->fg = tty_ansi_map_fg(code, a->fg);
        a->bg = tty_ansi_map_bg(code, a->bg);
    }
    tty_ansi_apply_current_color(a);
}

static void tty_ansi_reset_csi(tty_ansi_t* a) {
    a->param_count = 0;
    a->have_digits = false;
    for (int i = 0; i < 8; i++) {
        a->params[i] = 0;
    }
}

static void tty_ansi_clear_line_mode(int mode) {
    int cols = screen_get_cols();
    if (cols <= 0) {
        cols = 80;
    }
    int row = get_cursor_row();
    int col = get_cursor_col();
    int old_col = col;
    int from = 0;
    int to = cols - 1;
    if (mode == 0) {
        from = col;
    } else if (mode == 1) {
        to = col;
    } else if (mode == 2) {
        from = 0;
        to = cols - 1;
    }
    if (to < from) {
        return;
    }
    set_cursor(row, from);
    for (int i = from; i <= to; i++) {
        kprint_char(' ');
    }
    set_cursor(row, old_col);
}

static void tty_ansi_save_cursor(tty_ansi_t* a) {
    a->saved_row = get_cursor_row();
    a->saved_col = get_cursor_col();
    a->saved_fg = a->fg;
    a->saved_bg = a->bg;
    a->has_saved_cursor = true;
}

static void tty_ansi_restore_cursor(tty_ansi_t* a) {
    if (!a->has_saved_cursor) {
        return;
    }
    a->fg = a->saved_fg;
    a->bg = a->saved_bg;
    tty_ansi_apply_current_color(a);
    set_cursor(a->saved_row, a->saved_col);
}

static void tty_ansi_handle_csi_final(tty_ansi_t* a, char final) {
    int p0 = tty_ansi_param(a, 0, 0);
    int p1 = tty_ansi_param(a, 1, 0);
    int rows = screen_get_rows();
    int cols = screen_get_cols();
    if (rows <= 0) rows = 25;
    if (cols <= 0) cols = 80;
    int row = get_cursor_row();
    int col = get_cursor_col();

    switch (final) {
        case 'm':
            tty_ansi_apply_sgr(a);
            break;
        case 'J':
            if (p0 == 2 || p0 == 3) {
                clear_screen();
                set_cursor(0, 0);
            }
            break;
        case 'K':
            tty_ansi_clear_line_mode(p0);
            break;
        case 'H':
        case 'f': {
            int target_row = (p0 > 0) ? (p0 - 1) : 0;
            int target_col = (p1 > 0) ? (p1 - 1) : 0;
            set_cursor(tty_clamp(target_row, 0, rows - 1),
                       tty_clamp(target_col, 0, cols - 1));
            break;
        }
        case 'A': {
            int n = (p0 > 0) ? p0 : 1;
            set_cursor(tty_clamp(row - n, 0, rows - 1), col);
            break;
        }
        case 'B': {
            int n = (p0 > 0) ? p0 : 1;
            set_cursor(tty_clamp(row + n, 0, rows - 1), col);
            break;
        }
        case 'C': {
            int n = (p0 > 0) ? p0 : 1;
            set_cursor(row, tty_clamp(col + n, 0, cols - 1));
            break;
        }
        case 'D': {
            int n = (p0 > 0) ? p0 : 1;
            set_cursor(row, tty_clamp(col - n, 0, cols - 1));
            break;
        }
        case 'E': {
            int n = (p0 > 0) ? p0 : 1;
            set_cursor(tty_clamp(row + n, 0, rows - 1), 0);
            break;
        }
        case 'F': {
            int n = (p0 > 0) ? p0 : 1;
            set_cursor(tty_clamp(row - n, 0, rows - 1), 0);
            break;
        }
        case 'G': {
            int target_col = (p0 > 0) ? (p0 - 1) : 0;
            set_cursor(row, tty_clamp(target_col, 0, cols - 1));
            break;
        }
        case 's':
            tty_ansi_save_cursor(a);
            break;
        case 'u':
            tty_ansi_restore_cursor(a);
            break;
        default:
            break;
    }
}

static void tty_ansi_feed_char(tty_ansi_t* a, char ch) {
    if (a->state == TTY_ANSI_TEXT) {
        if ((uint8_t)ch == 0x1B) {
            a->state = TTY_ANSI_ESC;
            return;
        }
        kprint_char(ch);
        return;
    }

    if (a->state == TTY_ANSI_ESC) {
        if (ch == '[') {
            a->state = TTY_ANSI_CSI;
            tty_ansi_reset_csi(a);
            return;
        }
        if (ch == '7') {
            tty_ansi_save_cursor(a);
            a->state = TTY_ANSI_TEXT;
            return;
        }
        if (ch == '8') {
            tty_ansi_restore_cursor(a);
            a->state = TTY_ANSI_TEXT;
            return;
        }
        if (ch == 'c') {
            clear_screen();
            set_cursor(0, 0);
            a->fg = 7;
            a->bg = 0;
            a->bold = false;
            a->faint = false;
            a->inverse = false;
            tty_ansi_apply_current_color(a);
            a->state = TTY_ANSI_TEXT;
            return;
        }
        // Not a CSI sequence: emit ESC + char literally.
        kprint_char((char)0x1B);
        kprint_char(ch);
        a->state = TTY_ANSI_TEXT;
        return;
    }

    // CSI
    if (ch >= '0' && ch <= '9') {
        if (a->param_count == 0) {
            a->param_count = 1;
        }
        int idx = a->param_count - 1;
        a->params[idx] = a->params[idx] * 10 + (ch - '0');
        a->have_digits = true;
        return;
    }

    if (ch == ';') {
        if (a->param_count == 0) {
            a->param_count = 1;
        }
        if (a->param_count < 8) {
            a->param_count++;
        }
        a->have_digits = false;
        return;
    }

    tty_ansi_handle_csi_final(a, ch);
    a->state = TTY_ANSI_TEXT;
}

void tty_init(void) {
}

uint32_t tty_get_foreground(void) {
    return proc_get_foreground_pid();
}

void tty_set_foreground(uint32_t pid) {
    proc_set_foreground_pid(pid);
}

int tty_read_stdin(void* buf, uint32_t len) {
    if (!buf || len == 0) {
        return 0;
    }

    char* out = (char*)buf;
    uint32_t got = 0;

    while (got < len) {
        if (g_tty_line_ready_off >= g_tty_line_ready_len) {
            if (got > 0) {
                break;
            }
            tty_cooked_build_line_blocking();
            if (g_tty_line_ready_off >= g_tty_line_ready_len) {
                continue;
            }
        }

        char ch = g_tty_line_ready[g_tty_line_ready_off++];
        out[got++] = ch;
        if (g_tty_line_ready_off >= g_tty_line_ready_len || ch == '\n') {
            break;
        }
    }

    return (int)got;
}

int tty_write_stdout(const void* buf, uint32_t len) {
    if (!buf || len == 0) {
        return 0;
    }
    const char* s = (const char*)buf;
    uint32_t flags = tty_irq_save();
    for (uint32_t i = 0; i < len; i++) {
        tty_ansi_feed_char(&g_tty_ansi, s[i]);
    }
    tty_irq_restore(flags);
    return (int)len;
}

int tty_signal_int(void) {
    uint32_t fg_pid = tty_get_foreground();
    process_t* fg = fg_pid ? proc_lookup(fg_pid) : NULL;
    process_t* cur = proc_current();

    uint32_t target_pid = 0;
    if (fg && !fg->is_kernel && !is_ctrl_e_protected_process(fg->name)) {
        target_pid = fg->pid;
    } else if (cur && !cur->is_kernel && !is_ctrl_e_protected_process(cur->name)) {
        target_pid = cur->pid;
    } else if (fg_pid != 0 && !fg) {
        target_pid = fg_pid;
    }

    if (target_pid == 0) {
        return 0;
    }

    (void)proc_signal_enqueue(target_pid, PROC_SIG_INT);
#if TTY_SIG_FAST_KILL
    proc_request_kill_pid(target_pid);
#endif
    return 1;
}

void tty_timer_tick(void) {
    console_cursor_blink();
}
