#include "syscall.h"
#include "string.h"
#include <stdint.h>
#include <stdbool.h>

#define NOTE_KEY_LEFT  0x90
#define NOTE_KEY_RIGHT 0x91
#define NOTE_KEY_UP    0x92
#define NOTE_KEY_DOWN  0x93

#define EDITOR_BUF_MAX 4096
#define STATUS_MAX 96
#define SAVE_AS_MAX 128
#define CTRL_SHIFT_MASK 0xA0
#define CTRL_SHIFT_S (CTRL_SHIFT_MASK | 0x13)

static char g_buf[EDITOR_BUF_MAX];
static int g_len = 0;
static int g_cursor = 0;
static int g_view_start = 0;
static int g_desired_col = -1;
static bool g_dirty = false;
static char g_path[128] = "/home/note.txt";
static char g_save_as[SAVE_AS_MAX];
static int g_save_len = 0;
static bool g_save_mode = false;

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int buffer_insert(int pos, char ch) {
    if (g_len >= EDITOR_BUF_MAX - 1) return 0;
    pos = clamp_int(pos, 0, g_len);
    for (int i = g_len; i > pos; i--) {
        g_buf[i] = g_buf[i - 1];
    }
    g_buf[pos] = ch;
    g_len++;
    g_buf[g_len] = '\0';
    return 1;
}

static int buffer_delete(int pos) {
    if (pos <= 0 || pos > g_len) return 0;
    for (int i = pos - 1; i < g_len - 1; i++) {
        g_buf[i] = g_buf[i + 1];
    }
    g_len--;
    g_buf[g_len] = '\0';
    return 1;
}

static int line_start(int pos) {
    if (pos < 0) pos = 0;
    if (pos > g_len) pos = g_len;
    while (pos > 0 && g_buf[pos - 1] != '\n') {
        pos--;
    }
    return pos;
}

static int line_end(int pos) {
    if (pos < 0) pos = 0;
    if (pos > g_len) pos = g_len;
    while (pos < g_len && g_buf[pos] != '\n') {
        pos++;
    }
    return pos;
}

static int line_col(int pos) {
    int start = line_start(pos);
    return pos - start;
}

static void move_left(void) {
    if (g_cursor > 0) g_cursor--;
    g_desired_col = -1;
}

static void move_right(void) {
    if (g_cursor < g_len) g_cursor++;
    g_desired_col = -1;
}

static void move_up(void) {
    int cur_start = line_start(g_cursor);
    if (cur_start == 0) return;
    if (g_desired_col < 0) g_desired_col = line_col(g_cursor);
    int prev_end = cur_start - 1;
    int prev_start = line_start(prev_end);
    int prev_len = prev_end - prev_start;
    int col = g_desired_col;
    if (col > prev_len) col = prev_len;
    g_cursor = prev_start + col;
}

static void move_down(void) {
    int cur_end = line_end(g_cursor);
    if (cur_end >= g_len) return;
    if (g_desired_col < 0) g_desired_col = line_col(g_cursor);
    int next_start = cur_end + 1;
    int next_end = line_end(next_start);
    int next_len = next_end - next_start;
    int col = g_desired_col;
    if (col > next_len) col = next_len;
    g_cursor = next_start + col;
}

static void ensure_view(int max_body) {
    if (max_body <= 0) {
        g_view_start = 0;
        return;
    }
    if (g_view_start > g_cursor) {
        g_view_start = g_cursor;
    } else if (g_cursor >= g_view_start + max_body) {
        g_view_start = g_cursor - max_body + 1;
    }
    if (g_view_start < 0) g_view_start = 0;
    if (g_view_start > g_len) g_view_start = g_len;
}

static void build_text(char* out, int out_size, int max_body, const char* status) {
    int len = 0;
    out[0] = '\0';

    int line = snprintf(out, out_size, "File: %s%s\n", g_path, g_dirty ? " *" : "");
    if (line < 0) return;
    len += line;
    if (len >= out_size - 1) return;

    line = snprintf(out + len, out_size - len, "Ctrl+S: Save  Ctrl+Q: Quit\n");
    if (line < 0) return;
    len += line;
    if (len >= out_size - 1) return;

    if (g_save_mode) {
        line = snprintf(out + len, out_size - len, "Save as: %s\n", g_save_as);
    } else if (status && *status) {
        line = snprintf(out + len, out_size - len, "%s\n", status);
    } else {
        int ln = 1;
        int col = 1;
        for (int i = 0; i < g_cursor && i < g_len; i++) {
            if (g_buf[i] == '\n') {
                ln++;
                col = 1;
            } else {
                col++;
            }
        }
        line = snprintf(out + len, out_size - len, "Ln %d, Col %d\n", ln, col);
    }
    if (line < 0) return;
    len += line;
    if (len >= out_size - 1) return;

    int start = g_view_start;
    if (start < 0) start = 0;
    if (start > g_len) start = g_len;
    int end = start + max_body;
    if (end > g_len) end = g_len;

    int i = start;
    while (i < end && len < out_size - 1) {
        if (i == g_cursor) {
            out[len++] = '|';
            if (len >= out_size - 1) break;
        }
        out[len++] = g_buf[i++];
    }
    if (i == g_cursor && len < out_size - 1) {
        out[len++] = '|';
    }
    out[len] = '\0';
}

