#include "tty.h"
#include "proc/proc.h"
#include "input_queue.h"
#include "../drivers/hal.h"
#include "../drivers/font.h"
#include "../drivers/keyboard.h"
#include "../drivers/screen.h"
#include "kernel.h"
#include "ime.h"
#include "io/console.h"
#include "../libc/string.h"
#include <stdbool.h>

#ifndef TTY_SIG_FAST_KILL
#define TTY_SIG_FAST_KILL 1
#endif

#define TTY_COOKED_LINE_MAX 256u
#define TTY_VC_COUNT 1u

static inline uintptr_t tty_irq_save(void) {
    uintptr_t flags = 0;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void tty_irq_restore(uintptr_t flags) {
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
    bool private_mode;
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
    uint32_t utf8_codepoint;
    uint8_t utf8_remaining;
} tty_ansi_t;

static tty_ansi_t g_tty_ansi = {
    .state = TTY_ANSI_TEXT,
    .params = {0, 0, 0, 0, 0, 0, 0, 0},
    .param_count = 0,
    .have_digits = false,
    .private_mode = false,
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
static int g_tty_cooked_prompt_row = 0;
static int g_tty_cooked_prompt_col = 0;
static int g_tty_cooked_last_cells = 0;
static ime_state_t g_tty_ime_state;
static ime_t g_tty_ime = {0};

typedef struct {
    bool initialized;
    uint16_t cells[SCREEN_MAX_ROWS][SCREEN_MAX_COLS];
    int rows;
    int cols;
    int cursor_offset;
    tty_ansi_t ansi;
    uint32_t foreground_pid;
    uint32_t shell_pid;
    bool keyboard_input_enabled;
    bool prompt_enabled;
    bool shell_suspended;
} tty_vc_state_t;

static tty_vc_state_t g_tty_vcs[TTY_VC_COUNT];
static uint8_t g_tty_active_vc = 0;
static volatile uint8_t g_tty_switch_req = 0xffu;
static bool g_tty_ready = false;

static inline int tty_clamp(int v, int lo, int hi);
static void tty_ansi_feed_char(tty_vc_state_t* target, tty_ansi_t* a, char ch);

static uint16_t tty_blank_cell_for_ansi(const tty_ansi_t* a) {
    uint8_t fg = 7;
    uint8_t bg = 0;
    if (a) {
        fg = a->fg;
        bg = a->bg;
        if (a->bold && fg < 8) fg = (uint8_t)(fg + 8);
        if (a->faint && fg >= 8) fg = (uint8_t)(fg - 8);
        if (a->inverse) {
            uint8_t t = fg;
            fg = bg;
            bg = t;
        }
    }
    return (uint16_t)(((uint16_t)vga_attr(fg, bg) << 8) | ' ');
}

static void tty_vc_reset_state(tty_vc_state_t* s, int rows, int cols) {
    if (!s) {
        return;
    }
    if (rows <= 0) rows = screen_get_rows();
    if (cols <= 0) cols = screen_get_cols();
    if (rows <= 0) rows = 25;
    if (cols <= 0) cols = 80;
    if (rows > SCREEN_MAX_ROWS) rows = SCREEN_MAX_ROWS;
    if (cols > SCREEN_MAX_COLS) cols = SCREEN_MAX_COLS;
    memset(s, 0, sizeof(*s));
    s->rows = rows;
    s->cols = cols;
    s->ansi = g_tty_ansi;
    uint16_t blank = tty_blank_cell_for_ansi(&s->ansi);
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            s->cells[r][c] = blank;
        }
    }
    s->initialized = true;
}

