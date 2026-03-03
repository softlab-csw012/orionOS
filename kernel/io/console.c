#include "console.h"

#include "../tty.h"
#include "../../drivers/screen.h"
#include "../../libc/string.h"
#include "../log.h"

#include <stdarg.h>

typedef enum {
    CON_ANSI_TEXT = 0,
    CON_ANSI_ESC,
    CON_ANSI_CSI,
} con_ansi_state_t;

typedef struct {
    con_ansi_state_t state;
    int params[8];
    int param_count;
    bool private_mode;
    uint8_t fg;
    uint8_t bg;
    uint8_t saved_fg;
    uint8_t saved_bg;
    int saved_row;
    int saved_col;
    bool has_saved_cursor;
} con_ansi_t;

static con_ansi_t g_con_ansi = {
    .state = CON_ANSI_TEXT,
    .params = {0, 0, 0, 0, 0, 0, 0, 0},
    .param_count = 0,
    .private_mode = false,
    .fg = 7,
    .bg = 0,
    .saved_fg = 7,
    .saved_bg = 0,
    .saved_row = 0,
    .saved_col = 0,
    .has_saved_cursor = false,
};

static inline int console_clamp(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline int console_param(const con_ansi_t* a, int idx, int defv) {
    if (!a || idx < 0 || idx >= a->param_count) return defv;
    return a->params[idx];
}

static int console_write_raw(const char* buf, size_t len) {
    if (tty_is_ready()) {
        return tty_write_kernel(buf, (uint32_t)len);
    }
    return screen_write(buf, len);
}

static uint8_t console_map_fg(int code, uint8_t cur_fg) {
    if (code >= 30 && code <= 37) return (uint8_t)(code - 30);
    if (code == 39) return 7;
    if (code >= 90 && code <= 97) return (uint8_t)(8 + (code - 90));
    return cur_fg;
}

static uint8_t console_map_bg(int code, uint8_t cur_bg) {
    if (code >= 40 && code <= 47) return (uint8_t)(code - 40);
    if (code == 49) return 0;
    if (code >= 100 && code <= 107) return (uint8_t)(8 + (code - 100));
    return cur_bg;
}

static void console_ansi_reset_csi(con_ansi_t* a) {
    a->param_count = 0;
    a->private_mode = false;
    for (int i = 0; i < 8; i++) {
        a->params[i] = 0;
    }
}

static void console_ansi_apply_sgr(con_ansi_t* a) {
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
            continue;
        }
        a->fg = console_map_fg(code, a->fg);
        a->bg = console_map_bg(code, a->bg);
    }
    set_color(a->fg, a->bg);
}

static void console_ansi_clear_line_mode(int mode) {
    int cols = screen_get_cols();
    if (cols <= 0) cols = 80;
    int row = get_cursor_row();
    int col = get_cursor_col();
    int from = 0;
    int to = cols - 1;
    if (mode == 0) from = col;
    else if (mode == 1) to = col;
    if (to < from) return;
    set_cursor(row, from);
    for (int i = from; i <= to; i++) {
        char sp = ' ';
        console_write_raw(&sp, 1);
    }
    set_cursor(row, col);
}

static void console_ansi_save_cursor(con_ansi_t* a) {
    a->saved_row = get_cursor_row();
    a->saved_col = get_cursor_col();
    a->saved_fg = a->fg;
    a->saved_bg = a->bg;
    a->has_saved_cursor = true;
}

static void console_ansi_restore_cursor(con_ansi_t* a) {
    if (!a->has_saved_cursor) return;
    a->fg = a->saved_fg;
    a->bg = a->saved_bg;
    set_color(a->fg, a->bg);
    set_cursor(a->saved_row, a->saved_col);
}

