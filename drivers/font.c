#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "font.h"
#include "hal.h"
#include "../drivers/screen.h"
#include "../libc/string.h"
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
    if (!screen_is_framebuffer())
        vga_write_font(fontbuf);
}

static void write_korean(uint8_t *buf, int ascii, const uint8_t *glyph16) {
    int base = ascii * 32;
    for (int i = 0; i < 16; i++) {
        buf[base + i] = glyph16[i];
    }
}

static void copy_default_font(uint8_t* out) {
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

static void set_error(char* errbuf, size_t errbuf_len, const char* msg) {
    if (!errbuf || errbuf_len == 0)
        return;

    strncpy(errbuf, msg, errbuf_len - 1);
    errbuf[errbuf_len - 1] = '\0';
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

    copy_default_font(fontbuf);

    uint32_t glyphs_to_copy = (hdr->length > 256) ? 256 : hdr->length;
    const uint8_t* glyph_base = data + hdr->headersize;
    for (uint32_t i = 0; i < glyphs_to_copy; i++) {
        uint8_t* dst = fontbuf + i * 32;
        memset(dst, 0, 32);
        memcpy(dst, glyph_base + (size_t)i * hdr->charsize, hdr->charsize);
    }

    apply_orion_overrides(fontbuf);
    font_write_vga_if_enabled();

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

    apply_orion_overrides(fontbuf);
    font_write_vga_if_enabled();

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

    set_error(errbuf, errbuf_len, "psf: unknown format");
    return false;
}

// ---------------------------------------------------------------
// ★ 최종: 8×16 폰트 통째로 업로드
// ---------------------------------------------------------------
void init_font(void) {
    char errbuf[64] = {0};
    if (!font_load_psf(font_builtin_psf, font_builtin_psf_len, errbuf, sizeof(errbuf))) {
        g_font_width = 8;
        g_font_height = 16;
        g_font_row_bytes = 1;
        copy_default_font(fontbuf);
        apply_orion_overrides(fontbuf);
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
