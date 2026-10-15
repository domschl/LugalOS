#ifndef LUGALOS_DRIVERS_FONT_SERIF_H
#define LUGALOS_DRIVERS_FONT_SERIF_H

#include <stdint.h>

#define FONT_SERIF_FIRST 0x20
#define FONT_SERIF_LAST  0x7E
#define FONT_SERIF_COUNT (FONT_SERIF_LAST - FONT_SERIF_FIRST + 1)
#define FONT_SERIF_H     16

#include "drivers/canvas1.h"

extern const uint16_t font_serif_glyphs[FONT_SERIF_COUNT][FONT_SERIF_H];
extern const uint8_t  font_serif_widths[128];

static inline uint8_t font_serif_char_width(char c) {
    uint8_t uc = (uint8_t)c;
    return (uc < 128) ? font_serif_widths[uc] : 8;
}

static inline const uint16_t *font_serif_get_glyph(char c) {
    uint8_t uc = (uint8_t)c;
    if (uc < FONT_SERIF_FIRST || uc > FONT_SERIF_LAST) uc = '?';
    return font_serif_glyphs[uc - FONT_SERIF_FIRST];
}

unsigned font_serif_text_width(const char *s, uint32_t len);
int canvas1_serif_glyph(const canvas1_t *c, int x, int y, char ch, bool bold);
int canvas1_serif_text(const canvas1_t *c, int x, int y, const char *s, uint32_t len, bool bold);

#endif /* LUGALOS_DRIVERS_FONT_SERIF_H */
