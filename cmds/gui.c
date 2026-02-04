#include "syscall.h"
#include "string.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NOTE_KEY_LEFT  0x90
#define NOTE_KEY_RIGHT 0x91
#define NOTE_KEY_UP    0x92
#define NOTE_KEY_DOWN  0x93

#define LOG_LINES_MAX 32
#define LOG_LINE_MAX  96

#define RGB(r, g, b) ((uint32_t)(((r) << 16) | ((g) << 8) | (b)))

static const uint32_t COLOR_DESKTOP = RGB(0, 128, 128);
static const uint32_t COLOR_TASKBAR = RGB(192, 192, 192);
static const uint32_t COLOR_FACE = RGB(192, 192, 192);
static const uint32_t COLOR_LIGHT = RGB(255, 255, 255);
static const uint32_t COLOR_LIGHT2 = RGB(223, 223, 223);
static const uint32_t COLOR_SHADOW = RGB(128, 128, 128);
static const uint32_t COLOR_DARK = RGB(64, 64, 64);
static const uint32_t COLOR_TITLE = RGB(0, 0, 128);
static const uint32_t COLOR_TITLE_LIGHT = RGB(0, 0, 160);
static const uint32_t COLOR_TITLE_TEXT = RGB(255, 255, 255);
static const uint32_t COLOR_TEXT = RGB(0, 0, 0);
static const uint32_t COLOR_LOG_BG = RGB(255, 255, 255);
static const uint32_t COLOR_ICON_TEXT = RGB(255, 255, 255);

typedef struct {
    int width;
    int height;
    int font_w;
    int font_h;
    int margin;
    int line_h;
    int taskbar_h;
    int desktop_h;
    int icon_size;
    int icon_gap_y;
    int icon_label_w;
    int work_x;
    int work_y;
    int work_w;
    int work_h;
    int win_frame;
    int win_pad;
    int win_title_h;
    int win_min_w;
    int win_min_h;
    int default_win_w;
    int default_win_h;
    int log_lines;
    int log_cols;
    int start_x;
    int start_y;
    int start_w;
    int start_h;
    int clock_x;
    int clock_y;
    int clock_w;
    int clock_h;
} ui_layout_t;

static char log_lines[LOG_LINES_MAX][LOG_LINE_MAX];
static int log_count = 0;
static int log_capacity = 0;
static int log_cols = 0;

typedef struct {
    bool used;
    bool system;
    uint32_t pid;
    int x;
    int y;
    int w;
    int h;
    char title[32];
    char body[GUI_MSG_TEXT_MAX];
} gui_window_t;

static gui_window_t windows[8];
static int z_order[8];
static int z_count = 0;
static int focused_idx = -1;
static int next_cascade = 0;

static void fb_fill(int x, int y, int w, int h, uint32_t color) {
    sys_fb_rect_t rect = {
        .x = x,
        .y = y,
        .w = w,
        .h = h,
        .color = color,
    };
    sys_fb_fill_rect(&rect);
}

static void fb_text(int x, int y, const char* text, uint32_t fg, uint32_t bg, bool transparent) {
    sys_fb_text_t t = {
        .x = x,
        .y = y,
        .fg = fg,
        .bg = bg,
        .flags = transparent ? SYS_FB_TEXT_TRANSPARENT : 0u,
        .text = text,
    };
    sys_fb_draw_text(&t);
}

static void draw_frame(int x, int y, int w, int h, uint32_t top_left, uint32_t bottom_right) {
    if (w <= 0 || h <= 0) {
        return;
    }
    fb_fill(x, y, w, 1, top_left);
    fb_fill(x, y, 1, h, top_left);
    fb_fill(x, y + h - 1, w, 1, bottom_right);
    fb_fill(x + w - 1, y, 1, h, bottom_right);
}

static void draw_bevel(int x, int y, int w, int h, bool raised) {
    if (w <= 2 || h <= 2) {
        return;
    }
    if (raised) {
        draw_frame(x, y, w, h, COLOR_LIGHT, COLOR_DARK);
        draw_frame(x + 1, y + 1, w - 2, h - 2, COLOR_LIGHT2, COLOR_SHADOW);
    } else {
        draw_frame(x, y, w, h, COLOR_DARK, COLOR_LIGHT);
        draw_frame(x + 1, y + 1, w - 2, h - 2, COLOR_SHADOW, COLOR_LIGHT2);
    }
}

