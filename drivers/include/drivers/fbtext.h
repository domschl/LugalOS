#ifndef LUGALOS_DRIVERS_FBTEXT_H
#define LUGALOS_DRIVERS_FBTEXT_H

#include <stdbool.h>
#include <stdint.h>

/* Text on a 1-bpp framebuffer: 8x16 glyphs on a cell grid -- 36.5,
 * plan/phase36_rp2350_lcd7_terminal.md.
 *
 * Portable, with no hardware in it: the buffer is any byte-addressed 1-bpp
 * bitmap whose byte x/8 of a pixel row holds pixels x..x+7 with the leftmost
 * in bit 0 (the RP2350-LCD-7's order, drivers/lcd7.h). That is what lets
 * 36.6's terminal emulator be tested on QEMU against a plain RAM buffer.
 * 1 bits are foreground.
 *
 * The cell grid starts at the buffer's top-left. A text window smaller than
 * the screen (36.6, 36.10's `canvas-window`) extends this rather than
 * replacing it. */

typedef struct {
    uint8_t *fb;        /* the bitmap */
    uint32_t stride;    /* bytes per pixel row (100 for 800 pixels) */
    uint16_t cols;      /* cells across */
    uint16_t rows;      /* cells down */
} fbtext_t;

void fbtext_init(fbtext_t *t, void *fb, uint32_t stride, unsigned cols, unsigned rows);

/* One character into one cell. A byte outside printable ASCII draws as '?':
 * a visible mark, never nothing (a missing glyph that draws nothing is how a
 * stray control byte goes unnoticed). `inverse` draws it reversed, which is
 * also the terminal's SGR 7. Out-of-range cells are ignored. */
void fbtext_putc(fbtext_t *t, unsigned col, unsigned row, char c, bool inverse);

/* 36.6: the glyph for a Unicode code point -- printable ASCII, the box-drawing
 * set font8x16.h lists, and '?' for anything else. */
const uint8_t *fbtext_glyph(uint32_t cp);

/* One code point into one cell, as fbtext_putc(). */
void fbtext_putcp(fbtext_t *t, unsigned col, unsigned row, uint32_t cp, bool inverse);

/* Clear cells [col0, col1) of one cell row to background. */
void fbtext_clear_span(fbtext_t *t, unsigned row, unsigned col0, unsigned col1);

/* Clear `n` cell rows starting at `row` to background. */
void fbtext_clear_rows(fbtext_t *t, unsigned row, unsigned n);

/* Scroll the whole grid up by `n` cell rows and clear the rows that open at
 * the bottom. One memmove: ~46 KB on the 800x480 screen, measured in 36.5. */
void fbtext_scroll_up(fbtext_t *t, unsigned n);

/* Invert the bottom two pixel rows of a cell: an underline cursor. XOR, so
 * calling it twice restores the cell exactly; the caller removes it before
 * drawing into that cell and puts it back after (plan §4.3). */
void fbtext_cursor_xor(fbtext_t *t, unsigned col, unsigned row);

#endif /* LUGALOS_DRIVERS_FBTEXT_H */
