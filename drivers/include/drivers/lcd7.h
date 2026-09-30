#ifndef LUGALOS_DRIVERS_LCD7_H
#define LUGALOS_DRIVERS_LCD7_H

#include <stdint.h>

/* The RP2350-LCD-7's 800x480 RGB panel (ST7262), plan/phase36 36.3 onward.
 * Stubs everywhere else; the real body is built where the board file
 * declares CONFIG_LCD_PCLK_GPIO. */

/* Power, reset, backlight, and the PIO/DMA scan-out. Returns 0 when the
 * panel is being driven. Needs palloc; takes ~220 ms (the panel's reset). */
int lcd7_init(void);

/* 36.3: the whole screen one RGB565 colour (the data pins held from SIO). */
void lcd7_set_colour(uint16_t rgb565);

/* 36.4: the two colours the 1-bpp framebuffer maps to (1 = fg). Any pair,
 * instantly: done with the data pins' output overrides, not in PIO. */
void lcd7_set_colours(uint16_t fg, uint16_t bg);

/* 36.4: the 800 x 480 1-bpp framebuffer, 25 words per line; pixel x of line
 * y is bit x % 32 of word y * 25 + x / 32 (leftmost pixel = LSB). NULL if the
 * panel is not running.
 *
 * All 800 columns are driven, but on the RP2350-LCD-7 **column 799 is mostly
 * under the bezel** (measured 2026-09-30 with `lcd test ruler`/`stripes`):
 * lay things out on 0..798, and never put a frame line on x = 799. */
uint32_t *lcd7_framebuffer(void);

/* 36.4: `lcd test <name>`: clear, border, stripes, checker, grid, invert.
 * Returns -1 for an unknown name or no panel. */
int lcd7_test_pattern(const char *name);

/* 0..100; 0 also switches the backlight converter off. */
void lcd7_set_backlight(unsigned percent);

/* `lcd` in the shell: what the scan-out is doing. */
void lcd7_report(void);

#endif /* LUGALOS_DRIVERS_LCD7_H */
