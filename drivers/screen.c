#include "screen.h"
#include "hal.h"
#include "../mm/mem.h"
#include "../kernel/log.h"
#include "../libc/string.h"
#include "font.h"
#include <stdarg.h>
#include <stdint.h>

#ifndef FB_SCROLL_USE_MEMMOVE
#define FB_SCROLL_USE_MEMMOVE 0
#endif

#define FB_CURSOR_STYLE_INVERT 0
#define FB_CURSOR_STYLE_UNDERLINE 1
#ifndef FB_CURSOR_STYLE
#define FB_CURSOR_STYLE FB_CURSOR_STYLE_UNDERLINE
#endif
#ifndef FB_CURSOR_UNDERLINE_HEIGHT
#define FB_CURSOR_UNDERLINE_HEIGHT 2u
#endif
#ifndef FB_CURSOR_BLINK_TICKS
#define FB_CURSOR_BLINK_TICKS 50u
#endif

/* Declaration of private functions */
int get_cursor_offset();
void set_cursor_offset(int offset);
int print_char(char c, int col, int row, char attr);
int get_offset(int col, int row);
int get_offset_row(int offset);
int get_offset_col(int offset);
static int screen_visible_start(void);
static int screen_compute_start_for_scroll(int scroll_pos_value);
static void screen_set_geometry(int cols, int rows);
static void screen_update_textbuf_cell(int scr_row, int scr_col, uint16_t cell);
static void screen_append_blank_line(uint8_t attr);
static void screen_draw_cell(int col, int row, uint16_t cell);
static void fb_draw_cell(int col, int row, uint16_t cell);
static void fb_cursor_erase_at(int col, int row);
static void fb_cursor_erase_at_start(int start, int col, int row);
static void fb_cursor_draw_at(int col, int row);
static uint16_t screen_get_at_start(int start, int x, int y);
static void fb_redraw_rows(int start, int row_start, int row_count);
static bool fb_scroll_view(int old_start, int new_start);
static uint32_t fb_palette_color(uint8_t index);
static inline void hydrate_buffer_from_vga_once(void);
static void redraw_from_buffer(void);
static uint8_t g_text_fg = WHITE;
static uint8_t g_text_bg = BLACK;
static uint16_t textbuf[MAX_SCROLL_LINES][SCREEN_MAX_COLS];
static int buf_hydrated = 0;
static int total_lines = 0;   // 지금까지 출력된 줄 수
static int scroll_pos = 0;    // 화면에 보여줄 시작 줄
uint8_t g_text_attr = 0x07;
static int screen_cols = MAX_COLS;
static int screen_rows = MAX_ROWS;

typedef struct {
    uint8_t* addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t bpp;
    uint8_t bytes_per_pixel;
    bool enabled;
} fb_state_t;

static fb_state_t g_fb = {0};
static int fb_cursor_row = 0;
static int fb_cursor_col = 0;
static bool fb_cursor_visible = true;
static bool cursor_user_visible = true;
static bool cursor_blink_enabled = true;
static bool cursor_blink_state = true;
static uint32_t cursor_blink_ticks = 0;

/**********************************************************
 * Public Kernel API functions                            *
 **********************************************************/

/**
 * Print a message on the specified location
 * If col, row, are negative, we will use the current offset
 */

void print_dec(uint32_t num) {
    char buf[16];   // 32bit 최대 4294967295 → 10자리 + 널문자
    int i = 0;

    if (num == 0) {
        kprint("0");
        return;
    }

    while (num > 0) {
        buf[i++] = '0' + (num % 10);
        num /= 10;
    }

    // 거꾸로 출력
    for (int j = i - 1; j >= 0; j--) {
        char s[2] = { buf[j], 0 };
        kprint(s);
    }
}

void kprint_at(const char *message, int col, int row) {
    if (!message) {
        kprint("kprint_at: NULL message!\n");
        return;
    }

    int offset;
    if (col >= 0 && row >= 0)
        offset = get_offset(col, row);
    else {
        offset = get_cursor_offset();
        row = get_offset_row(offset);
        col = get_offset_col(offset);
    }

    int i = 0;
    while (message[i] != 0) {
        offset = print_char(message[i++], col, row, 0);
        row = get_offset_row(offset);
        col = get_offset_col(offset);
    }
}

void kprint(const char *message) {
    bootlog_add(message);
    kprint_at(message, -1, -1);
}

void kprint_float(double value) {
    // 음수 처리
    if (value < 0) {
        putchar('-');
        value = -value;
    }

    int int_part = (int)value;
    double frac = value - int_part;

    // 정수면 그대로 출력
    if (frac < 0.000001) {
        kprint_int(int_part);
        return;
    }

    // 소수점 이하 계산 (최대 6자리)
    int frac_int = (int)(frac * 1000000 + 0.5);
    char buf[16];
    int len = 0;

    // 뒤쪽 0 제거
    while (frac_int > 0) {
        buf[len++] = '0' + (frac_int % 10);
        frac_int /= 10;
    }
    while (len > 0 && buf[len - 1] == '0') len--;

    if (len == 0) {
        kprint_int(int_part);
        kprint(".0");
        return;
    }

    kprint_int(int_part);
    putchar('.');
    for (int i = len - 1; i >= 0; i--) putchar(buf[i]);
}

