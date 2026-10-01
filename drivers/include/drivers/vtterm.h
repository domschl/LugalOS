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
 * (0/1/2), m (SGR: 0 and 27 reset, 7 reverse video; bold and foreground
 * colours are parsed and ignored -- a 1-bpp screen has no colour -- but a
 * **dark background** (40-47, 100-107, 48;5;n, 48;2;r;g;b darker than
 * VT_DARK_LUMA, 37.4) is shown as reverse video, so a checkerboard printed in
 * colours, such as the console chess board's, comes out as one; SGR 7 on a
 * dark background reverses back), and ?25h/?25l (cursor on/off).
 *
 * In a reversed cell the twelve chess figurines are drawn with their
 * opposite-colour glyph (37.4): a white king on a dark square is the solid
 * king glyph reversed -- a white silhouette -- and so still reads as white;
 * a black king there is the outline glyph reversed, a white rim around a
 * black body. Text selection, when it comes, is reverse video too, and keeps
 * pieces readable the same way. UTF-8 is decoded to one cell per
 * code point, drawn by its glyph code (fbtext_code(): the replacement glyph
 * where the font has nothing, and for a malformed sequence). OSC 0 and 2
 * (`ESC ] 2 ; title BEL`, or ST for BEL) set the window title (37.1); other
 * OSCs are consumed. Anything else is consumed silently and counted, never
 * printed: printing an unknown sequence is how a terminal fills with
 * `[1;36m`.
 *
 * **The cell shadow** (37.1, plan/phase37_screen_layouts_and_apps.md §2.1):
 * when given one, every cell's glyph code and attribute is kept beside the
 * pixels, so vtterm_repaint() can redraw the text after something else drew
 * over it. One uint16_t per cell, row-major over cols: the code in the low
 * byte, VT_CELL_INVERSE above it.
 *
 * **Pending wrap** (the VT100 last-column rule): writing the last column
 * leaves the cursor there with a wrap pending, and only the next printable
 * character wraps. Without it, the line editor's redraw of a line exactly
 * as wide as the screen scrolls the screen by one on every keystroke. */

#define VT_CELL_INVERSE 0x100u
#define VT_TITLE_MAX    64u        /* bytes of UTF-8, with the NUL */
#define VT_OSC_MAX      72u
#define VT_DARK_LUMA    160u       /* 0..255: a background below this is dark */

typedef struct {
    fbtext_t  text;
    uint16_t *shadow;           /* shadow_cols x rows cells, or NULL for none */
    uint16_t  shadow_cols;      /* 37.5a: the width vtterm_init() was given */
    bool      hidden;           /* 37.3b: draw into the shadow only */
    uint16_t  col, row;
    bool      pending_wrap;
    bool      inverse;          /* SGR 7 */
    bool      bg_dark;          /* 37.4: the SGR background is dark: reverse video */
    bool      cursor_on;        /* ?25h / ?25l */
    bool      cursor_drawn;     /* the XOR underline is currently on screen */
    uint8_t   state;            /* parser state, vtterm.c */
    uint8_t   nparams;
    bool      private_mode;     /* CSI ? ... */
    uint16_t  params[8];
    uint32_t  utf8_cp;
    uint8_t   utf8_need;
    uint32_t  unknown;          /* sequences consumed without effect */
    uint8_t   osc_len;
    bool      osc_esc;          /* an ESC inside an OSC: ST if '\\' follows */
    char      osc[VT_OSC_MAX];
    char      title[VT_TITLE_MAX];  /* the last OSC 0/2, NUL-terminated */
    uint32_t  title_seq;        /* bumped whenever `title` changes */
} vtterm_t;

/* Takes over `text`'s grid: clears it and homes the cursor. `shadow` holds
 * text->cols * text->rows cells, or is NULL. */
void vtterm_init(vtterm_t *vt, const fbtext_t *text, uint16_t *shadow);

/* Redraws every cell from the shadow (a no-op without one), and the cursor. */
void vtterm_repaint(vtterm_t *vt);

/* 37.3b: the window moves and changes width (a layout change). Since 37.5a
 * the shadow keeps the width vtterm_init() gave it and the window shows its
 * left part, so narrowing and widening again loses nothing written wide
 * (only text written while narrow is wrapped at that width). The window is
 * never wider than the shadow, nor taller; the cursor is clamped. Nothing is
 * drawn -- the caller repaints. */
void vtterm_resize(vtterm_t *vt, const fbtext_t *text);

/* 37.3b: hide or show the window. Hidden, output still goes to the shadow
 * (so nothing is lost while a full-screen canvas is up), and the pixels are
 * left alone; showing it again does not draw -- the caller repaints. */
void vtterm_set_hidden(vtterm_t *vt, bool hidden);

/* Sets the title as an OSC 2 would: at most VT_TITLE_MAX - 1 bytes of `s`. */
void vtterm_set_title(vtterm_t *vt, const char *s, uint32_t n);

/* One byte of terminal output. */
void vtterm_putc(vtterm_t *vt, char c);

void vtterm_write(vtterm_t *vt, const char *s, uint32_t n);

/* `vtselftest`: the emulator against a small RAM grid, cell by cell against
 * fbtext's own rendering. Prints each case and VTTERM_SELFTEST_OK/_FAIL;
 * returns the number of failed cases. */
int vtterm_selftest(void);

#endif /* LUGALOS_DRIVERS_VTTERM_H */
