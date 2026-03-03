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
#define MAX_BUTTONS_PER_WIN 8
#define PM_LIST_MAX 32
#define WIN_CORNER_R_MAX 8

#define PM_BTN_REFRESH 1001
#define PM_BTN_FG      1002
#define PM_BTN_KILL    1003

#define RGB(r, g, b) ((uint32_t)(((r) << 16) | ((g) << 8) | (b)))

static const uint32_t COLOR_DESKTOP = RGB(0, 128, 128);
static const uint32_t COLOR_FACE = RGB(192, 192, 192);
static const uint32_t COLOR_LIGHT = RGB(255, 255, 255);
static const uint32_t COLOR_LIGHT2 = RGB(223, 223, 223);
static const uint32_t COLOR_SHADOW = RGB(128, 128, 128);
static const uint32_t COLOR_DARK = RGB(64, 64, 64);
static const uint32_t COLOR_TEXT = RGB(0, 0, 0);
static const uint32_t COLOR_LOG_BG = RGB(255, 255, 255);
static const uint32_t COLOR_ICON_TEXT = RGB(255, 255, 255);
static const uint32_t COLOR_DESKTOP_TOP = RGB(15, 68, 140);
static const uint32_t COLOR_DESKTOP_BOTTOM = RGB(29, 165, 240);
static const uint32_t COLOR_TASKBAR_TOP = RGB(80, 144, 201);
static const uint32_t COLOR_TASKBAR_BOTTOM = RGB(33, 88, 152);
static const uint32_t COLOR_TASKBAR_HILITE = RGB(176, 221, 255);
static const uint32_t COLOR_START_TOP = RGB(119, 196, 82);
static const uint32_t COLOR_START_BOTTOM = RGB(56, 128, 38);
static const uint32_t COLOR_START_TEXT = RGB(255, 255, 255);
static const uint32_t COLOR_MENU_LEFT = RGB(245, 245, 245);
static const uint32_t COLOR_MENU_RIGHT_TOP = RGB(82, 124, 172);
static const uint32_t COLOR_MENU_RIGHT_BOTTOM = RGB(47, 80, 125);
static const uint32_t COLOR_MENU_BORDER = RGB(36, 62, 98);
static const uint32_t COLOR_MENU_ITEM = RGB(32, 32, 32);
static const uint32_t COLOR_MENU_RIGHT_TEXT = RGB(244, 248, 255);
static const uint32_t COLOR_WIN_BORDER_OUT = RGB(32, 57, 88);
static const uint32_t COLOR_WIN_BORDER_IN = RGB(196, 223, 245);
static const uint32_t COLOR_WIN_GLASS_TOP_F = RGB(166, 210, 242);
static const uint32_t COLOR_WIN_GLASS_BOT_F = RGB(72, 157, 223);
static const uint32_t COLOR_WIN_GLASS_TOP_N = RGB(186, 198, 208);
static const uint32_t COLOR_WIN_GLASS_BOT_N = RGB(134, 152, 170);
static const uint32_t COLOR_WIN_CLIENT = RGB(241, 241, 241);
static const uint32_t COLOR_WIN_TEXT_F = RGB(255, 255, 255);
static const uint32_t COLOR_WIN_TEXT_N = RGB(32, 32, 32);
static const uint32_t COLOR_BTN_BLUE_TOP = RGB(132, 195, 241);
static const uint32_t COLOR_BTN_BLUE_BOT = RGB(78, 153, 218);
static const uint32_t COLOR_BTN_RED_TOP = RGB(230, 116, 95);
static const uint32_t COLOR_BTN_RED_BOT = RGB(201, 74, 47);

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
    bool minimized;
    bool maximized;
    int restore_x;
    int restore_y;
    int restore_w;
    int restore_h;
    char title[32];
    char body[GUI_MSG_TEXT_MAX];
    int button_count;
    struct {
        bool used;
        int id;
        int x;
        int y;
        int w;
        int h;
        bool pressed;
        char label[32];
    } buttons[MAX_BUTTONS_PER_WIN];
} gui_window_t;

static gui_window_t windows[8];
static int z_order[8];
static int z_count = 0;
static int focused_idx = -1;
static int next_cascade = 0;
static sys_gui_msg_t g_msg;
static char g_key_desc[16];
static char g_status_text[128];
static uint32_t g_gui_pid = 0;
static uint32_t* g_backbuf = NULL;
static int g_backbuf_w = 0;
static int g_backbuf_h = 0;
static uint32_t* g_basebuf = NULL;
static bool g_basebuf_valid = false;
static int g_pm_idx = -1;
static sys_proc_info_t g_pm_list[PM_LIST_MAX];
static int g_pm_count = 0;
static uint32_t g_pm_selected_pid = 0;
static uint32_t g_pm_last_refresh_sec = 0;
static bool g_start_menu_open = false;
static bool g_fast_title_effects = false;
static char g_clock_text[16] = "--:-- --";
static uint32_t g_clock_tick_sec = 0xffffffffu;

static const char* g_start_left_items[] = {
    "Getting Started",
    "Calculator",
    "Sticky Notes",
    "Paint",
    "Explorer",
};

static const char* g_start_right_items[] = {
    "Documents",
    "Pictures",
    "Computer",
    "Control Panel",
    "Devices and Printers",
    "Default Programs",
};

static void clamp_window_to_work(const ui_layout_t* ui, gui_window_t* win);

static bool is_proc_manager_window(const gui_window_t* win) {
    return win && win->system && win->pid == 0 && strcmp(win->title, "Process Manager") == 0;
}

static bool window_is_interactive(const gui_window_t* win) {
    if (!win) {
        return false;
    }
    if (!win->system) {
        return true;
    }
    return is_proc_manager_window(win);
}

static void update_foreground(void) {
    uint32_t pid = g_gui_pid;
    if (focused_idx >= 0 && focused_idx < (int)(sizeof(windows) / sizeof(windows[0]))) {
        if (windows[focused_idx].used && !windows[focused_idx].system) {
            pid = windows[focused_idx].pid;
        }
    }
    sys_set_foreground(pid);
}

static inline uint32_t fb_get(int x, int y) {
    if (!g_backbuf) return 0;
    if (x < 0 || y < 0 || x >= g_backbuf_w || y >= g_backbuf_h) return 0;
    return g_backbuf[(size_t)y * (size_t)g_backbuf_w + (size_t)x];
}

static inline void fb_put(int x, int y, uint32_t color) {
    if (!g_backbuf) return;
    if (x < 0 || y < 0 || x >= g_backbuf_w || y >= g_backbuf_h) return;
    g_backbuf[(size_t)y * (size_t)g_backbuf_w + (size_t)x] = color;
}

static void fb_fill(int x, int y, int w, int h, uint32_t color) {
    if (!g_backbuf || g_backbuf_w <= 0 || g_backbuf_h <= 0) {
        return;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    int x0 = x;
    int y0 = y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 <= 0 || y1 <= 0 || x0 >= g_backbuf_w || y0 >= g_backbuf_h) {
        return;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_backbuf_w) x1 = g_backbuf_w;
    if (y1 > g_backbuf_h) y1 = g_backbuf_h;
    for (int py = y0; py < y1; py++) {
        uint32_t* row = g_backbuf + (size_t)py * (size_t)g_backbuf_w + (size_t)x0;
        for (int px = x0; px < x1; px++) {
            *row++ = color;
        }
    }
}

static void fb_dither_overlay(int x, int y, int w, int h, uint32_t color,
                              uint8_t density, uint8_t phase) {
    if (!g_backbuf || g_backbuf_w <= 0 || g_backbuf_h <= 0) {
        return;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    if (density == 0) {
        return;
    }
    if (density >= 255) {
        fb_fill(x, y, w, h, color);
        return;
    }

    int x0 = x;
    int y0 = y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 <= 0 || y1 <= 0 || x0 >= g_backbuf_w || y0 >= g_backbuf_h) {
        return;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_backbuf_w) x1 = g_backbuf_w;
    if (y1 > g_backbuf_h) y1 = g_backbuf_h;
    int level = density >> 6; // 0..3

    for (int py = y0; py < y1; py++) {
        uint32_t* row = g_backbuf + (size_t)py * (size_t)g_backbuf_w + (size_t)x0;
        for (int px = x0; px < x1; px++) {
            bool paint = false;
            int p = px + py + (int)phase;
            if (level <= 0) {
                paint = ((p & 3) == 0);       // 25%
            } else if (level == 1) {
                paint = ((p & 1) == 0);       // 50%
            } else if (level == 2) {
                paint = ((p & 3) != 0);       // 75%
            } else {
                paint = true;                 // 100%
            }
            if (paint) {
                *row = color;
            }
            row++;
        }
    }
}

static uint32_t color_lerp(uint32_t a, uint32_t b, uint8_t t) {
    uint32_t ar = (a >> 16) & 0xffu;
    uint32_t ag = (a >> 8) & 0xffu;
    uint32_t ab = a & 0xffu;
    uint32_t br = (b >> 16) & 0xffu;
    uint32_t bg = (b >> 8) & 0xffu;
    uint32_t bb = b & 0xffu;
    uint32_t inv = 255u - t;
    uint32_t r = (ar * inv + br * t) / 255u;
    uint32_t g = (ag * inv + bg * t) / 255u;
    uint32_t bch = (ab * inv + bb * t) / 255u;
    return (r << 16) | (g << 8) | bch;
}

