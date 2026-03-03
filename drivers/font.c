#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "font.h"
#include "hal.h"
#include "../drivers/screen.h"
#include "../libc/string.h"
#include "../fs/fscmd.h"
#include "../mm/mem.h"
#include "font_builtin.h"
#include "kor.h"
#include "cur.h"

#define PSF1_MAGIC 0x0436u
#define PSF2_MAGIC 0x864ab572u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t headersize;
    uint32_t flags;
    uint32_t length;
    uint32_t charsize;
    uint32_t height;
    uint32_t width;
} psf2_header_t;

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t mode;
    uint8_t charsize;
} psf1_header_t;

enum {
    PSF1_MODE512   = 0x01,
    PSF1_MODEHASTAB = 0x02,
    PSF1_MODEHASSEQ = 0x04
};

// 8×16 glyph → 글자당 16바이트
//extern uint8_t font8x16[4096];    // 256 * 16

static uint8_t saved_charmap;
static uint8_t fontbuf[8192];   // 256 chars * 32 bytes each
static uint8_t g_font_width = 8;
static uint8_t g_font_height = 16;
static uint8_t g_font_row_bytes = 1;

#define FONT_UNICODE_SLOT_FIRST 0x80u
#define FONT_UNICODE_SLOT_LAST  0xFEu
#define FONT_UNICODE_MAX_GLYPHS 12000u

typedef struct {
    uint32_t codepoint;
    uint8_t glyph[FONT_GLYPH_STRIDE];
} font_unicode_glyph_t;

typedef struct {
    uint32_t codepoint;
    uint8_t left_slot;
    uint8_t right_slot;
    bool used;
} font_unicode_slot_t;

static font_unicode_glyph_t g_unicode_glyphs[FONT_UNICODE_MAX_GLYPHS];
static uint32_t g_unicode_glyph_count = 0;
static font_unicode_slot_t g_unicode_slots[(FONT_UNICODE_SLOT_LAST - FONT_UNICODE_SLOT_FIRST + 1u) / 2u];
static uint32_t g_unicode_slot_count = 0;

static void font_reset_unicode_state(void);

// ---------------------------------------------------------------
// VGA plane2 접근 시작
// ---------------------------------------------------------------
static inline void vga_begin_font_access(void) {
    hal_out8(0x3C4, 0x03);
    saved_charmap = hal_in8(0x3C5);

    hal_out8(0x3C4, 0x03);
    hal_out8(0x3C5, 0x00);

    hal_out8(0x3C4, 0x04);
    hal_out8(0x3C5, 0x07);

    hal_out8(0x3C4, 0x02);
    hal_out8(0x3C5, 0x04);

    hal_out8(0x3CE, 0x06);
    hal_out8(0x3CF, 0x00);

    hal_out8(0x3CE, 0x05);
    hal_out8(0x3CF, 0x00);

    hal_out8(0x3CE, 0x04);
    hal_out8(0x3CF, 0x02);
}

// ---------------------------------------------------------------
// VGA plane2 끝
// ---------------------------------------------------------------
static inline void vga_end_font_access(void) {
    hal_out8(0x3C4, 0x03);
    hal_out8(0x3C5, saved_charmap);

    hal_out8(0x3C4, 0x02);
    hal_out8(0x3C5, 0x03);

    hal_out8(0x3C4, 0x04);
    hal_out8(0x3C5, 0x03);

    hal_out8(0x3CE, 0x04);
    hal_out8(0x3CF, 0x00);

    hal_out8(0x3CE, 0x05);
    hal_out8(0x3CF, 0x10);

    hal_out8(0x3CE, 0x06);
    hal_out8(0x3CF, 0x0E);
}

static void vga_write_font(const uint8_t *buf8192) {
    vga_begin_font_access();
    volatile uint8_t* p = (uint8_t*)0xA0000;
    for (int i = 0; i < 8192; i++)
        p[i] = buf8192[i];
    vga_end_font_access();
}

static void font_write_vga_if_enabled(void) {
    if (!screen_is_framebuffer() && screen_vga_text_enabled())
        vga_write_font(fontbuf);
}

static void write_korean(uint8_t *buf, int ascii, const uint8_t *glyph16) {
    (void)buf;
    (void)ascii;
    (void)glyph16;
    /*
    int base = ascii * 32;
    for (int i = 0; i < 16; i++) {
        buf[base + i] = glyph16[i];
    }
    */
}

