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

#include <stdint.h>
#include <string.h>

/* Word-wise when the buffer allows it. This tree's memmove()/memset()
 * (libc/string.c) copy a byte at a time: scrolling the 800x480 screen with
 * them took 2.0 ms per line, measured on the RP2350-LCD-7 (36.5). The
 * framebuffer is page-aligned and a pixel row is 25 whole words, so the common
 * case copies words; anything else falls back to the byte routines. */
static bool word_ok(const void *p, uint32_t stride) {
    return (((uintptr_t)p | stride) & 3u) == 0;
}

void fbtext_init(fbtext_t *t, void *fb, uint32_t stride, unsigned cols, unsigned rows) {
    t->fb = (uint8_t *)fb;
    t->stride = stride;
    t->cols = (uint16_t)cols;
    t->rows = (uint16_t)rows;
}

void fbtext_putc(fbtext_t *t, unsigned col, unsigned row, char c, bool inverse) {
    if (col >= t->cols || row >= t->rows) return;
    unsigned char uc = (unsigned char)c;
    if (uc < FONT8X16_FIRST || uc > FONT8X16_LAST) uc = '?';
    const uint8_t *g = font8x16_glyphs[uc - FONT8X16_FIRST];
    uint8_t *p = t->fb + (uint32_t)row * FONT8X16_H * t->stride + col;
    uint8_t x = inverse ? 0xffu : 0x00u;
    for (unsigned r = 0; r < FONT8X16_H; r++) {
        *p = (uint8_t)(g[r] ^ x);
        p += t->stride;
    }
}

void fbtext_clear_rows(fbtext_t *t, unsigned row, unsigned n) {
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
        memset(p, 0, t->cols);
        p += t->stride;
    }
}

void fbtext_scroll_up(fbtext_t *t, unsigned n) {
    if (n == 0) return;
    if (n >= t->rows) {
        fbtext_clear_rows(t, 0, t->rows);
        return;
    }
    uint32_t line = FONT8X16_H * t->stride;
    uint32_t bytes = (uint32_t)(t->rows - n) * line;
    if (word_ok(t->fb, t->stride)) {
        /* Upward, so the source is always ahead of the destination: a
         * forward copy is correct although the two overlap. */
        uint32_t *d = (uint32_t *)(void *)t->fb;
        const uint32_t *src = (const uint32_t *)(const void *)(t->fb + (uint32_t)n * line);
        for (uint32_t i = 0; i < bytes / 4u; i++) d[i] = src[i];
    } else {
        memmove(t->fb, t->fb + (uint32_t)n * line, bytes);
    }
    fbtext_clear_rows(t, t->rows - n, n);
}

void fbtext_cursor_xor(fbtext_t *t, unsigned col, unsigned row) {
    if (col >= t->cols || row >= t->rows) return;
    uint8_t *p = t->fb + ((uint32_t)row * FONT8X16_H + FONT8X16_H - 2u) * t->stride + col;
    p[0] ^= 0xffu;
    p[t->stride] ^= 0xffu;
}
