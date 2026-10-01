/*
 * Text on a 1-bpp framebuffer -- 36.5, plan/phase36_rp2350_lcd7_terminal.md.
 * See drivers/fbtext.h for the buffer format and the contract.
 *
 * Deliberately byte-oriented. With 8-pixel cells and the leftmost pixel in
 * bit 0 of a byte, a glyph row is one byte store and a cell is sixteen of
 * them: no shifting, no masking, no read-modify-write. The font generator
 * (tools/gen_font_bdf.py) did the bit reversal once so this never has to.
 */

#include "drivers/fbtext.h"
#include "drivers/font8x16.h"
#include "drivers/lcdterm_attr.h"

#include <stdint.h>

/* Word-wise when the buffer allows it. Byte loops scrolled the 800x480
 * screen at 2.0 ms a line, measured on the RP2350-LCD-7 (36.5); the
 * framebuffer is page-aligned and a pixel row is 25 whole words, so the common
 * case copies words, and anything else falls back to the byte loops above. */
/* No libc on this path: on the RP2350-LCD-7 it runs in the U-mode `lcdterm`
 * task, which cannot execute kernel text (drivers/lcdterm_attr.h). The
 * compiler is kept from turning these loops back into library calls by
 * -fno-tree-loop-distribute-patterns, which the whole tree uses. */
LCDTERM_UTEXT static void bytes_move_down(uint8_t *dst, const uint8_t *src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i];     /* dst < src */
}

/* One pixel row's run of bytes, a word at a time in the middle (37.1a): a
 * text window inside frames starts a byte in from the buffer's edge, so its
 * rows are never word-aligned as a whole, but their middles are. */
LCDTERM_UTEXT static void row_zero(uint8_t *d, uint32_t n) {
    while (n && ((uintptr_t)d & 3u)) { *d++ = 0; n--; }
    uint32_t *dw = (uint32_t *)(void *)d;
    for (uint32_t i = 0; i < n / 4u; i++) dw[i] = 0;
    d += n & ~3u;
    for (uint32_t i = 0; i < (n & 3u); i++) d[i] = 0;
}

LCDTERM_UTEXT static void row_copy(uint8_t *d, const uint8_t *s, uint32_t n) {     /* d < s */
    while (n && ((uintptr_t)d & 3u)) { *d++ = *s++; n--; }
    if (((uintptr_t)s & 3u) == 0) {
        uint32_t *dw = (uint32_t *)(void *)d;
        const uint32_t *sw = (const uint32_t *)(const void *)s;
        for (uint32_t i = 0; i < n / 4u; i++) dw[i] = sw[i];
        d += n & ~3u;
        s += n & ~3u;
        n &= 3u;
    }
    bytes_move_down(d, s, n);
}

LCDTERM_UTEXT static bool word_ok(const void *p, uint32_t stride) {
    return (((uintptr_t)p | stride) & 3u) == 0;
}

LCDTERM_UTEXT void fbtext_init(fbtext_t *t, void *fb, uint32_t stride, unsigned cols, unsigned rows) {
    t->fb = (uint8_t *)fb;
    t->stride = stride;
    t->cols = (uint16_t)cols;
    t->rows = (uint16_t)rows;
    t->xbyte = 0;
    t->whole_rows = false;
}

LCDTERM_UTEXT uint8_t fbtext_code(uint32_t cp) {
    if ((cp >= 0x20u && cp <= 0x7eu) || (cp >= 0xa0u && cp <= 0xffu)) return (uint8_t)cp;
    unsigned lo = 0, hi = FONT8X16_MAP_COUNT;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2u;
        if (font8x16_map_cp[mid] == cp) return font8x16_map_code[mid];
        if (font8x16_map_cp[mid] < cp) lo = mid + 1u;
        else hi = mid;
    }
    return FONT8X16_REPLACEMENT;
}

LCDTERM_UTEXT const uint8_t *fbtext_glyph(uint32_t cp) {
    return font8x16_glyphs[fbtext_code(cp) - FONT8X16_FIRST];
}

LCDTERM_UTEXT void fbtext_putcode(fbtext_t *t, unsigned col, unsigned row, uint8_t code, bool inverse) {
    if (col >= t->cols || row >= t->rows) return;
    if (code < FONT8X16_FIRST) code = FONT8X16_REPLACEMENT;
    const uint8_t *g = font8x16_glyphs[code - FONT8X16_FIRST];
    uint8_t *p = t->fb + (uint32_t)row * FONT8X16_H * t->stride + col;
    uint8_t x = inverse ? 0xffu : 0x00u;
    for (unsigned r = 0; r < FONT8X16_H; r++) {
        *p = (uint8_t)(g[r] ^ x);
        p += t->stride;
    }
}