static void copy_default_font(uint8_t* out) {
    if (!out) {
        return;
    }

    if (font_builtin_psf_len >= sizeof(psf2_header_t) &&
        ((const psf2_header_t*)font_builtin_psf)->magic == PSF2_MAGIC) {
        const psf2_header_t* hdr = (const psf2_header_t*)font_builtin_psf;
        uint32_t glyphs_to_copy = hdr->length > 256 ? 256u : hdr->length;
        const uint8_t* glyph_base = font_builtin_psf + hdr->headersize;

        memset(out, 0, 8192);
        for (uint32_t i = 0; i < glyphs_to_copy; i++) {
            uint8_t* dst = out + i * 32u;
            memcpy(dst, glyph_base + (size_t)i * hdr->charsize, hdr->charsize);
        }
        return;
    }

    vga_begin_font_access();
    uint8_t* src = (uint8_t*)0xA0000;
    for (int i = 0; i < 8192; i++) {
        out[i] = src[i];
    }
    vga_end_font_access();
}

static void apply_orion_overrides(uint8_t* buf) {
    write_korean(buf, 0x80, font_ga);
    write_korean(buf, 0x81, font_na);
    write_korean(buf, 0x82, font_da);
    write_korean(buf, 0x83, font_de);
    write_korean(buf, 0x84, font_han);
    write_korean(buf, 0x85, font_min);
    write_korean(buf, 0x86, font_guk);
    write_korean(buf, 0x7F, font_cursor);
}

void font_prepare_builtin_buffer(void) {
    g_font_width = 8;
    g_font_height = 16;
    g_font_row_bytes = 1;
    font_reset_unicode_state();
    font_reset_unicode_state();
    copy_default_font(fontbuf);
    apply_orion_overrides(fontbuf);
}

static void set_error(char* errbuf, size_t errbuf_len, const char* msg) {
    if (!errbuf || errbuf_len == 0)
        return;

    strncpy(errbuf, msg, errbuf_len - 1);
    errbuf[errbuf_len - 1] = '\0';
}

static bool font_should_store_unicode(uint32_t codepoint) {
    return (codepoint >= 0x1100u && codepoint <= 0x11FFu) ||
           (codepoint >= 0x3130u && codepoint <= 0x318Fu) ||
           (codepoint >= 0xAC00u && codepoint <= 0xD7A3u);
}

static void font_reset_unicode_state(void) {
    memset(g_unicode_glyphs, 0, sizeof(g_unicode_glyphs));
    memset(g_unicode_slots, 0, sizeof(g_unicode_slots));
    g_unicode_glyph_count = 0;
    g_unicode_slot_count = 0;
}

static const uint8_t* font_find_unicode_glyph(uint32_t codepoint) {
    for (uint32_t i = 0; i < g_unicode_glyph_count; i++) {
        if (g_unicode_glyphs[i].codepoint == codepoint) {
            return g_unicode_glyphs[i].glyph;
        }
    }
    return NULL;
}

static void font_store_unicode_glyph(uint32_t codepoint, const uint8_t* glyph, uint32_t glyph_bytes) {
    if (!glyph || glyph_bytes != 32u) {
        return;
    }
    if (!font_should_store_unicode(codepoint)) {
        return;
    }
    if (font_find_unicode_glyph(codepoint)) {
        return;
    }
    if (g_unicode_glyph_count >= FONT_UNICODE_MAX_GLYPHS) {
        return;
    }

    g_unicode_glyphs[g_unicode_glyph_count].codepoint = codepoint;
    memcpy(g_unicode_glyphs[g_unicode_glyph_count].glyph, glyph, FONT_GLYPH_STRIDE);
    g_unicode_glyph_count++;
}

static void font_commit_loaded_buffer(void) {
    apply_orion_overrides(fontbuf);
    font_write_vga_if_enabled();
    screen_refresh_font_metrics();
}

static int hex_nibble(uint8_t ch) {
    if (ch >= '0' && ch <= '9') return (int)(ch - '0');
    if (ch >= 'a' && ch <= 'f') return (int)(10 + (ch - 'a'));
    if (ch >= 'A' && ch <= 'F') return (int)(10 + (ch - 'A'));
    return -1;
}

