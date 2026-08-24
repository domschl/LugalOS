/*
 * A 7-row proportional font for the Pico-Clock-Green matrix (C2,
 * plan/phase17_clock_ui_and_dcf77.md). Driver-private: nothing outside
 * drivers/pico_clock_*.c has any use for a glyph.
 *
 * Format, chosen to match the digits phase 11 already validated on hardware
 * rather than to be tidy: seven bytes, one per row, top row first, and within
 * a byte **bit 0 is the LEFTMOST column**. That is the order the SM16106
 * shift registers want and the order drivers/pico_clock_green_rp2350.c's
 * draw_glyph() already packs, so a glyph literal in this file reads left to
 * right as low bit to high bit.
 *
 * Proportional, not fixed. The text area is 22 columns (the panel is 24 and
 * the first two are the indicator LEDs), so each glyph carries its own width:
 * letters and digits are four, M and W five, 'I' three, ':' and '.' two. That
 * is the difference between a four-letter menu label fitting and having to
 * scroll -- at a uniform five columns, every one of them is exactly one
 * column too wide.
 *
 * The digits, the colon, the minus and the combined degree-C glyph are the
 * ones transcribed from the vendor's ziku.h in phase 11 -- unchanged, because
 * the clock face's fixed column layout was validated against them. Everything
 * else (A-Z and the punctuation) is authored here: ziku.h only ever had
 * A-F H L N P U.
 */

#ifndef DRIVERS_PICO_CLOCK_FONT_H
#define DRIVERS_PICO_CLOCK_FONT_H

#include "lugalos_config.h"

#include <stdbool.h>
#include <stdint.h>

/* Phase 17b, plan/phase17b_clock_task_split.md: the clock server renders text
 * from inside its U-mode domain, so this font -- code and tables both -- has
 * to live in the one executable page that domain grants (.clocktext,
 * board_clock_text_region()). Two macros rather than one because GCC refuses
 * to put const data and executable code in the same section ("section type
 * conflict"), even though the linker script's wildcard lands them in the same
 * page; drivers/st7735_rp2350.c's ST7735_UATTR/ST7735_UDATA pair is here for
 * the same reason.
 *
 * Kernel-mode callers (the appliance's own text-width arithmetic, the console
 * diagnostics) go on calling all of this normally: kernel mode is not
 * confined, and a flash page is a flash page. */
#if defined(CONFIG_BOARD_RP2350)
#define CLOCK_UATTR __attribute__((section(".clocktext"))) __attribute__((no_sanitize("undefined")))
#define CLOCK_UDATA __attribute__((section(".clocktext.rodata")))
#else
#define CLOCK_UATTR
#define CLOCK_UDATA
#endif

#define CLOCK_GLYPH_ROWS 7u

typedef struct {
    uint8_t width;                     /* columns this glyph occupies */
    uint8_t rows[CLOCK_GLYPH_ROWS];    /* bit 0 = leftmost column */
} clock_glyph_t;

/* Lower case is folded to upper case; anything with no glyph renders as a
 * space, so an unexpected character costs a gap rather than garbage. `~` is
 * the degree sign (there is no ASCII for it and the display has no code page
 * to negotiate). */
CLOCK_UATTR const clock_glyph_t *clock_font_glyph(char c);

/* Columns a string occupies, including the one-column gap between glyphs and
 * excluding a trailing one. */
CLOCK_UATTR unsigned clock_font_text_width(const char *s);

/* Render into a column-major bitmap: one byte per column, bit `r` = row r+1
 * of the display (row 0 is the indicator/weekday row and is never touched).
 * Returns the number of columns written, clamped to `cap`.
 *
 * Column-major is the opposite of the frame buffer's layout, on purpose:
 * scrolling is "blit a window of this at an offset", which in column-major is
 * a slice and in row-major would be a shift of every one of 32 bytes per
 * step. The conversion happens once, in the blit. */
CLOCK_UATTR unsigned clock_font_render(const char *s, uint8_t *cols, unsigned cap);

#endif /* DRIVERS_PICO_CLOCK_FONT_H */