static void fb_fill_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    if (w <= 0 || h <= 0) {
        return;
    }
    if (h == 1) {
        fb_fill(x, y, w, h, top);
        return;
    }
    for (int i = 0; i < h; i++) {
        uint8_t t = (uint8_t)((i * 255) / (h - 1));
        fb_fill(x, y + i, w, 1, color_lerp(top, bottom, t));
    }
}

static void desktop_bg_draw(const ui_layout_t* ui) {
    fb_fill_vgradient(0, 0, ui->width, ui->desktop_h, COLOR_DESKTOP_TOP, COLOR_DESKTOP_BOTTOM);

    int glow_w = ui->width / 2;
    int glow_h = ui->desktop_h / 2;
    if (glow_w > 0 && glow_h > 0) {
        int glow_x = (ui->width - glow_w) / 2;
        int glow_y = ui->desktop_h / 3 - glow_h / 2;
        if (glow_y < 0) glow_y = 0;
        for (int i = 0; i < glow_h; i++) {
            uint8_t fade = (uint8_t)(180u - (uint32_t)(i * 180u) / (uint32_t)glow_h);
            fb_dither_overlay(glow_x, glow_y + i, glow_w, 1, RGB(132, 200, 255), fade, (uint8_t)i);
        }
    }
}

static bool start_button_hit(const ui_layout_t* ui, int px, int py) {
    return (px >= ui->start_x && px < ui->start_x + ui->start_w &&
            py >= ui->start_y && py < ui->start_y + ui->start_h);
}

static void start_menu_rect(const ui_layout_t* ui, int* out_x, int* out_y, int* out_w, int* out_h) {
    int menu_w = ui->font_w * 42;
    int menu_h = ui->font_h * 20;
    int min_w = ui->font_w * 34;
    int min_h = ui->font_h * 14;
    if (menu_w < min_w) menu_w = min_w;
    if (menu_h < min_h) menu_h = min_h;
    if (menu_w > ui->width - 12) menu_w = ui->width - 12;
    if (menu_h > ui->desktop_h - 8) menu_h = ui->desktop_h - 8;
    if (menu_w < 80) menu_w = 80;
    if (menu_h < 80) menu_h = 80;
    int menu_x = 4;
    int menu_y = ui->height - ui->taskbar_h - menu_h - 2;
    if (menu_y < 2) menu_y = 2;
    if (out_x) *out_x = menu_x;
    if (out_y) *out_y = menu_y;
    if (out_w) *out_w = menu_w;
    if (out_h) *out_h = menu_h;
}

static void fb_text(int x, int y, const char* text, uint32_t fg, uint32_t bg, bool transparent) {
    if (!g_backbuf || !text || !*text) {
        return;
    }
    sys_fb_text_buf_t t = {
        .x = x,
        .y = y,
        .fg = fg,
        .bg = bg,
        .flags = transparent ? SYS_FB_TEXT_TRANSPARENT : 0u,
        .text = text,
        .dst = g_backbuf,
        .dst_w = (uint32_t)g_backbuf_w,
        .dst_h = (uint32_t)g_backbuf_h,
        .dst_pitch = (uint32_t)g_backbuf_w * sizeof(uint32_t),
    };
    (void)sys_fb_draw_text_buf(&t);
}

static void fb_begin_frame(void) {
    // Backbuffer-only rendering path: nothing to reset.
}

static void fb_present(void) {
    if (!g_backbuf) {
        return;
    }
    sys_fb_blit_t blit = {
        .x = 0,
        .y = 0,
        .w = g_backbuf_w,
        .h = g_backbuf_h,
        .pixels = g_backbuf,
        .pitch = (uint32_t)g_backbuf_w * sizeof(uint32_t),
    };
    sys_fb_blit(&blit);
}

static void fb_present_rect(int x, int y, int w, int h) {
    if (!g_backbuf) {
        return;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    int x0 = x;
    int y0 = y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 <= 0 || y1 <= 0 || x0 >= g_backbuf_w || y0 >= g_backbuf_h) {
        return;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_backbuf_w) x1 = g_backbuf_w;
    if (y1 > g_backbuf_h) y1 = g_backbuf_h;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    sys_fb_blit_t blit = {
        .x = x0,
        .y = y0,
        .w = x1 - x0,
        .h = y1 - y0,
        .pixels = g_backbuf + (size_t)y0 * (size_t)g_backbuf_w + (size_t)x0,
        .pitch = (uint32_t)g_backbuf_w * sizeof(uint32_t),
    };
    sys_fb_blit(&blit);
}

static void basebuf_invalidate(void) {
    g_basebuf_valid = false;
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

typedef struct {
    uint32_t tl[WIN_CORNER_R_MAX * WIN_CORNER_R_MAX];
    uint32_t tr[WIN_CORNER_R_MAX * WIN_CORNER_R_MAX];
    uint32_t bl[WIN_CORNER_R_MAX * WIN_CORNER_R_MAX];
    uint32_t br[WIN_CORNER_R_MAX * WIN_CORNER_R_MAX];
} win_corner_cache_t;

static void win_corner_capture(const gui_window_t* win, int r, win_corner_cache_t* cache) {
    if (!win || !cache || r <= 0 || r > WIN_CORNER_R_MAX) {
        return;
    }
    for (int y = 0; y < r; y++) {
        for (int x = 0; x < r; x++) {
            int idx = y * r + x;
            cache->tl[idx] = fb_get(win->x + x, win->y + y);
            cache->tr[idx] = fb_get(win->x + win->w - r + x, win->y + y);
            cache->bl[idx] = fb_get(win->x + x, win->y + win->h - r + y);
            cache->br[idx] = fb_get(win->x + win->w - r + x, win->y + win->h - r + y);
        }
    }
}

static void win_corner_restore_round(const gui_window_t* win, int r, const win_corner_cache_t* cache) {
    if (!win || !cache || r <= 0 || r > WIN_CORNER_R_MAX) {
        return;
    }
    int rr = (r - 1) * (r - 1);
    for (int y = 0; y < r; y++) {
        for (int x = 0; x < r; x++) {
            int idx = y * r + x;
            int tl_dx = r - 1 - x;
            int tl_dy = r - 1 - y;
            if (tl_dx * tl_dx + tl_dy * tl_dy > rr) {
                fb_put(win->x + x, win->y + y, cache->tl[idx]);
            }

            int tr_dx = x;
            int tr_dy = r - 1 - y;
            if (tr_dx * tr_dx + tr_dy * tr_dy > rr) {
                fb_put(win->x + win->w - r + x, win->y + y, cache->tr[idx]);
            }

            int bl_dx = r - 1 - x;
            int bl_dy = y;
            if (bl_dx * bl_dx + bl_dy * bl_dy > rr) {
                fb_put(win->x + x, win->y + win->h - r + y, cache->bl[idx]);
            }

            int br_dx = x;
            int br_dy = y;
            if (br_dx * br_dx + br_dy * br_dy > rr) {
                fb_put(win->x + win->w - r + x, win->y + win->h - r + y, cache->br[idx]);
            }
        }
    }
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
        update_foreground();
        return;
    }
    if (!windows[idx].used) {
        return;
    }
    windows[idx].minimized = false;
    window_remove_from_z(idx);
    if (z_count < (int)(sizeof(z_order) / sizeof(z_order[0]))) {
        z_order[z_count++] = idx;
    }
    focused_idx = idx;
    update_foreground();
}

static void window_recompute_focus(void) {
    focused_idx = -1;
    for (int i = z_count - 1; i >= 0; i--) {
        int idx = z_order[i];
        if (idx < 0) {
            continue;
        }
        if (!windows[idx].used || windows[idx].minimized) {
            continue;
        }
        focused_idx = idx;
        break;
    }
    update_foreground();
}

static void window_minimize_idx(int idx) {
    if (idx < 0 || idx >= (int)(sizeof(windows) / sizeof(windows[0]))) {
        return;
    }
    if (!windows[idx].used || windows[idx].system) {
        return;
    }
    windows[idx].minimized = true;
    if (focused_idx == idx) {
        window_recompute_focus();
    } else {
        update_foreground();
    }
}

static void window_toggle_maximize(const ui_layout_t* ui, int idx) {
    if (!ui || idx < 0 || idx >= (int)(sizeof(windows) / sizeof(windows[0]))) {
        return;
    }
    gui_window_t* win = &windows[idx];
    if (!win->used || win->system) {
        return;
    }
    if (win->minimized) {
        win->minimized = false;
    }
    if (!win->maximized) {
        win->restore_x = win->x;
        win->restore_y = win->y;
        win->restore_w = win->w;
        win->restore_h = win->h;
        win->x = ui->work_x;
        win->y = ui->work_y;
        win->w = ui->work_w;
        win->h = ui->work_h;
        win->maximized = true;
    } else {
        if (win->restore_w > 0 && win->restore_h > 0) {
            win->x = win->restore_x;
            win->y = win->restore_y;
            win->w = win->restore_w;
            win->h = win->restore_h;
        }
        win->maximized = false;
    }
    clamp_window_to_work(ui, win);
    window_focus(idx);
}

