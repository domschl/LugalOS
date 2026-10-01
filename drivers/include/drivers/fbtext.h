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
 * The cell grid starts at `fb`, which need not be the buffer's top-left: a
 * text window inside the screen (37.1, the rows below the status bar) is a
 * grid whose `fb` points at its first cell and whose `stride` is still the
 * whole buffer's. Nothing here touches a byte outside cols x rows cells. */

typedef struct {
    uint8_t *fb;        /* the bitmap */
    uint32_t stride;    /* bytes per pixel row (100 for 800 pixels) */
    uint16_t cols;      /* cells across */
    uint16_t rows;      /* cells down */
    /* 37.1a: the window's first byte is this far into its pixel row, and
     * whether a scroll may move its pixel rows whole -- true when whatever
     * lies beside the window repeats every 16 rows (a frame and the grey
     * desktop do), so moving it along changes nothing. A whole-row move is
     * one word-wise copy; otherwise each pixel row moves on its own.
     * fbtext_init() sets 0 and false. */
    uint16_t xbyte;
    bool     whole_rows;
} fbtext_t;

void fbtext_init(fbtext_t *t, void *fb, uint32_t stride, unsigned cols, unsigned rows);

/* One character into one cell. A byte outside printable ASCII draws as '?':
 * a visible mark, never nothing (a missing glyph that draws nothing is how a
 * stray control byte goes unnoticed). `inverse` draws it reversed, which is
 * also the terminal's SGR 7. Out-of-range cells are ignored. */
void fbtext_putc(fbtext_t *t, unsigned col, unsigned row, char c, bool inverse);

/* 37.1: a Unicode code point's internal glyph code (drivers/font8x16.h):
 * ASCII and Latin-1 as themselves, the mapped extras, and
 * FONT8X16_REPLACEMENT for anything else, controls included. */
uint8_t fbtext_code(uint32_t cp);

/* The glyph for a code point: fbtext_code(), then the table. */
const uint8_t *fbtext_glyph(uint32_t cp);

/* One glyph code into one cell; a code below 0x20 draws the replacement. */
void fbtext_putcode(fbtext_t *t, unsigned col, unsigned row, uint8_t code, bool inverse);

/* One code point into one cell, as fbtext_putc(). */
void fbtext_putcp(fbtext_t *t, unsigned col, unsigned row, uint32_t cp, bool inverse);

/* Clear cells [col0, col1) of one cell row to background. */
void fbtext_clear_span(fbtext_t *t, unsigned row, unsigned col0, unsigned col1);

/* Clear `n` cell rows starting at `row` to background. */
void fbtext_clear_rows(fbtext_t *t, unsigned row, unsigned n);

/* Scroll the whole grid up by `n` cell rows and clear the rows that open at
 * the bottom. One word-wise move when the grid spans whole pixel rows (~46 KB
 * on the 800x480 screen, measured in 36.5); row by row inside a narrower
 * window, so the pixels beside it stay put. */
void fbtext_scroll_up(fbtext_t *t, unsigned n);

/* 37.5b: move `n` cell rows from row `src` to row `dst` (either direction,
 * overlapping or not), inside the window; the rows left behind keep their
 * pixels -- the caller clears them. */
void fbtext_move_rows(fbtext_t *t, unsigned dst, unsigned src, unsigned n);

/* Invert the bottom two pixel rows of a cell: an underline cursor. XOR, so
 * calling it twice restores the cell exactly; the caller removes it before
 * drawing into that cell and puts it back after (plan §4.3). */
void fbtext_cursor_xor(fbtext_t *t, unsigned col, unsigned row);

#endif /* LUGALOS_DRIVERS_FBTEXT_H */