LCDTERM_UTEXT void fbtext_putcp(fbtext_t *t, unsigned col, unsigned row, uint32_t cp, bool inverse) {
    fbtext_putcode(t, col, row, fbtext_code(cp), inverse);
}

LCDTERM_UTEXT void fbtext_clear_span(fbtext_t *t, unsigned row, unsigned col0, unsigned col1) {
    if (row >= t->rows) return;
    if (col1 > t->cols) col1 = t->cols;
    if (col0 >= col1) return;
    uint8_t *p = t->fb + (uint32_t)row * FONT8X16_H * t->stride + col0;
    for (unsigned r = 0; r < FONT8X16_H; r++) {
        row_zero(p, col1 - col0);
        p += t->stride;
    }
}

LCDTERM_UTEXT void fbtext_putc(fbtext_t *t, unsigned col, unsigned row, char c, bool inverse) {
    unsigned char uc = (unsigned char)c;
    if (uc < 0x20u || uc > 0x7eu) uc = '?';
    fbtext_putcode(t, col, row, uc, inverse);
}

LCDTERM_UTEXT void fbtext_clear_rows(fbtext_t *t, unsigned row, unsigned n) {
    if (row >= t->rows) return;
    if (n > (unsigned)t->rows - row) n = t->rows - row;
    uint32_t px_rows = (uint32_t)n * FONT8X16_H;
    uint8_t *p = t->fb + (uint32_t)row * FONT8X16_H * t->stride;
    if (word_ok(t->fb, t->stride) && t->cols == t->stride) {
        /* The grid spans whole pixel rows: clear them as one run of words. */
        uint32_t *w = (uint32_t *)(void *)p;
        for (uint32_t i = 0; i < px_rows * t->stride / 4u; i++) w[i] = 0;
        return;
    }
    for (uint32_t y = 0; y < px_rows; y++) {
        row_zero(p, t->cols);
        p += t->stride;
    }
}

LCDTERM_UTEXT void fbtext_scroll_up(fbtext_t *t, unsigned n) {
    if (n == 0) return;
    if (n >= t->rows) {
        fbtext_clear_rows(t, 0, t->rows);
        return;
    }
    uint32_t line = FONT8X16_H * t->stride;
    uint8_t *row0 = t->fb - t->xbyte;
    if (t->whole_rows && word_ok(row0, t->stride)) {
        /* The rows beside the window move with it, unchanged (37.1a). */
        uint32_t *d = (uint32_t *)(void *)row0;
        const uint32_t *src = (const uint32_t *)(const void *)(row0 + (uint32_t)n * line);
        for (uint32_t i = 0; i < (uint32_t)(t->rows - n) * line / 4u; i++) d[i] = src[i];
    } else if (t->cols != t->stride) {
        /* A window narrower than the buffer: move only its own bytes of
         * each pixel row, top to bottom (the source is below). */
        uint32_t px_rows = (uint32_t)(t->rows - n) * FONT8X16_H;
        uint8_t *d = t->fb;
        const uint8_t *src = t->fb + (uint32_t)n * line;
        for (uint32_t y = 0; y < px_rows; y++, d += t->stride, src += t->stride)
            row_copy(d, src, t->cols);
    } else if (word_ok(t->fb, t->stride)) {
        /* Upward, so the source is always ahead of the destination: a
         * forward copy is correct although the two overlap. */
        uint32_t bytes = (uint32_t)(t->rows - n) * line;
        uint32_t *d = (uint32_t *)(void *)t->fb;
        const uint32_t *src = (const uint32_t *)(const void *)(t->fb + (uint32_t)n * line);
        for (uint32_t i = 0; i < bytes / 4u; i++) d[i] = src[i];
    } else {
        bytes_move_down(t->fb, t->fb + (uint32_t)n * line, (uint32_t)(t->rows - n) * line);
    }
    fbtext_clear_rows(t, t->rows - n, n);
}

LCDTERM_UTEXT void fbtext_cursor_xor(fbtext_t *t, unsigned col, unsigned row) {
    if (col >= t->cols || row >= t->rows) return;
    uint8_t *p = t->fb + ((uint32_t)row * FONT8X16_H + FONT8X16_H - 2u) * t->stride + col;
    p[0] ^= 0xffu;
    p[t->stride] ^= 0xffu;
}
