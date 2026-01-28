#ifndef SCREEN_H
#define SCREEN_H

#include <stdint.h>
#include <stdbool.h>

#define VIDEO_ADDRESS 0xB8000
#define MAX_SCROLL_LINES 500
#define MAX_ROWS 25
#define MAX_COLS 80
#define SCREEN_MAX_COLS 240
#define SCREEN_MAX_ROWS 100
#define WHITE_ON_BLACK 0x0f
#define RED_ON_WHITE 0xf4
#define BLACK       0
#define BLUE        1
#define GREEN       2
#define CYAN        3
#define RED         4
#define MAGENTA     5
#define BROWN       6
#define LIGHT_GREY  7
#define DARK_GREY   8
#define LIGHT_BLUE  9
#define LIGHT_GREEN 10
#define LIGHT_CYAN  11
#define LIGHT_RED   12
#define LIGHT_MAGENTA 13
#define YELLOW      14
#define WHITE       15

/* Screen i/o ports */
#define REG_SCREEN_CTRL 0x3d4
#define REG_SCREEN_DATA 0x3d5

extern int prompt_col;
extern int prompt_row;

extern int input_start_offset;

/* Public kernel API */
void clear_screen(void);
void clear_input_line(void);
void kprint_at(const char *message, int col, int row);
void kprint(const char *message);
void kprint_float(double value);
void kprint_backspace();
void print_hex(uint32_t num);
int int_to_str(int value, char *buf);
void kprint_int(uint32_t num);
int putchar(int c);
uint8_t vga_attr(uint8_t fg, uint8_t bg);
void kprint_color(const char* message, uint8_t fg, uint8_t bg);
int print_char(char c, int col, int row, char attr);
void screen_put_at(int x, int y, uint8_t ch, uint8_t color);
uint16_t screen_get_at(int x, int y);
int get_cursor_offset();
void set_cursor(int row, int col);
int  get_cursor_row(void);
int  get_cursor_col(void);
void set_color(uint8_t fg, uint8_t bg);
void update_prompt_position();
int get_offset(int col, int row);
void set_cursor_offset(int offset);
void kprint_char(char c);
void kprintf(const char* fmt, ...);
void vga_putc(int x, int y, char ch, uint8_t attr);
void scroll_up_screen(void);
void scroll_down_screen(void);
bool screen_is_scrolled(void);
void screen_scroll_to_bottom(void);
void print_hex_pad(uint32_t val, int width);
void print_byte(uint8_t val);
void print_dec(uint32_t num);
void print_offset(uint32_t val);
int putchar_color(uint8_t ch, uint8_t fg, uint8_t bg);
void screen_set_framebuffer(uint64_t addr, uint32_t width, uint32_t height, uint32_t pitch, uint8_t bpp);
bool screen_is_framebuffer(void);
int screen_get_cols(void);
int screen_get_rows(void);
void screen_set_cursor_visible(bool visible);
void screen_set_cursor_blink(bool enabled);
void screen_cursor_blink_tick(void);

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t bytes_per_pixel;
    uint32_t font_w;
    uint32_t font_h;
} screen_fb_info_t;

bool screen_get_framebuffer_info(screen_fb_info_t* out);
void screen_fb_fill_rect(int x, int y, int w, int h, uint32_t color);
void screen_fb_draw_text(int x, int y, const char* text, uint32_t fg, uint32_t bg, bool transparent);
bool screen_fb_get_pixel(int x, int y, uint32_t* out);
void screen_fb_set_pixel(int x, int y, uint32_t color);

#endif