static bool font_buffer_looks_like_hex(const uint8_t* data, uint32_t size) {
    if (!data || size == 0) {
        return false;
    }

    for (uint32_t i = 0; i < size && i < 32u; i++) {
        uint8_t ch = data[i];
        if (ch == ':') {
            return true;
        }
        if (!(ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' ||
              (ch >= '0' && ch <= '9') ||
              (ch >= 'A' && ch <= 'F') ||
              (ch >= 'a' && ch <= 'f') || ch == '#')) {
            break;
        }
    }

    return false;
}

static bool font_load_hex(const uint8_t* data, uint32_t size, char* errbuf, size_t errbuf_len) {
    if (!data || size == 0) {
        set_error(errbuf, errbuf_len, "hex: buffer too small");
        return false;
    }

    font_reset_unicode_state();
    copy_default_font(fontbuf);

    uint32_t pos = 0;
    uint32_t loaded = 0;
    uint32_t ignored_high = 0;
    uint32_t ignored_shape = 0;
    uint32_t font_height = 0;

    while (pos < size) {
        while (pos < size && (data[pos] == '\r' || data[pos] == '\n')) pos++;
        if (pos >= size) break;

        uint32_t line_start = pos;
        while (pos < size && data[pos] != '\r' && data[pos] != '\n') pos++;
        uint32_t line_end = pos;

        while (line_start < line_end && (data[line_start] == ' ' || data[line_start] == '\t')) {
            line_start++;
        }
        while (line_end > line_start && (data[line_end - 1] == ' ' || data[line_end - 1] == '\t')) {
            line_end--;
        }
        if (line_start >= line_end || data[line_start] == '#') {
            continue;
        }

        uint32_t colon = line_start;
        while (colon < line_end && data[colon] != ':') colon++;
        if (colon <= line_start || colon >= line_end) {
            continue;
        }

        uint32_t codepoint = 0;
        bool code_ok = true;
        for (uint32_t i = line_start; i < colon; i++) {
            int v = hex_nibble(data[i]);
            if (v < 0) {
                code_ok = false;
                break;
            }
            codepoint = (codepoint << 4) | (uint32_t)v;
        }
        if (!code_ok) {
            continue;
        }

        uint32_t glyph_hex_start = colon + 1;
        while (glyph_hex_start < line_end && (data[glyph_hex_start] == ' ' || data[glyph_hex_start] == '\t')) {
            glyph_hex_start++;
        }
        uint32_t glyph_hex_end = glyph_hex_start;
        while (glyph_hex_end < line_end && data[glyph_hex_end] != ' ' && data[glyph_hex_end] != '\t') {
            glyph_hex_end++;
        }

        uint32_t hex_len = glyph_hex_end - glyph_hex_start;
        if ((hex_len & 1u) != 0u || hex_len == 0) {
            continue;
        }

        uint32_t glyph_bytes = hex_len / 2u;
        if (glyph_bytes == 0 || glyph_bytes > FONT_GLYPH_STRIDE) {
            ignored_shape++;
            continue;
        }

        if (hex_len != 32u && hex_len != 64u) {
            ignored_shape++;
            continue;
        }

        if (hex_len == 32u && font_height == 0) {
            font_height = glyph_bytes;
        } else if (hex_len == 32u && font_height != glyph_bytes) {
            ignored_shape++;
            continue;
        }

        uint8_t glyph_raw[FONT_GLYPH_STRIDE];
        memset(glyph_raw, 0, sizeof(glyph_raw));
        bool glyph_ok = true;
        for (uint32_t i = 0; i < glyph_bytes; i++) {
            int hi = hex_nibble(data[glyph_hex_start + i * 2u]);
            int lo = hex_nibble(data[glyph_hex_start + i * 2u + 1u]);
            if (hi < 0 || lo < 0) {
                glyph_ok = false;
                break;
            }
            glyph_raw[i] = (uint8_t)((hi << 4) | lo);
        }
        if (!glyph_ok) {
            continue;
        }

        if (codepoint > 0xFFu) {
            if (hex_len == 64u) {
                font_store_unicode_glyph(codepoint, glyph_raw, glyph_bytes);
            } else {
                ignored_high++;
            }
            continue;
        }

        if (hex_len != 32u) {
            ignored_shape++;
            continue;
        }

        uint8_t* dst = fontbuf + ((size_t)codepoint * FONT_GLYPH_STRIDE);
        memset(dst, 0, FONT_GLYPH_STRIDE);
        memcpy(dst, glyph_raw, glyph_bytes);
        loaded++;
    }

    if (loaded == 0 || font_height == 0) {
        set_error(errbuf, errbuf_len, "hex: no loadable 8-bit glyphs");
        return false;
    }

    g_font_width = 8;
    g_font_height = (uint8_t)font_height;
    g_font_row_bytes = 1;
    font_commit_loaded_buffer();

    if (g_unicode_glyph_count > 0) {
        set_error(errbuf, errbuf_len, "hex: loaded 8-bit glyphs and Hangul UTF-8 glyphs");
    } else if (ignored_high > 0) {
        set_error(errbuf, errbuf_len, "hex: ignored codepoints above U+00FF");
    } else if (ignored_shape > 0) {
        set_error(errbuf, errbuf_len, "hex: ignored mismatched glyph sizes");
    } else {
        set_error(errbuf, errbuf_len, "");
    }
    return true;
}

