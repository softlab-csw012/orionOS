#include <stdint.h>
#include <stdbool.h>
#include "../drivers/screen.h"
#include "../drivers/keyboard.h"
#include "../libc/string.h"
#include "../mm/mem.h"
#include "../cpu/ports.h"
#include "../fs/fscmd.h"

#define MAX_LINES 256
#define NOTE_MAX_COLS  SCREEN_MAX_COLS
#define NOTE_TAB_WIDTH 4

#ifndef VGA_MEM
#define VGA_MEM ((volatile uint16_t*)0xB8000)
#endif

#ifndef ATTR
#define ATTR 0x0F
#endif

static char** buf = NULL;   // 이제 2차원 배열이 아니라 동적 포인터 배열
static int lines = 1;
static int cx = 0, cy = 0;
static int scroll_offset = 0;
static char filename[256];

static int note_cols(void) {
    int cols = screen_get_cols();
    if (cols < 1)
        cols = 1;
    if (cols > NOTE_MAX_COLS)
        cols = NOTE_MAX_COLS;
    return cols;
}

static int note_text_rows(void) {
    int rows = screen_get_rows();
    if (rows < 2)
        rows = 2;
    return rows - 1;
}

static int note_line_len(const char* s) {
    int n = 0;
    while (n < NOTE_MAX_COLS - 1 && s[n]) n++;
    return n;
}