static int file_load(const char* path) {
    int fd = sys_open(path, 0);
    if (fd < 0) return -1;
    g_len = 0;
    g_cursor = 0;
    g_view_start = 0;
    for (;;) {
        int avail = (EDITOR_BUF_MAX - 1) - g_len;
        if (avail <= 0) break;
        int n = sys_read(fd, g_buf + g_len, (uint32_t)avail);
        if (n <= 0) break;
        g_len += n;
    }
    g_buf[g_len] = '\0';
    sys_close(fd);
    g_dirty = false;
    return 0;
}

static int file_save(const char* path) {
    int fd = sys_open(path, SYS_OPEN_FLAG_CREATE);
    if (fd < 0) return -1;
    int n = sys_write(fd, g_buf, (uint32_t)g_len);
    sys_close(fd);
    if (n < 0) return -1;
    g_dirty = false;
    return 0;
}

static void save_as_start(void) {
    g_save_mode = true;
    g_save_len = 0;
    g_save_as[0] = '\0';
}

static void save_as_cancel(void) {
    g_save_mode = false;
    g_save_len = 0;
    g_save_as[0] = '\0';
}

static int save_as_commit(void) {
    if (g_save_len <= 0) {
        return -1;
    }
    strncpy(g_path, g_save_as, sizeof(g_path) - 1);
    g_path[sizeof(g_path) - 1] = '\0';
    g_save_mode = false;
    return file_save(g_path);
}

int main(int argc, char** argv) {
    if (argc > 1 && argv && argv[1] && argv[1][0]) {
        strncpy(g_path, argv[1], sizeof(g_path) - 1);
        g_path[sizeof(g_path) - 1] = '\0';
    }

    gui_create(-1, -1, 400, 260, "Editor");
    file_load(g_path);

    char status[STATUS_MAX] = "";
    char text[GUI_MSG_TEXT_MAX];
    int max_body = GUI_MSG_TEXT_MAX - 256;
    if (max_body < 64) max_body = 64;
    bool dirty = true;

    for (;;) {
        uint32_t key = sys_getkey_nb();
        if (key) {
            if (g_save_mode) {
                if (key == 27) {
                    save_as_cancel();
                    strncpy(status, "save canceled", sizeof(status) - 1);
                    status[sizeof(status) - 1] = '\0';
                    dirty = true;
                } else if (key == '\b' || key == 0x7f) {
                    if (g_save_len > 0) {
                        g_save_len--;
                        g_save_as[g_save_len] = '\0';
                        dirty = true;
                    }
                } else if (key == '\r' || key == '\n') {
                    if (save_as_commit() == 0) {
                        strncpy(status, "saved", sizeof(status) - 1);
                    } else {
                        strncpy(status, "save failed", sizeof(status) - 1);
                    }
                    status[sizeof(status) - 1] = '\0';
                    dirty = true;
                } else if (key >= 32 && key < 127) {
                    if (g_save_len < SAVE_AS_MAX - 1) {
                        g_save_as[g_save_len++] = (char)key;
                        g_save_as[g_save_len] = '\0';
                        dirty = true;
                    }
                }
            } else if (key == 17) { // Ctrl+Q
                break;
            } else if (key == 19) { // Ctrl+S
                if (file_save(g_path) == 0) {
                    strncpy(status, "saved", sizeof(status) - 1);
                } else {
                    strncpy(status, "save failed", sizeof(status) - 1);
                }
                status[sizeof(status) - 1] = '\0';
                dirty = true;
            } else if (key == CTRL_SHIFT_S) { // Ctrl+Shift+S
                save_as_start();
                dirty = true;
            } else if (key == '\b' || key == 0x7f) {
                if (buffer_delete(g_cursor)) {
                    g_cursor--;
                    g_dirty = true;
                    dirty = true;
                }
            } else if (key == '\r' || key == '\n') {
                if (buffer_insert(g_cursor, '\n')) {
                    g_cursor++;
                    g_dirty = true;
                    dirty = true;
                }
            } else if (key == NOTE_KEY_LEFT) {
                move_left();
                dirty = true;
            } else if (key == NOTE_KEY_RIGHT) {
                move_right();
                dirty = true;
            } else if (key == NOTE_KEY_UP) {
                move_up();
                dirty = true;
            } else if (key == NOTE_KEY_DOWN) {
                move_down();
                dirty = true;
            } else if (key >= 32 && key < 127) {
                if (buffer_insert(g_cursor, (char)key)) {
                    g_cursor++;
                    g_dirty = true;
                    dirty = true;
                }
            }
        }

        if (dirty) {
            ensure_view(max_body);
            build_text(text, sizeof(text), max_body, status);
            gui_set_text(text);
            status[0] = '\0';
            dirty = false;
        }
        sys_yield();
    }

    sys_gui_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = GUI_MSG_CLOSE;
    sys_gui_send(&msg);
    return 0;
}

void _start(void) {
    int rc = main(0, NULL);
    sys_exit((uint32_t)rc);
}