static void font_hex_parse_line_buf(const uint8_t* line, uint32_t len,
                                    uint32_t* loaded, uint32_t* ignored_high,
                                    uint32_t* ignored_shape, uint32_t* font_height) {
    if (!line || len == 0) {
        return;
    }

    uint32_t line_start = 0;
    uint32_t line_end = len;
    while (line_start < line_end && (line[line_start] == ' ' || line[line_start] == '\t')) {
        line_start++;
    }
    while (line_end > line_start && (line[line_end - 1] == ' ' || line[line_end - 1] == '\t')) {
        line_end--;
    }
    if (line_start >= line_end || line[line_start] == '#') {
        return;
    }

    uint32_t colon = line_start;
    while (colon < line_end && line[colon] != ':') colon++;
    if (colon <= line_start || colon >= line_end) {
        return;
    }

    uint32_t codepoint = 0;
    for (uint32_t i = line_start; i < colon; i++) {
        int v = hex_nibble(line[i]);
        if (v < 0) {
            return;
        }
        codepoint = (codepoint << 4) | (uint32_t)v;
    }

    uint32_t glyph_hex_start = colon + 1;
    while (glyph_hex_start < line_end && (line[glyph_hex_start] == ' ' || line[glyph_hex_start] == '\t')) {
        glyph_hex_start++;
    }
    uint32_t glyph_hex_end = glyph_hex_start;
    while (glyph_hex_end < line_end && line[glyph_hex_end] != ' ' && line[glyph_hex_end] != '\t') {
        glyph_hex_end++;
    }

    uint32_t hex_len = glyph_hex_end - glyph_hex_start;
    if ((hex_len & 1u) != 0u || hex_len == 0) {
        return;
    }

    uint32_t glyph_bytes = hex_len / 2u;
    if (glyph_bytes == 0 || glyph_bytes > FONT_GLYPH_STRIDE) {
        (*ignored_shape)++;
        return;
    }

    if (hex_len != 32u && hex_len != 64u) {
        (*ignored_shape)++;
        return;
    }

    if (hex_len == 32u && *font_height == 0) {
        *font_height = glyph_bytes;
    } else if (hex_len == 32u && *font_height != glyph_bytes) {
        (*ignored_shape)++;
        return;
    }

    uint8_t glyph_raw[FONT_GLYPH_STRIDE];
    memset(glyph_raw, 0, sizeof(glyph_raw));
    for (uint32_t i = 0; i < glyph_bytes; i++) {
        int hi = hex_nibble(line[glyph_hex_start + i * 2u]);
        int lo = hex_nibble(line[glyph_hex_start + i * 2u + 1u]);
        if (hi < 0 || lo < 0) {
            return;
        }
        glyph_raw[i] = (uint8_t)((hi << 4) | lo);
    }

    if (codepoint > 0xFFu) {
        if (hex_len == 64u) {
            font_store_unicode_glyph(codepoint, glyph_raw, glyph_bytes);
        } else {
            (*ignored_high)++;
        }
        return;
    }

    if (hex_len != 32u) {
        (*ignored_shape)++;
        return;
    }

    uint8_t* dst = fontbuf + ((size_t)codepoint * FONT_GLYPH_STRIDE);
    memset(dst, 0, FONT_GLYPH_STRIDE);
    memcpy(dst, glyph_raw, glyph_bytes);
    (*loaded)++;
}