static void console_ansi_handle_csi_final(con_ansi_t* a, char final) {
    int p0 = console_param(a, 0, 0);
    int p1 = console_param(a, 1, 0);
    int rows = screen_get_rows();
    int cols = screen_get_cols();
    if (rows <= 0) rows = 25;
    if (cols <= 0) cols = 80;
    int row = get_cursor_row();
    int col = get_cursor_col();

    switch (final) {
        case 'm':
            console_ansi_apply_sgr(a);
            break;
        case 'J':
            if (p0 == 2 || p0 == 3) {
                clear_screen();
                set_cursor(0, 0);
            }
            break;
        case 'K':
            console_ansi_clear_line_mode(p0);
            break;
        case 'H':
        case 'f': {
            int tr = (p0 > 0) ? (p0 - 1) : 0;
            int tc = (p1 > 0) ? (p1 - 1) : 0;
            set_cursor(console_clamp(tr, 0, rows - 1), console_clamp(tc, 0, cols - 1));
            break;
        }
        case 'A': {
            int n = (p0 > 0) ? p0 : 1;
            set_cursor(console_clamp(row - n, 0, rows - 1), col);
            break;
        }
        case 'B': {
            int n = (p0 > 0) ? p0 : 1;
            set_cursor(console_clamp(row + n, 0, rows - 1), col);
            break;
        }
        case 'C': {
            int n = (p0 > 0) ? p0 : 1;
            set_cursor(row, console_clamp(col + n, 0, cols - 1));
            break;
        }
        case 'D': {
            int n = (p0 > 0) ? p0 : 1;
            set_cursor(row, console_clamp(col - n, 0, cols - 1));
            break;
        }
        case 'E': {
            int n = (p0 > 0) ? p0 : 1;
            set_cursor(console_clamp(row + n, 0, rows - 1), 0);
            break;
        }
        case 'F': {
            int n = (p0 > 0) ? p0 : 1;
            set_cursor(console_clamp(row - n, 0, rows - 1), 0);
            break;
        }
        case 'G': {
            int tc = (p0 > 0) ? (p0 - 1) : 0;
            set_cursor(row, console_clamp(tc, 0, cols - 1));
            break;
        }
        case 's':
            console_ansi_save_cursor(a);
            break;
        case 'u':
            console_ansi_restore_cursor(a);
            break;
        case 'h':
        case 'l':
            if (a->private_mode && p0 == 25) {
                screen_set_cursor_visible(final == 'h');
            }
            break;
        default:
            break;
    }
}

static void console_ansi_feed_char(con_ansi_t* a, char ch) {
    if (a->state == CON_ANSI_TEXT) {
        if ((uint8_t)ch == 0x1B) {
            a->state = CON_ANSI_ESC;
            return;
        }
        console_write_raw(&ch, 1);
        return;
    }

    if (a->state == CON_ANSI_ESC) {
        if (ch == '[') {
            a->state = CON_ANSI_CSI;
            console_ansi_reset_csi(a);
            return;
        }
        if (ch == '7') {
            console_ansi_save_cursor(a);
            a->state = CON_ANSI_TEXT;
            return;
        }
        if (ch == '8') {
            console_ansi_restore_cursor(a);
            a->state = CON_ANSI_TEXT;
            return;
        }
        if (ch == 'c') {
            clear_screen();
            set_cursor(0, 0);
            a->fg = 7;
            a->bg = 0;
            set_color(a->fg, a->bg);
            a->state = CON_ANSI_TEXT;
            return;
        }
        char esc = (char)0x1B;
        console_write_raw(&esc, 1);
        console_write_raw(&ch, 1);
        a->state = CON_ANSI_TEXT;
        return;
    }

    if (ch >= '0' && ch <= '9') {
        if (a->param_count == 0) a->param_count = 1;
        int idx = a->param_count - 1;
        a->params[idx] = a->params[idx] * 10 + (ch - '0');
        return;
    }
    if (ch == ';') {
        if (a->param_count == 0) a->param_count = 1;
        if (a->param_count < 8) a->param_count++;
        return;
    }
    if (ch == '?' && a->param_count == 0) {
        a->private_mode = true;
        return;
    }

    console_ansi_handle_csi_final(a, ch);
    a->state = CON_ANSI_TEXT;
}

int console_write(const char* buf, size_t len) {
    if (!buf || len == 0) {
        return 0;
    }
    if (tty_is_ready()) {
        return tty_write_kernel(buf, (uint32_t)len);
    }
    for (size_t i = 0; i < len; i++) {
        console_ansi_feed_char(&g_con_ansi, buf[i]);
    }
    return (int)len;
}

