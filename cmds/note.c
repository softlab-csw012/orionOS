#include "syscall.h"
#include "string.h"
#include <stdbool.h>
#include <stdint.h>

#define NOTE_KEY_LEFT  0x90
#define NOTE_KEY_RIGHT 0x91
#define NOTE_KEY_UP    0x92
#define NOTE_KEY_DOWN  0x93

#define NOTE_MAX_LINES 256
#define NOTE_MAX_COLS  80
#define NOTE_TEXT_ROWS 24
#define NOTE_TAB_WIDTH 4

static char* g_buf[NOTE_MAX_LINES];
static int g_lines = 1;
static int g_cx = 0;
static int g_cy = 0;
static int g_scroll = 0;
static bool g_command_mode = false;
static char g_filename[128];

static int g_in_fd = -1;
static int g_out_fd = -1;

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

static int in_read_char(void) {
    if (g_in_fd < 0) {
        g_in_fd = sys_open("/dev/stdin", 0);
        if (g_in_fd < 0) g_in_fd = sys_open("stdin", 0);
    }
    if (g_in_fd >= 0) {
        char ch = 0;
        int rc = sys_read(g_in_fd, &ch, 1);
        if (rc == 1) return (uint8_t)ch;
        if (rc < 0) g_in_fd = -1;
    }
    return (int)sys_getkey();
}

static int line_len(const char* s) {
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
    int len = line_len(g_buf[g_cy]);
    if (g_cx > len) g_cx = len;

    int vis_col = col_from_index(g_buf[g_cy], g_cx);
    if (vis_col >= NOTE_MAX_COLS) {
        g_cx = index_from_col(g_buf[g_cy], NOTE_MAX_COLS - 1);
    }
}

static int note_cols(void) {
    return NOTE_MAX_COLS;
}

static int note_text_rows(void) {
    return NOTE_TEXT_ROWS;
}

static void draw_buffer(void) {
    sys_clear_screen();
    sys_cursor_visible(1);

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
            for (int i = 0; i < NOTE_MAX_COLS - 1 && g_buf[src][i] && col < cols; i++) {
                if ((uint8_t)g_buf[src][i] == '\t') {
                    int spaces = NOTE_TAB_WIDTH - (col % NOTE_TAB_WIDTH);
                    while (spaces-- > 0 && col < cols) out[col++] = ' ';
                } else {
                    out[col++] = visible_char(g_buf[src][i]);
                }
            }
        }
        while (col < cols) out[col++] = ' ';
        out[col] = '\0';
        out_write(out);
        out_write("\n");
    }

    int vis_col = col_from_index(g_buf[g_cy], g_cx);
    char status[NOTE_MAX_COLS + 1];
    int slen = snprintf(status, sizeof(status), "[%s] line %d/%d col %d  %s",
             g_filename, g_cy + 1, g_lines, vis_col + 1,
             g_command_mode ? "CMD" : "INS");
    if (slen < 0) slen = 0;
    if (slen > cols) slen = cols;
    while (slen < cols) status[slen++] = ' ';
    status[slen] = '\0';
    out_write(status);
    out_write("\n");

    int scr_y = g_cy - g_scroll;
    if (scr_y < 0) scr_y = 0;
    if (scr_y >= text_rows) scr_y = text_rows - 1;
    int scr_x = col_from_index(g_buf[g_cy], g_cx);
    if (scr_x < 0) scr_x = 0;
    if (scr_x >= cols) scr_x = cols - 1;
    sys_set_cursor_offset((uint32_t)(scr_y * cols + scr_x) * 2u);
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
}