static void clamp_text(char* out, int out_size, const char* text, int max_cols) {
    if (out_size <= 0) {
        return;
    }
    if (!text) {
        out[0] = '\0';
        return;
    }
    strncpy(out, text, out_size - 1);
    out[out_size - 1] = '\0';
    if (max_cols > 0 && max_cols < out_size) {
        out[max_cols] = '\0';
    }
}

static void layout_compute(ui_layout_t* ui, const sys_fb_info_t* fb) {
    memset(ui, 0, sizeof(*ui));
    ui->width = (int)fb->width;
    ui->height = (int)fb->height;
    ui->font_w = fb->font_w ? (int)fb->font_w : 8;
    ui->font_h = fb->font_h ? (int)fb->font_h : 16;
    ui->margin = 12;
    ui->line_h = ui->font_h + 2;
    ui->taskbar_h = ui->font_h + 10;
    if (ui->taskbar_h < 22) ui->taskbar_h = 22;
    if (ui->taskbar_h > ui->height) ui->taskbar_h = ui->height;
    ui->desktop_h = ui->height - ui->taskbar_h;
    if (ui->desktop_h < 0) ui->desktop_h = 0;

    ui->work_x = 0;
    ui->work_y = 0;
    ui->work_w = ui->width;
    ui->work_h = ui->desktop_h;

    ui->icon_size = ui->font_h;
    if (ui->icon_size < 16) ui->icon_size = 16;
    ui->icon_gap_y = ui->icon_size + ui->font_h + 6;
    ui->icon_label_w = ui->font_w * 12;
    if (ui->icon_label_w < ui->icon_size + 2) ui->icon_label_w = ui->icon_size + 2;

    ui->win_frame = 2;
    ui->win_pad = 3;
    ui->win_title_h = ui->font_h + 6;
    if (ui->win_title_h < 18) ui->win_title_h = 18;

    ui->win_min_w = ui->font_w * 16;
    ui->win_min_h = ui->font_h * 6 + ui->win_title_h + ui->win_frame * 2 + ui->win_pad * 2;

    int max_w = ui->work_w - ui->margin * 2 - ui->icon_label_w;
    if (max_w < ui->win_min_w) {
        max_w = ui->work_w - ui->margin * 2;
    }
    if (max_w < ui->win_min_w) {
        max_w = ui->win_min_w;
    }
    ui->default_win_w = ui->font_w * 42;
    if (ui->default_win_w > max_w) ui->default_win_w = max_w;
    if (ui->default_win_w < ui->win_min_w) ui->default_win_w = ui->win_min_w;

    int max_h = ui->work_h - ui->margin * 2;
    if (max_h < ui->win_min_h) {
        max_h = ui->win_min_h;
    }
    ui->default_win_h = ui->font_h * 12 + ui->win_title_h + 12;
    if (ui->default_win_h > max_h) ui->default_win_h = max_h;
    if (ui->default_win_h < ui->win_min_h) ui->default_win_h = ui->win_min_h;

    ui->log_lines = 0;
    ui->log_cols = 0;

    ui->start_w = ui->font_w * 5 + 18;
    ui->start_h = ui->taskbar_h - 8;
    if (ui->start_h < 16) ui->start_h = 16;
    if (ui->start_h > ui->taskbar_h) ui->start_h = ui->taskbar_h;
    ui->start_x = 6;
    ui->start_y = ui->height - ui->taskbar_h + (ui->taskbar_h - ui->start_h) / 2;

    ui->clock_w = ui->font_w * 8 + 12;
    ui->clock_h = ui->start_h;
    ui->clock_x = ui->width - ui->clock_w - 6;
    ui->clock_y = ui->start_y;
    if (ui->clock_x < ui->start_x + ui->start_w + 6) {
        ui->clock_x = ui->start_x + ui->start_w + 6;
    }
}

static void log_clear(void) {
    log_count = 0;
}

static void log_store(char* dest, const char* src) {
    if (!src) {
        dest[0] = '\0';
        return;
    }
    strncpy(dest, src, LOG_LINE_MAX - 1);
    dest[LOG_LINE_MAX - 1] = '\0';
    if (log_cols > 0 && log_cols < LOG_LINE_MAX) {
        dest[log_cols] = '\0';
    }
}

