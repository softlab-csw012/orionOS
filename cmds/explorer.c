#include "syscall.h"
#include "string.h"
#include <stdbool.h>
#include <stdint.h>

#define MAX_ENTRIES 512
#define NAME_LEN 64
#define WINDOW_W 360
#define WINDOW_H 240
#define LINE_H_DEFAULT 18
#define HEADER_LINES 3
#define NOTE_KEY_LEFT  0x90
#define NOTE_KEY_RIGHT 0x91
#define NOTE_KEY_UP    0x92
#define NOTE_KEY_DOWN  0x93
#define UP_BUTTON_ID      1000
#define OPEN_BUTTON_ID    1001
#define HOME_BUTTON_ID    1002
#define REFRESH_BUTTON_ID 1003
#define PREV_BUTTON_ID    1004
#define NEXT_BUTTON_ID    1005

static char names[MAX_ENTRIES * NAME_LEN];
static uint8_t is_dir[MAX_ENTRIES];
static int g_line_h = LINE_H_DEFAULT;
static int g_font_w = 8;
static char g_text[GUI_MSG_TEXT_MAX];
static sys_gui_msg_t g_evt;
static int g_last_click_idx = -1;
static uint32_t g_last_click_sec = 0;

static void build_child_path(const char* base, const char* name, char* out, int out_size);
static int refresh_list(const char* path, int* out_count);

static void swap_entries(int a, int b) {
    if (a == b) {
        return;
    }
    char tmp_name[NAME_LEN];
    memcpy(tmp_name, names + a * NAME_LEN, NAME_LEN);
    memcpy(names + a * NAME_LEN, names + b * NAME_LEN, NAME_LEN);
    memcpy(names + b * NAME_LEN, tmp_name, NAME_LEN);
    uint8_t t = is_dir[a];
    is_dir[a] = is_dir[b];
    is_dir[b] = t;
}

static void sort_entries(int count) {
    if (count <= 1) {
        return;
    }
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            const char* ni = names + i * NAME_LEN;
            const char* nj = names + j * NAME_LEN;
            bool di = is_dir[i] != 0;
            bool dj = is_dir[j] != 0;
            bool swap = false;
            if (di != dj) {
                swap = (!di && dj);
            } else if (strcmp(ni, nj) > 0) {
                swap = true;
            }
            if (swap) {
                swap_entries(i, j);
            }
        }
    }
}

static char ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

static bool ends_with_icase(const char* s, const char* suffix) {
    if (!s || !suffix) {
        return false;
    }
    int sl = (int)strlen(s);
    int pl = (int)strlen(suffix);
    if (pl <= 0 || sl < pl) {
        return false;
    }
    for (int i = 0; i < pl; i++) {
        if (ascii_lower(s[sl - pl + i]) != ascii_lower(suffix[i])) {
            return false;
        }
    }
    return true;
}

static uint32_t launch_file_with_assoc(const char* fullpath) {
    if (!fullpath || !*fullpath) {
        return 0;
    }

    if (ends_with_icase(fullpath, ".txt") ||
        ends_with_icase(fullpath, ".md") ||
        ends_with_icase(fullpath, ".log") ||
        ends_with_icase(fullpath, ".c") ||
        ends_with_icase(fullpath, ".h")) {
        const char* argv[] = {"/cmd/editor", fullpath};
        return sys_spawn(argv[0], argv, 2);
    }
    if (ends_with_icase(fullpath, ".wav")) {
        const char* argv[] = {"/cmd/dw", fullpath};
        return sys_spawn(argv[0], argv, 2);
    }

    {
        const char* argv[] = {fullpath};
        uint32_t pid = sys_spawn(fullpath, argv, 1);
        if (pid != 0) {
            return pid;
        }
    }

    {
        const char* argv[] = {"/cmd/view", fullpath};
        return sys_spawn(argv[0], argv, 2);
    }
}