static bool font_load_hex_file(const char* path, char* errbuf, size_t errbuf_len) {
    uint32_t size = fscmd_get_file_size(path);
    if (size == 0) {
        set_error(errbuf, errbuf_len, "hex: invalid size or file not found");
        return false;
    }

    uint8_t* chunk = (uint8_t*)kmalloc(4096, 0, NULL);
    if (!chunk) {
        set_error(errbuf, errbuf_len, "hex: out of memory");
        return false;
    }

    font_reset_unicode_state();
    copy_default_font(fontbuf);

    uint8_t linebuf[512];
    uint32_t line_len = 0;
    uint32_t loaded = 0;
    uint32_t ignored_high = 0;
    uint32_t ignored_shape = 0;
    uint32_t font_height = 0;

    for (uint32_t offset = 0; offset < size; ) {
        uint32_t to_read = size - offset;
        if (to_read > 4096u) to_read = 4096u;
        if (!fscmd_read_file_partial(path, offset, chunk, to_read)) {
            kfree(chunk);
            set_error(errbuf, errbuf_len, "hex: failed to read file");
            return false;
        }

        for (uint32_t i = 0; i < to_read; i++) {
            uint8_t ch = chunk[i];
            if (ch == '\r' || ch == '\n') {
                font_hex_parse_line_buf(linebuf, line_len, &loaded, &ignored_high,
                                        &ignored_shape, &font_height);
                line_len = 0;
                continue;
            }
            if (line_len + 1u < sizeof(linebuf)) {
                linebuf[line_len++] = ch;
            }
        }

        offset += to_read;
    }

    kfree(chunk);

    if (line_len > 0) {
        font_hex_parse_line_buf(linebuf, line_len, &loaded, &ignored_high,
                                &ignored_shape, &font_height);
    }

    if (loaded == 0 || font_height == 0) {
        set_error(errbuf, errbuf_len, "hex: no loadable 8-bit glyphs");
        return false;
    }

    g_font_width = 8;
    g_font_height = (uint8_t)font_height;
    g_font_row_bytes = 1;
    font_commit_loaded_buffer();

    if (g_unicode_glyph_count > 0) {
        set_error(errbuf, errbuf_len, "hex: loaded 8-bit glyphs and Hangul UTF-8 glyphs");
    } else if (ignored_high > 0) {
        set_error(errbuf, errbuf_len, "hex: ignored codepoints above U+00FF");
    } else if (ignored_shape > 0) {
        set_error(errbuf, errbuf_len, "hex: ignored mismatched glyph sizes");
    } else {
        set_error(errbuf, errbuf_len, "");
    }
    return true;
}

bool font_load_psf2(const uint8_t* data, uint32_t size, char* errbuf, size_t errbuf_len) {
    if (!data || size < sizeof(psf2_header_t)) {
        set_error(errbuf, errbuf_len, "psf2: buffer too small");
        return false;
    }

    const psf2_header_t* hdr = (const psf2_header_t*)data;

    if (hdr->magic != PSF2_MAGIC) {
        set_error(errbuf, errbuf_len, "psf2: magic mismatch");
        return false;
    }

    if (hdr->version != 0) {
        set_error(errbuf, errbuf_len, "psf2: unsupported version");
        return false;
    }

    if (hdr->headersize < sizeof(psf2_header_t) || hdr->headersize >= size) {
        set_error(errbuf, errbuf_len, "psf2: invalid header size");
        return false;
    }

    if (hdr->width == 0 || hdr->width > 8) {
        set_error(errbuf, errbuf_len, "psf2: width must be 1..8 for VGA text mode");
        return false;
    }

    if (hdr->height == 0 || hdr->height > 32) {
        set_error(errbuf, errbuf_len, "psf2: height must be 1..32");
        return false;
    }

    if (hdr->charsize == 0 || hdr->charsize > 32) {
        set_error(errbuf, errbuf_len, "psf2: charsize exceeds VGA limit");
        return false;
    }

    g_font_width = hdr->width;
    g_font_height = hdr->height;
    g_font_row_bytes = (hdr->width + 7) / 8;

    uint32_t row_bytes = (hdr->width + 7) / 8;
    if (hdr->charsize != hdr->height * row_bytes) {
        set_error(errbuf, errbuf_len, "psf2: unexpected charsize for glyphs");
        return false;
    }

    uint64_t needed = (uint64_t)hdr->headersize +
                      (uint64_t)hdr->length * (uint64_t)hdr->charsize;
    if (needed > size) {
        set_error(errbuf, errbuf_len, "psf2: file truncated");
        return false;
    }

    if (hdr->length == 0) {
        set_error(errbuf, errbuf_len, "psf2: no glyphs in file");
        return false;
    }

    font_reset_unicode_state();
    copy_default_font(fontbuf);

    uint32_t glyphs_to_copy = (hdr->length > 256) ? 256 : hdr->length;
    const uint8_t* glyph_base = data + hdr->headersize;
    for (uint32_t i = 0; i < glyphs_to_copy; i++) {
        uint8_t* dst = fontbuf + i * 32;
        memset(dst, 0, 32);
        memcpy(dst, glyph_base + (size_t)i * hdr->charsize, hdr->charsize);
    }

    font_commit_loaded_buffer();

    if (hdr->length > 256)
        set_error(errbuf, errbuf_len, "psf2: loaded first 256 glyphs only");
    else
        set_error(errbuf, errbuf_len, "");

    return true;
}

