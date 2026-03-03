#include "syscall.h"
#include "string.h"
#include <stdbool.h>
#include <stdint.h>

#define NOTE_KEY_LEFT  0x90
#define NOTE_KEY_RIGHT 0x91
#define NOTE_KEY_UP    0x92
#define NOTE_KEY_DOWN  0x93

#define NOTE_MAX_LINES 256
#define NOTE_MAX_COLS  256
#define NOTE_TAB_WIDTH 4

static char* g_buf[NOTE_MAX_LINES];
static int g_lines = 1;
static int g_cx = 0;
static int g_cy = 0;
static int g_scroll = 0;
static bool g_command_mode = false;
static bool g_dirty = false;
static bool g_quit_confirm = false;
static char g_filename[128];

static int g_out_fd = -1;
static bool g_screen_inited = false;
static int g_screen_cols = 80;
static int g_screen_rows = 25;
static int g_ime_shadow_len = 0;
static int g_ime_shadow_cells = 0;
static char g_ime_shadow[8];
static bool g_pending_key_valid = false;
static sys_key_event_t g_pending_key;

static int note_line_len(const char* s);

static void out_write_len(const char* s, int len) {
    if (!s || len <= 0) return;
    if (g_out_fd < 0) {
        g_out_fd = sys_open("/dev/stdout", 0);
        if (g_out_fd < 0) g_out_fd = sys_open("stdout", 0);
    }
    if (g_out_fd >= 0) {
        int rc = sys_write(g_out_fd, s, (uint32_t)len);
        if (rc >= 0) return;
        g_out_fd = -1;
    }

    char tmp[128];
    int off = 0;
    while (off < len) {
        int n = len - off;
        if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1;
        memcpy(tmp, s + off, (size_t)n);
        tmp[n] = '\0';
        sys_kprint(tmp);
        off += n;
    }
}

static void out_write(const char* s) {
    if (!s) return;
    out_write_len(s, strlen(s));
}

static void ansi_move_cursor(int row, int col) {
    char seq[24];
    int n = snprintf(seq, sizeof(seq), "\x1b[%d;%dH", row, col);
    if (n > 0) out_write_len(seq, n);
}

static void ansi_clear_screen(void) {
    out_write("\x1b[2J");
}

static void ansi_clear_line(void) {
    out_write("\x1b[2K");
}

static void ansi_reset_style(void) {
    out_write("\x1b[0m");
}

static void ansi_status_style(void) {
    out_write("\x1b[7m");
}

static int in_read_char(void) {
    return (int)sys_getkey();
}

static int in_read_key_event(sys_key_event_t* event) {
    if (!event) {
        return 0;
    }
    if (g_pending_key_valid) {
        *event = g_pending_key;
        g_pending_key_valid = false;
        return 1;
    }
    return sys_getkey_event(event);
}

static bool note_same_key_event(const sys_key_event_t* a, const sys_key_event_t* b) {
    if (!a || !b) {
        return false;
    }
    return a->code == b->code &&
           a->modifiers == b->modifiers &&
           a->toggles == b->toggles;
}

static void note_coalesce_key_event(sys_key_event_t* event) {
    sys_key_event_t extra;

    if (!event) {
        return;
    }
    while (sys_getkey_event_nb(&extra) > 0) {
        if (note_same_key_event(event, &extra)) {
            continue;
        }
        g_pending_key = extra;
        g_pending_key_valid = true;
        break;
    }
}