static bool open_entry(char* path, int idx, int count, int* out_count,
                       int* out_page, int* out_selected, char* status, int status_size) {
    if (!path || !out_count || !out_page || !out_selected || !status || status_size <= 1) {
        return false;
    }
    if (idx < 0 || idx >= count) {
        return false;
    }
    char fullpath[128];
    const char* name = names + (idx * NAME_LEN);
    char name_buf[NAME_LEN];
    strncpy(name_buf, name, sizeof(name_buf) - 1);
    name_buf[sizeof(name_buf) - 1] = '\0';
    build_child_path(path, name_buf, fullpath, sizeof(fullpath));

    if (is_dir[idx]) {
        strncpy(path, fullpath, 127);
        path[127] = '\0';
        if (refresh_list(path, out_count) < 0) {
            snprintf(status, status_size, "list failed");
            return false;
        }
        *out_page = 0;
        *out_selected = -1;
        status[0] = '\0';
        return true;
    }

    uint32_t pid = launch_file_with_assoc(fullpath);
    if (pid == 0) {
        snprintf(status, status_size, "open failed");
        return false;
    }
    snprintf(status, status_size, "launched pid:%d", (int)pid);
    return true;
}

static void append_line(char* out, int out_size, int* len, const char* line) {
    if (*len >= out_size - 1) return;

    int avail = out_size - 1 - *len;
    int written = snprintf(out + *len, avail + 1, "%s%s",
                            line,
                            (line[0] && line[strlen(line)-1] != '\n') ? "\n" : "");
    if (written > 0) {
        if (written > avail) written = avail;
        *len += written;
    }
}

static void build_page_text(const char* path, int count, int page, int per_page,
                            int selected, int max_name_cols, const char* status,
                            char* out, int out_size) {
    int len = 0;
    out[0] = '\0';

    char line[64];
    append_line(out, out_size, &len, " ");
    snprintf(line, sizeof(line), "Path: %s", (path && *path) ? path : "/");
    append_line(out, out_size, &len, line);

    int total_pages = (count + per_page - 1) / per_page;
    if (total_pages < 1) total_pages = 1;
    if (page >= total_pages) page = total_pages - 1;
    if (page < 0) page = 0;
    if (status && *status) {
        snprintf(line, sizeof(line), "Page %d/%d  (Enter/DblClick:Open, Prev/Next, r/q)  %s",
                 page + 1, total_pages, status);
    } else {
        snprintf(line, sizeof(line), "Page %d/%d  (Enter/DblClick:Open, Prev/Next, r/q)", page + 1, total_pages);
    }
    append_line(out, out_size, &len, line);

    if (count <= 0) {
        append_line(out, out_size, &len, "(empty)");
        return;
    }

    int start = page * per_page;
    int end = start + per_page;
    if (end > count) end = count;

    for (int i = start; i < end; i++) {
        const char* name = names + (i * NAME_LEN);
        char name_buf[NAME_LEN];
        strncpy(name_buf, name, sizeof(name_buf) - 1);
        name_buf[sizeof(name_buf) - 1] = '\0';
        if (max_name_cols > 0 && (int)strlen(name_buf) > max_name_cols) {
            name_buf[max_name_cols] = '\0';
        }
        const char* mark = (i == selected) ? ">" : " ";
        snprintf(line, sizeof(line), "%s[%c] %s", mark, is_dir[i] ? 'D' : 'F', name_buf);
        append_line(out, out_size, &len, line);
    }
}

static void normalize_path(char* path) {
    if (!path || !*path) {
        strcpy(path, "/");
        return;
    }
    size_t len = strlen(path);
    if (len > 1 && path[len - 1] == '/') {
        path[len - 1] = '\0';
    }
}

static void path_up(char* path) {
    if (!path || !*path) {
        strcpy(path, "/");
        return;
    }
    normalize_path(path);
    if (strcmp(path, "/") == 0) {
        return;
    }
    char* slash = strrchr(path, '/');
    if (!slash || slash == path) {
        strcpy(path, "/");
        return;
    }
    *slash = '\0';
}

