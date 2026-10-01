/*
 * Drawing on a 1-bpp buffer at any pixel -- 37.1a,
 * plan/phase37_screen_layouts_and_apps.md §1.1. See drivers/canvas1.h.
 *
 * In the lcdterm task's U-mode section on the RP2350-LCD-7: no libc and no
 * string literals (a literal is .rodata, outside the task's domain).
 */

#include "drivers/canvas1.h"
#include "drivers/fbtext.h"
#include "drivers/font8x16.h"
#include "drivers/lcdterm_attr.h"

LCDTERM_UTEXT void canvas1_init(canvas1_t *c, void *fb, uint32_t stride, unsigned w, unsigned h) {
    c->fb = (uint8_t *)fb;
    c->stride = stride;
    c->w = (uint16_t)w;
    c->h = (uint16_t)h;
}

/* The pattern's byte for pixel row y. Bit b of a byte at x = 8k is pixel
 * x + b, and 8k is even, so grey's set pixels are the odd bits on even rows
 * and the even bits on odd rows. */
LCDTERM_UTEXT static uint8_t pattern_byte(unsigned pattern, int y) {
    if (pattern == CANVAS1_BLACK) return 0xffu;
    if (pattern == CANVAS1_GREY) return (y & 1) ? 0x55u : 0xaau;
    return 0x00u;
}

LCDTERM_UTEXT void canvas1_fill(const canvas1_t *c, int x0, int y0, int x1, int y1, unsigned pattern) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= (int)c->w) x1 = (int)c->w - 1;
    if (y1 >= (int)c->h) y1 = (int)c->h - 1;
    if (x0 > x1 || y0 > y1) return;
    int b0 = x0 >> 3, b1 = x1 >> 3;
    uint8_t m0 = (uint8_t)(0xffu << (x0 & 7));
    uint8_t m1 = (uint8_t)(0xffu >> (7 - (x1 & 7)));
    if (b0 == b1) m0 = m1 = (uint8_t)(m0 & m1);
    for (int y = y0; y <= y1; y++) {
        uint8_t pat = pattern_byte(pattern, y);
        uint8_t *p = c->fb + (uint32_t)y * c->stride;
        p[b0] = (uint8_t)((p[b0] & ~m0) | (pat & m0));
        for (int b = b0 + 1; b < b1; b++) p[b] = pat;
        if (b1 != b0) p[b1] = (uint8_t)((p[b1] & ~m1) | (pat & m1));
    }
}

LCDTERM_UTEXT void canvas1_hline(const canvas1_t *c, int x0, int x1, int y, unsigned pattern) {
    canvas1_fill(c, x0, y, x1, y, pattern);
}

LCDTERM_UTEXT void canvas1_vline(const canvas1_t *c, int x, int y0, int y1, unsigned pattern) {
    canvas1_fill(c, x, y0, x, y1, pattern);
}

LCDTERM_UTEXT void canvas1_glyph(const canvas1_t *c, int x, int y, uint8_t code,
                                 unsigned row0, unsigned row1, bool bold) {
    if (code < FONT8X16_FIRST) code = FONT8X16_REPLACEMENT;
    const uint8_t *g = font8x16_glyphs[code - FONT8X16_FIRST];
    if (row1 > FONT8X16_H) row1 = FONT8X16_H;
    /* Whole glyph inside: two OR-stores a row. Near an edge: pixel by pixel. */
    bool inside = x >= 0 && x + 9 <= (int)c->w;
    for (unsigned r = row0; r < row1; r++) {
        int py = y + (int)r;
        if (py < 0 || py >= (int)c->h) continue;
        uint32_t bits = g[r];
        if (bold) bits |= bits << 1;
        if (!bits) continue;
        uint8_t *p = c->fb + (uint32_t)py * c->stride;
        if (inside) {
            uint32_t v = bits << (x & 7);
            p[x >> 3] |= (uint8_t)v;
            if (v >> 8) p[(x >> 3) + 1] |= (uint8_t)(v >> 8);
            continue;
        }
        for (int b = 0; b < 9; b++) {
            int px = x + b;
            if ((bits >> b) & 1u && px >= 0 && px < (int)c->w)
                p[px >> 3] |= (uint8_t)(1u << (px & 7));
        }
    }
}

/* One code point from s[*i..n), advancing *i. A malformed or truncated
 * sequence is one replacement character, and *i moves past what it took. */
LCDTERM_UTEXT static uint32_t utf8_next(const char *s, uint32_t n, uint32_t *i) {
    unsigned char ch = (unsigned char)s[(*i)++];
    if (ch < 0x80u) return ch;
    uint32_t cp;
    unsigned need;
    if ((ch & 0xe0u) == 0xc0u)      { cp = ch & 0x1fu; need = 1; }
    else if ((ch & 0xf0u) == 0xe0u) { cp = ch & 0x0fu; need = 2; }
    else if ((ch & 0xf8u) == 0xf0u) { cp = ch & 0x07u; need = 3; }
    else return FONT8X16_REPLACEMENT;
    while (need--) {
        if (*i >= n || ((unsigned char)s[*i] & 0xc0u) != 0x80u) return FONT8X16_REPLACEMENT;
        cp = (cp << 6) | ((unsigned char)s[(*i)++] & 0x3fu);
    }
    return cp;
}

LCDTERM_UTEXT unsigned canvas1_text(const canvas1_t *c, int x, int y, const char *s, uint32_t n,
                                    unsigned max, unsigned row0, unsigned row1, bool bold) {
    uint32_t i = 0;
    unsigned k = 0;
    for (; k < max && i < n; k++)
        canvas1_glyph(c, x + 8 * (int)k, y, fbtext_code(utf8_next(s, n, &i)), row0, row1, bold);
    return k;
}

LCDTERM_UTEXT unsigned canvas1_text_len(const char *s, uint32_t n) {
    uint32_t i = 0;
    unsigned k = 0;
    while (i < n) { (void)utf8_next(s, n, &i); k++; }
    return k;
}