static void tty_vc_ensure_initialized(tty_vc_state_t* s) {
    if (!s) {
        return;
    }
    int rows = screen_get_rows();
    int cols = screen_get_cols();
    if (!s->initialized) {
        tty_vc_reset_state(s, rows, cols);
        return;
    }
    if (rows <= 0) rows = s->rows;
    if (cols <= 0) cols = s->cols;
    if (rows <= 0) rows = 25;
    if (cols <= 0) cols = 80;
    if (rows > SCREEN_MAX_ROWS) rows = SCREEN_MAX_ROWS;
    if (cols > SCREEN_MAX_COLS) cols = SCREEN_MAX_COLS;
    if (s->rows == rows && s->cols == cols) {
        return;
    }
    uint16_t old_cells[SCREEN_MAX_ROWS][SCREEN_MAX_COLS];
    int old_rows = s->rows;
    int old_cols = s->cols;
    memcpy(old_cells, s->cells, sizeof(old_cells));
    uint32_t fg_pid = s->foreground_pid;
    uint32_t shell_pid = s->shell_pid;
    bool key_enabled = s->keyboard_input_enabled;
    bool prompt = s->prompt_enabled;
    bool suspended = s->shell_suspended;
    int old_cursor = s->cursor_offset;
    tty_ansi_t old_ansi = s->ansi;
    tty_vc_reset_state(s, rows, cols);
    s->foreground_pid = fg_pid;
    s->shell_pid = shell_pid;
    s->keyboard_input_enabled = key_enabled;
    s->prompt_enabled = prompt;
    s->shell_suspended = suspended;
    s->ansi = old_ansi;
    int copy_rows = (old_rows < rows) ? old_rows : rows;
    int copy_cols = (old_cols < cols) ? old_cols : cols;
    for (int r = 0; r < copy_rows; r++) {
        for (int c = 0; c < copy_cols; c++) {
            s->cells[r][c] = old_cells[r][c];
        }
    }
    int max_off = rows * cols;
    if (max_off <= 0) {
        s->cursor_offset = 0;
    } else if (old_cursor < 0) {
        s->cursor_offset = 0;
    } else if (old_cursor >= max_off) {
        s->cursor_offset = max_off - 1;
    } else {
        s->cursor_offset = old_cursor;
    }
}

static bool tty_is_cooked_input_char(uint8_t key) {
    char ch = (char)key;
    if (ch == '\n' || ch == '\r' || ch == '\b' || ch == 0x7f) {
        return true;
    }
    if (ch == '\t') {
        return true;
    }
    return (key >= 0x80u) || (ch >= 32 && ch <= 126);
}

static uint32_t tty_ime_modifiers_from_input(const input_event_t* input) {
    uint32_t mods = 0;
    if (input && (input->modifiers & INPUT_MOD_SHIFT)) {
        mods |= IME_MOD_SHIFT;
    }
    if (input && (input->modifiers & INPUT_MOD_CTRL)) {
        mods |= IME_MOD_CTRL;
    }
    if (input && (input->modifiers & INPUT_MOD_ALT)) {
        mods |= IME_MOD_ALT;
    }
    return mods;
}

static bool tty_cooked_translate_ime_event(const input_event_t* input, ime_key_event_t* event) {
    if (!input || !event) {
        return false;
    }
    uint8_t key = input->code;

    event->type = IME_KEY_CHAR;
    event->codepoint = 0;
    event->modifiers = tty_ime_modifiers_from_input(input);

    switch (key) {
        case '\b':
        case 0x7f:
            event->type = IME_KEY_BACKSPACE;
            return true;
        case '\r':
        case '\n':
            event->type = IME_KEY_ENTER;
            return true;
        case '\t':
            event->type = IME_KEY_TAB;
            return true;
        case NOTE_KEY_LEFT:
            event->type = IME_KEY_LEFT;
            return true;
        case NOTE_KEY_RIGHT:
            event->type = IME_KEY_RIGHT;
            return true;
        case NOTE_KEY_UP:
            event->type = IME_KEY_UP;
            return true;
        case NOTE_KEY_DOWN:
            event->type = IME_KEY_DOWN;
            return true;
        case NOTE_KEY_HOME:
            event->type = IME_KEY_HOME;
            return true;
        case NOTE_KEY_END:
            event->type = IME_KEY_END;
            return true;
        case NOTE_KEY_DEL:
            event->type = IME_KEY_DELETE;
            return true;
        default:
            break;
    }

    if (key < 0x80u) {
        event->type = IME_KEY_CHAR;
        event->codepoint = key;
        return true;
    }
    return false;
}