static void build_child_path(const char* base, const char* name, char* out, int out_size) {
    if (!out || out_size <= 1) {
        return;
    }
    if (!base || !*base) {
        base = "/";
    }
    if (strcmp(base, "/") == 0) {
        snprintf(out, out_size, "/%s", name ? name : "");
    } else {
        snprintf(out, out_size, "%s/%s", base, name ? name : "");
    }
    out[out_size - 1] = '\0';
}

static int entry_index_from_click(int rel_y, int page, int per_page) {
    if (g_line_h <= 0) {
        return -1;
    }
    int line = rel_y / g_line_h;
    int row = line - HEADER_LINES;
    if (row < 0 || row >= per_page) {
        return -1;
    }
    return page * per_page + row;
}

static int refresh_list(const char* path, int* out_count) {
    sys_dir_list_t req;
    memset(&req, 0, sizeof(req));
    req.path = path;
    req.names = names;
    req.is_dir = is_dir;
    req.max_entries = MAX_ENTRIES;
    req.name_len = NAME_LEN;
    int count = sys_dir_list(&req);
    if (count < 0) {
        return -1;
    }
    sort_entries(count);
    *out_count = count;
    return 0;
}

int main(void) {
    char path[128] = "/";
    sys_fb_info_t fb;
    if (sys_fb_info(&fb)) {
        int fh = fb.font_h ? (int)fb.font_h : 16;
        g_font_w = fb.font_w ? (int)fb.font_w : 8;
        g_line_h = fh + 2;
        if (g_line_h < 12) g_line_h = 12;
        if (g_line_h > 24) g_line_h = 24;
    }
    int count = 0;
    int page = 0;
    int selected = -1;
    char status[64] = "";
    if (refresh_list(path, &count) < 0) {
        if (!gui_create(-1, -1, 360, 220, "Explorer")) {
            sys_kprint("explorer: gui not running\n");
            return 1;
        }
        gui_set_text("explorer: list failed");
        sys_pause();
        return 1;
    }

    if (!gui_create(-1, -1, WINDOW_W, WINDOW_H, "Explorer")) {
        sys_kprint("explorer: gui not running\n");
        return 1;
    }
    gui_clear_buttons();
    gui_add_button(UP_BUTTON_ID, 4, 2, 36, g_line_h - 4, "Up");
    gui_add_button(OPEN_BUTTON_ID, 44, 2, 48, g_line_h - 4, "Open");
    gui_add_button(HOME_BUTTON_ID, 96, 2, 48, g_line_h - 4, "Home");
    gui_add_button(REFRESH_BUTTON_ID, 148, 2, 62, g_line_h - 4, "Refresh");
    gui_add_button(PREV_BUTTON_ID, 214, 2, 42, g_line_h - 4, "Prev");
    gui_add_button(NEXT_BUTTON_ID, 260, 2, 42, g_line_h - 4, "Next");
    int page_lines = (WINDOW_H - (HEADER_LINES * g_line_h) - 8) / g_line_h;
    if (page_lines < 1) page_lines = 1;
    int max_cols = g_font_w ? (WINDOW_W / g_font_w) : 0;
    int max_name_cols = max_cols - 4;
    if (max_name_cols < 8) max_name_cols = 8;
    build_page_text(path, count, page, page_lines, selected, max_name_cols,
                    status, g_text, sizeof(g_text));
    gui_set_text(g_text);

    bool dirty = true;
    for (;;) {
        while (gui_recv_event(&g_evt)) {
            if (g_evt.type == GUI_MSG_BUTTON_CLICK) {
                if (g_evt.a == UP_BUTTON_ID) {
                    path_up(path);
                    if (refresh_list(path, &count) < 0) {
                        strcpy(status, "list failed");
                    } else {
                        status[0] = '\0';
                    }
                    page = 0;
                    selected = -1;
                    dirty = true;
                } else if (g_evt.a == OPEN_BUTTON_ID) {
                    if (selected >= 0 && selected < count) {
                        (void)open_entry(path, selected, count, &count, &page, &selected, status, sizeof(status));
                    } else {
                        strcpy(status, "select first");
                    }
                    dirty = true;
                } else if (g_evt.a == HOME_BUTTON_ID) {
                    strcpy(path, "/");
                    if (refresh_list(path, &count) < 0) {
                        strcpy(status, "list failed");
                    } else {
                        status[0] = '\0';
                    }
                    page = 0;
                    selected = -1;
                    dirty = true;
                } else if (g_evt.a == REFRESH_BUTTON_ID) {
                    if (refresh_list(path, &count) < 0) {
                        strcpy(status, "list failed");
                    } else {
                        status[0] = '\0';
                    }
                    dirty = true;
                } else if (g_evt.a == PREV_BUTTON_ID) {
                    page--;
                    dirty = true;
                } else if (g_evt.a == NEXT_BUTTON_ID) {
                    page++;
                    dirty = true;
                }
            } else if (g_evt.type == GUI_MSG_CLICK) {
                int idx = entry_index_from_click(g_evt.b, page, page_lines);
                if (idx >= 0 && idx < count) {
                    uint32_t now_sec = sys_uptime_seconds();
                    bool open_now = (idx == selected &&
                                     idx == g_last_click_idx &&
                                     (now_sec == g_last_click_sec || now_sec == g_last_click_sec + 1));
                    selected = idx;
                    g_last_click_idx = idx;
                    g_last_click_sec = now_sec;
                    {
                        const char* name = names + (idx * NAME_LEN);
                        snprintf(status, sizeof(status), "[%c] %s",
                                 is_dir[idx] ? 'D' : 'F', name);
                    }
                    if (open_now) {
                        (void)open_entry(path, idx, count, &count, &page, &selected, status, sizeof(status));
                    }
                    dirty = true;
                }
            }
        }

        uint32_t key = sys_getkey_nb();
        if (key) {
            if (key == 27 || key == 'q' || key == 'Q') {
                break;
            }
            if (key == '\r' || key == '\n') {
                if (selected >= 0 && selected < count) {
                    (void)open_entry(path, selected, count, &count, &page, &selected, status, sizeof(status));
                } else {
                    strcpy(status, "select first");
                }
                dirty = true;
            } else if (key == '\b') {
                path_up(path);
                if (refresh_list(path, &count) < 0) {
                    strcpy(status, "list failed");
                } else {
                    status[0] = '\0';
                }
                page = 0;
                selected = -1;
                dirty = true;
            } else if (key == NOTE_KEY_UP) {
                if (selected > 0) {
                    selected--;
                    dirty = true;
                }
            } else if (key == NOTE_KEY_DOWN) {
                if (selected < count - 1) {
                    selected++;
                    dirty = true;
                }
            } else if (key == NOTE_KEY_LEFT) {
                page--;
                dirty = true;
            } else if (key == NOTE_KEY_RIGHT) {
                page++;
                dirty = true;
            } else if (key == 'r' || key == 'R') {
                if (refresh_list(path, &count) < 0) {
                    strcpy(status, "list failed");
                } else {
                    status[0] = '\0';
                }
                dirty = true;
            } else if (key == 'n' || key == 'N') {
                page++;
                dirty = true;
            } else if (key == 'p' || key == 'P') {
                page--;
                dirty = true;
            }
        }

        if (dirty) {
            int total_pages = (count + page_lines - 1) / page_lines;
            if (total_pages < 1) total_pages = 1;
            if (page < 0) page = 0;
            if (page >= total_pages) page = total_pages - 1;
            build_page_text(path, count, page, page_lines, selected, max_name_cols,
                            status, g_text, sizeof(g_text));
            gui_set_text(g_text);
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
    int rc = main();
    sys_exit((uint32_t)rc);
}