void kprint_backspace(void) {
    int cur = get_cursor_offset();
    if (cur <= input_start_offset) {
        // 프롬프트 이전으로는 못 지움
        return;
    }

    int prev = cur - 2;  // 이전 셀(문자 1개 = 2바이트)
    // 커서를 한 칸 뒤로 이동
    set_cursor_offset(prev);

    // 그 자리를 공백으로 덮어서 “지워진” 효과
    print_char(' ', -1, -1, WHITE_ON_BLACK);

    // 커서를 지운 자리로 위치 고정
    set_cursor_offset(prev);
}

void print_hex(uint32_t num) {
    char str[11] = "0x00000000";
    const char* hex = "0123456789ABCDEF";

    for (int i = 9; i >= 2; i--) {
        str[i] = hex[num & 0xF];
        num >>= 4;
    }
    kprint(str);
}

void print_hex_pad(uint32_t val, int width) {
    char hex[16];
    itoa(val, hex, 16); // 너네 libc/string.h 에 있는 itoa 쓰면 됨
    int len = strlen(hex);
    for (int i = 0; i < width - len; i++)
        kprint("0");     // 앞에 0 채움
    kprint(hex);
}

void print_byte(uint8_t val) {
    const char* hex = "0123456789ABCDEF";
    char out[3];
    out[0] = hex[(val >> 4) & 0xF];
    out[1] = hex[val & 0xF];
    out[2] = '\0';
    kprint(out);
}

void print_offset(uint32_t val) {
    if (val < 0x1000) kprint("0");
    if (val < 0x100)  kprint("0");
    if (val < 0x10)   kprint("0");
    print_hex(val);   // 기존 print_hex는 1개 인자만 받는다고 했으니 그대로 사용
}

int int_to_str(int value, char *buf) {
    char tmp[16];
    int i = 0, neg = 0;

    if (value < 0) {
        neg = 1;
        value = -value;
    }

    do {
        tmp[i++] = '0' + (value % 10);
        value /= 10;
    } while (value > 0);

    int len = 0;
    if (neg) buf[len++] = '-';
    while (i--) buf[len++] = tmp[i];
    buf[len] = '\0';
    return len;
}

int uint_to_str(uint32_t value, char *buf) {
    char tmp[16];
    int i = 0;

    do {
        tmp[i++] = '0' + (value % 10);
        value /= 10;
    } while (value > 0);

    int len = 0;
    while (i--) buf[len++] = tmp[i];
    buf[len] = '\0';
    return len;
}

int hex_to_str(uint32_t value, char *buf, bool upper) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[16];
    int i = 0;

    do {
        tmp[i++] = digits[value & 0xF];
        value >>= 4;
    } while (value > 0);

    int len = 0;
    buf[len++] = '0';
    buf[len++] = 'x';
    while (i--) buf[len++] = tmp[i];
    buf[len] = '\0';
    return len;
}

void kprint_int(uint32_t num) {
    char buf[12];
    int_to_str(num, buf);
    kprint(buf);
}

uint8_t vga_attr(uint8_t fg, uint8_t bg) {
    return (bg << 4) | (fg & 0x0F);
}

void set_color(uint8_t fg, uint8_t bg) {
    g_text_fg = fg;
    g_text_bg = bg;
}

int putchar(int c) {
    uint8_t attr = vga_attr(g_text_fg, g_text_bg);

    print_char((char)c, -1, -1, attr);
    return (uint8_t)c;
}

int putchar_color(uint8_t ch, uint8_t fg, uint8_t bg) {
    uint8_t attr = vga_attr(fg, bg);
    print_char((char)ch, -1, -1, attr);
    return ch;
}

void kprint_color(const char* message, uint8_t fg, uint8_t bg) {
    bootlog_add(message);
    uint8_t attr = vga_attr(fg, bg);

    for (int i = 0; message[i] != 0; i++) {
        print_char(message[i], -1, -1, attr);
    }
}

uint8_t color_current() {
    return (g_text_bg << 4) | (g_text_fg & 0x0F);
}

bool screen_is_framebuffer(void) {
    return g_fb.enabled;
}

int screen_get_cols(void) {
    return screen_cols;
}

int screen_get_rows(void) {
    return screen_rows;
}

void screen_set_cursor_visible(bool visible) {
    cursor_user_visible = visible;
    if (visible) {
        cursor_blink_state = true;
        cursor_blink_ticks = 0;
    }

    if (!g_fb.enabled)
        return;

    bool effective = cursor_user_visible && (!cursor_blink_enabled || cursor_blink_state);
    if (fb_cursor_visible == effective)
        return;
    fb_cursor_visible = effective;
    if (fb_cursor_visible) fb_cursor_draw_at(fb_cursor_col, fb_cursor_row);
    else fb_cursor_erase_at(fb_cursor_col, fb_cursor_row);
}

void screen_set_cursor_blink(bool enabled) {
    cursor_blink_enabled = enabled;
    cursor_blink_state = true;
    cursor_blink_ticks = 0;
    if (!g_fb.enabled)
        return;
    bool effective = cursor_user_visible && (!cursor_blink_enabled || cursor_blink_state);
    if (fb_cursor_visible == effective)
        return;
    fb_cursor_visible = effective;
    if (fb_cursor_visible) fb_cursor_draw_at(fb_cursor_col, fb_cursor_row);
    else fb_cursor_erase_at(fb_cursor_col, fb_cursor_row);
}

