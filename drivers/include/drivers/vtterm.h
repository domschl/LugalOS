#ifndef LUGALOS_DRIVERS_VTTERM_H
#define LUGALOS_DRIVERS_VTTERM_H

#include <stdbool.h>
#include <stdint.h>

#include "drivers/fbtext.h"

/* A terminal emulator for the 1-bpp screen: exactly the escape sequences this
 * tree emits -- 36.6, plan/phase36_rp2350_lcd7_terminal.md §4.3.
 *
 * Portable: it draws through drivers/fbtext.h into any 1-bpp buffer, so it
 * runs, and is tested (`vtselftest`), on QEMU against plain RAM.
 *
 * Understood: CR, LF (scrolling at the bottom), BS, TAB (stops every 8), and
 * the CSI sequences A/B/C/D (with counts), H/f (cursor position), J and K
 * (0/1/2), m (SGR: 0 and 27 reset, 7 reverse video; colours, 38/48 with
 * their ;5;n or ;2;r;g;b arguments, and bold are parsed and ignored -- a
 * 1-bpp screen has no colour, and reverse is the one attribute it can show
 * exactly), and ?25h/?25l (cursor on/off). UTF-8 is decoded to one cell per
 * code point, drawn with fbtext_glyph() ('?' where the font has nothing).
 * Anything else is consumed silently and counted, never printed: printing an
 * unknown sequence is how a terminal fills with `[1;36m`.
 *
 * **Pending wrap** (the VT100 last-column rule): writing the last column
 * leaves the cursor there with a wrap pending, and only the next printable
 * character wraps. Without it, the line editor's redraw of a line exactly
 * as wide as the screen scrolls the screen by one on every keystroke. */

typedef struct {
    fbtext_t  text;
    uint16_t  col, row;
    bool      pending_wrap;
    bool      inverse;          /* SGR 7 */
    bool      cursor_on;        /* ?25h / ?25l */
    bool      cursor_drawn;     /* the XOR underline is currently on screen */
    uint8_t   state;            /* parser state, vtterm.c */
    uint8_t   nparams;
    bool      private_mode;     /* CSI ? ... */
    uint16_t  params[8];
    uint32_t  utf8_cp;
    uint8_t   utf8_need;
    uint32_t  unknown;          /* sequences consumed without effect */
} vtterm_t;

/* Takes over `text`'s grid: clears it and homes the cursor. */
void vtterm_init(vtterm_t *vt, const fbtext_t *text);

/* One byte of terminal output. */
void vtterm_putc(vtterm_t *vt, char c);

void vtterm_write(vtterm_t *vt, const char *s, uint32_t n);

/* `vtselftest`: the emulator against a small RAM grid, cell by cell against
 * fbtext's own rendering. Prints each case and VTTERM_SELFTEST_OK/_FAIL;
 * returns the number of failed cases. */
int vtterm_selftest(void);

#endif /* LUGALOS_DRIVERS_VTTERM_H */
