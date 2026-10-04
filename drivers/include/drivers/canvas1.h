#ifndef LUGALOS_DRIVERS_CANVAS1_H
#define LUGALOS_DRIVERS_CANVAS1_H

#include <stdbool.h>
#include <stdint.h>

/* Drawing on a 1-bpp buffer at any pixel -- 37.1a,
 * plan/phase37_screen_layouts_and_apps.md §1.1. The buffer format is
 * fbtext.h's: byte x/8 of a pixel row holds pixels x..x+7, the leftmost in
 * bit 0, 1 = foreground.
 *
 * fbtext.c draws terminal text on the 8-px grid, one byte store per glyph
 * row, because text is drawn thousands of glyphs at a time. This is for
 * everything else -- the screen's frames, title bars and menu bar now, and
 * Lisp's canvas in 37.3 -- which is drawn rarely and wants to sit at any
 * pixel: a glyph here is shifted and masked across two bytes.
 *
 * Every call clips to the canvas, so a caller may pass coordinates that
 * are partly off it. Coordinates are inclusive (x0..x1, y0..y1), and
 * relative to the canvas's origin (canvas1_window(), 37.3b). Portable,
 * and in the lcdterm task's U-mode section on the RP2350-LCD-7. */

typedef struct {
    uint8_t *fb;
    uint32_t stride;            /* bytes per pixel row */
    uint16_t w, h;              /* pixels: the drawable area, from the origin */
    int16_t  ox, oy;            /* 37.3b: the origin's place in the buffer */
} canvas1_t;

enum {
    CANVAS1_WHITE = 0,          /* background */
    CANVAS1_BLACK = 1,          /* foreground */
    CANVAS1_GREY  = 2,          /* 50 %: pixel (x, y) is set when x + y is odd */
};

void canvas1_init(canvas1_t *c, void *fb, uint32_t stride, unsigned w, unsigned h);

/* 37.3b: a canvas that is a w x h window of `parent` with its origin at
 * (x, y) in it -- a tile. Coordinates passed to the calls below are then
 * the tile's own, and everything clips to the tile. */
void canvas1_window(canvas1_t *c, const canvas1_t *parent, int x, int y, unsigned w, unsigned h);

/* A filled rectangle in one of the patterns above. */
void canvas1_fill(const canvas1_t *c, int x0, int y0, int x1, int y1, unsigned pattern);

void canvas1_hline(const canvas1_t *c, int x0, int x1, int y, unsigned pattern);
void canvas1_vline(const canvas1_t *c, int x, int y0, int y1, unsigned pattern);

/* Glyph code `code` (drivers/font8x16.h) with its top-left at (x, y),
 * rows [row0, row1) of the cell only, foreground pixels only: what is
 * under the glyph's background stays. `bold` also sets each pixel's right
 * neighbour -- the Macintosh's way, one pixel wider. */
void canvas1_glyph(const canvas1_t *c, int x, int y, uint8_t code,
                   unsigned row0, unsigned row1, bool bold);

/* UTF-8 text from (x, y), at most `max` characters, 8 px apart also in
 * bold (Spleen's glyphs leave their rightmost column blank, which is where
 * the bold pixel goes). Returns how many characters it drew. */
unsigned canvas1_text(const canvas1_t *c, int x, int y, const char *s, uint32_t n,
                      unsigned max, unsigned row0, unsigned row1, bool bold);

/* How many characters `s` is, counting a malformed byte as one. */
unsigned canvas1_text_len(const char *s, uint32_t n);

/* 37.3b: Lisp's and chess's primitives. `pattern` as for canvas1_fill();
 * GREY in a line or circle sets the pixels the grey pattern would.
 *
 *   line      Bresenham, both end points included
 *   circle    the midpoint circle's outline, centre (cx, cy), radius r
 *   invert    every pixel of the rectangle XORed: the cheapest highlight
 *   row       `n` pixels from (x, y) rightwards from packed bits, bit i of
 *             byte i/8 for pixel i (1 = foreground, 0 = background), each
 *             pixel `scale` x `scale` -- one call per cellular-automaton
 *             generation or image row instead of one per pixel
 *   get       a pixel's value, 0 or 1; 0 outside the canvas */
void canvas1_line(const canvas1_t *c, int x0, int y0, int x1, int y1, unsigned pattern);
void canvas1_circle(const canvas1_t *c, int cx, int cy, int r, unsigned pattern);
void canvas1_invert(const canvas1_t *c, int x0, int y0, int x1, int y1);
void canvas1_row(const canvas1_t *c, int x, int y, const uint8_t *bits, unsigned n, unsigned scale);
unsigned canvas1_get(const canvas1_t *c, int x, int y);

#endif /* LUGALOS_DRIVERS_CANVAS1_H */
