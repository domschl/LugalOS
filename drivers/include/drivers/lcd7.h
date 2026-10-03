#ifndef LUGALOS_DRIVERS_LCD7_H
#define LUGALOS_DRIVERS_LCD7_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel/console.h"

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

/* 36.6: the screen as a terminal. lcd7_screen_putc() draws only (the kernel
 * log's `lcd` sink); lcd7_console_putc() draws and tees to the UART console
 * path (the `lcd` console device). No-ops until the panel is running.
 * lcd7_unknown_sequences(): escape sequences swallowed without effect. */
void lcd7_screen_putc(char c);
void lcd7_screen_putc_vterm(int vid, char c);
void lcd7_console_putc(char c);

/* Where the `lcd` console also writes: UART (UART0 and its USB mirror, paced
 * at 115200 baud: 11.3 K chars/s), USB only (ACM0, the default: 89.8 K
 * chars/s), or nowhere. */
#define LCD_TEE_UART 0u
#define LCD_TEE_USB  1u
#define LCD_TEE_OFF  2u
void lcd7_set_tee(unsigned mode);
unsigned lcd7_tee(void);
uint32_t lcd7_unknown_sequences(void);

/* 36.6a: the terminal as the U-mode `lcdterm` task (after sched_init()), the
 * batch flush kernel/console.c calls at write boundaries and before input
 * waits, the batches served, and `lcdtermisotest` (a store outside the task's
 * domain that must fault; returns whether the probe entered U-mode). */
int lcd7_task_start(void);
void lcd7_screen_flush(void);

/* 37.1, plan/phase37_screen_layouts_and_apps.md: the screen's side of the
 * console (kernel/console.h's console_screen_t): flush, the text window's
 * size, and the status bar's title. NULL where there is no panel. */
const console_screen_t *lcd7_console_screen(void);
void lcd7_set_title(const char *title);

/* 37.1a: `lcd repaint` -- the whole screen again, the text from the cell
 * shadow, so whatever a test pattern drew over it is gone. */
void lcd7_repaint(void);
uint32_t lcd7_task_call_count(void);
bool lcd7_isolation_test(uintptr_t *out_canary, bool *out_exited_clean);

/* 36.5: `lcd test text` -- every glyph, a pangram, reverse video, a cursor.
 * `lcd scroll <n>` -- n numbered lines through the 100 x 30 grid, timing
 * each scroll. */
int lcd7_text_test(void);
void lcd7_scroll_test(unsigned n);

/* 36.4: `lcd test <name>`: clear, border, stripes, checker, grid, invert.
 * Returns -1 for an unknown name or no panel. */
int lcd7_test_pattern(const char *name);

/* 0..100; 0 also switches the backlight converter off. */
void lcd7_set_backlight(unsigned percent);

/* `lcd` in the shell: what the scan-out is doing. */
void lcd7_report(void);

#endif /* LUGALOS_DRIVERS_LCD7_H */