static int note_col_from_index(const char* s, int idx) {
    int col = 0;
    int i = 0;
    while (s[i] && i < idx && i < NOTE_MAX_COLS - 1) {
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

static int note_index_from_col(const char* s, int target_col) {
    int col = 0;
    int i = 0;
    while (s[i] && i < NOTE_MAX_COLS - 1) {
        int step = 1;
        if ((uint8_t)s[i] == '\t') {
            step = NOTE_TAB_WIDTH - (col % NOTE_TAB_WIDTH);
        }
        if (col + step > target_col)
            break;
        col += step;
        i++;
    }
    return i;
}

static char note_visible_char(char ch) {
    uint8_t u = (uint8_t)ch;
    if (u == '\t') return ' ';
    if (u < 32 || u == 127) return '.';
    return ch;
}

static void note_clamp_cursor(void) {
    if (lines < 1) lines = 1;
    if (cy < 0) cy = 0;
    if (cy >= lines) cy = lines - 1;
    if (cy < 0) cy = 0;
    int len = note_line_len(buf[cy]);
    if (cx < 0) cx = 0;
    if (cx > len) cx = len;
    int cols = note_cols();
    int vis_col = note_col_from_index(buf[cy], cx);
    if (cols > 0 && vis_col >= cols) {
        cx = note_index_from_col(buf[cy], cols - 1);
    }
}

/* ─────────────── VGA helpers ─────────────── */
void enable_cursor(uint8_t start, uint8_t end) {
    port_byte_out(0x3D4, 0x0A);
    port_byte_out(0x3D5, (port_byte_in(0x3D5) & 0xC0) | start);
    port_byte_out(0x3D4, 0x0B);
    port_byte_out(0x3D5, (port_byte_in(0x3D5) & 0xE0) | end);
    screen_set_cursor_visible(true);
}
 
void disable_cursor(void) {
    port_byte_out(0x3D4, 0x0A);
    port_byte_out(0x3D5, 0x20);
    screen_set_cursor_visible(false);
}

/* ─────────────── 화면 출력 ─────────────── */
static void draw_buffer(void) {
    int cols = note_cols();
    int text_rows = note_text_rows();
    int status_row = text_rows;
    if (scroll_offset < 0)
        scroll_offset = 0;
    if (scroll_offset > lines - text_rows)
        scroll_offset = (lines > text_rows) ? (lines - text_rows) : 0;

    for (int r = 0; r < text_rows; r++) {
        int buf_row = r + scroll_offset;
        int col = 0;
        if (buf_row < lines) {
            const char* line = buf[buf_row];
            for (int i = 0; i < NOTE_MAX_COLS - 1 && line[i] && col < cols; i++) {
                if ((uint8_t)line[i] == '\t') {
                    int spaces = NOTE_TAB_WIDTH - (col % NOTE_TAB_WIDTH);
                    for (int s = 0; s < spaces && col < cols; s++) {
                        vga_putc(col++, r, ' ', ATTR);
                    }
                } else {
                    vga_putc(col++, r, note_visible_char(line[i]), ATTR);
                }
            }
        }
        while (col < cols) {
            vga_putc(col++, r, ' ', ATTR);
        }
    }

    char status[NOTE_MAX_COLS + 1];
    int vis_col = note_col_from_index(buf[cy], cx);
    int slen = snprintf(status, sizeof(status),
                        "[%s]  line %d/%d  col %d",
                        filename, cy+1, lines, vis_col + 1);
    for (int c = 0; c < cols; c++) {
        char ch = (c < slen) ? status[c] : ' ';
        vga_putc(c, status_row, ch, 0x70);
    }

    int scr_y = cy - scroll_offset;
    if (scr_y < 0) scr_y = 0;
    if (scr_y >= text_rows) scr_y = text_rows - 1;
    if (cx < 0) cx = 0;
    int scr_x = note_col_from_index(buf[cy], cx);
    if (scr_x < 0) scr_x = 0;
    if (scr_x >= cols) scr_x = cols - 1;

    char highlight = ' ';
    if (cy < lines) {
        int len = note_line_len(buf[cy]);
        if (cx < len)
            highlight = note_visible_char(buf[cy][cx]);
    }
    vga_putc(scr_x, scr_y, highlight, 0x70);

    set_cursor(scr_y, scr_x);
}

/* ─────────────── note 메인 ─────────────── */
void note(const char* fname) {
    keyboard_input_enabled = false;
    keyboard_note_debounce();
    disable_cursor();
    strcpy(filename, fname);

    /* 힙에 buf 생성 */
    buf = kmalloc(sizeof(char*) * MAX_LINES, 0, 0);
    for (int i = 0; i < MAX_LINES; i++) {
        buf[i] = kmalloc(NOTE_MAX_COLS, 0, 0);
        memset(buf[i], 0, NOTE_MAX_COLS);
    }

    lines = 1;
    cx = cy = 0;
    scroll_offset = 0;
    bool command_mode = false;

    /* 파일 로드 */
    if (fscmd_exists(fname)) {
        char* in = kmalloc(8192, 0, 0);
        int size = fscmd_read_file_by_name(fname, (uint8_t*)in, 8191);
        if (size > 0) {
            int pos = 0;
            lines = 0;
            while (pos < size && lines < MAX_LINES) {
                int c = 0;
                while (pos < size && in[pos] != '\n' &&
                       in[pos] != '\r' && in[pos] != '\0' &&
                       c < NOTE_MAX_COLS - 1) {
                    buf[lines][c++] = in[pos++];
                }
                buf[lines][c] = 0;
                if (pos < size) {
                    if (in[pos] == '\r' && pos + 1 < size && in[pos + 1] == '\n')
                        pos += 2;
                    else
                        pos += 1;
                }
                lines++;
            }
            if (lines == 0) lines = 1;
        }
        kfree(in);
    }

    draw_buffer();

    while (1) {
        int cols = note_cols();
        int text_rows = note_text_rows();
        int k = getkey();
        if (command_mode) {
            if (k == 's') {  // 저장 후 종료
                int total = 0;
                for (int i = 0; i < lines; i++)
                    total += note_line_len(buf[i]);
                if (lines > 1)
                    total += (lines - 1);

                char* out = kmalloc(total + 1, 0, 0);
                int p = 0;
                for (int i = 0; i < lines; i++) {
                    int len = note_line_len(buf[i]);
                    memcpy(&out[p], buf[i], len);
                    p += len;
                    if (i + 1 < lines)
                        out[p++] = '\n';
                }
                out[p] = '\0';
                fscmd_write_file(filename, out, (uint32_t)p);
                kfree(out);

                kprint("file saved\n");
                break;
            }
            else if (k == 'x') { // 저장 안 하고 종료
                kprint("cancel saving file\n");
                break;
            }
            else if (k == 'i') {
                command_mode = false;
            }
        }
        else {
            if (k == 27) command_mode = true;  // ESC
            else if (k == '\r' || k == '\n') {
                if (lines < MAX_LINES - 1) {
                    for (int r = lines; r > cy + 1; r--)
                        strcpy(buf[r], buf[r - 1]);

                    strcpy(buf[cy + 1], &buf[cy][cx]);
                    memset(&buf[cy][cx], 0, NOTE_MAX_COLS - cx);
                    lines++; cy++; cx = 0;
                    if (cy - scroll_offset >= text_rows) scroll_offset++;
                }
            }
            else if (k == '\b') {
                if (cx > 0) {
                    int len = note_line_len(buf[cy]);
                    if (cx > len) cx = len;
                    memmove(&buf[cy][cx - 1], &buf[cy][cx], len - cx + 1);
                    cx--;
                } else if (cy > 0) {
                    int prev_len = note_line_len(buf[cy - 1]);
                    int cur_len = note_line_len(buf[cy]);
                    if (prev_len + cur_len < cols) {
                        strcat(buf[cy - 1], buf[cy]);
                        for (int r = cy; r < lines - 1; r++)
                            strcpy(buf[r], buf[r + 1]);
                        lines--; cy--; cx = prev_len;
                    }
                }
            }
            else if (k == NOTE_KEY_LEFT) {
                if (cx > 0) cx--;
                else if (cy > 0) {
                    int prev_len = note_line_len(buf[cy - 1]);
                    if (prev_len >= cols) prev_len = cols - 1;
                    cy--; cx = prev_len;
                    if (cy < scroll_offset) scroll_offset--;
                }
            }
            else if (k == NOTE_KEY_RIGHT) {
                int len = note_line_len(buf[cy]);
                if (cx < len) cx++;
                else if (cy + 1 < lines) {
                    cy++; cx = 0;
                    if (cy - scroll_offset >= text_rows) scroll_offset++;
                }
            }
            else if (k == NOTE_KEY_UP) {
                if (cy > 0) {
                    int target_col = note_col_from_index(buf[cy], cx);
                    cy--;
                    cx = note_index_from_col(buf[cy], target_col);
                    if (cy < scroll_offset) scroll_offset--;
                }
            }
            else if (k == NOTE_KEY_DOWN) {
                if (cy + 1 < lines) {
                    int target_col = note_col_from_index(buf[cy], cx);
                    cy++;
                    cx = note_index_from_col(buf[cy], target_col);
                    if (cy - scroll_offset >= text_rows) scroll_offset++;
                }
            }
            else if (k >= 32 && k <= 126) {
                int len = note_line_len(buf[cy]);
                if (cx > len) cx = len;
                if (len < cols - 1) {
                    memmove(&buf[cy][cx + 1], &buf[cy][cx], len - cx + 1);
                    buf[cy][cx++] = (char)k;
                }
            }
        }
        note_clamp_cursor();
        draw_buffer();
    }

    /* 메모리 해제 */
    for (int i = 0; i < MAX_LINES; i++)
        kfree(buf[i]);
    kfree(buf);

    keyboard_input_enabled = true;
    enable_cursor(14, 15);
}
