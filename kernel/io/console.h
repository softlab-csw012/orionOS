#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int console_write(const char* buf, size_t len);
int console_writestr(const char* s);

extern int prompt_col;
extern int prompt_row;
extern int input_start_offset;

void kprint(const char* message);
void kprint_at(const char* message, int col, int row);
void kprint_int(uint32_t num);
void kprint_float(double value);
void print_dec(uint32_t num);
void print_hex(uint32_t num);
void print_hex_pad(uint32_t val, int width);
void print_byte(uint8_t val);
void print_offset(uint32_t val);
void print_uint(uint32_t val);
void print_uint_padded(uint32_t val, int width, char pad);
void print_int_padded(int val, int width, char pad);
void print_hex_padded(uint32_t val, int width, char pad);
void print_HEX_padded(uint32_t val, int width, char pad);
void put_str(const char* s);
int int_to_str(int value, char* buf);
int uint_to_str(uint32_t value, char* buf);
int hex_to_str(uint32_t value, char* buf, bool upper);
int putchar(int c);
int putchar_color(uint8_t ch, uint8_t fg, uint8_t bg);
void kprint_char(char c);
void kprint_color(const char* message, uint8_t fg, uint8_t bg);
void kprintf(const char* fmt, ...);

void clear_screen(void);
void clear_input_line(void);
void set_color(uint8_t fg, uint8_t bg);
int get_cursor_offset(void);
void set_cursor_offset(int offset);
int get_cursor_row(void);
int get_cursor_col(void);
void set_cursor(int row, int col);

void console_cursor_blink(void);

#endif
