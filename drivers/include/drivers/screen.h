#ifndef LUGALOS_DRIVERS_SCREEN_H
#define LUGALOS_DRIVERS_SCREEN_H

#include <stdbool.h>
#include <stdint.h>

#include "drivers/canvas1.h"
#include "drivers/vtterm.h"

/* The whole 1-bpp screen as the terminal sees it -- 37.1/37.1a,
 * plan/phase37_screen_layouts_and_apps.md §1.1: a menu bar, the grey
 * desktop, and a framed tile with a striped title bar holding the terminal.
 * 37.3 adds the canvas tile and the layouts; this is the object they extend.
 *
 * Portable and hardware-free like vtterm.c, so `vtselftest` checks it on
 * QEMU. On the RP2350-LCD-7 it lives in the `lcdterm` task's own state block
 * and is driven only by that task (drivers/lcd7_rp2350.c).
 *
 * The geometry, in pixels (§1.2), for a screen of w x h:
 *
 *   menu bar     y 0..19, white, a rule at y 19; the system name in bold
 *                from x 8, the indicators ending 16 px from the right edge
 *   desktop      everything else not covered, 50 % grey
 *   tile frame   x 4 .. 8 + 8*cols + 2, y 22 .. 40 + 16*rows + 1, with a
 *                1-px shadow right and below
 *   title bar    the frame's top 17 rows: border, a white row, six
 *                stripes, a white row, border; the title centred in bold
 *   text         from (8, 40): cols = w/8 - 2, rows = (h - 43) / 16
 *
 * On the 800 x 480 panel: 98 x 27 cells, the frame x 4..794, y 22..473 --
 * nothing on x 799, which is under the bezel. Text stays on the 8-px grid;
 * everything else is drawn by canvas1.c at any pixel. */

#define SCREEN_MENU_H        20
#define SCREEN_TILE_Y        22
#define SCREEN_TEXT_X        8
#define SCREEN_TEXT_Y        40
#define SCREEN_TEXT_COLS(w)  ((unsigned)(w) / 8u - 2u)
#define SCREEN_TEXT_ROWS(h)  (((unsigned)(h) - SCREEN_TEXT_Y - 3u) / 16u)

#define SCREEN_RIGHT_MAX 24u            /* bytes of UTF-8, with the NUL */
#define SCREEN_NAME_MAX  16u

typedef struct {
    canvas1_t cv;                       /* the whole buffer */
    int16_t   fx0, fy0, fx1, fy1;       /* the text tile's frame */
    uint32_t  title_drawn;              /* vt.title_seq the title bar shows */
    char      name[SCREEN_NAME_MAX];    /* the menu bar's left */
    char      right[SCREEN_RIGHT_MAX];  /* its indicators */
    vtterm_t  vt;
    uint16_t  shadow[];                 /* the text window's cells */
} screen_t;

/* What a screen of w x h pixels needs, shadow included. */
#define SCREEN_BYTES(w, h) \
    (sizeof(screen_t) + SCREEN_TEXT_COLS(w) * SCREEN_TEXT_ROWS(h) * sizeof(uint16_t))

/* Takes over the whole buffer and draws everything, with the terminal
 * empty and the tile titled `LugalOS` until something says otherwise.
 * `scr` must hold SCREEN_BYTES(w, h); w must be a multiple of 8. */
void screen_init(screen_t *scr, void *fb, uint32_t stride, unsigned w, unsigned h);

/* Terminal output; redraws the title bar if it changed the title. */
void screen_write(screen_t *scr, const char *s, uint32_t n);

void screen_set_title(screen_t *scr, const char *s, uint32_t n);
void screen_set_right(screen_t *scr, const char *s, uint32_t n);

/* Everything again: the chrome, and every text cell from the shadow. */
void screen_repaint(screen_t *scr);

/* The text window's size, which is what a program may use. */
void screen_text_size(const screen_t *scr, unsigned *cols, unsigned *rows);

#endif /* LUGALOS_DRIVERS_SCREEN_H */