void screen_cursor_blink_tick(void) {
    if (!g_fb.enabled)
        return;
    if (!cursor_blink_enabled || !cursor_user_visible)
        return;
    cursor_blink_ticks++;
    if (cursor_blink_ticks < FB_CURSOR_BLINK_TICKS)
        return;
    cursor_blink_ticks = 0;
    cursor_blink_state = !cursor_blink_state;
    bool effective = cursor_user_visible && cursor_blink_state;
    if (fb_cursor_visible == effective)
        return;
    fb_cursor_visible = effective;
    if (fb_cursor_visible) fb_cursor_draw_at(fb_cursor_col, fb_cursor_row);
    else fb_cursor_erase_at(fb_cursor_col, fb_cursor_row);
}

static const uint32_t fb_palette[16] = {
    0x000000, 0x0000aa, 0x00aa00, 0x00aaaa,
    0xaa0000, 0xaa00aa, 0xaa5500, 0xaaaaaa,
    0x555555, 0x5555ff, 0x55ff55, 0x55ffff,
    0xff5555, 0xff55ff, 0xffff55, 0xffffff
};

static uint32_t fb_palette_color(uint8_t index) {
    return fb_palette[index & 0x0f];
}

static void fb_draw_cell(int col, int row, uint16_t cell) {
    if (!g_fb.enabled)
        return;

    uint8_t ch = (uint8_t)(cell & 0xff);
    uint8_t attr = (uint8_t)(cell >> 8);
    uint32_t fg = fb_palette_color(attr & 0x0f);
    uint32_t bg = fb_palette_color((attr >> 4) & 0x0f);

    uint8_t font_w = font_get_width();
    uint8_t font_h = font_get_height();
    uint8_t row_bytes = font_get_row_bytes();
    if (!font_w || !font_h || !row_bytes)
        return;

    int px = col * font_w;
    int py = row * font_h;
    const uint8_t* glyph = font_get_glyph(ch);

    if (g_fb.bpp == 32) {
        for (uint8_t y = 0; y < font_h; y++) {
            const uint8_t* row_ptr = glyph + (size_t)y * row_bytes;
            uint8_t bits = row_ptr[0];
            uint32_t* dst = (uint32_t*)(g_fb.addr + (uint32_t)(py + y) * g_fb.pitch +
                                        (uint32_t)px * 4);
            if (row_bytes == 1) {
                for (uint8_t x = 0; x < font_w; x++)
                    dst[x] = (bits & (uint8_t)(0x80u >> x)) ? fg : bg;
            } else {
                for (uint8_t x = 0; x < font_w; x++) {
                    uint8_t byte = row_ptr[x >> 3];
                    uint8_t bit = (uint8_t)(0x80u >> (x & 7));
                    dst[x] = (byte & bit) ? fg : bg;
                }
            }
        }
    } else if (g_fb.bpp == 24) {
        for (uint8_t y = 0; y < font_h; y++) {
            const uint8_t* row_ptr = glyph + (size_t)y * row_bytes;
            uint8_t bits = row_ptr[0];
            uint8_t* dst = g_fb.addr + (uint32_t)(py + y) * g_fb.pitch + (uint32_t)px * 3;
            if (row_bytes == 1) {
                for (uint8_t x = 0; x < font_w; x++) {
                    uint32_t color = (bits & (uint8_t)(0x80u >> x)) ? fg : bg;
                    dst[0] = (uint8_t)(color & 0xff);
                    dst[1] = (uint8_t)((color >> 8) & 0xff);
                    dst[2] = (uint8_t)((color >> 16) & 0xff);
                    dst += 3;
                }
            } else {
                for (uint8_t x = 0; x < font_w; x++) {
                    uint8_t byte = row_ptr[x >> 3];
                    uint8_t bit = (uint8_t)(0x80u >> (x & 7));
                    uint32_t color = (byte & bit) ? fg : bg;
                    dst[0] = (uint8_t)(color & 0xff);
                    dst[1] = (uint8_t)((color >> 8) & 0xff);
                    dst[2] = (uint8_t)((color >> 16) & 0xff);
                    dst += 3;
                }
            }
        }
    }
}

static void fb_cursor_erase_at(int col, int row) {
    if (!g_fb.enabled)
        return;
    if (col < 0 || row < 0 || col >= screen_cols || row >= screen_rows)
        return;
    fb_draw_cell(col, row, screen_get_at(col, row));
}

static void fb_cursor_draw_at(int col, int row) {
    if (!g_fb.enabled || !fb_cursor_visible)
        return;
    if (col < 0 || row < 0 || col >= screen_cols || row >= screen_rows)
        return;

    uint16_t cell = screen_get_at(col, row);
    uint8_t ch = (uint8_t)(cell & 0xff);
    uint8_t attr = (uint8_t)(cell >> 8);
    uint8_t fg = attr & 0x0f;
    uint8_t bg = (attr >> 4) & 0x0f;
#if FB_CURSOR_STYLE == FB_CURSOR_STYLE_UNDERLINE
    (void)ch;
    (void)bg;
    uint8_t font_w = font_get_width();
    uint8_t font_h = font_get_height();
    if (!font_w || !font_h)
        return;

    uint32_t color = fb_palette_color(fg);
    uint32_t px = (uint32_t)col * font_w;
    uint32_t start_y = (uint32_t)row * font_h;
    uint32_t thickness = FB_CURSOR_UNDERLINE_HEIGHT;
    if (thickness > font_h) thickness = font_h;
    uint32_t py = start_y + (uint32_t)font_h - thickness;
    if (px >= g_fb.width || py >= g_fb.height)
        return;

    uint32_t max_w = g_fb.width - px;
    uint32_t w = font_w;
    if (w > max_w) w = max_w;
    for (uint32_t y = 0; y < thickness && (py + y) < g_fb.height; y++) {
        uint8_t* dst = g_fb.addr + (uint32_t)(py + y) * g_fb.pitch + px * g_fb.bytes_per_pixel;
        for (uint32_t x = 0; x < w; x++) {
            dst[0] = (uint8_t)(color & 0xff);
            dst[1] = (uint8_t)((color >> 8) & 0xff);
            dst[2] = (uint8_t)((color >> 16) & 0xff);
            if (g_fb.bytes_per_pixel == 4) dst[3] = 0;
            dst += g_fb.bytes_per_pixel;
        }
    }
#else
    uint8_t inv_attr = (uint8_t)((fg << 4) | bg);
    uint16_t inv_cell = ((uint16_t)inv_attr << 8) | ch;
    fb_draw_cell(col, row, inv_cell);
#endif
}