static bool font_load_psf1(const uint8_t* data, uint32_t size, char* errbuf, size_t errbuf_len) {
    if (!data || size < sizeof(psf1_header_t)) {
        set_error(errbuf, errbuf_len, "psf1: buffer too small");
        return false;
    }

    const psf1_header_t* hdr = (const psf1_header_t*)data;
    if (hdr->magic != PSF1_MAGIC) {
        set_error(errbuf, errbuf_len, "psf1: magic mismatch");
        return false;
    }

    if (hdr->charsize == 0 || hdr->charsize > 32) {
        set_error(errbuf, errbuf_len, "psf1: invalid charsize");
        return false;
    }

    g_font_width = 8;
    g_font_height = hdr->charsize;
    g_font_row_bytes = 1;

    uint32_t glyph_count = (hdr->mode & PSF1_MODE512) ? 512u : 256u;
    uint64_t needed = (uint64_t)sizeof(psf1_header_t) + (uint64_t)glyph_count * (uint64_t)hdr->charsize;
    if (needed > size) {
        set_error(errbuf, errbuf_len, "psf1: file truncated");
        return false;
    }

    copy_default_font(fontbuf);

    uint32_t glyphs_to_copy = (glyph_count > 256) ? 256 : glyph_count;
    const uint8_t* glyph_base = data + sizeof(psf1_header_t);
    for (uint32_t i = 0; i < glyphs_to_copy; i++) {
        uint8_t* dst = fontbuf + i * 32;
        memset(dst, 0, 32);
        memcpy(dst, glyph_base + (size_t)i * hdr->charsize, hdr->charsize);
    }

    font_commit_loaded_buffer();

    if (glyph_count > 256)
        set_error(errbuf, errbuf_len, "psf1: loaded first 256 glyphs only");
    else
        set_error(errbuf, errbuf_len, "");

    return true;
}

bool font_load_psf(const uint8_t* data, uint32_t size, char* errbuf, size_t errbuf_len) {
    if (!data || size < 4) {
        set_error(errbuf, errbuf_len, "psf: buffer too small");
        return false;
    }

    // Try PSF2 first
    if (size >= sizeof(psf2_header_t) && ((const psf2_header_t*)data)->magic == PSF2_MAGIC)
        return font_load_psf2(data, size, errbuf, errbuf_len);

    // Fallback to PSF1
    if (size >= sizeof(psf1_header_t) && ((const psf1_header_t*)data)->magic == PSF1_MAGIC)
        return font_load_psf1(data, size, errbuf, errbuf_len);

    if (font_buffer_looks_like_hex(data, size)) {
        return font_load_hex(data, size, errbuf, errbuf_len);
    }

    set_error(errbuf, errbuf_len, "font: unknown format");
    return false;
}