static void log_push(const char* msg) {
    if (log_capacity <= 0) {
        return;
    }
    if (log_count < log_capacity) {
        log_store(log_lines[log_count], msg);
        log_count++;
        return;
    }
    for (int i = 1; i < log_capacity; i++) {
        log_store(log_lines[i - 1], log_lines[i]);
    }
    log_store(log_lines[log_capacity - 1], msg);
}

static void update_log_metrics(const ui_layout_t* ui, int text_w, int text_h) {
    if (!ui || text_w <= 0 || text_h <= 0) {
        log_capacity = 0;
        log_cols = 0;
        return;
    }
    int lines = ui->line_h ? (text_h / ui->line_h) : 0;
    if (lines > LOG_LINES_MAX) lines = LOG_LINES_MAX;
    int cols = ui->font_w ? (text_w / ui->font_w) : 0;
    if (cols > LOG_LINE_MAX - 1) cols = LOG_LINE_MAX - 1;
    log_capacity = lines;
    log_cols = cols;
    if (log_count > log_capacity) {
        log_count = log_capacity;
    }
}

static void window_text_area(const ui_layout_t* ui, const gui_window_t* win,
                             int* out_x, int* out_y, int* out_w, int* out_h) {
    int frame = ui->win_frame;
    int pad = ui->win_pad;
    int title_h = ui->win_title_h;
    int x = win->x + frame + pad;
    int y = win->y + frame + title_h + pad;
    int w = win->w - (frame * 2) - (pad * 2);
    int h = win->h - (frame * 2) - title_h - (pad * 2);
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

static void windows_reset(void) {
    memset(windows, 0, sizeof(windows));
    for (int i = 0; i < (int)(sizeof(z_order) / sizeof(z_order[0])); i++) {
        z_order[i] = -1;
    }
    z_count = 0;
    focused_idx = -1;
    next_cascade = 0;
}

static int window_find_by_pid(uint32_t pid) {
    for (int i = 0; i < (int)(sizeof(windows) / sizeof(windows[0])); i++) {
        if (windows[i].used && !windows[i].system && windows[i].pid == pid) {
            return i;
        }
    }
    return -1;
}

static void window_remove_from_z(int idx) {
    for (int i = 0; i < z_count; i++) {
        if (z_order[i] == idx) {
            for (int j = i + 1; j < z_count; j++) {
                z_order[j - 1] = z_order[j];
            }
            z_count--;
            z_order[z_count] = -1;
            return;
        }
    }
}

static void window_focus(int idx) {
    if (idx < 0) {
        focused_idx = -1;
        return;
    }
    if (!windows[idx].used) {
        return;
    }
    window_remove_from_z(idx);
    if (z_count < (int)(sizeof(z_order) / sizeof(z_order[0]))) {
        z_order[z_count++] = idx;
    }
    focused_idx = idx;
}

static void clamp_window_to_work(const ui_layout_t* ui, gui_window_t* win) {
    int min_w = ui->win_min_w;
    int min_h = ui->win_min_h;
    if (min_w > ui->work_w) min_w = ui->work_w;
    if (min_h > ui->work_h) min_h = ui->work_h;

    if (win->w > ui->work_w) win->w = ui->work_w;
    if (win->h > ui->work_h) win->h = ui->work_h;
    if (win->w < min_w) win->w = min_w;
    if (win->h < min_h) win->h = min_h;
    if (win->x < ui->work_x) win->x = ui->work_x;
    if (win->y < ui->work_y) win->y = ui->work_y;
    if (win->x + win->w > ui->work_x + ui->work_w) {
        win->x = ui->work_x + ui->work_w - win->w;
    }
    if (win->y + win->h > ui->work_y + ui->work_h) {
        win->y = ui->work_y + ui->work_h - win->h;
    }
    if (win->x < ui->work_x) win->x = ui->work_x;
    if (win->y < ui->work_y) win->y = ui->work_y;
}

static int window_create(const ui_layout_t* ui, uint32_t pid, int x, int y, int w, int h,
                         const char* title, bool system) {
    int slot = -1;
    for (int i = 0; i < (int)(sizeof(windows) / sizeof(windows[0])); i++) {
        if (!windows[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -1;
    }

    gui_window_t* win = &windows[slot];
    memset(win, 0, sizeof(*win));
    win->used = true;
    win->system = system;
    win->pid = pid;
    win->w = (w > 0) ? w : ui->default_win_w;
    win->h = (h > 0) ? h : ui->default_win_h;

    if (x < 0 || y < 0) {
        int base_x = ui->margin + ui->icon_label_w + 12;
        if (base_x + win->w > ui->work_w - ui->margin) {
            base_x = ui->margin;
        }
        int offset = next_cascade;
        win->x = base_x + offset;
        win->y = ui->margin + offset;
        next_cascade = (next_cascade + 20) % 120;
    } else {
        win->x = x;
        win->y = y;
    }

    clamp_window_to_work(ui, win);

    if (title && *title) {
        strncpy(win->title, title, sizeof(win->title) - 1);
        win->title[sizeof(win->title) - 1] = '\0';
    } else {
        snprintf(win->title, sizeof(win->title), "App %u", (unsigned)pid);
    }

    if (z_count < (int)(sizeof(z_order) / sizeof(z_order[0]))) {
        z_order[z_count++] = slot;
    }
    focused_idx = slot;
    return slot;
}

static void window_destroy(int idx) {
    if (idx < 0 || idx >= (int)(sizeof(windows) / sizeof(windows[0]))) {
        return;
    }
    if (!windows[idx].used) {
        return;
    }
    if (windows[idx].system) {
        return;
    }
    windows[idx].used = false;
    window_remove_from_z(idx);
    if (focused_idx == idx) {
        focused_idx = (z_count > 0) ? z_order[z_count - 1] : -1;
    }
}

static int window_find_at(const ui_layout_t* ui, int px, int py) {
    (void)ui;
    for (int i = z_count - 1; i >= 0; i--) {
        int idx = z_order[i];
        if (idx < 0) continue;
        gui_window_t* win = &windows[idx];
        if (!win->used) continue;
        if (px >= win->x && px < win->x + win->w &&
            py >= win->y && py < win->y + win->h) {
            return idx;
        }
    }
    return -1;
}

static bool window_hit_close(const ui_layout_t* ui, const gui_window_t* win, int px, int py) {
    int title_x = win->x + ui->win_frame;
    int title_y = win->y + ui->win_frame;
    int title_w = win->w - ui->win_frame * 2;
    int title_h = ui->win_title_h;
    if (title_h > win->h - ui->win_frame * 2) {
        title_h = win->h - ui->win_frame * 2;
    }
    if (title_w <= 0 || title_h <= 0) {
        return false;
    }
    int btn = title_h - 6;
    if (btn < 12) btn = 12;
    if (btn > title_h) btn = title_h;
    int close_x = title_x + title_w - btn - 4;
    int close_y = title_y + (title_h - btn) / 2;
    if (px >= close_x && px < close_x + btn &&
        py >= close_y && py < close_y + btn) {
        return true;
    }
    return false;
}

static void draw_icon(const ui_layout_t* ui, int x, int y, const char* label, const char* glyph) {
    if (ui->icon_size <= 0) {
        return;
    }
    fb_fill(x, y, ui->icon_size, ui->icon_size, COLOR_FACE);
    draw_bevel(x, y, ui->icon_size, ui->icon_size, true);
    if (glyph && *glyph) {
        int gx = x + (ui->icon_size - ui->font_w) / 2;
        int gy = y + (ui->icon_size - ui->font_h) / 2;
        fb_text(gx, gy, glyph, COLOR_TEXT, COLOR_FACE, true);
    }
    if (label && *label) {
        char buf[32];
        int label_cols = ui->font_w ? (ui->icon_label_w / ui->font_w) : 0;
        clamp_text(buf, sizeof(buf), label, label_cols);
        fb_text(x, y + ui->icon_size + 2, buf, COLOR_ICON_TEXT, COLOR_DESKTOP, true);
    }
}

static void draw_desktop_icons(const ui_layout_t* ui) {
    int x = ui->margin;
    int y = ui->margin;
    const char* labels[] = {
        "Explorer",
    };
    const char* glyphs[] = {"E"};
    int count = (int)(sizeof(labels) / sizeof(labels[0]));

    for (int i = 0; i < count; i++) {
        if (y + ui->icon_size + ui->font_h >= ui->desktop_h - ui->margin) {
            break;
        }
        draw_icon(ui, x, y, labels[i], glyphs[i]);
        y += ui->icon_gap_y;
    }
}

static bool icon_hit_explorer(const ui_layout_t* ui, int px, int py) {
    int x = ui->margin;
    int y = ui->margin;
    int w = ui->icon_label_w;
    if (w < ui->icon_size) w = ui->icon_size;
    int h = ui->icon_size + ui->font_h + 4;
    return (px >= x && px < x + w && py >= y && py < y + h);
}

static void launch_explorer(void) {
    const char* argv[] = {"/cmd/explorer.sys"};
    uint32_t pid = sys_spawn(argv[0], argv, 1);
    if (pid == 0) {
        log_push("explorer: spawn failed");
    } else {
        log_push("explorer: launched");
    }
}

static void draw_taskbar(const ui_layout_t* ui, const char* status) {
    int y = ui->height - ui->taskbar_h;
    if (y < 0) y = 0;
    fb_fill(0, y, ui->width, ui->taskbar_h, COLOR_TASKBAR);
    draw_bevel(0, y, ui->width, ui->taskbar_h, true);

    if (ui->start_w > 0 && ui->start_h > 0) {
        fb_fill(ui->start_x, ui->start_y, ui->start_w, ui->start_h, COLOR_TASKBAR);
        draw_bevel(ui->start_x, ui->start_y, ui->start_w, ui->start_h, true);
        fb_text(ui->start_x + 8, ui->start_y + (ui->start_h - ui->font_h) / 2,
                "Start", COLOR_TEXT, COLOR_TASKBAR, true);
    }

    if (ui->clock_w > 0 && ui->clock_h > 0 && ui->clock_x < ui->width) {
        fb_fill(ui->clock_x, ui->clock_y, ui->clock_w, ui->clock_h, COLOR_TASKBAR);
        draw_bevel(ui->clock_x, ui->clock_y, ui->clock_w, ui->clock_h, false);
        fb_text(ui->clock_x + 6, ui->clock_y + (ui->clock_h - ui->font_h) / 2,
                "3:48 PM", COLOR_TEXT, COLOR_TASKBAR, true);
    }

    int status_x = ui->start_x + ui->start_w + 10;
    int status_w = ui->clock_x - status_x - 8;
    if (status_w > ui->font_w * 6) {
        char buf[128];
        const char* text = (status && *status) ? status : "Ready";
        int cols = ui->font_w ? (status_w / ui->font_w) : 0;
        clamp_text(buf, sizeof(buf), text, cols);
        fb_text(status_x, ui->start_y + (ui->start_h - ui->font_h) / 2,
                buf, COLOR_TEXT, COLOR_TASKBAR, true);
    }
}

static void draw_window_frame(const ui_layout_t* ui, const gui_window_t* win, bool focused) {
    if (win->w <= 0 || win->h <= 0) {
        return;
    }
    fb_fill(win->x, win->y, win->w, win->h, COLOR_FACE);
    draw_bevel(win->x, win->y, win->w, win->h, true);

    int title_x = win->x + ui->win_frame;
    int title_y = win->y + ui->win_frame;
    int title_w = win->w - ui->win_frame * 2;
    int title_h = ui->win_title_h;
    if (title_h > win->h - ui->win_frame * 2) title_h = win->h - ui->win_frame * 2;
    if (title_h < 0) title_h = 0;
    if (title_w < 0) title_w = 0;

    if (title_h > 0 && title_w > 0) {
        uint32_t title_color = focused ? COLOR_TITLE : COLOR_LIGHT2;
        uint32_t title_text = focused ? COLOR_TITLE_TEXT : COLOR_TEXT;
        fb_fill(title_x, title_y, title_w, title_h, title_color);
        if (focused) {
            fb_fill(title_x, title_y, title_w, 1, COLOR_TITLE_LIGHT);
        }

        int btn = title_h - 6;
        if (btn < 12) btn = 12;
        if (btn > title_h) btn = title_h;
        int close_x = title_x + title_w - btn - 4;
        int close_y = title_y + (title_h - btn) / 2;
        int text_x = title_x + 6;
        int text_w = close_x - text_x - 4;
        if (text_w < 0) text_w = 0;
        int text_cols = ui->font_w ? (text_w / ui->font_w) : 0;
        char title_buf[32];
        clamp_text(title_buf, sizeof(title_buf), win->title, text_cols);
        fb_text(text_x, title_y + (title_h - ui->font_h) / 2,
                title_buf, title_text, title_color, true);

        if (btn > 0 && close_x >= title_x) {
            fb_fill(close_x, close_y, btn, btn, COLOR_FACE);
            draw_bevel(close_x, close_y, btn, btn, true);
            fb_text(close_x + (btn - ui->font_w) / 2,
                    close_y + (btn - ui->font_h) / 2,
                    "X", COLOR_TEXT, COLOR_FACE, true);
        }
    }
}

static void draw_window_content(const ui_layout_t* ui, const gui_window_t* win) {
    int tx, ty, tw, th;
    window_text_area(ui, win, &tx, &ty, &tw, &th);
    if (tw <= 0 || th <= 0) {
        return;
    }
    fb_fill(tx, ty, tw, th, COLOR_LOG_BG);

    if (win->system) {
        update_log_metrics(ui, tw, th);
        if (log_count > log_capacity) {
            log_count = log_capacity;
        }
        int lines = log_count;
        if (lines > log_capacity) lines = log_capacity;
        for (int i = 0; i < lines; i++) {
            int y = ty + i * ui->line_h;
            fb_text(tx, y, log_lines[i], COLOR_TEXT, COLOR_LOG_BG, true);
        }
        return;
    }

    int cols = ui->font_w ? (tw / ui->font_w) : 0;
    char buf[160];
    clamp_text(buf, sizeof(buf), win->body, cols);
    fb_text(tx, ty, buf, COLOR_TEXT, COLOR_LOG_BG, true);
}

static void draw_windows(const ui_layout_t* ui) {
    for (int i = 0; i < z_count; i++) {
        int idx = z_order[i];
        if (idx < 0) continue;
        gui_window_t* win = &windows[idx];
        if (!win->used) continue;
        bool focused = (idx == focused_idx);
        draw_window_frame(ui, win, focused);
        draw_window_content(ui, win);
    }
}

static void draw_full_ui(const ui_layout_t* ui, const char* status) {
    fb_fill(0, 0, ui->width, ui->height, COLOR_DESKTOP);
    draw_desktop_icons(ui);
    draw_windows(ui);
    draw_taskbar(ui, status);
}

static void set_text(char* out, int size, const char* text) {
    if (size <= 0) {
        return;
    }
    strncpy(out, text, size - 1);
    out[size - 1] = '\0';
}

static void format_buttons(int buttons, char* out, int size) {
    if (size <= 1) {
        return;
    }
    int idx = 0;
    if (buttons & 1) out[idx++] = 'L';
    if (buttons & 2) out[idx++] = 'R';
    if (buttons & 4) out[idx++] = 'M';
    if (idx == 0) out[idx++] = '-';
    out[idx] = '\0';
}

static void format_hex(uint32_t value, char* out, int size) {
    if (size <= 1) {
        return;
    }
    char tmp[16];
    itoa((int)value, tmp, 16);
    int idx = 0;
    out[idx++] = '0';
    if (idx < size - 1) {
        out[idx++] = 'x';
    }
    for (int i = 0; tmp[i] && idx < size - 1; i++) {
        out[idx++] = tmp[i];
    }
    out[idx] = '\0';
}

static void format_key(uint32_t key, char* out, int size) {
    if (size <= 1) {
        return;
    }
    if (key == 0) {
        set_text(out, size, "-");
        return;
    }
    if (key == '\r' || key == '\n') {
        set_text(out, size, "ENTER");
        return;
    }
    if (key == '\b') {
        set_text(out, size, "BS");
        return;
    }
    if (key == 0x7f) {
        set_text(out, size, "DEL");
        return;
    }
    if (key == 27) {
        set_text(out, size, "ESC");
        return;
    }
    if (key == NOTE_KEY_LEFT) {
        set_text(out, size, "LEFT");
        return;
    }
    if (key == NOTE_KEY_RIGHT) {
        set_text(out, size, "RIGHT");
        return;
    }
    if (key == NOTE_KEY_UP) {
        set_text(out, size, "UP");
        return;
    }
    if (key == NOTE_KEY_DOWN) {
        set_text(out, size, "DOWN");
        return;
    }
    if (key >= 32 && key < 127) {
        out[0] = (char)key;
        out[1] = '\0';
        return;
    }
    format_hex(key, out, size);
}

static void build_status_text(const ui_layout_t* ui, const char* key_desc,
                              const sys_mouse_state_t* mouse, char* out, int size) {
    if (size <= 1) {
        return;
    }
    char btns[8];
    format_buttons(mouse ? mouse->buttons : 0, btns, sizeof(btns));
    int mx = mouse ? mouse->x : 0;
    int my = mouse ? mouse->y : 0;
    int px = mx * ui->font_w;
    int py = my * ui->font_h;

    int window_total = 0;
    for (int i = 0; i < (int)(sizeof(windows) / sizeof(windows[0])); i++) {
        if (windows[i].used && !windows[i].system) {
            window_total++;
        }
    }
    const char* focus_title = NULL;
    if (focused_idx >= 0 && focused_idx < (int)(sizeof(windows) / sizeof(windows[0]))) {
        if (windows[focused_idx].used) {
            focus_title = windows[focused_idx].title;
        }
    }
    if (!focus_title) {
        focus_title = "Desktop";
    }

    snprintf(out, size, "Key:%s  Mouse:%d,%d  Win:%d  Active:%s  [%s]",
             key_desc, px, py, window_total, focus_title, btns);
}

// GUI_MSG_CREATE: a=x, b=y, c=(w<<16)|h, text=title
// GUI_MSG_TEXT: text=body
static bool handle_gui_message(const ui_layout_t* ui, const sys_gui_msg_t* msg) {
    bool dirty = false;
    int idx = window_find_by_pid(msg->sender_pid);
    switch (msg->type) {
        case GUI_MSG_CREATE: {
            int w = -1;
            int h = -1;
            if (msg->c > 0) {
                w = (int)((msg->c >> 16) & 0xffff);
                h = (int)(msg->c & 0xffff);
                if (w <= 0) w = -1;
                if (h <= 0) h = -1;
            }
            if (idx < 0) {
                idx = window_create(ui, msg->sender_pid, msg->a, msg->b, w, h,
                                    msg->text, false);
                if (idx >= 0) {
                    char line[LOG_LINE_MAX];
                    snprintf(line, sizeof(line), "pid %d: window created",
                             (int)msg->sender_pid);
                    log_push(line);
                    dirty = true;
                }
            } else {
                if (msg->text[0]) {
                    strncpy(windows[idx].title, msg->text, sizeof(windows[idx].title) - 1);
                    windows[idx].title[sizeof(windows[idx].title) - 1] = '\0';
                }
                window_focus(idx);
                dirty = true;
            }
            break;
        }
        case GUI_MSG_TEXT: {
            if (idx < 0) {
                idx = window_create(ui, msg->sender_pid, -1, -1, -1, -1, NULL, false);
                if (idx >= 0) {
                    char line[LOG_LINE_MAX];
                    snprintf(line, sizeof(line), "pid %d: window created (text)",
                             (int)msg->sender_pid);
                    log_push(line);
                }
            }
            if (idx >= 0) {
                strncpy(windows[idx].body, msg->text, sizeof(windows[idx].body) - 1);
                windows[idx].body[sizeof(windows[idx].body) - 1] = '\0';
                dirty = true;
            }
            break;
        }
        case GUI_MSG_CLOSE: {
            if (idx >= 0 && !windows[idx].system) {
                char line[LOG_LINE_MAX];
                snprintf(line, sizeof(line), "pid %d: window closed",
                         (int)msg->sender_pid);
                log_push(line);
                window_destroy(idx);
                dirty = true;
            }
            break;
        }
        default: {
            char line[LOG_LINE_MAX];
            snprintf(line, sizeof(line), "pid %d: msg %d",
                     (int)msg->sender_pid, (int)msg->type);
            log_push(line);
            break;
        }
    }
    return dirty;
}

int main(void) {
    sys_fb_info_t fb;
    if (!sys_fb_info(&fb)) {
        sys_kprint("gui: framebuffer unavailable\n");
        return 1;
    }
    if (!sys_gui_bind()) {
        sys_kprint("gui: already running\n");
        return 1;
    }

    sys_cursor_visible(0);
    sys_mouse_draw(0);

    ui_layout_t ui;
    layout_compute(&ui, &fb);
    windows_reset();

    int sys_w = ui.default_win_w;
    int sys_h = ui.default_win_h + ui.font_h * 4;
    int sys_idx = window_create(&ui, 0, -1, -1, sys_w, sys_h, "System Log", true);
    if (sys_idx >= 0) {
        int tx, ty, tw, th;
        window_text_area(&ui, &windows[sys_idx], &tx, &ty, &tw, &th);
        update_log_metrics(&ui, tw, th);
    }

    log_clear();
    if (log_capacity > 0) {
        log_push("GUI server ready");
        log_push("Waiting for messages...");
    }

    sys_mouse_state_t mouse = {0};
    sys_mouse_state(&mouse);
    int last_mouse_x = mouse.x;
    int last_mouse_y = mouse.y;
    int last_buttons = mouse.buttons;

    char key_desc[16];
    format_key(0, key_desc, sizeof(key_desc));
    char status_text[128];
    build_status_text(&ui, key_desc, &mouse, status_text, sizeof(status_text));
    draw_full_ui(&ui, status_text);
    sys_mouse_draw(1);

    bool running = true;
    while (running) {
        bool windows_dirty = false;
        bool status_dirty = false;

        sys_gui_msg_t msg;
        while (sys_gui_recv(&msg)) {
            msg.text[GUI_MSG_TEXT_MAX - 1] = '\0';
            if (handle_gui_message(&ui, &msg)) {
                windows_dirty = true;
            }
        }

        bool has_client = false;
        for (int i = 0; i < (int)(sizeof(windows) / sizeof(windows[0])); i++) {
            if (windows[i].used && !windows[i].system) {
                has_client = true;
                break;
            }
        }
        if (!has_client) {
            uint32_t key = sys_getkey_nb();
            if (key) {
                format_key(key, key_desc, sizeof(key_desc));
                status_dirty = true;
                if (key == 27 || key == 'q' || key == 'Q') {
                    running = false;
                } else if (key == 'c' || key == 'C') {
                    log_clear();
                    log_push("log cleared");
                    windows_dirty = true;
                }
            }
        }

        sys_mouse_state_t cur_mouse = mouse;
        if (sys_mouse_state(&cur_mouse)) {
            if (cur_mouse.x != last_mouse_x || cur_mouse.y != last_mouse_y ||
                cur_mouse.buttons != last_buttons) {
                int px = cur_mouse.x * ui.font_w;
                int py = cur_mouse.y * ui.font_h;
                if ((cur_mouse.buttons & 1) && !(last_buttons & 1)) {
                    int hit = window_find_at(&ui, px, py);
                    if (hit >= 0) {
                        if (!windows[hit].system && window_hit_close(&ui, &windows[hit], px, py)) {
                            window_destroy(hit);
                        } else {
                            window_focus(hit);
                        }
                        windows_dirty = true;
                    } else if (icon_hit_explorer(&ui, px, py)) {
                        launch_explorer();
                        windows_dirty = true;
                    }
                }
                last_mouse_x = cur_mouse.x;
                last_mouse_y = cur_mouse.y;
                last_buttons = cur_mouse.buttons;
                mouse = cur_mouse;
                status_dirty = true;
            }
        }

        if (windows_dirty || status_dirty) {
            build_status_text(&ui, key_desc, &mouse, status_text, sizeof(status_text));
        }
        if (windows_dirty) {
            sys_mouse_draw(0);
            draw_full_ui(&ui, status_text);
            sys_mouse_draw(1);
        } else if (status_dirty) {
            bool over_taskbar = false;
            int mouse_py = mouse.y * ui.font_h;
            if (mouse_py >= ui.height - ui.taskbar_h) {
                over_taskbar = true;
            }
            if (over_taskbar) sys_mouse_draw(0);
            draw_taskbar(&ui, status_text);
            if (over_taskbar) sys_mouse_draw(1);
        }

        sys_yield();
    }

    sys_mouse_draw(0);
    sys_clear_screen();
    sys_cursor_visible(1);
    return 0;
}

void _start(void) {
    int rc = main();
    sys_exit((uint32_t)rc);
}