static int tty_utf8_encode(uint32_t cp, char* out, int cap) {
    if (!out || cap <= 0) {
        return 0;
    }
    if (cp <= 0x7Fu) {
        if (cap < 1) return 0;
        out[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7FFu) {
        if (cap < 2) return 0;
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp <= 0xFFFFu) {
        if (cap < 3) return 0;
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cap < 4) return 0;
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

static int tty_display_cells_for_codepoint(uint32_t cp) {
    uint8_t cells[2];
    int n = font_encode_codepoint(cp, cells, (int)sizeof(cells));
    return n > 0 ? n : 1;
}

static bool tty_cooked_append_bytes(const char* data, uint32_t len) {
    if (!data || len == 0) {
        return true;
    }
    if (g_tty_line_build_len + len >= TTY_COOKED_LINE_MAX) {
        return false;
    }
    memcpy(&g_tty_line_build[g_tty_line_build_len], data, len);
    g_tty_line_build_len += len;
    return true;
}

static bool tty_cooked_append_codepoint(uint32_t cp) {
    char utf8[4];
    int len = tty_utf8_encode(cp, utf8, (int)sizeof(utf8));
    return len > 0 && tty_cooked_append_bytes(utf8, (uint32_t)len);
}

static void tty_cooked_delete_last_codepoint(void) {
    while (g_tty_line_build_len > 0) {
        uint8_t b = (uint8_t)g_tty_line_build[g_tty_line_build_len - 1u];
        g_tty_line_build_len--;
        if ((b & 0xC0u) != 0x80u) {
            break;
        }
    }
}

static void tty_cooked_commit_composition(void) {
    ime_key_event_t event = { .type = IME_KEY_ENTER, .codepoint = 0, .modifiers = 0 };
    ime_result_t ime;
    ime_handle_event(&g_tty_ime, &event, &ime);
    for (uint32_t i = 0; i < ime.commit_count; i++) {
        (void)tty_cooked_append_codepoint(ime.commit[i]);
    }
}

static int tty_target_rows(tty_vc_state_t* s) {
    if (!s) {
        int rows = screen_get_rows();
        return rows > 0 ? rows : 25;
    }
    tty_vc_ensure_initialized(s);
    return s->rows > 0 ? s->rows : 25;
}

static int tty_target_cols(tty_vc_state_t* s) {
    if (!s) {
        int cols = screen_get_cols();
        return cols > 0 ? cols : 80;
    }
    tty_vc_ensure_initialized(s);
    return s->cols > 0 ? s->cols : 80;
}

static int tty_target_cursor_offset(tty_vc_state_t* s) {
    if (!s) {
        return get_cursor_offset() / 2;
    }
    tty_vc_ensure_initialized(s);
    return s->cursor_offset;
}

static void tty_target_set_cursor(tty_vc_state_t* s, int row, int col) {
    int rows = tty_target_rows(s);
    int cols = tty_target_cols(s);
    row = tty_clamp(row, 0, rows - 1);
    col = tty_clamp(col, 0, cols - 1);
    if (!s) {
        set_cursor(row, col);
        return;
    }
    s->cursor_offset = row * cols + col;
}

static int tty_target_cursor_row(tty_vc_state_t* s) {
    int cols = tty_target_cols(s);
    int off = tty_target_cursor_offset(s);
    if (cols <= 0) return 0;
    return off / cols;
}

static int tty_target_cursor_col(tty_vc_state_t* s) {
    int cols = tty_target_cols(s);
    int off = tty_target_cursor_offset(s);
    if (cols <= 0) return 0;
    return off % cols;
}

static void tty_target_put_cell(tty_vc_state_t* s, int row, int col, uint16_t cell) {
    if (!s) {
        screen_put_at(col, row, (uint8_t)(cell & 0xffu), (uint8_t)((cell >> 8) & 0xffu));
        return;
    }
    tty_vc_ensure_initialized(s);
    if (row < 0 || col < 0 || row >= s->rows || col >= s->cols) {
        return;
    }
    s->cells[row][col] = cell;
}

static void tty_target_scroll_up(tty_vc_state_t* s, const tty_ansi_t* a) {
    uint16_t blank = tty_blank_cell_for_ansi(a);
    int rows = tty_target_rows(s);
    int cols = tty_target_cols(s);
    if (!s) {
        screen_scroll_output((uint8_t)((blank >> 8) & 0xffu));
        return;
    }
    for (int r = 1; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            s->cells[r - 1][c] = s->cells[r][c];
        }
    }
    for (int c = 0; c < cols; c++) {
        s->cells[rows - 1][c] = blank;
    }
    s->cursor_offset = (rows - 1) * cols;
}

static void tty_target_write_char(tty_vc_state_t* s, tty_ansi_t* a, char ch) {
    int rows = tty_target_rows(s);
    int cols = tty_target_cols(s);
    int row = tty_target_cursor_row(s);
    int col = tty_target_cursor_col(s);

    if (ch == '\r') {
        tty_target_set_cursor(s, row, 0);
        return;
    }
    if (ch == '\n') {
        row++;
        col = 0;
        if (row >= rows) {
            tty_target_scroll_up(s, a);
            row = rows - 1;
        }
        tty_target_set_cursor(s, row, col);
        return;
    }
    if (ch == '\b' || ch == 0x7f) {
        if (col > 0) {
            col--;
        } else if (row > 0) {
            row--;
            col = cols - 1;
        } else {
            col = 0;
        }
        tty_target_put_cell(s, row, col, tty_blank_cell_for_ansi(a));
        tty_target_set_cursor(s, row, col);
        return;
    }
    if (ch == '\t') {
        int spaces = 4 - (col % 4);
        while (spaces-- > 0) {
            tty_target_write_char(s, a, ' ');
        }
        return;
    }

    uint8_t fg = a ? a->fg : 7;
    uint8_t bg = a ? a->bg : 0;
    if (a) {
        if (a->bold && fg < 8) fg = (uint8_t)(fg + 8);
        if (a->faint && fg >= 8) fg = (uint8_t)(fg - 8);
        if (a->inverse) {
            uint8_t t = fg;
            fg = bg;
            bg = t;
        }
    }
    tty_target_put_cell(s, row, col, (uint16_t)(((uint16_t)vga_attr(fg, bg) << 8) | (uint8_t)ch));
    col++;
    if (col >= cols) {
        col = 0;
        row++;
        if (row >= rows) {
            tty_target_scroll_up(s, a);
            row = rows - 1;
        }
    }
    tty_target_set_cursor(s, row, col);
}

static void tty_target_write_codepoint(tty_vc_state_t* s, tty_ansi_t* a, uint32_t codepoint) {
    uint8_t bytes[2];
    int count = font_encode_codepoint(codepoint, bytes, (int)sizeof(bytes));
    if (count <= 0) {
        return;
    }
    for (int i = 0; i < count; i++) {
        tty_target_write_char(s, a, (char)bytes[i]);
    }
}

static void tty_cooked_capture_prompt(void) {
    if (g_tty_line_build_len == 0 && ime_preview_codepoint(&g_tty_ime) == 0) {
        g_tty_cooked_prompt_row = get_cursor_row();
        g_tty_cooked_prompt_col = get_cursor_col();
        g_tty_cooked_last_cells = 0;
    }
}

static void tty_cooked_write_utf8_bytes(const char* data, uint32_t len, tty_ansi_t* ansi) {
    if (!data || !ansi) {
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        tty_ansi_feed_char(NULL, ansi, data[i]);
    }
}

static void tty_cooked_redraw_line(void) {
    tty_ansi_t ansi = g_tty_ansi;
    uint32_t preview = ime_preview_codepoint(&g_tty_ime);
    int cells = 0;

    set_cursor(g_tty_cooked_prompt_row, g_tty_cooked_prompt_col);
    tty_cooked_write_utf8_bytes(g_tty_line_build, g_tty_line_build_len, &ansi);

    for (uint32_t i = 0; i < g_tty_line_build_len;) {
        uint8_t ch = (uint8_t)g_tty_line_build[i];
        if (ch < 0x80u) {
            cells += tty_display_cells_for_codepoint(ch);
            i++;
            continue;
        }
        if ((ch & 0xE0u) == 0xC0u && i + 1u < g_tty_line_build_len) {
            uint32_t cp = ((uint32_t)(ch & 0x1Fu) << 6) |
                          (uint32_t)((uint8_t)g_tty_line_build[i + 1u] & 0x3Fu);
            cells += tty_display_cells_for_codepoint(cp);
            i += 2u;
            continue;
        }
        if ((ch & 0xF0u) == 0xE0u && i + 2u < g_tty_line_build_len) {
            uint32_t cp = ((uint32_t)(ch & 0x0Fu) << 12) |
                          ((uint32_t)((uint8_t)g_tty_line_build[i + 1u] & 0x3Fu) << 6) |
                          (uint32_t)((uint8_t)g_tty_line_build[i + 2u] & 0x3Fu);
            cells += tty_display_cells_for_codepoint(cp);
            i += 3u;
            continue;
        }
        if ((ch & 0xF8u) == 0xF0u && i + 3u < g_tty_line_build_len) {
            uint32_t cp = ((uint32_t)(ch & 0x07u) << 18) |
                          ((uint32_t)((uint8_t)g_tty_line_build[i + 1u] & 0x3Fu) << 12) |
                          ((uint32_t)((uint8_t)g_tty_line_build[i + 2u] & 0x3Fu) << 6) |
                          (uint32_t)((uint8_t)g_tty_line_build[i + 3u] & 0x3Fu);
            cells += tty_display_cells_for_codepoint(cp);
            i += 4u;
            continue;
        }
        cells += 1;
        i++;
    }

    if (preview != 0) {
        char utf8[4];
        int len = tty_utf8_encode(preview, utf8, (int)sizeof(utf8));
        if (len > 0) {
            tty_cooked_write_utf8_bytes(utf8, (uint32_t)len, &ansi);
            cells += tty_display_cells_for_codepoint(preview);
        }
    }

    int end_row = get_cursor_row();
    int end_col = get_cursor_col();
    for (int i = cells; i < g_tty_cooked_last_cells; i++) {
        tty_target_write_char(NULL, &ansi, ' ');
    }
    set_cursor(end_row, end_col);
    g_tty_cooked_last_cells = cells;
}

static void tty_cooked_backspace(void) {
    ime_key_event_t event = { .type = IME_KEY_BACKSPACE, .codepoint = 0, .modifiers = 0 };
    ime_result_t ime;
    ime_handle_event(&g_tty_ime, &event, &ime);
    if (ime.consumed) {
        tty_cooked_redraw_line();
        return;
    }

    if (g_tty_line_build_len > 0) {
        tty_cooked_delete_last_codepoint();
        tty_cooked_redraw_line();
    }
}

static void tty_cooked_handle_hangul_char(char ch, uint32_t modifiers) {
    ime_key_event_t event = {
        .type = IME_KEY_CHAR,
        .codepoint = (uint32_t)(uint8_t)ch,
        .modifiers = modifiers,
    };
    ime_result_t ime;
    ime_handle_event(&g_tty_ime, &event, &ime);
    for (uint32_t i = 0; i < ime.commit_count; i++) {
        (void)tty_cooked_append_codepoint(ime.commit[i]);
    }
    if (!ime.consumed) {
        (void)tty_cooked_append_bytes(&ch, 1u);
    }
    tty_cooked_redraw_line();
}

static void tty_cooked_build_line_blocking(void) {
    while (g_tty_line_ready_off >= g_tty_line_ready_len) {
        input_event_t input = {0};
        ime_key_event_t event;
        if (!input_queue_pop_event(&input)) {
            hal_enable_interrupts();
            hal_halt();
            continue;
        }
        uint8_t key = input.code;

        if (!tty_is_cooked_input_char(key)) {
            continue;
        }

        if (!tty_cooked_translate_ime_event(&input, &event)) {
            continue;
        }

        char ch = (char)key;
        tty_cooked_capture_prompt();
        if (event.type == IME_KEY_ENTER) {
            ch = '\n';
        }

        if (event.type == IME_KEY_BACKSPACE) {
            tty_cooked_backspace();
            continue;
        }

        if (event.type == IME_KEY_ENTER) {
            tty_cooked_commit_composition();
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
            ime_reset(&g_tty_ime);
            g_tty_cooked_last_cells = 0;
            kprint_char('\n');
            return;
        }

        if (event.type != IME_KEY_CHAR) {
            tty_cooked_commit_composition();
            continue;
        }

        if (keyboard_korean_ime_enabled && key < 0x80u && ch != '\t') {
            tty_cooked_handle_hangul_char(ch, tty_ime_modifiers_from_input(&input));
            continue;
        }

        tty_cooked_commit_composition();
        if (g_tty_line_build_len < TTY_COOKED_LINE_MAX - 1u) {
            g_tty_line_build[g_tty_line_build_len++] = ch;
            tty_cooked_redraw_line();
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

static void tty_ansi_apply_current_color(tty_vc_state_t* target, tty_ansi_t* a) {
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
    if (!target) {
        set_color(fg, bg);
    }
}

static void tty_ansi_apply_sgr(tty_vc_state_t* target, tty_ansi_t* a) {
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
    tty_ansi_apply_current_color(target, a);
}

static void tty_ansi_reset_csi(tty_ansi_t* a) {
    a->param_count = 0;
    a->have_digits = false;
    a->private_mode = false;
    for (int i = 0; i < 8; i++) {
        a->params[i] = 0;
    }
}

static void tty_ansi_clear_line_mode(tty_vc_state_t* target, tty_ansi_t* a, int mode) {
    int cols = tty_target_cols(target);
    int row = tty_target_cursor_row(target);
    int col = tty_target_cursor_col(target);
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
    for (int i = from; i <= to; i++) {
        tty_target_put_cell(target, row, i, tty_blank_cell_for_ansi(a));
    }
    tty_target_set_cursor(target, row, old_col);
}

static void tty_ansi_save_cursor(tty_vc_state_t* target, tty_ansi_t* a) {
    a->saved_row = tty_target_cursor_row(target);
    a->saved_col = tty_target_cursor_col(target);
    a->saved_fg = a->fg;
    a->saved_bg = a->bg;
    a->has_saved_cursor = true;
}

static void tty_ansi_restore_cursor(tty_vc_state_t* target, tty_ansi_t* a) {
    if (!a->has_saved_cursor) {
        return;
    }
    a->fg = a->saved_fg;
    a->bg = a->saved_bg;
    tty_ansi_apply_current_color(target, a);
    tty_target_set_cursor(target, a->saved_row, a->saved_col);
}

static void tty_ansi_handle_csi_final(tty_vc_state_t* target, tty_ansi_t* a, char final) {
    int p0 = tty_ansi_param(a, 0, 0);
    int p1 = tty_ansi_param(a, 1, 0);
    int rows = tty_target_rows(target);
    int cols = tty_target_cols(target);
    int row = tty_target_cursor_row(target);
    int col = tty_target_cursor_col(target);

    switch (final) {
        case 'm':
            tty_ansi_apply_sgr(target, a);
            break;
        case 'J':
            if (p0 == 2 || p0 == 3) {
                uint16_t blank = tty_blank_cell_for_ansi(a);
                for (int r = 0; r < rows; r++) {
                    for (int c = 0; c < cols; c++) {
                        tty_target_put_cell(target, r, c, blank);
                    }
                }
                tty_target_set_cursor(target, 0, 0);
            }
            break;
        case 'K':
            tty_ansi_clear_line_mode(target, a, p0);
            break;
        case 'H':
        case 'f': {
            int target_row = (p0 > 0) ? (p0 - 1) : 0;
            int target_col = (p1 > 0) ? (p1 - 1) : 0;
            tty_target_set_cursor(target,
                                  tty_clamp(target_row, 0, rows - 1),
                                  tty_clamp(target_col, 0, cols - 1));
            break;
        }
        case 'A': {
            int n = (p0 > 0) ? p0 : 1;
            tty_target_set_cursor(target, tty_clamp(row - n, 0, rows - 1), col);
            break;
        }
        case 'B': {
            int n = (p0 > 0) ? p0 : 1;
            tty_target_set_cursor(target, tty_clamp(row + n, 0, rows - 1), col);
            break;
        }
        case 'C': {
            int n = (p0 > 0) ? p0 : 1;
            tty_target_set_cursor(target, row, tty_clamp(col + n, 0, cols - 1));
            break;
        }
        case 'D': {
            int n = (p0 > 0) ? p0 : 1;
            tty_target_set_cursor(target, row, tty_clamp(col - n, 0, cols - 1));
            break;
        }
        case 'E': {
            int n = (p0 > 0) ? p0 : 1;
            tty_target_set_cursor(target, tty_clamp(row + n, 0, rows - 1), 0);
            break;
        }
        case 'F': {
            int n = (p0 > 0) ? p0 : 1;
            tty_target_set_cursor(target, tty_clamp(row - n, 0, rows - 1), 0);
            break;
        }
        case 'G': {
            int target_col = (p0 > 0) ? (p0 - 1) : 0;
            tty_target_set_cursor(target, row, tty_clamp(target_col, 0, cols - 1));
            break;
        }
        case 's':
            tty_ansi_save_cursor(target, a);
            break;
        case 'u':
            tty_ansi_restore_cursor(target, a);
            break;
        case 'h':
        case 'l':
            if (a->private_mode && p0 == 25 && !target) {
                screen_set_cursor_visible(final == 'h');
            }
            break;
        default:
            break;
    }
}

static void tty_ansi_feed_char(tty_vc_state_t* target, tty_ansi_t* a, char ch) {
    if (a->state == TTY_ANSI_TEXT) {
        if ((uint8_t)ch == 0x1B) {
            a->utf8_codepoint = 0;
            a->utf8_remaining = 0;
            a->state = TTY_ANSI_ESC;
            return;
        }

        uint8_t uch = (uint8_t)ch;
        if (a->utf8_remaining == 0) {
            if (uch < 0x80u) {
                tty_target_write_codepoint(target, a, uch);
                return;
            }
            if ((uch & 0xE0u) == 0xC0u) {
                a->utf8_codepoint = (uint32_t)(uch & 0x1Fu);
                a->utf8_remaining = 1;
                return;
            }
            if ((uch & 0xF0u) == 0xE0u) {
                a->utf8_codepoint = (uint32_t)(uch & 0x0Fu);
                a->utf8_remaining = 2;
                return;
            }
            if ((uch & 0xF8u) == 0xF0u) {
                a->utf8_codepoint = (uint32_t)(uch & 0x07u);
                a->utf8_remaining = 3;
                return;
            }
            tty_target_write_codepoint(target, a, '?');
            return;
        }

        if ((uch & 0xC0u) != 0x80u) {
            a->utf8_codepoint = 0;
            a->utf8_remaining = 0;
            tty_target_write_codepoint(target, a, '?');
            tty_ansi_feed_char(target, a, ch);
            return;
        }

        a->utf8_codepoint = (a->utf8_codepoint << 6) | (uint32_t)(uch & 0x3Fu);
        a->utf8_remaining--;
        if (a->utf8_remaining == 0) {
            uint32_t cp = a->utf8_codepoint;
            a->utf8_codepoint = 0;
            tty_target_write_codepoint(target, a, cp);
        }
        return;
    }

    if (a->state == TTY_ANSI_ESC) {
        if (ch == '[') {
            a->state = TTY_ANSI_CSI;
            tty_ansi_reset_csi(a);
            return;
        }
        if (ch == '7') {
            tty_ansi_save_cursor(target, a);
            a->state = TTY_ANSI_TEXT;
            return;
        }
        if (ch == '8') {
            tty_ansi_restore_cursor(target, a);
            a->state = TTY_ANSI_TEXT;
            return;
        }
        if (ch == 'c') {
            tty_ansi_handle_csi_final(target, a, 'J');
            a->fg = 7;
            a->bg = 0;
            a->bold = false;
            a->faint = false;
            a->inverse = false;
            tty_ansi_apply_current_color(target, a);
            a->state = TTY_ANSI_TEXT;
            return;
        }
        // Not a CSI sequence: emit ESC + char literally.
        tty_target_write_char(target, a, (char)0x1B);
        tty_target_write_char(target, a, ch);
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

    if (ch == '?' && a->param_count == 0 && !a->have_digits) {
        a->private_mode = true;
        return;
    }

    tty_ansi_handle_csi_final(target, a, ch);
    a->state = TTY_ANSI_TEXT;
}

void tty_init(void) {
    memset(g_tty_vcs, 0, sizeof(g_tty_vcs));
    ime_init(&g_tty_ime, ime_korean_ops(), &g_tty_ime_state);
    ime_reset(&g_tty_ime);
    tty_vc_reset_state(&g_tty_vcs[0], screen_get_rows(), screen_get_cols());
    g_tty_vcs[0].foreground_pid = proc_get_foreground_pid();
    g_tty_vcs[0].keyboard_input_enabled = keyboard_input_enabled;
    g_tty_vcs[0].prompt_enabled = prompt_enabled;
    g_tty_vcs[0].shell_suspended = shell_suspended;
    g_tty_ready = true;
}

bool tty_is_ready(void) {
    return g_tty_ready;
}

uint32_t tty_get_foreground(void) {
    return proc_get_foreground_pid();
}

void tty_set_foreground(uint32_t pid) {
    proc_set_foreground_pid(pid);
}

static void tty_vc_capture(uint8_t vc) {
    if (vc >= TTY_VC_COUNT) {
        return;
    }
    tty_vc_state_t* s = &g_tty_vcs[vc];
    int rows = screen_get_rows();
    int cols = screen_get_cols();
    tty_vc_ensure_initialized(s);
    if (rows > SCREEN_MAX_ROWS) rows = SCREEN_MAX_ROWS;
    if (cols > SCREEN_MAX_COLS) cols = SCREEN_MAX_COLS;
    s->rows = rows;
    s->cols = cols;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            s->cells[r][c] = screen_get_at(c, r);
        }
    }
    s->cursor_offset = get_cursor_offset();
    s->ansi = g_tty_ansi;
    s->foreground_pid = proc_get_foreground_pid();
    s->keyboard_input_enabled = keyboard_input_enabled;
    s->prompt_enabled = prompt_enabled;
    s->shell_suspended = shell_suspended;
    s->initialized = true;
}

static void tty_vc_restore(uint8_t vc) {
    if (vc >= TTY_VC_COUNT) {
        return;
    }
    tty_vc_state_t* s = &g_tty_vcs[vc];
    int rows = screen_get_rows();
    int cols = screen_get_cols();
    if (!s->initialized) {
        tty_vc_reset_state(s, rows, cols);
        clear_screen();
        set_cursor(0, 0);
        g_tty_ansi = s->ansi;
        tty_ansi_apply_current_color(NULL, &g_tty_ansi);
        return;
    }
    tty_vc_ensure_initialized(s);
    if (rows > SCREEN_MAX_ROWS) rows = SCREEN_MAX_ROWS;
    if (cols > SCREEN_MAX_COLS) cols = SCREEN_MAX_COLS;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            uint16_t cell = (r < s->rows && c < s->cols) ? s->cells[r][c] : (((uint16_t)WHITE_ON_BLACK << 8) | ' ');
            screen_put_at(c, r, (uint8_t)(cell & 0xffu), (uint8_t)((cell >> 8) & 0xffu));
        }
    }
    g_tty_ansi = s->ansi;
    tty_ansi_apply_current_color(NULL, &g_tty_ansi);
    proc_set_foreground_pid(s->foreground_pid);
    keyboard_input_enabled = s->keyboard_input_enabled;
    prompt_enabled = s->prompt_enabled;
    shell_suspended = s->shell_suspended;
    set_cursor_offset(s->cursor_offset);
}

uint8_t tty_get_active_vc(void) {
    return g_tty_active_vc;
}

void tty_request_vc_switch(uint8_t vc) {
    (void)vc;
}

uint32_t tty_vc_get_shell_pid(uint8_t vc) {
    if (vc >= TTY_VC_COUNT) {
        return 0;
    }
    return g_tty_vcs[vc].shell_pid;
}

void tty_vc_set_shell_pid(uint8_t vc, uint32_t pid) {
    if (vc >= TTY_VC_COUNT) {
        return;
    }
    g_tty_vcs[vc].shell_pid = pid;
}

bool tty_vc_should_spawn_shell(uint8_t vc) {
    if (vc >= TTY_VC_COUNT) {
        return false;
    }
    uint32_t pid = g_tty_vcs[vc].shell_pid;
    return pid == 0 || !proc_pid_alive(pid);
}

void tty_reset_input_state(void) {
    uintptr_t flags = tty_irq_save();
    g_tty_line_build_len = 0;
    memset(g_tty_line_build, 0, sizeof(g_tty_line_build));
    g_tty_line_ready_len = 0;
    g_tty_line_ready_off = 0;
    memset(g_tty_line_ready, 0, sizeof(g_tty_line_ready));
    g_tty_cooked_last_cells = 0;
    ime_reset(&g_tty_ime);
    tty_irq_restore(flags);
    keyboard_flush();
}

static void tty_apply_pending_vc_switch(void) {
    uint8_t target = g_tty_switch_req;
    if (target >= TTY_VC_COUNT || target == g_tty_active_vc) {
        return;
    }
    tty_vc_capture(g_tty_active_vc);
    g_tty_active_vc = target;
    g_tty_switch_req = 0xffu;
    tty_vc_restore(target);
    keyboard_flush();
}

int tty_read_stdin(void* buf, uint32_t len) {
    if (!buf || len == 0) {
        return 0;
    }

    while (proc_current_pid() != tty_get_foreground() ||
           proc_current_vc() != g_tty_active_vc) {
        hal_enable_interrupts();
        hal_halt();
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
    uintptr_t flags = tty_irq_save();
    tty_vc_state_t* target = NULL;
    tty_ansi_t* ansi = &g_tty_ansi;
    if (proc_current()) {
        uint8_t vc = proc_current_vc();
        if (vc < TTY_VC_COUNT && vc != g_tty_active_vc) {
            target = &g_tty_vcs[vc];
            tty_vc_ensure_initialized(target);
            ansi = &target->ansi;
        }
    }
    for (uint32_t i = 0; i < len; i++) {
        tty_ansi_feed_char(target, ansi, s[i]);
    }
    tty_irq_restore(flags);
    return (int)len;
}

int tty_write_kernel(const void* buf, uint32_t len) {
    if (!buf || len == 0) {
        return 0;
    }
    const char* s = (const char*)buf;
    uintptr_t flags = tty_irq_save();
    tty_vc_ensure_initialized(&g_tty_vcs[g_tty_active_vc]);
    for (uint32_t i = 0; i < len; i++) {
        tty_ansi_feed_char(NULL, &g_tty_ansi, s[i]);
    }
    tty_vc_capture(g_tty_active_vc);
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
    tty_apply_pending_vc_switch();
    console_cursor_blink();
}
