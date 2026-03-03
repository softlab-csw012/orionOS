#ifndef FONT_H
#define FONT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define FONT_GLYPH_STRIDE 32

void init_font(void);
void font_prepare_builtin_buffer(void);
bool font_load_psf(const uint8_t* data, uint32_t size, char* errbuf, size_t errbuf_len);
bool font_load_file(const char* path, char* errbuf, size_t errbuf_len);
int font_encode_codepoint(uint32_t codepoint, uint8_t* out_bytes, int out_cap);
void font_reset_default(void);
const uint8_t* font_get_glyph(uint8_t ch);
uint8_t font_get_width(void);
uint8_t font_get_height(void);
uint8_t font_get_row_bytes(void);

#endif