static uint16_t screen_get_at_start(int start, int x, int y) {
    if (x < 0 || x >= screen_cols || y < 0 || y >= screen_rows)
        return 0;
    int buf_row = start + y;
    if (buf_row >= total_lines)
        return ((uint16_t)WHITE_ON_BLACK << 8) | ' ';
    return textbuf[buf_row][x];
}

static void fb_cursor_erase_at_start(int start, int col, int row) {
    if (!g_fb.enabled)
        return;
    if (col < 0 || row < 0 || col >= screen_cols || row >= screen_rows)
        return;
    fb_draw_cell(col, row, screen_get_at_start(start, col, row));
}

static void screen_draw_cell(int col, int row, uint16_t cell) {
    if (col < 0 || row < 0)
        return;
    if (col >= screen_cols || row >= screen_rows)
        return;

    if (g_fb.enabled) {
        fb_draw_cell(col, row, cell);
    } else {
        vga_putc(col, row, (char)(cell & 0xff), (uint8_t)(cell >> 8));
    }
}

void kprint_char(char c) {
    print_char(c, -1, -1, 0);  // 현재 커서에 문자 출력 (색상 0 = 기본)
}

void print_uint(uint32_t val) {
    char buf[16];
    int i = 0;
    if (val == 0) {
        putchar('0');
        return;
    }
    while (val > 0) {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    }
    while (i--) {
        putchar(buf[i]);
    }
}

static void put_str(const char* s) {
    while (*s) putchar(*s++);
}

static void print_uint_padded(uint32_t val, int width, char pad) {
    char buf[16];
    int i = 0;

    // 숫자 → 문자열 (역순)
    if (val == 0) {
        buf[i++] = '0';
    } else {
        while (val > 0 && i < (int)sizeof(buf)) {
            buf[i++] = '0' + (val % 10);
            val /= 10;
        }
    }

    int len = i;

    // padding 채우기 (width > len일 경우)
    for (int j = len; j < width; j++) {
        putchar(pad);
    }

    // 실제 숫자 출력 (뒤집어서)
    for (int j = i - 1; j >= 0; j--) {
        putchar(buf[j]);
    }
}

static void print_int_padded(int val, int width, char pad) {
    if (val < 0) {
        putchar('-');
        print_uint_padded((uint32_t)(-val), width - 1, pad);
    } else {
        print_uint_padded((uint32_t)val, width, pad);
    }
}

static void print_hex_padded(uint32_t val, int width, char pad) {
    char buf[16];
    int i = 0;

    if (val == 0) buf[i++] = '0';
    else {
        while (val > 0 && i < (int)sizeof(buf)) {
            int nib = val & 0xF;
            buf[i++] = (nib < 10) ? ('0' + nib) : ('A' + (nib - 10));
            val >>= 4;
        }
    }

    while (i < width) buf[i++] = pad;

    putchar('0');
    putchar('x');
    for (int j = i - 1; j >= 0; j--)
        putchar(buf[j]);
}

void print_HEX_padded(uint32_t val, int width, char pad) {
    char buf[16];
    int i = 0;
    do {
        int digit = val & 0xF;
        buf[i++] = (digit < 10) ? ('0' + digit) : ('A' + (digit - 10));
        val >>= 4;
    } while (val);

    while (i < width) buf[i++] = pad;

    for (int j = i - 1; j >= 0; j--)
        putchar(buf[j]);
}

void kprintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    char buffer[512];
    int buf_i = 0;

    for (const char* p = fmt; *p; p++) {
        if (*p != '%') {
            putchar(*p);
            if (buf_i < (int)sizeof(buffer) - 1)
                buffer[buf_i++] = *p;
            continue;
        }

        // % 시작 → 포맷 파싱
        p++;
        char pad = ' ';
        int width = 0;

        if (*p == '0') { // zero-padding
            pad = '0';
            p++;
        }

        while (*p >= '0' && *p <= '9') { // width 읽기
            width = width * 10 + (*p - '0');
            p++;
        }

        switch (*p) {
        case 's': {
            char* str = va_arg(args, char*);
            put_str(str);
            while (*str && buf_i < (int)sizeof(buffer) - 1)
                buffer[buf_i++] = *str++;
            break;
        }
        case 'd': {
            int val = va_arg(args, int);
            print_int_padded(val, width, pad);
            // 로그용 숫자 버퍼
            char tmp[16]; 
            int len = int_to_str(val, tmp);  // int→문자열 함수 필요
            for (int i = 0; i < len && buf_i < (int)sizeof(buffer) - 1; i++)
                buffer[buf_i++] = tmp[i];
            break;
        }
        case 'u': {
            uint32_t val = va_arg(args, uint32_t);
            print_uint_padded(val, width, pad);
            char tmp[16];
            int len = uint_to_str(val, tmp);
            for (int i = 0; i < len && buf_i < (int)sizeof(buffer) - 1; i++)
                buffer[buf_i++] = tmp[i];
            break;
        }
        case 'x': {
            uint32_t val = va_arg(args, uint32_t);
            print_hex_padded(val, width, pad);
            // 로그엔 "0x..." 형태 추가
            char tmp[16];
            int len = hex_to_str(val, tmp, false);
            for (int i = 0; i < len && buf_i < (int)sizeof(buffer) - 1; i++)
                buffer[buf_i++] = tmp[i];
            break;
        }
        case 'X': {
            uint32_t val = va_arg(args, uint32_t);
            print_HEX_padded(val, width, pad);
            char tmp[16];
            int len = hex_to_str(val, tmp, true);
            for (int i = 0; i < len && buf_i < (int)sizeof(buffer) - 1; i++)
                buffer[buf_i++] = tmp[i];
            break;
        }
        case 'c': {
            char c = (char)va_arg(args, int);
            putchar(c);
            if (buf_i < (int)sizeof(buffer) - 1)
                buffer[buf_i++] = c;
            break;
        }
        case 'p': {
            uintptr_t ptr = (uintptr_t)va_arg(args, void*);
            uint32_t val = (uint32_t)ptr;
            print_hex_padded(val, width ? width : 8, '0'); // formatted as 32-bit address
            char tmp[16];
            int len = hex_to_str(val, tmp, false);
            for (int i = 0; i < len && buf_i < (int)sizeof(buffer) - 1; i++)
                buffer[buf_i++] = tmp[i];
            break;
        }
        case '%':
            putchar('%');
            if (buf_i < (int)sizeof(buffer) - 1)
                buffer[buf_i++] = '%';
            break;
        default:
            putchar('%');
            putchar(*p);
            if (buf_i < (int)sizeof(buffer) - 2) {
                buffer[buf_i++] = '%';
                buffer[buf_i++] = *p;
            }
            break;
        }
    }

    buffer[buf_i] = '\0';
    va_end(args);

    // 로그 기록
    bootlog_add(buffer);
}

void vga_putc(int x, int y, char ch, uint8_t attr) {
    if (x < 0 || x >= screen_cols || y < 0 || y >= screen_rows) return;
    if (g_fb.enabled) {
        screen_put_at(x, y, (uint8_t)ch, attr);
        return;
    }
    uint16_t* vm = (uint16_t*) VIDEO_ADDRESS;
    vm[y * screen_cols + x] = ((uint16_t)attr << 8) | (uint8_t)ch;
}

static int screen_visible_start(void) {
    int max_start = (total_lines > screen_rows) ? (total_lines - screen_rows) : 0;
    if (scroll_pos > max_start) scroll_pos = max_start;
    if (scroll_pos < 0) scroll_pos = 0;

    int start = max_start - scroll_pos;
    if (start < 0) start = 0;
    return start;
}

static int screen_compute_start_for_scroll(int scroll_pos_value) {
    int max_start = (total_lines > screen_rows) ? (total_lines - screen_rows) : 0;
    if (scroll_pos_value > max_start) scroll_pos_value = max_start;
    if (scroll_pos_value < 0) scroll_pos_value = 0;
    int start = max_start - scroll_pos_value;
    if (start < 0) start = 0;
    return start;
}

static void screen_set_geometry(int cols, int rows) {
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols > SCREEN_MAX_COLS) cols = SCREEN_MAX_COLS;
    if (rows > SCREEN_MAX_ROWS) rows = SCREEN_MAX_ROWS;
    screen_cols = cols;
    screen_rows = rows;
}

static void screen_update_textbuf_cell(int scr_row, int scr_col, uint16_t cell) {
    if (scr_row < 0 || scr_col < 0)
        return;
    if (scr_row >= screen_rows || scr_col >= screen_cols)
        return;

    int start = screen_visible_start();
    int buf_row = start + scr_row;

    if (buf_row >= MAX_SCROLL_LINES) {
        memmove(textbuf, textbuf + 1, (MAX_SCROLL_LINES - 1) * sizeof(textbuf[0]));
        buf_row = MAX_SCROLL_LINES - 1;
        total_lines = MAX_SCROLL_LINES;
    } else if (buf_row >= total_lines) {
        total_lines = buf_row + 1;
    }

    textbuf[buf_row][scr_col] = cell;
}

static void screen_append_blank_line(uint8_t attr) {
    uint16_t blank = ((uint16_t)attr << 8) | ' ';
    if (total_lines >= MAX_SCROLL_LINES) {
        memmove(textbuf, textbuf + 1, (MAX_SCROLL_LINES - 1) * sizeof(textbuf[0]));
        total_lines = MAX_SCROLL_LINES - 1;
    }
    for (int c = 0; c < SCREEN_MAX_COLS; c++)
        textbuf[total_lines][c] = blank;
    total_lines++;
    scroll_pos = 0;
}

