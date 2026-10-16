#ifndef LUGALOS_DRIVERS_FONT_NOTO_MONO_H
#define LUGALOS_DRIVERS_FONT_NOTO_MONO_H

#include <stdint.h>
#include <stdbool.h>

#include "drivers/lcdterm_attr.h"

#define FONT_NOTO_MONO_FIRST        0x20
#define FONT_NOTO_MONO_LAST         0xff
#define FONT_NOTO_MONO_REPLACEMENT  0x7f
#define FONT_NOTO_MONO_COUNT        (FONT_NOTO_MONO_LAST - FONT_NOTO_MONO_FIRST + 1)
#define FONT_NOTO_MONO_W            16
#define FONT_NOTO_MONO_H            32

/* 1-bpp bilevel glyphs: uint16_t rows, bit 0 = leftmost pixel */
extern const uint16_t font_noto_mono_glyphs[FONT_NOTO_MONO_COUNT][FONT_NOTO_MONO_H];

/* 4-bpp anti-aliased coverage masks: 16 levels (0..15), 2 pixels/byte (8 bytes/row) */
extern const uint8_t font_noto_mono_aa_glyphs[FONT_NOTO_MONO_COUNT][FONT_NOTO_MONO_H][8];

/* The code points at 0x80..0x9F, sorted, with their glyph codes */
#define FONT_NOTO_MONO_MAP_COUNT 32
extern const uint16_t font_noto_mono_map_cp[FONT_NOTO_MONO_MAP_COUNT];
extern const uint8_t font_noto_mono_map_code[FONT_NOTO_MONO_MAP_COUNT];

static inline const uint16_t *font_noto_mono_get_glyph(uint8_t code) {
    if (code < FONT_NOTO_MONO_FIRST) code = FONT_NOTO_MONO_REPLACEMENT;
    return font_noto_mono_glyphs[code - FONT_NOTO_MONO_FIRST];
}

static inline const uint8_t *font_noto_mono_get_aa_glyph(uint8_t code) {
    if (code < FONT_NOTO_MONO_FIRST) code = FONT_NOTO_MONO_REPLACEMENT;
    return &font_noto_mono_aa_glyphs[code - FONT_NOTO_MONO_FIRST][0][0];
}

/* Fast integer blending onto 8-bpp (GRAY8) */
int font_noto_mono_aa_glyph_gray8(uint8_t *fb, uint32_t stride, int clip_w, int clip_h,
                                  int x, int y, uint8_t code, uint8_t fg, uint8_t bg);

/* Fast integer blending onto 16-bpp (RGB565) */
int font_noto_mono_aa_glyph_rgb565(uint16_t *fb, uint32_t stride_pixels, int clip_w, int clip_h,
                                   int x, int y, uint8_t code, uint16_t fg, uint16_t bg);

#endif /* LUGALOS_DRIVERS_FONT_NOTO_MONO_H */
