#ifndef LUGALOS_DRIVERS_SCREEN_H
#define LUGALOS_DRIVERS_SCREEN_H

#include <stdbool.h>
#include <stdint.h>

#include "drivers/vtterm.h"

/* The whole 1-bpp screen as the terminal sees it: a status bar on the top
 * text row and the text window below it -- 37.1,
 * plan/phase37_screen_layouts_and_apps.md §1. 37.3 adds the canvas tile and
 * the layouts beside the text window; this is the object they extend.
 *
 * Portable and hardware-free like vtterm.c, so `vtselftest` checks it on
 * QEMU. On the RP2350-LCD-7 it lives in the `lcdterm` task's own state block
 * and is driven only by that task (drivers/lcd7_rp2350.c).
 *
 * **The status bar** is one inverted text row: the title on the left (an OSC
 * 0/2 from the program, or screen_set_title() from the kernel), and the
 * indicators on the right (screen_set_right(): the time, and 37.2's compose
 * state). Neither ever reaches the text window, and the text window never
 * scrolls into the bar. */

#define SCREEN_RIGHT_MAX 24u            /* bytes of UTF-8, with the NUL */

typedef struct {
    uint8_t  *fb;
    uint32_t  stride;                   /* bytes per pixel row */
    uint16_t  cols, rows;               /* the whole screen, in cells */
    uint32_t  title_drawn;              /* vt.title_seq the bar shows */
    char      right[SCREEN_RIGHT_MAX];
    vtterm_t  vt;                       /* rows 1 .. rows-1 */
    uint16_t  shadow[];                 /* the text window's cells */
} screen_t;

/* What a screen of cols x rows cells needs, shadow included. */
#define SCREEN_BYTES(cols, rows) \
    (sizeof(screen_t) + (uint32_t)(cols) * ((uint32_t)(rows) - 1u) * sizeof(uint16_t))

/* Takes over the whole buffer: clears it, draws the bar (titled `LugalOS`
 * until something says otherwise) and starts the terminal below it. `scr`
 * must hold SCREEN_BYTES(cols, rows). */
void screen_init(screen_t *scr, void *fb, uint32_t stride, unsigned cols, unsigned rows);

/* Terminal output; redraws the bar if it changed the title. */
void screen_write(screen_t *scr, const char *s, uint32_t n);

void screen_set_title(screen_t *scr, const char *s, uint32_t n);
void screen_set_right(screen_t *scr, const char *s, uint32_t n);

/* Everything again, from the shadow: the bar and every text cell. */
void screen_repaint(screen_t *scr);

/* The text window's size, which is what a program may use. */
void screen_text_size(const screen_t *scr, unsigned *cols, unsigned *rows);

#endif /* LUGALOS_DRIVERS_SCREEN_H */