static inline void hydrate_buffer_from_vga_once(void) {
    if (buf_hydrated) return;
    for (int r = 0; r < screen_rows; r++) {
        for (int c = 0; c < SCREEN_MAX_COLS; c++) {
            textbuf[r][c] = ((uint16_t)WHITE_ON_BLACK << 8) | ' ';
        }
    }
    total_lines   = screen_rows;
    scroll_pos    = 0;
    buf_hydrated  = 1;
}

static void fb_redraw_rows(int start, int row_start, int row_count) {
    if (row_count <= 0)
        return;
    if (row_start < 0) {
        row_count += row_start;
        row_start = 0;
    }
    if (row_start >= screen_rows)
        return;
    if (row_start + row_count > screen_rows)
        row_count = screen_rows - row_start;

    uint16_t blank = ((uint16_t)WHITE_ON_BLACK << 8) | ' ';
    for (int r = 0; r < row_count; r++) {
        int screen_row = row_start + r;
        int buf_row = start + screen_row;
        if (buf_row < total_lines) {
            for (int c = 0; c < screen_cols; c++)
                fb_draw_cell(c, screen_row, textbuf[buf_row][c]);
        } else {
            for (int c = 0; c < screen_cols; c++)
                fb_draw_cell(c, screen_row, blank);
        }
    }
}

static bool fb_scroll_view(int old_start, int new_start) {
    if (!FB_SCROLL_USE_MEMMOVE)
        return false;
    if (!g_fb.enabled)
        return false;
    int delta = new_start - old_start;
    if (delta == 0)
        return true;

    uint8_t font_h = font_get_height();
    if (!font_h)
        return false;

    int abs_delta = (delta > 0) ? delta : -delta;
    if (abs_delta >= screen_rows)
        return false;

    uint32_t visible_height = (uint32_t)screen_rows * font_h;
    if (visible_height == 0)
        return false;
    if (visible_height > g_fb.height)
        visible_height = g_fb.height;

    uint32_t move_rows = (uint32_t)abs_delta * font_h;
    if (move_rows >= visible_height)
        return false;

    size_t row_bytes = (size_t)g_fb.pitch;
    size_t move_bytes = (size_t)(visible_height - move_rows) * row_bytes;

    if (delta > 0) {
        memmove(g_fb.addr, g_fb.addr + move_rows * row_bytes, move_bytes);
        fb_redraw_rows(new_start, screen_rows - abs_delta, abs_delta);
    } else {
        memmove(g_fb.addr + move_rows * row_bytes, g_fb.addr, move_bytes);
        fb_redraw_rows(new_start, 0, abs_delta);
    }
    return true;
}

static void redraw_from_buffer(void) {
    int start = screen_visible_start();

    if (g_fb.enabled) {
        fb_redraw_rows(start, 0, screen_rows);
        fb_cursor_draw_at(fb_cursor_col, fb_cursor_row);
        return;
    }

    uint16_t* vm = (uint16_t*)VIDEO_ADDRESS;
    for (int r = 0; r < screen_rows; r++) {
        if (start + r < total_lines) {
            memcpy(vm + r * screen_cols, textbuf[start + r], screen_cols * 2);
        } else {
            for (int c = 0; c < screen_cols; c++)
                vm[r * screen_cols + c] = (0x07 << 8) | ' ';
        }
    }
}

void putchar_with_buf(char c, uint8_t attr) {
    static int col = 0;

    if (total_lines >= MAX_SCROLL_LINES) {
        // 버퍼 꽉 찼으면 한 줄 위로 밀기
        memmove(textbuf, textbuf + 1,
                (MAX_SCROLL_LINES - 1) * sizeof(textbuf[0]));
        total_lines = MAX_SCROLL_LINES - 1;
    }

    textbuf[total_lines][col] = ((uint16_t)attr << 8) | (uint8_t)c;

    col++;
    if (col >= screen_cols || c == '\n') {
        col = 0;
        total_lines++;
    }

    redraw_from_buffer();
}

static int scroll_step(void) {
    int step = screen_rows / 4;
    if (step < 3) step = 3;
    return step;
}

void scroll_up_screen(void) {
    hydrate_buffer_from_vga_once();
    int old_start = screen_compute_start_for_scroll(scroll_pos);
    int max_scroll = (total_lines > screen_rows) ? (total_lines - screen_rows) : 0;
    int step = scroll_step();
    if (scroll_pos + step <= max_scroll)
        scroll_pos += step;
    else if (scroll_pos < max_scroll)
        scroll_pos = max_scroll; // 끝까지 스크롤
    if (scroll_pos > max_scroll)
        scroll_pos = max_scroll;
    if (scroll_pos < 0)
        scroll_pos = 0;
    int new_start = screen_compute_start_for_scroll(scroll_pos);
    if (old_start == new_start)
        return;
    if (g_fb.enabled) {
        if (fb_cursor_visible)
            fb_cursor_erase_at_start(old_start, fb_cursor_col, fb_cursor_row);
        if (!fb_scroll_view(old_start, new_start))
            redraw_from_buffer();
        if (fb_cursor_visible)
            fb_cursor_draw_at(fb_cursor_col, fb_cursor_row);
    } else {
        redraw_from_buffer();
    }
}