static int utf8_encode(uint32_t cp, char* out, int cap) {
    if (!out || cap <= 0) return 0;
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

static int note_insert_bytes(const char* data, int len) {
    int line_len_now = note_line_len(g_buf[g_cy]);
    if (!data || len <= 0) return 0;
    if (g_cx > line_len_now) g_cx = line_len_now;
    if (line_len_now + len >= NOTE_MAX_COLS) return 0;
    memmove(&g_buf[g_cy][g_cx + len], &g_buf[g_cy][g_cx], (size_t)(line_len_now - g_cx + 1));
    memcpy(&g_buf[g_cy][g_cx], data, (size_t)len);
    g_cx += len;
    g_dirty = true;
    return 1;
}

static void note_delete_last_bytes(void) {
    int start;
    int len;

    if (g_cx <= 0) {
        return;
    }
    start = g_cx - 1;
    while (start > 0 && (((uint8_t)g_buf[g_cy][start]) & 0xC0u) == 0x80u) {
        start--;
    }
    len = note_line_len(g_buf[g_cy]);
    memmove(&g_buf[g_cy][start], &g_buf[g_cy][g_cx], (size_t)(len - g_cx + 1));
    g_cx = start;
    g_dirty = true;
}

static void note_ime_clear_shadow(void) {
    if (g_ime_shadow_len <= 0) {
        return;
    }
    memset(g_ime_shadow, 0, sizeof(g_ime_shadow));
    g_ime_shadow_len = 0;
    g_ime_shadow_cells = 0;
}

static void note_ime_process_result(const sys_ime_kernel_res_t* res) {
    if (!res) {
        return;
    }
    for (uint32_t i = 0; i < res->commit_count; i++) {
        char utf8[4];
        int len = utf8_encode(res->commit[i], utf8, (int)sizeof(utf8));
        if (len > 0) {
            (void)note_insert_bytes(utf8, len);
        }
    }
    g_ime_shadow_len = 0;
    g_ime_shadow_cells = 0;
    if (res->has_preedit && res->preedit != 0) {
        char utf8[4];
        int len = utf8_encode(res->preedit, utf8, (int)sizeof(utf8));
        if (len > 0 && len < (int)sizeof(g_ime_shadow)) {
            memcpy(g_ime_shadow, utf8, (size_t)len);
            g_ime_shadow_len = len;
            g_ime_shadow_cells = (res->preedit <= 0x7Fu) ? len : 2;
        }
    }
}

static void note_ime_send(uint32_t type, uint32_t codepoint, uint32_t modifiers) {
    sys_ime_kernel_req_t req;
    sys_ime_kernel_res_t res;

    note_ime_clear_shadow();
    req.type = type;
    req.codepoint = codepoint;
    req.modifiers = modifiers;
    if (sys_ime_process(&req, &res) > 0) {
        note_ime_process_result(&res);
    } else {
        g_ime_shadow_len = 0;
        g_ime_shadow_cells = 0;
    }
}

static void note_ime_reset(void) {
    note_ime_send(SYS_IME_KEY_RESET, 0, 0);
}

static void note_ime_flush(void) {
    if (g_ime_shadow_len <= 0) {
        return;
    }
    note_ime_send(SYS_IME_KEY_ENTER, 0, 0);
}

static int note_line_len(const char* s) {
    int n = 0;
    while (n < NOTE_MAX_COLS - 1 && s && s[n]) n++;
    return n;
}

static int col_from_index(const char* s, int idx) {
    int col = 0;
    int i = 0;
    while (s && s[i] && i < idx && i < NOTE_MAX_COLS - 1) {
        if ((uint8_t)s[i] == '\t') {
            int step = NOTE_TAB_WIDTH - (col % NOTE_TAB_WIDTH);
            col += step;
        } else {
            col++;
        }
        i++;
    }
    return col;
}

static int index_from_col(const char* s, int target_col) {
    int col = 0;
    int i = 0;
    while (s && s[i] && i < NOTE_MAX_COLS - 1) {
        int step = 1;
        if ((uint8_t)s[i] == '\t') {
            step = NOTE_TAB_WIDTH - (col % NOTE_TAB_WIDTH);
        }
        if (col + step > target_col) break;
        col += step;
        i++;
    }
    return i;
}

static char visible_char(char ch) {
    uint8_t u = (uint8_t)ch;
    if (u == '\t') return ' ';
    if (u < 32 || u == 127) return '.';
    return ch;
}

static void clamp_cursor(void) {
    if (g_lines < 1) g_lines = 1;
    if (g_cy < 0) g_cy = 0;
    if (g_cy >= g_lines) g_cy = g_lines - 1;
    if (g_cx < 0) g_cx = 0;
    int len = note_line_len(g_buf[g_cy]);
    if (g_cx > len) g_cx = len;

    int vis_col = col_from_index(g_buf[g_cy], g_cx);
    if (vis_col >= NOTE_MAX_COLS) {
        g_cx = index_from_col(g_buf[g_cy], NOTE_MAX_COLS - 1);
    }
}

static int note_cols(void) {
    return g_screen_cols;
}

static int note_text_rows(void) {
    return g_screen_rows - 1;
}

static void note_refresh_screen_size(void) {
    sys_fb_info_t fb;
    if (sys_fb_info(&fb) > 0 && fb.font_w > 0 && fb.font_h > 0) {
        int cols = (int)(fb.width / fb.font_w);
        int rows = (int)(fb.height / fb.font_h);
        if (cols > 0) g_screen_cols = cols;
        if (rows > 1) g_screen_rows = rows;
    }
    if (g_screen_cols < 20) g_screen_cols = 20;
    if (g_screen_cols > NOTE_MAX_COLS) g_screen_cols = NOTE_MAX_COLS;
    if (g_screen_rows < 2) g_screen_rows = 2;
}

static void draw_buffer(void) {
    sys_cursor_visible(1);
    if (!g_screen_inited) {
        note_refresh_screen_size();
        ansi_clear_screen();
        g_screen_inited = true;
    }

    int cols = note_cols();
    int text_rows = note_text_rows();
    if (g_scroll < 0) g_scroll = 0;
    if (g_scroll > g_lines - text_rows) {
        g_scroll = (g_lines > text_rows) ? (g_lines - text_rows) : 0;
    }

    for (int r = 0; r < text_rows; r++) {
        int src = r + g_scroll;
        char out[NOTE_MAX_COLS + 1];
        int col = 0;
        if (src < g_lines) {
            int line_len = note_line_len(g_buf[src]);
            for (int i = 0; i < line_len && col < cols; i++) {
                if (src == g_cy && i == g_cx && g_ime_shadow_len > 0) {
                    for (int j = 0; j < g_ime_shadow_len && col < cols; j++) {
                        out[col++] = g_ime_shadow[j];
                    }
                }
                if ((uint8_t)g_buf[src][i] == '\t') {
                    int spaces = NOTE_TAB_WIDTH - (col % NOTE_TAB_WIDTH);
                    while (spaces-- > 0 && col < cols) out[col++] = ' ';
                } else {
                    out[col++] = visible_char(g_buf[src][i]);
                }
            }
            if (src == g_cy && g_cx >= line_len && g_ime_shadow_len > 0) {
                for (int j = 0; j < g_ime_shadow_len && col < cols; j++) {
                    out[col++] = g_ime_shadow[j];
                }
            }
        }
        while (col < cols) out[col++] = ' ';
        out[col] = '\0';
        ansi_move_cursor(r + 1, 1);
        ansi_clear_line();
        out_write(out);
    }

    int vis_col = col_from_index(g_buf[g_cy], g_cx);
    char status[NOTE_MAX_COLS + 1];
    int slen = snprintf(status, sizeof(status), "[%s%s] line %d/%d col %d  %s",
             g_filename, g_dirty ? "*" : "", g_cy + 1, g_lines, vis_col + 1,
             g_command_mode ? "CMD w:save x:save+quit q:quit i:ins" : "INS esc:cmd");
    if (slen < 0) slen = 0;
    if (slen > cols) slen = cols;
    while (slen < cols) status[slen++] = ' ';
    status[slen] = '\0';
    ansi_move_cursor(text_rows + 1, 1);
    ansi_clear_line();
    ansi_status_style();
    out_write(status);
    ansi_reset_style();

    int scr_y = g_cy - g_scroll;
    if (scr_y < 0) scr_y = 0;
    if (scr_y >= text_rows) scr_y = text_rows - 1;
    int scr_x = col_from_index(g_buf[g_cy], g_cx);
    if (g_ime_shadow_len > 0) {
        scr_x += g_ime_shadow_cells;
    }
    if (scr_x < 0) scr_x = 0;
    if (scr_x >= cols) scr_x = cols - 1;
    ansi_move_cursor(scr_y + 1, scr_x + 1);
}

static bool init_lines(void) {
    for (int i = 0; i < NOTE_MAX_LINES; i++) {
        g_buf[i] = (char*)malloc(NOTE_MAX_COLS);
        if (!g_buf[i]) return false;
        memset(g_buf[i], 0, NOTE_MAX_COLS);
    }
    g_lines = 1;
    g_cx = 0;
    g_cy = 0;
    g_scroll = 0;
    g_command_mode = false;
    g_dirty = false;
    g_quit_confirm = false;
    g_ime_shadow_len = 0;
    g_ime_shadow_cells = 0;
    memset(g_ime_shadow, 0, sizeof(g_ime_shadow));
    g_pending_key_valid = false;
    memset(&g_pending_key, 0, sizeof(g_pending_key));
    note_refresh_screen_size();
    return true;
}

static void free_lines(void) {
    for (int i = 0; i < NOTE_MAX_LINES; i++) {
        if (g_buf[i]) free(g_buf[i]);
        g_buf[i] = NULL;
    }
}

static void load_file(const char* fname) {
    int fd = sys_open(fname, 0);
    if (fd < 0) return;

    int cap = 8192;
    int len = 0;
    char* in = (char*)malloc((size_t)cap);
    if (!in) {
        sys_close(fd);
        return;
    }

    for (;;) {
        if (len + 256 > cap) {
            int ncap = cap * 2;
            char* ni = (char*)realloc(in, (size_t)ncap);
            if (!ni) break;
            in = ni;
            cap = ncap;
        }
        int n = sys_read(fd, in + len, 256);
        if (n <= 0) break;
        len += n;
    }
    sys_close(fd);

    int pos = 0;
    g_lines = 0;
    while (pos < len && g_lines < NOTE_MAX_LINES) {
        int c = 0;
        while (pos < len && in[pos] != '\n' && in[pos] != '\r' && in[pos] != '\0' &&
               c < NOTE_MAX_COLS - 1) {
            g_buf[g_lines][c++] = in[pos++];
        }
        g_buf[g_lines][c] = '\0';
        if (pos < len) {
            if (in[pos] == '\r' && pos + 1 < len && in[pos + 1] == '\n') pos += 2;
            else pos += 1;
        }
        g_lines++;
    }
    if (g_lines == 0) g_lines = 1;
    free(in);
    g_dirty = false;
    g_quit_confirm = false;
}

static bool save_file(const char* fname) {
    int total = 0;
    for (int i = 0; i < g_lines; i++) total += note_line_len(g_buf[i]);
    if (g_lines > 1) total += (g_lines - 1);

    char* out = (char*)malloc((size_t)total + 1u);
    if (!out) return false;
    int p = 0;
    for (int i = 0; i < g_lines; i++) {
        int n = note_line_len(g_buf[i]);
        if (n > 0) memcpy(out + p, g_buf[i], (size_t)n);
        p += n;
        if (i + 1 < g_lines) out[p++] = '\n';
    }
    out[p] = '\0';

    int fd = sys_open(fname, SYS_OPEN_FLAG_CREATE);
    if (fd < 0) {
        free(out);
        return false;
    }
    bool ok = (sys_write(fd, out, (uint32_t)p) >= 0);
    sys_close(fd);
    free(out);
    return ok;
}

static int prompt_file(char* out, int cap) {
    int len = 0;
    out_write("file path: ");
    while (1) {
        int k = in_read_char();
        if (k == '\r' || k == '\n') {
            out_write("\n");
            break;
        }
        if (k == '\b' || k == 0x7f) {
            if (len > 0) {
                len--;
                out_write("\b \b");
            }
            continue;
        }
        if (k < 32 || k == 127) continue;
        if (len < cap - 1) {
            out[len++] = (char)k;
            char ch = (char)k;
            out_write_len(&ch, 1);
        }
    }
    out[len] = '\0';
    return len;
}

int main(int argc, char** argv) {
    if (!init_lines()) {
        out_write("note: out of memory\n");
        return 1;
    }

    (void)sys_keyboard_input_mode(0);

    if (argc > 1 && argv && argv[1] && argv[1][0]) {
        strncpy(g_filename, argv[1], sizeof(g_filename) - 1);
        g_filename[sizeof(g_filename) - 1] = '\0';
    } else {
        if (prompt_file(g_filename, sizeof(g_filename)) <= 0) {
            out_write("note: no file\n");
            (void)sys_keyboard_input_mode(1);
            free_lines();
            return 1;
        }
    }

    load_file(g_filename);
    draw_buffer();

    while (1) {
        sys_key_event_t ev;
        int k;
        bool ime_mode;
        uint32_t ime_mods = 0;

        if (!in_read_key_event(&ev)) {
            continue;
        }
        note_coalesce_key_event(&ev);
        k = ev.code;
        ime_mode = (ev.toggles & SYS_KEY_TOGGLE_KOREAN) != 0 || g_ime_shadow_len > 0;
        if (ev.modifiers & SYS_KEY_MOD_SHIFT) ime_mods |= SYS_IME_MOD_SHIFT;
        if (ev.modifiers & SYS_KEY_MOD_CTRL) ime_mods |= SYS_IME_MOD_CTRL;
        if (ev.modifiers & SYS_KEY_MOD_ALT) ime_mods |= SYS_IME_MOD_ALT;

        if (g_command_mode) {
            if (k == 'w') {
                if (save_file(g_filename)) {
                    g_dirty = false;
                    g_quit_confirm = false;
                } else {
                    out_write("save failed\n");
                }
            } else if (k == 'x') {
                if (save_file(g_filename)) {
                    g_dirty = false;
                    break;
                }
                out_write("save failed\n");
            } else if (k == 'q') {
                break;
            } else if (k == 'i') {
                note_ime_reset();
                g_command_mode = false;
                g_quit_confirm = false;
            }
            draw_buffer();
            continue;
        }

        if (k == 27) {
            note_ime_flush();
            g_command_mode = true;
            g_quit_confirm = false;
        } else if (ime_mode && (k == '\r' || k == '\n')) {
            note_ime_send(SYS_IME_KEY_ENTER, 0, 0);
            if (g_ime_shadow_len > 0) {
                clamp_cursor();
                draw_buffer();
                continue;
            }
            if (g_lines < NOTE_MAX_LINES - 1) {
                for (int r = g_lines; r > g_cy + 1; r--) strcpy(g_buf[r], g_buf[r - 1]);
                strcpy(g_buf[g_cy + 1], &g_buf[g_cy][g_cx]);
                memset(&g_buf[g_cy][g_cx], 0, (size_t)(NOTE_MAX_COLS - g_cx));
                g_lines++;
                g_cy++;
                g_cx = 0;
                g_dirty = true;
                if (g_cy - g_scroll >= note_text_rows()) g_scroll++;
            }
        } else if (k == '\r' || k == '\n') {
            if (g_lines < NOTE_MAX_LINES - 1) {
                for (int r = g_lines; r > g_cy + 1; r--) strcpy(g_buf[r], g_buf[r - 1]);
                strcpy(g_buf[g_cy + 1], &g_buf[g_cy][g_cx]);
                memset(&g_buf[g_cy][g_cx], 0, (size_t)(NOTE_MAX_COLS - g_cx));
                g_lines++;
                g_cy++;
                g_cx = 0;
                g_dirty = true;
                if (g_cy - g_scroll >= note_text_rows()) g_scroll++;
            }
        } else if (ime_mode && (k == '\b' || k == 0x7f)) {
            note_ime_send(SYS_IME_KEY_BACKSPACE, 0, 0);
            if (g_ime_shadow_len > 0) {
                clamp_cursor();
                draw_buffer();
                continue;
            }
            if (g_cx > 0) {
                note_delete_last_bytes();
            } else if (g_cy > 0) {
                int prev_len = note_line_len(g_buf[g_cy - 1]);
                int cur_len = note_line_len(g_buf[g_cy]);
                if (prev_len + cur_len < NOTE_MAX_COLS) {
                    strcat(g_buf[g_cy - 1], g_buf[g_cy]);
                    for (int r = g_cy; r < g_lines - 1; r++) strcpy(g_buf[r], g_buf[r + 1]);
                    g_lines--;
                    g_cy--;
                    g_cx = prev_len;
                    g_dirty = true;
                }
            }
        } else if (k == '\b' || k == 0x7f) {
            if (g_cx > 0) {
                note_delete_last_bytes();
            } else if (g_cy > 0) {
                int prev_len = note_line_len(g_buf[g_cy - 1]);
                int cur_len = note_line_len(g_buf[g_cy]);
                if (prev_len + cur_len < NOTE_MAX_COLS) {
                    strcat(g_buf[g_cy - 1], g_buf[g_cy]);
                    for (int r = g_cy; r < g_lines - 1; r++) strcpy(g_buf[r], g_buf[r + 1]);
                    g_lines--;
                    g_cy--;
                    g_cx = prev_len;
                    g_dirty = true;
                }
            }
        } else if (k == NOTE_KEY_LEFT) {
            note_ime_flush();
            if (g_cx > 0) g_cx--;
            else if (g_cy > 0) {
                g_cy--;
                g_cx = note_line_len(g_buf[g_cy]);
                if (g_cx >= NOTE_MAX_COLS) g_cx = NOTE_MAX_COLS - 1;
                if (g_cy < g_scroll) g_scroll--;
            }
        } else if (k == NOTE_KEY_RIGHT) {
            note_ime_flush();
            int len = note_line_len(g_buf[g_cy]);
            if (g_cx < len) g_cx++;
            else if (g_cy + 1 < g_lines) {
                g_cy++;
                g_cx = 0;
                if (g_cy - g_scroll >= note_text_rows()) g_scroll++;
            }
        } else if (k == NOTE_KEY_UP) {
            note_ime_flush();
            if (g_cy > 0) {
                int target = col_from_index(g_buf[g_cy], g_cx);
                g_cy--;
                g_cx = index_from_col(g_buf[g_cy], target);
                if (g_cy < g_scroll) g_scroll--;
            }
        } else if (k == NOTE_KEY_DOWN) {
            note_ime_flush();
            if (g_cy + 1 < g_lines) {
                int target = col_from_index(g_buf[g_cy], g_cx);
                g_cy++;
                g_cx = index_from_col(g_buf[g_cy], target);
                if (g_cy - g_scroll >= note_text_rows()) g_scroll++;
            }
        } else if (ime_mode && k >= 32 && k != 127) {
            note_ime_send(SYS_IME_KEY_CHAR, (uint32_t)(uint8_t)k, ime_mods);
        } else if (k >= 32 && k != 127) {
            char ch = (char)k;
            (void)note_insert_bytes(&ch, 1);
        }

        clamp_cursor();
        draw_buffer();
    }

    ansi_reset_style();
    ansi_move_cursor(note_text_rows() + 2, 1);
    note_ime_reset();
    (void)sys_keyboard_input_mode(1);
    free_lines();
    return 0;
}

void _start(int argc, char** argv) {
    int rc = main(argc, argv);
    sys_exit((uint32_t)rc);
}