static bool window_focus_cycle_client(bool reverse) {
    if (z_count <= 0) {
        return false;
    }

    int start_pos = z_count - 1;
    if (focused_idx >= 0) {
        for (int i = 0; i < z_count; i++) {
            if (z_order[i] == focused_idx) {
                start_pos = i;
                break;
            }
        }
    }

    for (int step = 1; step <= z_count; step++) {
        int pos;
        if (reverse) {
            pos = (start_pos + step) % z_count;
        } else {
            pos = (start_pos - step + z_count) % z_count;
        }
        int idx = z_order[pos];
        if (idx < 0 || idx >= (int)(sizeof(windows) / sizeof(windows[0]))) {
            continue;
        }
        gui_window_t* win = &windows[idx];
        if (!win->used || win->system) {
            continue;
        }
        if (win->minimized) {
            win->minimized = false;
        }
        window_focus(idx);
        return true;
    }
    return false;
}

static bool window_apply_snap(const ui_layout_t* ui, int idx, int px, int py) {
    if (!ui || idx < 0 || idx >= (int)(sizeof(windows) / sizeof(windows[0]))) {
        return false;
    }
    gui_window_t* win = &windows[idx];
    if (!win->used || win->system || win->minimized) {
        return false;
    }

    int margin = ui->font_h / 2 + 8;
    if (margin < 12) margin = 12;
    int left_edge = ui->work_x;
    int top_edge = ui->work_y;
    int right_edge = ui->work_x + ui->work_w - 1;
    int half_w = ui->work_w / 2;
    if (half_w < ui->win_min_w) {
        half_w = ui->win_min_w;
    }
    if (half_w > ui->work_w) {
        half_w = ui->work_w;
    }

    int old_x = win->x;
    int old_y = win->y;
    int old_w = win->w;
    int old_h = win->h;

    if (py <= top_edge + margin) {
        if (!win->maximized) {
            win->restore_x = win->x;
            win->restore_y = win->y;
            win->restore_w = win->w;
            win->restore_h = win->h;
        }
        win->x = ui->work_x;
        win->y = ui->work_y;
        win->w = ui->work_w;
        win->h = ui->work_h;
        win->maximized = true;
    } else if (px <= left_edge + margin) {
        win->x = ui->work_x;
        win->y = ui->work_y;
        win->w = half_w;
        win->h = ui->work_h;
        win->maximized = false;
    } else if (px >= right_edge - margin) {
        win->w = half_w;
        win->h = ui->work_h;
        win->x = ui->work_x + ui->work_w - win->w;
        win->y = ui->work_y;
        win->maximized = false;
    } else {
        return false;
    }

    clamp_window_to_work(ui, win);
    return (win->x != old_x || win->y != old_y || win->w != old_w || win->h != old_h);
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
        snprintf(win->title, sizeof(win->title), "App %d", (int)pid);
    }

    if (z_count < (int)(sizeof(z_order) / sizeof(z_order[0]))) {
        z_order[z_count++] = slot;
    }
    focused_idx = slot;
    update_foreground();
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
    window_recompute_focus();
}

static int window_find_at(const ui_layout_t* ui, int px, int py) {
    (void)ui;
    for (int i = z_count - 1; i >= 0; i--) {
        int idx = z_order[i];
        if (idx < 0) continue;
        gui_window_t* win = &windows[idx];
        if (!win->used || win->minimized) continue;
        if (px >= win->x && px < win->x + win->w &&
            py >= win->y && py < win->y + win->h) {
            return idx;
        }
    }
    return -1;
}

static int window_title_button_size(const ui_layout_t* ui, const gui_window_t* win) {
    int title_h = ui->win_title_h;
    if (title_h > win->h - ui->win_frame * 2) {
        title_h = win->h - ui->win_frame * 2;
    }
    int btn = title_h - 6;
    if (btn < 12) btn = 12;
    if (btn > title_h) btn = title_h;
    return btn;
}

static void window_caption_button_rects(const ui_layout_t* ui, const gui_window_t* win,
                                        int* min_x, int* max_x, int* close_x,
                                        int* out_y, int* out_w, int* out_h) {
    int title_x = win->x + ui->win_frame;
    int title_y = win->y + ui->win_frame;
    int title_w = win->w - ui->win_frame * 2;
    int title_h = ui->win_title_h;
    if (title_h > win->h - ui->win_frame * 2) {
        title_h = win->h - ui->win_frame * 2;
    }
    int btn = window_title_button_size(ui, win);
    int close = title_x + title_w - btn - 4;
    int max = close - (btn + 3);
    int min = max - (btn + 3);
    int y = title_y + (title_h - btn) / 2;
    if (min_x) *min_x = min;
    if (max_x) *max_x = max;
    if (close_x) *close_x = close;
    if (out_y) *out_y = y;
    if (out_w) *out_w = btn;
    if (out_h) *out_h = btn;
}

static bool window_hit_close(const ui_layout_t* ui, const gui_window_t* win, int px, int py) {
    int title_w = win->w - ui->win_frame * 2;
    int title_h = ui->win_title_h;
    if (title_h > win->h - ui->win_frame * 2) {
        title_h = win->h - ui->win_frame * 2;
    }
    if (title_w <= 0 || title_h <= 0) {
        return false;
    }

    int min_x, max_x, close_x, close_y, close_w, close_h;
    window_caption_button_rects(ui, win, &min_x, &max_x, &close_x, &close_y, &close_w, &close_h);
    if (px >= close_x && px < close_x + close_w &&
        py >= close_y && py < close_y + close_h) {
        return true;
    }
    return false;
}

static bool window_hit_maximize(const ui_layout_t* ui, const gui_window_t* win, int px, int py) {
    int min_x, max_x, close_x, by, bw, bh;
    window_caption_button_rects(ui, win, &min_x, &max_x, &close_x, &by, &bw, &bh);
    return (px >= max_x && px < max_x + bw && py >= by && py < by + bh);
}

static bool window_hit_minimize(const ui_layout_t* ui, const gui_window_t* win, int px, int py) {
    int min_x, max_x, close_x, by, bw, bh;
    window_caption_button_rects(ui, win, &min_x, &max_x, &close_x, &by, &bw, &bh);
    return (px >= min_x && px < min_x + bw && py >= by && py < by + bh);
}

static void window_buttons_clear(gui_window_t* win) {
    if (!win) {
        return;
    }
    win->button_count = 0;
    for (int i = 0; i < MAX_BUTTONS_PER_WIN; i++) {
        win->buttons[i].used = false;
        win->buttons[i].pressed = false;
    }
}