int console_writestr(const char* s) {
    size_t len = 0;
    while (s[len]) len++;
    return console_write(s, len);
}

void kprint(const char* message) {
    bootlog_add(message);
    console_writestr(message);
}

void print_dec(uint32_t num) {
    char buf[16];
    int i = 0;

    if (num == 0) {
        kprint("0");
        return;
    }

    while (num > 0) {
        buf[i++] = (char)('0' + (num % 10u));
        num /= 10u;
    }

    for (int j = i - 1; j >= 0; j--) {
        char s[2] = { buf[j], 0 };
        kprint(s);
    }
}

void kprint_at(const char* message, int col, int row) {
    if (!message) {
        return;
    }
    int offset = get_offset(col, row);
    set_cursor_offset(offset);
    while (*message) {
        print_char(*message++, -1, -1, 0);
    }
}

void kprint_float(double value) {
    if (value < 0) {
        putchar('-');
        value = -value;
    }

    int int_part = (int)value;
    double frac = value - int_part;
    if (frac < 0.000001) {
        kprint_int((uint32_t)int_part);
        return;
    }

    int frac_int = (int)(frac * 1000000 + 0.5);
    char buf[16];
    int len = 0;

    while (frac_int > 0) {
        buf[len++] = (char)('0' + (frac_int % 10));
        frac_int /= 10;
    }
    while (len > 0 && buf[len - 1] == '0')
        len--;

    if (len == 0) {
        kprint_int((uint32_t)int_part);
        kprint(".0");
        return;
    }

    kprint_int((uint32_t)int_part);
    putchar('.');
    for (int i = len - 1; i >= 0; i--)
        putchar(buf[i]);
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
    itoa((int)val, hex, 16);
    int len = (int)strlen(hex);
    for (int i = 0; i < width - len; i++)
        kprint("0");
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
    print_hex(val);
}