bool font_load_file(const char* path, char* errbuf, size_t errbuf_len) {
    if (!path || !*path) {
        set_error(errbuf, errbuf_len, "font: invalid path");
        return false;
    }

    uint32_t size = fscmd_get_file_size(path);
    if (size == 0) {
        set_error(errbuf, errbuf_len, "font: invalid size or file not found");
        return false;
    }

    uint8_t probe[64];
    uint32_t probe_size = size;
    if (probe_size > sizeof(probe)) probe_size = sizeof(probe);
    if (!fscmd_read_file_partial(path, 0, probe, probe_size)) {
        set_error(errbuf, errbuf_len, "font: failed to read file");
        return false;
    }

    if (font_buffer_looks_like_hex(probe, probe_size)) {
        return font_load_hex_file(path, errbuf, errbuf_len);
    }

    uint8_t* buf = (uint8_t*)kmalloc(size, 0, NULL);
    if (!buf) {
        set_error(errbuf, errbuf_len, "font: out of memory");
        return false;
    }

    int read = fscmd_read_file_by_name(path, buf, size);
    if (read < 0 || (uint32_t)read < size) {
        kfree(buf);
        set_error(errbuf, errbuf_len, "font: failed to read file");
        return false;
    }

    bool ok = font_load_psf(buf, size, errbuf, errbuf_len);
    kfree(buf);
    return ok;
}

static bool font_find_unicode_slots(uint32_t codepoint, uint8_t* out_left, uint8_t* out_right) {
    for (uint32_t i = 0; i < g_unicode_slot_count; i++) {
        if (g_unicode_slots[i].used && g_unicode_slots[i].codepoint == codepoint) {
            if (out_left) *out_left = g_unicode_slots[i].left_slot;
            if (out_right) *out_right = g_unicode_slots[i].right_slot;
            return true;
        }
    }
    return false;
}

static bool font_assign_unicode_slots(uint32_t codepoint, uint8_t* out_left, uint8_t* out_right) {
    const uint8_t* glyph = font_find_unicode_glyph(codepoint);
    if (!glyph) {
        return false;
    }
    if (font_find_unicode_slots(codepoint, out_left, out_right)) {
        return true;
    }
    if (g_unicode_slot_count >= (uint32_t)(sizeof(g_unicode_slots) / sizeof(g_unicode_slots[0]))) {
        return false;
    }

    uint8_t left = (uint8_t)(FONT_UNICODE_SLOT_FIRST + g_unicode_slot_count * 2u);
    uint8_t right = (uint8_t)(left + 1u);
    uint8_t* left_glyph = fontbuf + ((size_t)left * FONT_GLYPH_STRIDE);
    uint8_t* right_glyph = fontbuf + ((size_t)right * FONT_GLYPH_STRIDE);
    memset(left_glyph, 0, FONT_GLYPH_STRIDE);
    memset(right_glyph, 0, FONT_GLYPH_STRIDE);
    for (int row = 0; row < 16; row++) {
        left_glyph[row] = glyph[row * 2];
        right_glyph[row] = glyph[row * 2 + 1];
    }

    g_unicode_slots[g_unicode_slot_count].codepoint = codepoint;
    g_unicode_slots[g_unicode_slot_count].left_slot = left;
    g_unicode_slots[g_unicode_slot_count].right_slot = right;
    g_unicode_slots[g_unicode_slot_count].used = true;
    g_unicode_slot_count++;
    font_write_vga_if_enabled();

    if (out_left) *out_left = left;
    if (out_right) *out_right = right;
    return true;
}

int font_encode_codepoint(uint32_t codepoint, uint8_t* out_bytes, int out_cap) {
    if (!out_bytes || out_cap < 1) {
        return 0;
    }

    if (codepoint <= 0xFFu) {
        out_bytes[0] = (uint8_t)codepoint;
        return 1;
    }

    uint8_t left = 0;
    uint8_t right = 0;
    if (out_cap >= 2 && font_assign_unicode_slots(codepoint, &left, &right)) {
        out_bytes[0] = left;
        out_bytes[1] = right;
        return 2;
    }

    out_bytes[0] = '?';
    return 1;
}

// ---------------------------------------------------------------
// ★ 최종: 8×16 폰트 통째로 업로드
// ---------------------------------------------------------------
void init_font(void) {
    char errbuf[64] = {0};
    if (!font_load_psf(font_builtin_psf, font_builtin_psf_len, errbuf, sizeof(errbuf))) {
        font_prepare_builtin_buffer();
        font_write_vga_if_enabled();
    }
}

void font_reset_default(void) {
    init_font();
}

const uint8_t* font_get_glyph(uint8_t ch) {
    return fontbuf + ((size_t)ch * FONT_GLYPH_STRIDE);
}

uint8_t font_get_width(void) {
    return g_font_width;
}

uint8_t font_get_height(void) {
    return g_font_height;
}

uint8_t font_get_row_bytes(void) {
    return g_font_row_bytes;
}