static bool save_file(const char* fname) {
    int total = 0;
    for (int i = 0; i < g_lines; i++) total += line_len(g_buf[i]);
    if (g_lines > 1) total += (g_lines - 1);

    char* out = (char*)malloc((size_t)total + 1u);
    if (!out) return false;
    int p = 0;
    for (int i = 0; i < g_lines; i++) {
        int n = line_len(g_buf[i]);
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
        if (k < 32 || k >= 127) continue;
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

    if (argc > 1 && argv && argv[1] && argv[1][0]) {
        strncpy(g_filename, argv[1], sizeof(g_filename) - 1);
        g_filename[sizeof(g_filename) - 1] = '\0';
    } else {
        if (prompt_file(g_filename, sizeof(g_filename)) <= 0) {
            out_write("note: no file\n");
            free_lines();
            return 1;
        }
    }

    load_file(g_filename);
    draw_buffer();

    while (1) {
        int k = in_read_char();

        if (g_command_mode) {
            if (k == 's') {
                if (save_file(g_filename)) out_write("file saved\n");
                else out_write("save failed\n");
                break;
            } else if (k == 'x') {
                out_write("cancel saving file\n");
                break;
            } else if (k == 'i') {
                g_command_mode = false;
            }
            draw_buffer();
            continue;
        }

        if (k == 27) {
            g_command_mode = true;
        } else if (k == '\r' || k == '\n') {
            if (g_lines < NOTE_MAX_LINES - 1) {
                for (int r = g_lines; r > g_cy + 1; r--) strcpy(g_buf[r], g_buf[r - 1]);
                strcpy(g_buf[g_cy + 1], &g_buf[g_cy][g_cx]);
                memset(&g_buf[g_cy][g_cx], 0, (size_t)(NOTE_MAX_COLS - g_cx));
                g_lines++;
                g_cy++;
                g_cx = 0;
                if (g_cy - g_scroll >= note_text_rows()) g_scroll++;
            }
        } else if (k == '\b' || k == 0x7f) {
            if (g_cx > 0) {
                int len = line_len(g_buf[g_cy]);
                memmove(&g_buf[g_cy][g_cx - 1], &g_buf[g_cy][g_cx], (size_t)(len - g_cx + 1));
                g_cx--;
            } else if (g_cy > 0) {
                int prev_len = line_len(g_buf[g_cy - 1]);
                int cur_len = line_len(g_buf[g_cy]);
                if (prev_len + cur_len < NOTE_MAX_COLS) {
                    strcat(g_buf[g_cy - 1], g_buf[g_cy]);
                    for (int r = g_cy; r < g_lines - 1; r++) strcpy(g_buf[r], g_buf[r + 1]);
                    g_lines--;
                    g_cy--;
                    g_cx = prev_len;
                }
            }
        } else if (k == NOTE_KEY_LEFT) {
            if (g_cx > 0) g_cx--;
            else if (g_cy > 0) {
                g_cy--;
                g_cx = line_len(g_buf[g_cy]);
                if (g_cx >= NOTE_MAX_COLS) g_cx = NOTE_MAX_COLS - 1;
                if (g_cy < g_scroll) g_scroll--;
            }
        } else if (k == NOTE_KEY_RIGHT) {
            int len = line_len(g_buf[g_cy]);
            if (g_cx < len) g_cx++;
            else if (g_cy + 1 < g_lines) {
                g_cy++;
                g_cx = 0;
                if (g_cy - g_scroll >= note_text_rows()) g_scroll++;
            }
        } else if (k == NOTE_KEY_UP) {
            if (g_cy > 0) {
                int target = col_from_index(g_buf[g_cy], g_cx);
                g_cy--;
                g_cx = index_from_col(g_buf[g_cy], target);
                if (g_cy < g_scroll) g_scroll--;
            }
        } else if (k == NOTE_KEY_DOWN) {
            if (g_cy + 1 < g_lines) {
                int target = col_from_index(g_buf[g_cy], g_cx);
                g_cy++;
                g_cx = index_from_col(g_buf[g_cy], target);
                if (g_cy - g_scroll >= note_text_rows()) g_scroll++;
            }
        } else if (k >= 32 && k <= 126) {
            int len = line_len(g_buf[g_cy]);
            if (g_cx > len) g_cx = len;
            if (len < NOTE_MAX_COLS - 1) {
                memmove(&g_buf[g_cy][g_cx + 1], &g_buf[g_cy][g_cx], (size_t)(len - g_cx + 1));
                g_buf[g_cy][g_cx++] = (char)k;
            }
        }

        clamp_cursor();
        draw_buffer();
    }

    free_lines();
    return 0;
}

void _start(void) {
    uint32_t* sp = NULL;
    __asm__ volatile("mov %%esp, %0" : "=r"(sp));
    int argc = 0;
    char** argv = NULL;
    if (sp) {
        argc = (int)sp[0];
        argv = (char**)sp[1];
    }
    int rc = main(argc, argv);
    sys_exit((uint32_t)rc);
}