void scroll_down_screen(void) {
    hydrate_buffer_from_vga_once();
    int old_start = screen_compute_start_for_scroll(scroll_pos);
    int max_scroll = (total_lines > screen_rows) ? (total_lines - screen_rows) : 0;
    int step = scroll_step();
    if (scroll_pos >= step)
        scroll_pos -= step;
    else if (scroll_pos > 0)
        scroll_pos = 0; // 맨 위로
    if (scroll_pos > max_scroll)
        scroll_pos = max_scroll;
    if (scroll_pos < 0)
        scroll_pos = 0;
    int new_start = screen_compute_start_for_scroll(scroll_pos);
    if (old_start == new_start)
        return;
    if (g_fb.enabled) {
        if (fb_cursor_visible)
            fb_cursor_erase_at_start(old_start, fb_cursor_col, fb_cursor_row);
        if (!fb_scroll_view(old_start, new_start))
            redraw_from_buffer();
        if (fb_cursor_visible)
            fb_cursor_draw_at(fb_cursor_col, fb_cursor_row);
    } else {
        redraw_from_buffer();
    }
}

bool screen_is_scrolled(void) {
    return scroll_pos != 0;
}

void screen_scroll_to_bottom(void) {
    hydrate_buffer_from_vga_once();
    if (scroll_pos == 0) return;
    int old_start = screen_compute_start_for_scroll(scroll_pos);
    scroll_pos = 0;
    int new_start = screen_compute_start_for_scroll(scroll_pos);
    if (g_fb.enabled) {
        if (fb_cursor_visible)
            fb_cursor_erase_at_start(old_start, fb_cursor_col, fb_cursor_row);
        if (!fb_scroll_view(old_start, new_start))
            redraw_from_buffer();
        if (fb_cursor_visible)
            fb_cursor_draw_at(fb_cursor_col, fb_cursor_row);
    } else {
        redraw_from_buffer();
    }
}

/**********************************************************
 * Private kernel functions                               *
 **********************************************************/
/**
 * Innermost print function for our kernel, directly accesses the video memory 
 *
 * If 'col' and 'row' are negative, we will print at current cursor location
 * If 'attr' is zero it will use 'white on black' as default
 * Returns the offset of the next character
 * Sets the video cursor to the returned offset
 */
int print_char(char c, int col, int row, char attr) {
    if (!attr) attr = color_current();

    hydrate_buffer_from_vga_once();
    
    /* 좌표/오프셋 계산 */
    int offset;
    if (col >= 0 && row >= 0) offset = get_offset(col, row);
    else                      offset = get_cursor_offset();

    int scr_row = get_offset_row(offset);
    int scr_col = get_offset_col(offset);

    if ((uint8_t)c == 0x08) { // backspace
        if (offset >= 2) {
            offset -= 2;
            scr_row = get_offset_row(offset);
            scr_col = get_offset_col(offset);
            uint16_t cell = ((uint16_t)attr << 8) | ' ';
            screen_draw_cell(scr_col, scr_row, cell);
            screen_update_textbuf_cell(scr_row, scr_col, cell);
        }
        set_cursor_offset(offset);
        return offset;
    }

    /* 문자 출력 */
    if (c == '\n') {
        // 줄바꿈: 커서를 다음 줄 맨 앞으로
        offset = get_offset(0, scr_row + 1);
    } else {
        uint16_t cell = ((uint16_t)attr << 8) | (uint8_t)c;
        screen_draw_cell(scr_col, scr_row, cell);
        screen_update_textbuf_cell(scr_row, scr_col, cell);
        offset += 2;
    }

    /* 화면 넘어가면 VGA 스크롤 (기존 로직) */
    if (offset >= screen_rows * screen_cols * 2) {
        int old_start = 0;
        if (g_fb.enabled) {
            old_start = screen_compute_start_for_scroll(scroll_pos);
            if (fb_cursor_visible)
                fb_cursor_erase_at_start(old_start, fb_cursor_col, fb_cursor_row);
        }
        if (!g_fb.enabled) {
            // VGA 한 줄 위로
            for (int i = 1; i < screen_rows; i++)
                memory_copy((uint8_t*)(uintptr_t)(get_offset(0, i) + VIDEO_ADDRESS),
                            (uint8_t*)(uintptr_t)(get_offset(0, i-1) + VIDEO_ADDRESS),
                            screen_cols * 2);

            // 마지막 줄 공백
            char *last_line = (char*)(uintptr_t)(get_offset(0, screen_rows - 1) + (uint32_t)VIDEO_ADDRESS);
            for (int i = 0; i < screen_cols * 2; i++) last_line[i] = 0;
        }

        screen_append_blank_line(attr);
        offset -= 2 * screen_cols;
        if (g_fb.enabled) {
            int new_start = screen_compute_start_for_scroll(scroll_pos);
            if (!fb_scroll_view(old_start, new_start))
                redraw_from_buffer();
        }
    }

    set_cursor_offset(offset);
    return offset;
}

void screen_put_at(int x, int y, uint8_t ch, uint8_t color) {
    if (x < 0 || x >= screen_cols || y < 0 || y >= screen_rows) return;
    hydrate_buffer_from_vga_once();

    uint16_t cell = ((uint16_t)color << 8) | ch;
    screen_update_textbuf_cell(y, x, cell);
    screen_draw_cell(x, y, cell);
    if (g_fb.enabled && fb_cursor_visible && x == fb_cursor_col && y == fb_cursor_row)
        fb_cursor_draw_at(x, y);
}

