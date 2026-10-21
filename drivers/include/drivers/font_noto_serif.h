#ifndef LUGALOS_DRIVERS_FONT_NOTO_SERIF_H
#define LUGALOS_DRIVERS_FONT_NOTO_SERIF_H

#include <stdint.h>
#include <stdbool.h>

#include "drivers/lcdterm_attr.h"
#include "drivers/canvas1.h"

#define FONT_NOTO_SERIF_FIRST  0x20
#define FONT_NOTO_SERIF_LAST   0x7e
#define FONT_NOTO_SERIF_COUNT  (FONT_NOTO_SERIF_LAST - FONT_NOTO_SERIF_FIRST + 1)
#define FONT_NOTO_SERIF_H      32
#define FONT_NOTO_SERIF_MAX_W  32

extern const uint8_t  font_noto_serif_widths[128];
extern const uint8_t  font_noto_serif_aa_glyphs[FONT_NOTO_SERIF_COUNT][FONT_NOTO_SERIF_H][16];
extern const uint32_t font_noto_serif_glyphs[FONT_NOTO_SERIF_COUNT][FONT_NOTO_SERIF_H];

static inline uint8_t font_noto_serif_char_width(char c) {
    uint8_t uc = (uint8_t)c;
    return (uc < 128) ? font_noto_serif_widths[uc] : 16;
}

static inline const uint8_t *font_noto_serif_get_aa_glyph(char c) {
    uint8_t uc = (uint8_t)c;
    if (uc < FONT_NOTO_SERIF_FIRST || uc > FONT_NOTO_SERIF_LAST) uc = '?';
    return &font_noto_serif_aa_glyphs[uc - FONT_NOTO_SERIF_FIRST][0][0];
}

unsigned font_noto_serif_text_width(const char *s, uint32_t len);

/* 1-bpp canvas rendering (classic bilevel) */
int canvas1_noto_serif_glyph(const canvas1_t *c, int x, int y, char ch, bool bold, bool italic);
int canvas1_noto_serif_text(const canvas1_t *c, int x, int y, const char *s, uint32_t len, bool bold, bool italic);

/* Fast integer blending onto 8-bpp (GRAY8) */
int font_noto_serif_aa_glyph_gray8(uint8_t *fb, uint32_t stride, int clip_w, int clip_h,
                                   int x, int y, char ch, uint8_t fg, uint8_t bg, bool bold, bool italic);
int font_noto_serif_aa_text_gray8(uint8_t *fb, uint32_t stride, int clip_w, int clip_h,
                                  int x, int y, const char *s, uint32_t len,
                                  uint8_t fg, uint8_t bg, bool bold, bool italic);

/* Fast integer blending onto 16-bpp (RGB565) */
int font_noto_serif_aa_glyph_rgb565(uint16_t *fb, uint32_t stride_pixels, int clip_w, int clip_h,
                                    int x, int y, char ch, uint16_t fg, uint16_t bg, bool bold, bool italic);
int font_noto_serif_aa_text_rgb565(uint16_t *fb, uint32_t stride_pixels, int clip_w, int clip_h,
                                   int x, int y, const char *s, uint32_t len,
                                   uint16_t fg, uint16_t bg, bool bold, bool italic);

#endif /* LUGALOS_DRIVERS_FONT_NOTO_SERIF_H */