int int_to_str(int value, char* buf) {
    char tmp[16];
    int i = 0;
    int neg = 0;

    if (value < 0) {
        neg = 1;
        value = -value;
    }

    do {
        tmp[i++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value > 0);

    int len = 0;
    if (neg)
        buf[len++] = '-';
    while (i--)
        buf[len++] = tmp[i];
    buf[len] = '\0';
    return len;
}

int uint_to_str(uint32_t value, char* buf) {
    char tmp[16];
    int i = 0;

    do {
        tmp[i++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value > 0u);

    int len = 0;
    while (i--)
        buf[len++] = tmp[i];
    buf[len] = '\0';
    return len;
}

int hex_to_str(uint32_t value, char* buf, bool upper) {
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[16];
    int i = 0;

    do {
        tmp[i++] = digits[value & 0xF];
        value >>= 4;
    } while (value > 0u);

    int len = 0;
    buf[len++] = '0';
    buf[len++] = 'x';
    while (i--)
        buf[len++] = tmp[i];
    buf[len] = '\0';
    return len;
}

void kprint_int(uint32_t num) {
    char buf[12];
    int_to_str((int)num, buf);
    kprint(buf);
}

int putchar(int c) {
    char ch = (char)c;
    console_write(&ch, 1);
    return (uint8_t)c;
}

int putchar_color(uint8_t ch, uint8_t fg, uint8_t bg) {
    char seq[16];
    int len = snprintf(seq, sizeof(seq), "\x1b[%d;%dm%c\x1b[0m",
                       30 + fg, 40 + bg, ch);
    console_write(seq, len);
    return ch;
}

void kprint_color(const char* message, uint8_t fg, uint8_t bg) {
    char seq[16];
    int len = snprintf(seq, sizeof(seq), "\x1b[%d;%dm", 30 + fg, 40 + bg);
    console_write(seq, len);

    console_writestr(message);

    console_writestr("\x1b[0m");
}

void kprint_char(char c) {
    console_write(&c, 1);
}

void print_uint(uint32_t val) {
    char buf[16];
    int i = 0;
    if (val == 0) {
        putchar('0');
        return;
    }
    while (val > 0u) {
        buf[i++] = (char)('0' + (val % 10u));
        val /= 10u;
    }
    while (i--) {
        putchar(buf[i]);
    }
}

void put_str(const char* s) {
    while (*s)
        putchar(*s++);
}

void print_uint_padded(uint32_t val, int width, char pad) {
    char buf[16];
    int i = 0;

    if (val == 0u) {
        buf[i++] = '0';
    } else {
        while (val > 0u && i < (int)sizeof(buf)) {
            buf[i++] = (char)('0' + (val % 10u));
            val /= 10u;
        }
    }

    int len = i;
    for (int j = len; j < width; j++) {
        putchar(pad);
    }
    for (int j = i - 1; j >= 0; j--) {
        putchar(buf[j]);
    }
}

void print_int_padded(int val, int width, char pad) {
    if (val < 0) {
        putchar('-');
        print_uint_padded((uint32_t)(-val), width - 1, pad);
    } else {
        print_uint_padded((uint32_t)val, width, pad);
    }
}

void print_hex_padded(uint32_t val, int width, char pad) {
    char buf[16];
    int i = 0;

    if (val == 0u) {
        buf[i++] = '0';
    } else {
        while (val > 0u && i < (int)sizeof(buf)) {
            int nib = (int)(val & 0xFu);
            buf[i++] = (char)((nib < 10) ? ('0' + nib) : ('A' + (nib - 10)));
            val >>= 4;
        }
    }

    while (i < width)
        buf[i++] = pad;

    putchar('0');
    putchar('x');
    for (int j = i - 1; j >= 0; j--)
        putchar(buf[j]);
}

void print_HEX_padded(uint32_t val, int width, char pad) {
    char buf[16];
    int i = 0;
    do {
        int digit = (int)(val & 0xFu);
        buf[i++] = (char)((digit < 10) ? ('0' + digit) : ('A' + (digit - 10)));
        val >>= 4;
    } while (val);

    while (i < width)
        buf[i++] = pad;

    for (int j = i - 1; j >= 0; j--)
        putchar(buf[j]);
}

static int u64_to_dec_str(uint64_t val, char* out) {
    char buf[32];
    int i = 0;
    if (val == 0) {
        out[0] = '0';
        out[1] = '\0';
        return 1;
    }
    while (val > 0) {
        buf[i++] = (char)('0' + (val % 10u));
        val /= 10u;
    }
    for (int j = 0; j < i; j++) {
        out[j] = buf[i - 1 - j];
    }
    out[i] = '\0';
    return i;
}

static int s64_to_dec_str(int64_t val, char* out) {
    uint64_t mag = (uint64_t)val;
    int pos = 0;
    if (val < 0) {
        out[pos++] = '-';
        mag = (uint64_t)(-(val + 1)) + 1u;
    }
    return pos + u64_to_dec_str(mag, out + pos);
}

static int u64_to_hex_str(uint64_t val, char* out, bool upper) {
    char buf[32];
    int i = 0;
    if (val == 0) {
        out[0] = '0';
        out[1] = '\0';
        return 1;
    }
    while (val > 0) {
        int digit = (int)(val & 0xFu);
        buf[i++] = (char)((digit < 10) ? ('0' + digit)
                                       : ((upper ? 'A' : 'a') + (digit - 10)));
        val >>= 4;
    }
    for (int j = 0; j < i; j++) {
        out[j] = buf[i - 1 - j];
    }
    out[i] = '\0';
    return i;
}

static void print_u64_padded(uint64_t val, int width, char pad) {
    char tmp[32];
    int len = u64_to_dec_str(val, tmp);
    for (int i = len; i < width; i++) {
        putchar(pad);
    }
    put_str(tmp);
}

static void print_s64_padded(int64_t val, int width, char pad) {
    char tmp[32];
    int len = s64_to_dec_str(val, tmp);
    int pad_count = width - len;
    if (tmp[0] == '-' && pad == '0' && pad_count > 0) {
        putchar('-');
        for (int i = 0; i < pad_count; i++) {
            putchar('0');
        }
        put_str(tmp + 1);
        return;
    }
    for (int i = len; i < width; i++) {
        putchar(pad);
    }
    put_str(tmp);
}

static void print_hex64_padded(uint64_t val, int width, char pad, bool upper) {
    char tmp[32];
    int len = u64_to_hex_str(val, tmp, upper);
    for (int i = len; i < width; i++) {
        putchar(pad);
    }
    put_str(tmp);
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

        p++;
        char pad = ' ';
        int width = 0;
        int length = 0;

        if (*p == '0') {
            pad = '0';
            p++;
        }
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }
        while (*p == 'l') {
            length++;
            p++;
        }

        switch (*p) {
        case 's': {
            char* str = va_arg(args, char*);
            if (!str)
                str = "(null)";
            put_str(str);
            while (*str && buf_i < (int)sizeof(buffer) - 1)
                buffer[buf_i++] = *str++;
            break;
        }
        case 'd': {
            char tmp[32];
            int len = 0;
            if (length >= 2) {
                long long val = va_arg(args, long long);
                print_s64_padded((int64_t)val, width, pad);
                len = s64_to_dec_str((int64_t)val, tmp);
            } else if (length == 1) {
                long val = va_arg(args, long);
                print_s64_padded((int64_t)val, width, pad);
                len = s64_to_dec_str((int64_t)val, tmp);
            } else {
                int val = va_arg(args, int);
                print_int_padded(val, width, pad);
                len = int_to_str(val, tmp);
            }
            for (int i = 0; i < len && buf_i < (int)sizeof(buffer) - 1; i++)
                buffer[buf_i++] = tmp[i];
            break;
        }
        case 'u': {
            char tmp[32];
            int len = 0;
            if (length >= 2) {
                unsigned long long val = va_arg(args, unsigned long long);
                print_u64_padded((uint64_t)val, width, pad);
                len = u64_to_dec_str((uint64_t)val, tmp);
            } else if (length == 1) {
                unsigned long val = va_arg(args, unsigned long);
                print_u64_padded((uint64_t)val, width, pad);
                len = u64_to_dec_str((uint64_t)val, tmp);
            } else {
                uint32_t val = va_arg(args, uint32_t);
                print_uint_padded(val, width, pad);
                len = uint_to_str(val, tmp);
            }
            for (int i = 0; i < len && buf_i < (int)sizeof(buffer) - 1; i++)
                buffer[buf_i++] = tmp[i];
            break;
        }
        case 'x': {
            char tmp[32];
            int len = 0;
            if (length >= 2) {
                unsigned long long val = va_arg(args, unsigned long long);
                print_hex64_padded((uint64_t)val, width, pad, false);
                len = u64_to_hex_str((uint64_t)val, tmp, false);
            } else if (length == 1) {
                unsigned long val = va_arg(args, unsigned long);
                print_hex64_padded((uint64_t)val, width, pad, false);
                len = u64_to_hex_str((uint64_t)val, tmp, false);
            } else {
                uint32_t val = va_arg(args, uint32_t);
                print_hex_padded(val, width, pad);
                len = hex_to_str(val, tmp, false);
            }
            for (int i = 0; i < len && buf_i < (int)sizeof(buffer) - 1; i++)
                buffer[buf_i++] = tmp[i];
            break;
        }
        case 'X': {
            char tmp[32];
            int len = 0;
            if (length >= 2) {
                unsigned long long val = va_arg(args, unsigned long long);
                print_hex64_padded((uint64_t)val, width, pad, true);
                len = u64_to_hex_str((uint64_t)val, tmp, true);
            } else if (length == 1) {
                unsigned long val = va_arg(args, unsigned long);
                print_hex64_padded((uint64_t)val, width, pad, true);
                len = u64_to_hex_str((uint64_t)val, tmp, true);
            } else {
                uint32_t val = va_arg(args, uint32_t);
                print_HEX_padded(val, width, pad);
                len = hex_to_str(val, tmp, true);
            }
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
            print_hex_padded(val, width ? width : 8, '0');
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
    bootlog_add(buffer);
}

void console_cursor_blink(void) {
    screen_cursor_blink_tick();
}