uint16_t screen_get_at(int x, int y) {
    if (x < 0 || x >= screen_cols || y < 0 || y >= screen_rows)
        return 0;

    if (!g_fb.enabled) {
        uint16_t *video = (uint16_t*)0xB8000;
        return video[y * 80 + x];
    }

    hydrate_buffer_from_vga_once();
    int start = screen_visible_start();
    int buf_row = start + y;
    if (buf_row >= total_lines)
        return ((uint16_t)WHITE_ON_BLACK << 8) | ' ';
    return textbuf[buf_row][x];
}

void screen_set_framebuffer(uint64_t addr, uint32_t width, uint32_t height, uint32_t pitch, uint8_t bpp) {
    if (!addr || !width || !height || !pitch)
        return;
    if (bpp != 32 && bpp != 24)
        return;

    int cur_row = get_cursor_row();
    int cur_col = get_cursor_col();
    int prev_cols = screen_cols;
    uint8_t font_w = font_get_width();
    uint8_t font_h = font_get_height();
    int cols = font_w ? (int)(width / font_w) : MAX_COLS;
    int rows = font_h ? (int)(height / font_h) : MAX_ROWS;

    g_fb.addr = (uint8_t*)(uintptr_t)addr;
    g_fb.width = width;
    g_fb.height = height;
    g_fb.pitch = pitch;
    g_fb.bpp = bpp;
    g_fb.bytes_per_pixel = (uint8_t)((bpp + 7) / 8);
    g_fb.enabled = true;

    screen_set_geometry(cols, rows);

    hydrate_buffer_from_vga_once();

    if (screen_cols > prev_cols) {
        uint16_t blank = ((uint16_t)g_text_attr << 8) | ' ';
        for (int r = 0; r < total_lines; r++) {
            for (int c = prev_cols; c < screen_cols; c++)
                textbuf[r][c] = blank;
        }
    }

    fb_cursor_row = cur_row;
    fb_cursor_col = cur_col;
    if (fb_cursor_row >= screen_rows) fb_cursor_row = screen_rows - 1;
    if (fb_cursor_col >= screen_cols) fb_cursor_col = screen_cols - 1;

    redraw_from_buffer();
    screen_set_cursor_visible(cursor_user_visible);
}

int get_cursor_offset() {
    if (g_fb.enabled)
        return get_offset(fb_cursor_col, fb_cursor_row);

    /* Use the VGA ports to get the current cursor position
     * 1. Ask for high byte of the cursor offset (data 14)
     * 2. Ask for low byte (data 15)
     */
    hal_out8(REG_SCREEN_CTRL, 14);
    int offset = hal_in8(REG_SCREEN_DATA) << 8; /* High byte: << 8 */
    hal_out8(REG_SCREEN_CTRL, 15);
    offset += hal_in8(REG_SCREEN_DATA);
    return offset * 2; /* Position * size of character cell */
}

void set_cursor_offset(int offset) {
    if (g_fb.enabled) {
        int old_row = fb_cursor_row;
        int old_col = fb_cursor_col;

        fb_cursor_row = get_offset_row(offset);
        fb_cursor_col = get_offset_col(offset);
        if (fb_cursor_row < 0) fb_cursor_row = 0;
        if (fb_cursor_col < 0) fb_cursor_col = 0;
        if (fb_cursor_row >= screen_rows) fb_cursor_row = screen_rows - 1;
        if (fb_cursor_col >= screen_cols) fb_cursor_col = screen_cols - 1;
        if (fb_cursor_visible) {
            fb_cursor_erase_at(old_col, old_row);
            fb_cursor_draw_at(fb_cursor_col, fb_cursor_row);
        }
        return;
    }

    /* Similar to get_cursor_offset, but instead of reading we write data */
    offset /= 2;
    hal_out8(REG_SCREEN_CTRL, 14);
    hal_out8(REG_SCREEN_DATA, (uint8_t)(offset >> 8));
    hal_out8(REG_SCREEN_CTRL, 15);
    hal_out8(REG_SCREEN_DATA, (uint8_t)(offset & 0xff));
}

void clear_screen(void) {
    for (int r = 0; r < MAX_SCROLL_LINES; r++)
        for (int c = 0; c < SCREEN_MAX_COLS; c++)
            textbuf[r][c] = ((uint16_t)WHITE_ON_BLACK << 8) | ' ';

    total_lines = 1;
    scroll_pos = 0;

    redraw_from_buffer();

    // 커서 초기화
    set_cursor_offset(get_offset(0, 0));

    buf_hydrated = 1;
}

void clear_input_line(void) {
    int col = prompt_col;
    int row = prompt_row;
    hydrate_buffer_from_vga_once();

    uint16_t blank = ((uint16_t)g_text_attr << 8) | ' ';
    for (int i = col; i < screen_cols; i++) {
        screen_update_textbuf_cell(row, i, blank);
        screen_draw_cell(i, row, blank);
    }

    set_cursor_offset(get_offset(col, row));
}

int get_offset(int col, int row) { return 2 * (row * screen_cols + col); }
int get_offset_row(int offset) { return offset / (2 * screen_cols); }
int get_offset_col(int offset) { return (offset - (get_offset_row(offset)*2*screen_cols))/2; }

void set_cursor(int row, int col) {
    set_cursor_offset(get_offset(col, row));
}

// 현재 커서 row/col 반환
int get_cursor_row(void) {
    return get_offset_row(get_cursor_offset());
}

int get_cursor_col(void) {
    return get_offset_col(get_cursor_offset());
}