static bool window_add_button(gui_window_t* win, int id, int x, int y, int w, int h,
                              const char* label) {
    if (!win || win->button_count >= MAX_BUTTONS_PER_WIN) {
        return false;
    }
    int slot = -1;
    for (int i = 0; i < MAX_BUTTONS_PER_WIN; i++) {
        if (!win->buttons[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return false;
    }
    win->buttons[slot].used = true;
    win->buttons[slot].id = id;
    win->buttons[slot].x = x;
    win->buttons[slot].y = y;
    win->buttons[slot].w = w;
    win->buttons[slot].h = h;
    win->buttons[slot].pressed = false;
    if (label && *label) {
        strncpy(win->buttons[slot].label, label, sizeof(win->buttons[slot].label) - 1);
        win->buttons[slot].label[sizeof(win->buttons[slot].label) - 1] = '\0';
    } else {
        win->buttons[slot].label[0] = '\0';
    }
    win->button_count++;
    return true;
}

static bool window_hit_button(const ui_layout_t* ui, const gui_window_t* win, int px, int py,
                              int* out_btn, int* out_rel_x, int* out_rel_y) {
    if (!win || win->button_count <= 0) {
        return false;
    }
    int tx, ty, tw, th;
    window_text_area(ui, win, &tx, &ty, &tw, &th);
    if (tw <= 0 || th <= 0) {
        return false;
    }
    for (int i = 0; i < MAX_BUTTONS_PER_WIN; i++) {
        const int bx = win->buttons[i].x;
        const int by = win->buttons[i].y;
        const int bw = win->buttons[i].w;
        const int bh = win->buttons[i].h;
        if (!win->buttons[i].used || bw <= 0 || bh <= 0) {
            continue;
        }
        int ax = tx + bx;
        int ay = ty + by;
        if (px >= ax && px < ax + bw &&
            py >= ay && py < ay + bh) {
            if (out_btn) *out_btn = i;
            if (out_rel_x) *out_rel_x = px - tx;
            if (out_rel_y) *out_rel_y = py - ty;
            return true;
        }
    }
    return false;
}

static void draw_button(const ui_layout_t* ui, int x, int y, int w, int h,
                        const char* label, bool pressed) {
    if (w <= 0 || h <= 0) {
        return;
    }
    fb_fill(x, y, w, h, COLOR_FACE);
    draw_bevel(x, y, w, h, !pressed);
    if (label && *label) {
        int cols = ui->font_w ? (w / ui->font_w) : 0;
        char buf[32];
        clamp_text(buf, sizeof(buf), label, cols);
        int tx = x + (w - (int)strlen(buf) * ui->font_w) / 2;
        int ty = y + (h - ui->font_h) / 2;
        if (tx < x + 2) tx = x + 2;
        if (ty < y + 1) ty = y + 1;
        fb_text(tx, ty, buf, COLOR_TEXT, COLOR_FACE, true);
    }
}

static void draw_window_buttons(const ui_layout_t* ui, const gui_window_t* win) {
    if (!win || win->button_count <= 0) {
        return;
    }
    int tx, ty, tw, th;
    window_text_area(ui, win, &tx, &ty, &tw, &th);
    if (tw <= 0 || th <= 0) {
        return;
    }
    for (int i = 0; i < MAX_BUTTONS_PER_WIN; i++) {
        if (!win->buttons[i].used) {
            continue;
        }
        int bx = tx + win->buttons[i].x;
        int by = ty + win->buttons[i].y;
        draw_button(ui, bx, by, win->buttons[i].w, win->buttons[i].h,
                    win->buttons[i].label, win->buttons[i].pressed);
    }
}

static bool window_hit_title_bar(const ui_layout_t* ui, const gui_window_t* win, int px, int py) {
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
    if (px < title_x || px >= title_x + title_w ||
        py < title_y || py >= title_y + title_h) {
        return false;
    }
    if (window_hit_close(ui, win, px, py) ||
        window_hit_maximize(ui, win, px, py) ||
        window_hit_minimize(ui, win, px, py)) {
        return false;
    }
    return true;
}

static int window_resize_grip_size(const ui_layout_t* ui, const gui_window_t* win) {
    int grip = ui->font_h / 2 + 6;
    if (grip < 8) grip = 8;
    if (grip < 12) grip = 12;
    int max_grip = win->w / 3;
    if (grip > max_grip) grip = max_grip;
    if (grip > win->h / 3) grip = win->h / 3;
    if (grip > win->w - 4) grip = win->w - 4;
    if (grip > win->h - 4) grip = win->h - 4;
    if (grip < 4) grip = 4;
    return grip;
}

static bool window_hit_resize_grip(const ui_layout_t* ui, const gui_window_t* win, int px, int py) {
    if (!ui || !win || !win->used) {
        return false;
    }
    int grip = window_resize_grip_size(ui, win);
    int frame = ui->win_frame > 0 ? ui->win_frame : 1;
    int x0 = win->x + win->w - frame - grip;
    int y0 = win->y + win->h - frame - grip;
    int x1 = win->x + win->w - frame;
    int y1 = win->y + win->h - frame;
    if (x0 >= x1 || y0 >= y1) {
        return false;
    }
    if (px < x0 || px >= x1 || py < y0 || py >= y1) {
        return false;
    }
    return true;
}

static void draw_window_resize_grip(const ui_layout_t* ui, const gui_window_t* win, bool focused) {
    if (!ui || !win || win->w <= 0 || win->h <= 0) {
        return;
    }
    int grip = window_resize_grip_size(ui, win);
    int frame = ui->win_frame > 0 ? ui->win_frame : 1;
    int gx = win->x + win->w - frame - grip;
    int gy = win->y + win->h - frame - grip;
    uint32_t c1 = focused ? RGB(255, 255, 255) : RGB(220, 220, 220);
    uint32_t c2 = RGB(120, 120, 120);
    for (int i = 0; i < 3; i++) {
        int off = i * 4 + 1;
        int len = grip - off;
        if (len <= 2) {
            break;
        }
        fb_fill(gx + off, gy + grip - 2, len, 1, c1);
        fb_fill(gx + grip - 2, gy + off, 1, len, c1);
        fb_fill(gx + off + 1, gy + grip - 1, len - 1, 1, c2);
        fb_fill(gx + grip - 1, gy + off + 1, 1, len - 1, c2);
    }
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
    const char* argv[] = {"/cmd/explorer"};
    uint32_t pid = sys_spawn(argv[0], argv, 1);
    if (pid == 0) {
        log_push("explorer: spawn failed");
    } else {
        log_push("explorer: launched");
    }
}

static bool clock_refresh_text(void) {
    sys_rtc_time_t t = {0};
    char next[16];
    if (!sys_rtc_read(&t)) {
        strcpy(next, "--:-- --");
    } else {
        int h = (int)t.hour;
        if (h < 0) h = 0;
        if (h > 23) h = h % 24;
        const char* ap = (h >= 12) ? "PM" : "AM";
        int h12 = h % 12;
        if (h12 == 0) h12 = 12;
        snprintf(next, sizeof(next), "%d:%02d %s", h12, (int)t.min, ap);
    }
    if (strcmp(g_clock_text, next) == 0) {
        return false;
    }
    strncpy(g_clock_text, next, sizeof(g_clock_text) - 1);
    g_clock_text[sizeof(g_clock_text) - 1] = '\0';
    return true;
}

static int taskbar_collect_windows(int* out, int out_max) {
    if (!out || out_max <= 0) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < z_count && n < out_max; i++) {
        int idx = z_order[i];
        if (idx < 0) {
            continue;
        }
        if (!windows[idx].used || windows[idx].system) {
            continue;
        }
        out[n++] = idx;
    }
    return n;
}

static bool taskbar_window_button_hit(const ui_layout_t* ui, int px, int py, int* out_idx) {
    int ids[8];
    int count = taskbar_collect_windows(ids, (int)(sizeof(ids) / sizeof(ids[0])));
    if (count <= 0) {
        return false;
    }
    int x0 = ui->start_x + ui->start_w + 8;
    int y0 = ui->start_y;
    int h = ui->start_h;
    int avail = ui->clock_x - x0 - 8;
    if (avail <= 0 || h <= 0) {
        return false;
    }

    int gap = 4;
    int btn_min = ui->font_w * 9 + 12;
    if (btn_min < 72) btn_min = 72;
    int visible = (avail + gap) / (btn_min + gap);
    if (visible < 1) visible = 1;
    if (visible > count) visible = count;
    int btn_w = (avail - (visible - 1) * gap) / visible;
    if (btn_w <= 0) {
        return false;
    }

    for (int i = 0; i < visible; i++) {
        int bx = x0 + i * (btn_w + gap);
        if (px >= bx && px < bx + btn_w && py >= y0 && py < y0 + h) {
            if (out_idx) *out_idx = ids[i];
            return true;
        }
    }
    return false;
}

static void draw_taskbar(const ui_layout_t* ui, const char* status) {
    int y = ui->height - ui->taskbar_h;
    if (y < 0) y = 0;
    fb_fill_vgradient(0, y, ui->width, ui->taskbar_h, COLOR_TASKBAR_TOP, COLOR_TASKBAR_BOTTOM);
    fb_dither_overlay(0, y, ui->width, 2, COLOR_TASKBAR_HILITE, 224, 0);
    draw_frame(0, y, ui->width, ui->taskbar_h, COLOR_TASKBAR_HILITE, COLOR_MENU_BORDER);

    if (ui->start_w > 0 && ui->start_h > 0) {
        bool pressed = g_start_menu_open;
        uint32_t top = pressed ? COLOR_START_BOTTOM : COLOR_START_TOP;
        uint32_t bottom = pressed ? COLOR_START_TOP : COLOR_START_BOTTOM;
        fb_fill_vgradient(ui->start_x, ui->start_y, ui->start_w, ui->start_h, top, bottom);
        draw_bevel(ui->start_x, ui->start_y, ui->start_w, ui->start_h, !pressed);
        fb_text(ui->start_x + 8, ui->start_y + (ui->start_h - ui->font_h) / 2,
                "Start", COLOR_START_TEXT, COLOR_START_BOTTOM, true);
    }

    if (ui->clock_w > 0 && ui->clock_h > 0 && ui->clock_x < ui->width) {
        fb_fill_vgradient(ui->clock_x, ui->clock_y, ui->clock_w, ui->clock_h,
                          COLOR_TASKBAR_TOP, COLOR_TASKBAR_BOTTOM);
        draw_bevel(ui->clock_x, ui->clock_y, ui->clock_w, ui->clock_h, false);
        fb_text(ui->clock_x + 6, ui->clock_y + (ui->clock_h - ui->font_h) / 2,
                g_clock_text, COLOR_START_TEXT, COLOR_TASKBAR_BOTTOM, true);
    }

    int status_x = ui->start_x + ui->start_w + 10;
    int status_w = ui->clock_x - status_x - 8;

    int ids[8];
    int count = taskbar_collect_windows(ids, (int)(sizeof(ids) / sizeof(ids[0])));
    if (count > 0) {
        int task_x = ui->start_x + ui->start_w + 8;
        int task_y = ui->start_y;
        int task_h = ui->start_h;
        int avail = ui->clock_x - task_x - 8;
        int gap = 4;
        int btn_min = ui->font_w * 9 + 12;
        if (btn_min < 72) btn_min = 72;
        int visible = (avail + gap) / (btn_min + gap);
        if (visible < 1) visible = 1;
        if (visible > count) visible = count;
        int btn_w = (avail - (visible - 1) * gap) / visible;
        if (btn_w > 0 && task_h > 0) {
            for (int i = 0; i < visible; i++) {
                int idx = ids[i];
                bool active = (idx == focused_idx) && !windows[idx].minimized;
                int bx = task_x + i * (btn_w + gap);
                draw_button(ui, bx, task_y, btn_w, task_h, windows[idx].title, active);
            }
            status_x = task_x + visible * (btn_w + gap);
            status_w = ui->clock_x - status_x - 8;
        }
    }

    if (status_w > ui->font_w * 6) {
        char buf[128];
        const char* text = (status && *status) ? status : "Ready";
        int cols = ui->font_w ? (status_w / ui->font_w) : 0;
        clamp_text(buf, sizeof(buf), text, cols);
        fb_text(status_x, ui->start_y + (ui->start_h - ui->font_h) / 2,
                buf, COLOR_START_TEXT, COLOR_TASKBAR_BOTTOM, true);
    }
}

static void draw_start_menu(const ui_layout_t* ui) {
    if (!g_start_menu_open) {
        return;
    }
    int mx, my, mw, mh;
    start_menu_rect(ui, &mx, &my, &mw, &mh);
    int right_w = mw / 3;
    if (right_w < ui->font_w * 12) right_w = ui->font_w * 12;
    if (right_w > mw - 10) right_w = mw - 10;
    int left_w = mw - right_w;

    fb_fill(mx, my, mw, mh, COLOR_MENU_LEFT);
    fb_fill(mx + left_w, my, right_w, mh, COLOR_MENU_RIGHT_BOTTOM);
    fb_fill_vgradient(mx + left_w, my, right_w, mh, COLOR_MENU_RIGHT_TOP, COLOR_MENU_RIGHT_BOTTOM);
    draw_frame(mx, my, mw, mh, COLOR_TASKBAR_HILITE, COLOR_MENU_BORDER);

    int y = my + 10;
    int count_left = (int)(sizeof(g_start_left_items) / sizeof(g_start_left_items[0]));
    for (int i = 0; i < count_left; i++) {
        fb_text(mx + 10, y, g_start_left_items[i], COLOR_MENU_ITEM, COLOR_MENU_LEFT, true);
        y += ui->line_h + 1;
        if (y > my + mh - ui->font_h * 3) break;
    }

    y = my + 10;
    int count_right = (int)(sizeof(g_start_right_items) / sizeof(g_start_right_items[0]));
    for (int i = 0; i < count_right; i++) {
        fb_text(mx + left_w + 8, y, g_start_right_items[i],
                COLOR_MENU_RIGHT_TEXT, COLOR_MENU_RIGHT_BOTTOM, true);
        y += ui->line_h + 2;
        if (y > my + mh - ui->font_h * 3) break;
    }

    int search_h = ui->font_h + 6;
    int search_w = left_w - 20;
    if (search_w > ui->font_w * 10) {
        int sx = mx + 10;
        int sy = my + mh - search_h - 8;
        fb_fill(sx, sy, search_w, search_h, RGB(255, 255, 255));
        draw_bevel(sx, sy, search_w, search_h, false);
        fb_text(sx + 4, sy + (search_h - ui->font_h) / 2, "Search", RGB(96, 96, 96), RGB(255, 255, 255), true);
    }
}

static bool start_menu_hit(const ui_layout_t* ui, int px, int py) {
    int mx, my, mw, mh;
    start_menu_rect(ui, &mx, &my, &mw, &mh);
    return (px >= mx && px < mx + mw && py >= my && py < my + mh);
}

static bool start_menu_click(const ui_layout_t* ui, int px, int py) {
    int mx, my, mw, mh;
    start_menu_rect(ui, &mx, &my, &mw, &mh);
    if (px < mx || px >= mx + mw || py < my || py >= my + mh) {
        return false;
    }

    int right_w = mw / 3;
    if (right_w < ui->font_w * 12) right_w = ui->font_w * 12;
    if (right_w > mw - 10) right_w = mw - 10;
    int left_w = mw - right_w;

    if (px < mx + left_w) {
        int row = (py - (my + 10)) / (ui->line_h + 1);
        if (row >= 0 && row < (int)(sizeof(g_start_left_items) / sizeof(g_start_left_items[0]))) {
            if (strcmp(g_start_left_items[row], "Explorer") == 0) {
                launch_explorer();
                log_push("start: explorer");
            } else {
                log_push("start: item");
            }
            g_start_menu_open = false;
            return true;
        }
    } else {
        log_push("start: system item");
        g_start_menu_open = false;
        return true;
    }

    return true;
}

static void draw_caption_button(const ui_layout_t* ui, int x, int y, int w, int h,
                                bool close, char symbol) {
    uint32_t top = close ? COLOR_BTN_RED_TOP : COLOR_BTN_BLUE_TOP;
    uint32_t bottom = close ? COLOR_BTN_RED_BOT : COLOR_BTN_BLUE_BOT;
    fb_fill_vgradient(x, y, w, h, top, bottom);
    draw_frame(x, y, w, h, COLOR_LIGHT, COLOR_MENU_BORDER);
    int tx = x + (w - ui->font_w) / 2;
    int ty = y + (h - ui->font_h) / 2;
    char s[2] = {symbol, '\0'};
    fb_text(tx, ty, s, COLOR_WIN_TEXT_F, bottom, true);
}

static void blur_title_bar(int x, int y, int w, int h) {
    if (w <= 4 || h <= 4) return;

    uint32_t* temp = malloc((size_t)w * h * sizeof(uint32_t));
    if (!temp) return;

    // 가로 패스
    for (int py = 0; py < h; py++) {
        for (int px = 2; px < w - 2; px++) {
            uint32_t c0 = fb_get(x + px - 2, y + py);
            uint32_t c1 = fb_get(x + px - 1, y + py);
            uint32_t c2 = fb_get(x + px,     y + py);
            uint32_t c3 = fb_get(x + px + 1, y + py);
            uint32_t c4 = fb_get(x + px + 2, y + py);

            uint32_t r =
                (((c0>>16)&255)*1 +
                 ((c1>>16)&255)*4 +
                 ((c2>>16)&255)*6 +
                 ((c3>>16)&255)*4 +
                 ((c4>>16)&255)*1) >> 4;

            uint32_t g =
                (((c0>>8)&255)*1 +
                 ((c1>>8)&255)*4 +
                 ((c2>>8)&255)*6 +
                 ((c3>>8)&255)*4 +
                 ((c4>>8)&255)*1) >> 4;

            uint32_t b =
                ((c0&255)*1 +
                 (c1&255)*4 +
                 (c2&255)*6 +
                 (c3&255)*4 +
                 (c4&255)*1) >> 4;

            temp[(size_t)py * w + px] = (r<<16)|(g<<8)|b;
        }
    }

    // 세로 패스
    for (int py = 2; py < h - 2; py++) {
        for (int px = 2; px < w - 2; px++) {
            uint32_t c0 = temp[(size_t)(py-2)*w + px];
            uint32_t c1 = temp[(size_t)(py-1)*w + px];
            uint32_t c2 = temp[(size_t)py*w + px];
            uint32_t c3 = temp[(size_t)(py+1)*w + px];
            uint32_t c4 = temp[(size_t)(py+2)*w + px];

            uint32_t r =
                (((c0>>16)&255)*1 +
                 ((c1>>16)&255)*4 +
                 ((c2>>16)&255)*6 +
                 ((c3>>16)&255)*4 +
                 ((c4>>16)&255)*1) >> 4;

            uint32_t g =
                (((c0>>8)&255)*1 +
                 ((c1>>8)&255)*4 +
                 ((c2>>8)&255)*6 +
                 ((c3>>8)&255)*4 +
                 ((c4>>8)&255)*1) >> 4;

            uint32_t b =
                ((c0&255)*1 +
                 (c1&255)*4 +
                 (c2&255)*6 +
                 (c3&255)*4 +
                 (c4&255)*1) >> 4;

            fb_put(x + px, y + py, (r<<16)|(g<<8)|b);
        }
    }

    free(temp);
}

static void draw_window_frame(const ui_layout_t* ui, const gui_window_t* win, bool focused) {
    if (win->w <= 0 || win->h <= 0) {
        return;
    }
    int corner_r = 6;
    if (corner_r > WIN_CORNER_R_MAX) corner_r = WIN_CORNER_R_MAX;
    if (corner_r > win->w / 2) corner_r = win->w / 2;
    if (corner_r > win->h / 2) corner_r = win->h / 2;
    if (corner_r < 2) corner_r = 0;
    win_corner_cache_t corner_cache;
    if (corner_r > 0) {
        memset(&corner_cache, 0, sizeof(corner_cache));
        win_corner_capture(win, corner_r, &corner_cache);
    }

    uint32_t frame_top = focused ? RGB(213, 229, 244) : RGB(204, 210, 217);
    uint32_t frame_bot = focused ? RGB(177, 201, 224) : RGB(178, 186, 196);
    //fb_fill_vgradient(win->x, win->y, win->w, win->h, frame_top, frame_bot);
    fb_dither_overlay(win->x + 1, win->y + 1, win->w - 2, win->h / 3, RGB(255, 255, 255), 128, 1);
    draw_frame(win->x, win->y, win->w, win->h, COLOR_WIN_BORDER_IN, COLOR_WIN_BORDER_OUT);
    if (win->w > 2 && win->h > 2) {
        draw_frame(win->x + 1, win->y + 1, win->w - 2, win->h - 2, COLOR_LIGHT, COLOR_MENU_BORDER);
    }

    int title_x = win->x + ui->win_frame;
    int title_y = win->y + ui->win_frame;
    int title_w = win->w - ui->win_frame * 2;
    int title_h = ui->win_title_h;
    if (title_h > win->h - ui->win_frame * 2) title_h = win->h - ui->win_frame * 2;
    if (title_h < 0) title_h = 0;
    if (title_w < 0) title_w = 0;

    if (title_h > 0 && title_w > 0) {
        uint32_t top = focused ? COLOR_WIN_GLASS_TOP_F : COLOR_WIN_GLASS_TOP_N;
        uint32_t bot = focused ? COLOR_WIN_GLASS_BOT_F : COLOR_WIN_GLASS_BOT_N;
        uint32_t title_text = focused ? COLOR_WIN_TEXT_F : COLOR_WIN_TEXT_N;
        bool use_blur_glass = focused && !g_fast_title_effects && title_h > 1;
        if (use_blur_glass) {
            blur_title_bar(title_x, title_y, title_w, title_h);
            for (int i = 0; i < title_h; i++) {
                uint8_t t = (uint8_t)((i * 255) / (title_h - 1));
                uint32_t grad = color_lerp(top, bot, t);
                for (int px = 0; px < title_w; px++) {
                    int sx = title_x + px;
                    int sy = title_y + i;
                    uint32_t base = fb_get(sx, sy);
                    uint32_t mixed = color_lerp(base, grad, 96);
                    fb_put(sx, sy, mixed);
                }
            }
        } else {
            fb_fill_vgradient(title_x, title_y, title_w, title_h, top, bot);
        }

        int btn = window_title_button_size(ui, win);
        int close_x, close_y;
        int min_x = 0;
        int max_x = 0;
        window_caption_button_rects(ui, win, &min_x, &max_x, &close_x, &close_y, NULL, NULL);
        int text_x = title_x + 6;
        int text_w = min_x - text_x - 4;
        if (text_w < 0) text_w = 0;

        int text_cols = ui->font_w ? (text_w / ui->font_w) : 0;
        char title_buf[32];
        clamp_text(title_buf, sizeof(title_buf), win->title, text_cols);

        fb_text(text_x,
                title_y + (title_h - ui->font_h) / 2,
                title_buf,
                title_text,
                bot,
                true);

        if (btn > 0 && min_x >= title_x) {
            char max_sym = win->maximized ? 'r' : 'o';
            draw_caption_button(ui, min_x, close_y, btn, btn, false, '_');
            draw_caption_button(ui, max_x, close_y, btn, btn, false, max_sym);
            draw_caption_button(ui, close_x, close_y, btn, btn, true, 'x');
        }

        if (title_w > 2) {
            fb_dither_overlay(title_x + 1,
                            title_y + title_h - 1,
                            title_w - 2,
                            1,
                            COLOR_LIGHT,
                            192,
                            0);
        }
    }

    int tx, ty, tw, th;
    window_text_area(ui, win, &tx, &ty, &tw, &th);
    if (tw > 0 && th > 0) {
        fb_fill(tx, ty, tw, th, COLOR_WIN_CLIENT);
    }
    draw_window_resize_grip(ui, win, focused);
    if (corner_r > 0) {
        win_corner_restore_round(win, corner_r, &corner_cache);
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
        if (is_proc_manager_window(win)) {
            int cols = ui->font_w ? (tw / ui->font_w) : 0;
            int max_lines = ui->line_h ? (th / ui->line_h) : 0;
            if (cols < 1 || max_lines < 1) {
                draw_window_buttons(ui, win);
                return;
            }

            const char* p = win->body;
            int line = 0;
            while (*p && line < max_lines) {
                char buf[256];
                int len = 0;
                while (p[len] && p[len] != '\n' && len < (int)sizeof(buf) - 1) {
                    buf[len] = p[len];
                    len++;
                }
                buf[len] = '\0';

                int offset = 0;
                while (offset < len && line < max_lines) {
                    char out[256];
                    int chunk = len - offset;
                    if (chunk > cols) chunk = cols;
                    if (chunk > (int)sizeof(out) - 1) chunk = (int)sizeof(out) - 1;
                    memcpy(out, buf + offset, (size_t)chunk);
                    out[chunk] = '\0';
                    fb_text(tx, ty + line * ui->line_h, out, COLOR_TEXT, COLOR_LOG_BG, true);
                    line++;
                    offset += chunk;
                }

                p += len;
                if (*p == '\n') {
                    p++;
                    if (len == 0 && line < max_lines) {
                        line++;
                    }
                }
            }
            draw_window_buttons(ui, win);
            return;
        }

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
    int max_lines = ui->line_h ? (th / ui->line_h) : 0;
    if (cols < 1 || max_lines < 1) {
        draw_window_buttons(ui, win);
        return;
    }

    const char* p = win->body;
    int line = 0;
    while (*p && line < max_lines) {
        char buf[256];
        int len = 0;
        while (p[len] && p[len] != '\n' && len < (int)sizeof(buf) - 1) {
            buf[len] = p[len];
            len++;
        }
        buf[len] = '\0';

        int offset = 0;
        while (offset < len && line < max_lines) {
            char out[256];
            int chunk = len - offset;
            if (chunk > cols) chunk = cols;
            if (chunk > (int)sizeof(out) - 1) chunk = (int)sizeof(out) - 1;
            memcpy(out, buf + offset, (size_t)chunk);
            out[chunk] = '\0';
            fb_text(tx, ty + line * ui->line_h, out, COLOR_TEXT, COLOR_LOG_BG, true);
            line++;
            offset += chunk;
        }

        p += len;
        if (*p == '\n') {
            p++;
            if (len == 0 && line < max_lines) {
                line++;
            }
        }
    }

    draw_window_buttons(ui, win);
}

static void draw_windows(const ui_layout_t* ui) {
    for (int i = 0; i < z_count; i++) {
        int idx = z_order[i];
        if (idx < 0) continue;
        gui_window_t* win = &windows[idx];
        if (!win->used || win->minimized) continue;
        bool focused = (idx == focused_idx);
        draw_window_frame(ui, win, focused);
        draw_window_content(ui, win);
    }
}

static void draw_base_ui(const ui_layout_t* ui) {
    desktop_bg_draw(ui);
    draw_desktop_icons(ui);
}

static void basebuf_ensure(const ui_layout_t* ui) {
    if (!g_basebuf || !g_backbuf) {
        return;
    }
    if (g_basebuf_valid) {
        return;
    }

    uint32_t* saved = g_backbuf;
    g_backbuf = g_basebuf;
    draw_base_ui(ui);
    g_backbuf = saved;
    g_basebuf_valid = true;
}

static void draw_full_ui(const ui_layout_t* ui, const char* status) {
    if (g_basebuf && g_backbuf && g_backbuf_w > 0 && g_backbuf_h > 0) {
        basebuf_ensure(ui);
        memcpy(g_backbuf, g_basebuf, (size_t)g_backbuf_w * (size_t)g_backbuf_h * sizeof(uint32_t));
    } else {
        draw_base_ui(ui);
    }
    draw_windows(ui);
    draw_start_menu(ui);
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

static const char* pm_state_name(uint32_t state) {
    switch (state) {
        case 1: return "ready";
        case 2: return "run";
        case 3: return "block";
        case 4: return "zombie";
        case 5: return "exit";
        default: return "?";
    }
}

static void pm_refresh_body(void) {
    if (g_pm_idx < 0 || g_pm_idx >= (int)(sizeof(windows) / sizeof(windows[0]))) {
        return;
    }
    gui_window_t* win = &windows[g_pm_idx];
    if (!win->used || !is_proc_manager_window(win)) {
        return;
    }

    int n = sys_proc_list(g_pm_list, PM_LIST_MAX);
    if (n < 0) {
        n = 0;
    }
    g_pm_count = (n > PM_LIST_MAX) ? PM_LIST_MAX : n;

    bool selected_alive = false;
    for (int i = 0; i < g_pm_count; i++) {
        if (g_pm_list[i].pid == g_pm_selected_pid) {
            selected_alive = true;
            break;
        }
    }
    if (!selected_alive) {
        g_pm_selected_pid = (g_pm_count > 0) ? g_pm_list[0].pid : 0;
    }

    int off = 0;
    int cap = (int)sizeof(win->body);
    off += snprintf(win->body + off, cap - off, "PID  ST     NAME\n");
    off += snprintf(win->body + off, cap - off, "----------------------\n");
    for (int i = 0; i < g_pm_count && off < cap - 1; i++) {
        char mark = (g_pm_list[i].pid == g_pm_selected_pid) ? '>' : ' ';
        g_pm_list[i].name[31] = '\0';
        off += snprintf(win->body + off, cap - off, "%c%d %s %s\n",
                        mark, (int)g_pm_list[i].pid, pm_state_name(g_pm_list[i].state),
                        g_pm_list[i].name[0] ? g_pm_list[i].name : "(unnamed)");
    }
    if (g_pm_count == 0 && off < cap - 1) {
        off += snprintf(win->body + off, cap - off, "(no process)\n");
    }
    if (off < cap - 1) {
        if (g_pm_selected_pid) {
            snprintf(win->body + off, cap - off, "\nSelected: %d", (int)g_pm_selected_pid);
        } else {
            snprintf(win->body + off, cap - off, "\nSelected: -");
        }
    }
}

static void pm_handle_button(int id) {
    if (id == PM_BTN_REFRESH) {
        pm_refresh_body();
        log_push("pm: refreshed");
        return;
    }
    if (g_pm_selected_pid == 0) {
        log_push("pm: no selected pid");
        return;
    }
    if (id == PM_BTN_FG) {
        if (!sys_set_foreground(g_pm_selected_pid)) {
            log_push("pm: fg failed");
        } else {
            log_push("pm: foreground set");
        }
        return;
    }
    if (id == PM_BTN_KILL) {
        if (g_pm_selected_pid == g_gui_pid) {
            log_push("pm: refuse kill gui");
            return;
        }
        int rc = sys_kill(g_pm_selected_pid, 0);
        if (rc == SYS_KILL_OK) {
            log_push("pm: kill sent");
        } else {
            log_push("pm: kill failed");
        }
        pm_refresh_body();
    }
}

static bool pm_select_row_at(const ui_layout_t* ui, const gui_window_t* win, int rel_y) {
    if (!win || !is_proc_manager_window(win)) {
        return false;
    }
    int line_h = ui->line_h > 0 ? ui->line_h : 18;
    int rows_y = 2 * line_h;
    if (rel_y < rows_y) {
        return false;
    }
    int row = (rel_y - rows_y) / line_h;
    if (row < 0 || row >= g_pm_count) {
        return false;
    }
    g_pm_selected_pid = g_pm_list[row].pid;
    pm_refresh_body();
    return true;
}

static void build_status_text(const ui_layout_t* ui, const char* key_desc,
                              const sys_mouse_state_t* mouse, char* out, int size) {
    if (size <= 1) {
        return;
    }
    (void)ui;
    char btns[8];
    format_buttons(mouse ? mouse->buttons : 0, btns, sizeof(btns));
    int px = mouse ? mouse->x : 0;
    int py = mouse ? mouse->y : 0;

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
// GUI_MSG_BUTTON_ADD: a=x, b=y, c=(w<<16)|h, d=button_id, text=label
// GUI_MSG_BUTTON_CLEAR: clear all buttons
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
        case GUI_MSG_BUTTON_ADD: {
            if (idx < 0) {
                idx = window_create(ui, msg->sender_pid, -1, -1, -1, -1, NULL, false);
            }
            if (idx >= 0) {
                int bw = (int)((msg->c >> 16) & 0xffff);
                int bh = (int)(msg->c & 0xffff);
                if (bw <= 0) bw = ui->font_w * 8 + 12;
                if (bh <= 0) bh = ui->font_h + 8;
                if (window_add_button(&windows[idx], msg->d, msg->a, msg->b, bw, bh, msg->text)) {
                    dirty = true;
                }
            }
            break;
        }
        case GUI_MSG_BUTTON_CLEAR: {
            if (idx >= 0) {
                window_buttons_clear(&windows[idx]);
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
        case GUI_MSG_SYS_ALT_TAB: {
            if (window_focus_cycle_client(msg->a != 0)) {
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

/// @brief 
/// @param  
/// @return 
int main(void) {
    sys_fb_info_t fb;
    if (!sys_fb_info(&fb)) {
        sys_kprint("gui: framebuffer unavailable\n");
        return 1;
    }

    if (fb.width > 0 && fb.height > 0) {
        uint64_t pixels = (uint64_t)fb.width * (uint64_t)fb.height;
        uint64_t bytes = pixels * sizeof(uint32_t);
        if (pixels <= 0xffffffffu && bytes <= 0xffffffffu) {
            g_backbuf = (uint32_t*)malloc((size_t)bytes);
            if (g_backbuf) {
                g_backbuf_w = (int)fb.width;
                g_backbuf_h = (int)fb.height;
                memset(g_backbuf, 0, (size_t)bytes);
                g_basebuf = (uint32_t*)malloc((size_t)bytes);
                if (g_basebuf) {
                    memset(g_basebuf, 0, (size_t)bytes);
                }
                g_basebuf_valid = false;
            } else {
                sys_kprint("gui: backbuffer alloc failed\n");
                return 1;
            }
        } else {
            sys_kprint("gui: framebuffer too large for backbuffer\n");
            return 1;
        }
    }

    if (!sys_gui_bind()) {
        sys_kprint("gui: already running\n");
        if (g_backbuf) {
            free(g_backbuf);
            g_backbuf = NULL;
        }
        if (g_basebuf) {
            free(g_basebuf);
            g_basebuf = NULL;
        }
        return 1;
    }
    g_gui_pid = sys_getpid();
    sys_set_foreground(g_gui_pid);

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

    int pm_w = ui.default_win_w;
    int pm_h = ui.default_win_h;
    int pm_x = ui.work_x + ui.work_w - pm_w - ui.margin;
    int pm_y = ui.margin;
    g_pm_idx = window_create(&ui, 0, pm_x, pm_y, pm_w, pm_h, "Process Manager", true);
    if (g_pm_idx >= 0) {
        gui_window_t* pm = &windows[g_pm_idx];
        int bw = ui.font_w * 8 + 12;
        int bh = ui.font_h + 8;
        if (bw < 56) bw = 56;
        if (bh < 18) bh = 18;
        (void)window_add_button(pm, PM_BTN_REFRESH, 0, 0, bw, bh, "Refresh");
        (void)window_add_button(pm, PM_BTN_FG, bw + 6, 0, bw, bh, "FG");
        (void)window_add_button(pm, PM_BTN_KILL, (bw + 6) * 2, 0, bw, bh, "Kill");
        pm_refresh_body();
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
    bool dragging = false;
    int drag_idx = -1;
    int drag_off_x = 0;
    int drag_off_y = 0;
    bool resizing = false;
    int resize_idx = -1;
    int resize_start_mouse_x = 0;
    int resize_start_mouse_y = 0;
    int resize_start_w = 0;
    int resize_start_h = 0;
    int pressed_win = -1;
    int pressed_btn = -1;

    format_key(0, g_key_desc, sizeof(g_key_desc));
    (void)clock_refresh_text();
    build_status_text(&ui, g_key_desc, &mouse, g_status_text, sizeof(g_status_text));
    fb_begin_frame();
    draw_full_ui(&ui, g_status_text);
    fb_present();
    sys_mouse_draw(1);

    bool running = true;
    while (running) {
        bool windows_dirty = false;
        bool status_dirty = false;
        bool move_present_dirty = false;
        int move_x0 = 0;
        int move_y0 = 0;
        int move_x1 = 0;
        int move_y1 = 0;
        uint32_t now_sec = sys_uptime_seconds();

        if (now_sec != g_clock_tick_sec) {
            g_clock_tick_sec = now_sec;
            if (clock_refresh_text()) {
                status_dirty = true;
            }
        }

        while (sys_gui_recv(&g_msg)) {
            g_msg.text[GUI_MSG_TEXT_MAX - 1] = '\0';
            if (handle_gui_message(&ui, &g_msg)) {
                windows_dirty = true;
            }
        }

        if (g_pm_idx >= 0) {
            if (!dragging && !resizing && now_sec != g_pm_last_refresh_sec) {
                g_pm_last_refresh_sec = now_sec;
                pm_refresh_body();
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
                format_key(key, g_key_desc, sizeof(g_key_desc));
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
                int prev_x = last_mouse_x;
                int prev_y = last_mouse_y;
                int prev_buttons = last_buttons;
                int px = cur_mouse.x;
                int py = cur_mouse.y;
                bool left_down = (cur_mouse.buttons & 1) != 0;
                bool left_pressed = left_down && !(prev_buttons & 1);
                bool left_released = !left_down && (prev_buttons & 1);

                if (left_pressed) {
                    bool consumed = false;
                    if (start_button_hit(&ui, px, py)) {
                        g_start_menu_open = !g_start_menu_open;
                        basebuf_invalidate();
                        windows_dirty = true;
                        consumed = true;
                    } else if (g_start_menu_open) {
                        if (start_menu_hit(&ui, px, py)) {
                            (void)start_menu_click(&ui, px, py);
                            basebuf_invalidate();
                            windows_dirty = true;
                            consumed = true;
                        } else {
                            g_start_menu_open = false;
                            basebuf_invalidate();
                            windows_dirty = true;
                        }
                    }
                    if (!consumed) {
                        int task_idx = -1;
                        if (taskbar_window_button_hit(&ui, px, py, &task_idx)) {
                            if (task_idx >= 0 && task_idx < (int)(sizeof(windows) / sizeof(windows[0])) &&
                                windows[task_idx].used && !windows[task_idx].system) {
                                if (windows[task_idx].minimized) {
                                    windows[task_idx].minimized = false;
                                    window_focus(task_idx);
                                } else if (focused_idx == task_idx) {
                                    window_minimize_idx(task_idx);
                                } else {
                                    window_focus(task_idx);
                                }
                                windows_dirty = true;
                                consumed = true;
                            }
                        }
                    }

                    if (!consumed) {
                        int hit = window_find_at(&ui, px, py);
                        if (hit >= 0) {
                            if (!windows[hit].system && window_hit_close(&ui, &windows[hit], px, py)) {
                                window_destroy(hit);
                            } else if (!windows[hit].system &&
                                       window_hit_maximize(&ui, &windows[hit], px, py)) {
                                window_toggle_maximize(&ui, hit);
                            } else if (!windows[hit].system &&
                                       window_hit_minimize(&ui, &windows[hit], px, py)) {
                                window_minimize_idx(hit);
                            } else {
                                window_focus(hit);
                                if (window_hit_resize_grip(&ui, &windows[hit], px, py)) {
                                    resizing = true;
                                    resize_idx = hit;
                                    resize_start_mouse_x = px;
                                    resize_start_mouse_y = py;
                                    resize_start_w = windows[hit].w;
                                    resize_start_h = windows[hit].h;
                                    dragging = false;
                                    drag_idx = -1;
                                    windows_dirty = true;
                                } else if (window_is_interactive(&windows[hit])) {
                                    int btn_idx = -1;
                                    int rel_x = 0;
                                    int rel_y = 0;
                                    if (window_hit_button(&ui, &windows[hit], px, py,
                                                          &btn_idx, &rel_x, &rel_y)) {
                                        windows[hit].buttons[btn_idx].pressed = true;
                                        pressed_win = hit;
                                        pressed_btn = btn_idx;
                                        windows_dirty = true;
                                    } else {
                                        int tx, ty, tw, th;
                                        window_text_area(&ui, &windows[hit], &tx, &ty, &tw, &th);
                                        if (window_hit_title_bar(&ui, &windows[hit], px, py)) {
                                            dragging = true;
                                            drag_idx = hit;
                                            drag_off_x = px - windows[hit].x;
                                            drag_off_y = py - windows[hit].y;
                                            resizing = false;
                                            resize_idx = -1;
                                        } else if (px >= tx && px < tx + tw &&
                                                   py >= ty && py < ty + th) {
                                            if (is_proc_manager_window(&windows[hit])) {
                                                if (pm_select_row_at(&ui, &windows[hit], py - ty)) {
                                                    windows_dirty = true;
                                                }
                                            } else {
                                                sys_gui_msg_t evt;
                                                memset(&evt, 0, sizeof(evt));
                                                evt.type = GUI_MSG_CLICK;
                                                evt.a = px - tx;
                                                evt.b = py - ty;
                                                evt.c = cur_mouse.buttons;
                                                int ok = sys_gui_send_to(windows[hit].pid, &evt);
                                                if (!ok) {
                                                    log_push("gui: send click failed");
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            windows_dirty = true;
                        } else if (icon_hit_explorer(&ui, px, py)) {
                            launch_explorer();
                            windows_dirty = true;
                        }
                    }
                }

                if (left_released) {
                    if (resizing) {
                        resizing = false;
                        resize_idx = -1;
                    }
                    if (pressed_win >= 0 && pressed_btn >= 0 &&
                        pressed_win < (int)(sizeof(windows) / sizeof(windows[0])) &&
                        windows[pressed_win].used) {
                        int btn_idx = pressed_btn;
                        windows[pressed_win].buttons[btn_idx].pressed = false;

                        int rel_x = 0;
                        int rel_y = 0;
                        if (window_hit_button(&ui, &windows[pressed_win], px, py,
                                              &btn_idx, &rel_x, &rel_y) &&
                            btn_idx == pressed_btn) {
                            if (is_proc_manager_window(&windows[pressed_win])) {
                                pm_handle_button(windows[pressed_win].buttons[btn_idx].id);
                                windows_dirty = true;
                            } else {
                                sys_gui_msg_t evt;
                                memset(&evt, 0, sizeof(evt));
                                evt.type = GUI_MSG_BUTTON_CLICK;
                                evt.a = windows[pressed_win].buttons[btn_idx].id;
                                evt.b = rel_x;
                                evt.c = rel_y;
                                int ok = sys_gui_send_to(windows[pressed_win].pid, &evt);
                                if (!ok) {
                                    log_push("gui: send button click failed");
                                }
                            }
                        }
                        pressed_win = -1;
                        pressed_btn = -1;
                        windows_dirty = true;
                    }
                }

                if (resizing) {
                    if (left_released) {
                        resizing = false;
                        resize_idx = -1;
                    } else if (resize_idx >= 0 &&
                               resize_idx < (int)(sizeof(windows) / sizeof(windows[0])) &&
                               windows[resize_idx].used) {
                        gui_window_t* rw = &windows[resize_idx];
                        int old_w = rw->w;
                        int old_h = rw->h;
                        int old_x = rw->x;
                        int old_y = rw->y;
                        int nw = resize_start_w + (px - resize_start_mouse_x);
                        int nh = resize_start_h + (py - resize_start_mouse_y);
                        rw->w = nw;
                        rw->h = nh;
                        clamp_window_to_work(&ui, rw);
                        if (rw->w != old_w || rw->h != old_h || rw->x != old_x || rw->y != old_y) {
                            rw->maximized = false;
                            windows_dirty = true;
                            int pad = 8;
                            int ox0 = old_x - pad;
                            int oy0 = old_y - pad;
                            int ox1 = old_x + old_w + pad;
                            int oy1 = old_y + old_h + pad;
                            int nx0 = rw->x - pad;
                            int ny0 = rw->y - pad;
                            int nx1 = rw->x + rw->w + pad;
                            int ny1 = rw->y + rw->h + pad;
                            if (!move_present_dirty) {
                                move_x0 = ox0;
                                move_y0 = oy0;
                                move_x1 = ox1;
                                move_y1 = oy1;
                                move_present_dirty = true;
                            }
                            if (nx0 < move_x0) move_x0 = nx0;
                            if (ny0 < move_y0) move_y0 = ny0;
                            if (nx1 > move_x1) move_x1 = nx1;
                            if (ny1 > move_y1) move_y1 = ny1;
                        }
                    } else {
                        resizing = false;
                        resize_idx = -1;
                    }
                }

                if (dragging) {
                    if (left_released) {
                        if (drag_idx >= 0 &&
                            drag_idx < (int)(sizeof(windows) / sizeof(windows[0])) &&
                            windows[drag_idx].used) {
                            if (window_apply_snap(&ui, drag_idx, px, py)) {
                                windows_dirty = true;
                            }
                        }
                        dragging = false;
                        drag_idx = -1;
                    } else if (drag_idx >= 0 &&
                               drag_idx < (int)(sizeof(windows) / sizeof(windows[0])) &&
                               windows[drag_idx].used) {
                        int old_x = windows[drag_idx].x;
                        int old_y = windows[drag_idx].y;
                        windows[drag_idx].x = px - drag_off_x;
                        windows[drag_idx].y = py - drag_off_y;
                        clamp_window_to_work(&ui, &windows[drag_idx]);
                        if (windows[drag_idx].x != old_x || windows[drag_idx].y != old_y) {
                            windows[drag_idx].maximized = false;
                            windows_dirty = true;
                            int pad = 8;
                            int ox0 = old_x - pad;
                            int oy0 = old_y - pad;
                            int ox1 = old_x + windows[drag_idx].w + pad;
                            int oy1 = old_y + windows[drag_idx].h + pad;
                            int nx0 = windows[drag_idx].x - pad;
                            int ny0 = windows[drag_idx].y - pad;
                            int nx1 = windows[drag_idx].x + windows[drag_idx].w + pad;
                            int ny1 = windows[drag_idx].y + windows[drag_idx].h + pad;
                            if (!move_present_dirty) {
                                move_x0 = ox0;
                                move_y0 = oy0;
                                move_x1 = ox1;
                                move_y1 = oy1;
                                move_present_dirty = true;
                            }
                            if (nx0 < move_x0) move_x0 = nx0;
                            if (ny0 < move_y0) move_y0 = ny0;
                            if (nx1 > move_x1) move_x1 = nx1;
                            if (ny1 > move_y1) move_y1 = ny1;
                        }
                    } else {
                        dragging = false;
                        drag_idx = -1;
                    }
                }
                last_mouse_x = cur_mouse.x;
                last_mouse_y = cur_mouse.y;
                last_buttons = cur_mouse.buttons;
                mouse = cur_mouse;
                int dx = cur_mouse.x - prev_x;
                if (dx < 0) dx = -dx;
                int dy = cur_mouse.y - prev_y;
                if (dy < 0) dy = -dy;
                bool button_changed = (cur_mouse.buttons != prev_buttons);
                if (button_changed || ((!dragging && !resizing) && (dx >= 2 || dy >= 2))) {
                    status_dirty = true;
                }
            }
        }

        if (windows_dirty || status_dirty) {
            build_status_text(&ui, g_key_desc, &mouse, g_status_text, sizeof(g_status_text));
        }
        if (windows_dirty) {
            g_fast_title_effects = dragging || resizing;
            sys_mouse_draw(0);
            fb_begin_frame();
            draw_full_ui(&ui, g_status_text);
            if ((dragging || resizing) && move_present_dirty) {
                fb_present_rect(move_x0, move_y0, move_x1 - move_x0, move_y1 - move_y0);
            } else {
                fb_present();
            }
            sys_mouse_draw(1);
            g_fast_title_effects = false;
        } else if (status_dirty) {
            basebuf_invalidate();
            sys_mouse_draw(0);
            fb_begin_frame();
            draw_taskbar(&ui, g_status_text);
            fb_present();
            sys_mouse_draw(1);
        }

        sys_yield();
        sleep(0.5);
    }

    sys_mouse_draw(1);
    sys_cursor_visible(1);
    sys_clear_screen();
    if (g_backbuf) {
        free(g_backbuf);
        g_backbuf = NULL;
    }
    if (g_basebuf) {
        free(g_basebuf);
        g_basebuf = NULL;
    }
    return 0;
}

void _start(void) {
    int rc = main();
    sys_exit((uint32_t)rc);
}
